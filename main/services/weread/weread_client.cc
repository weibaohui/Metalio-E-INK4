#include "weread_client.h"

#include "weread_crypto.h"

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <ctime>
#include <iterator>
#include <memory>
#include <mutex>
#include <vector>

namespace {
constexpr char kTag[] = "WereadClient";
constexpr char kHost[] = "https://weread.qq.com";
constexpr char kUserAgent[] =
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36";
constexpr size_t kMaxResponseBytes = 4 * 1024 * 1024;
constexpr char kReaderToken[] = "3c5c8717f3daf09iop3423zafeqoi";

std::mutex g_reader_token_mutex;
struct ReaderTokenCacheEntry {
    std::string book_id;
    std::string psvts;
};
std::vector<ReaderTokenCacheEntry> g_reader_token_cache;
constexpr size_t kMaxReaderTokenBooks = 8;

struct JsonDeleter { void operator()(cJSON* value) const { cJSON_Delete(value); } };
using Json = std::unique_ptr<cJSON, JsonDeleter>;

std::string JsonString(cJSON* object, const char* key) {
    cJSON* value = object != nullptr ? cJSON_GetObjectItemCaseSensitive(object, key) : nullptr;
    if (cJSON_IsString(value)) return value->valuestring;
    if (cJSON_IsNumber(value)) return std::to_string(static_cast<long long>(value->valuedouble));
    return {};
}

int JsonInt(cJSON* object, const char* key, int fallback = 0) {
    cJSON* value = object != nullptr ? cJSON_GetObjectItemCaseSensitive(object, key) : nullptr;
    if (cJSON_IsNumber(value)) return value->valueint;
    if (cJSON_IsString(value) && value->valuestring != nullptr) {
        char* end = nullptr;
        const long parsed = std::strtol(value->valuestring, &end, 10);
        if (end != value->valuestring && *end == '\0') return static_cast<int>(parsed);
    }
    return fallback;
}

int JsonErrorCode(cJSON* object) {
    const int lower = JsonInt(object, "errcode", 0);
    return lower != 0 ? lower : JsonInt(object, "errCode", 0);
}

bool BodyHasExpiredLogin(const std::string& body) {
    Json root(cJSON_ParseWithLength(body.data(), body.size()));
    return root && JsonErrorCode(root.get()) == -2012;
}

std::string EscapeJson(const std::string& input) {
    std::string output;
    for (char c : input) {
        if (c == '"' || c == '\\') output += '\\';
        if (static_cast<unsigned char>(c) >= 0x20) output += c;
    }
    return output;
}

std::string ParamsJson(const std::vector<std::pair<std::string, std::string>>& params) {
    // WeRead validates JSON types as well as the signed string values. These
    // content fields must be JSON numbers; quoting them makes e_0 return {}.
    static constexpr const char* kNumericKeys[] = {"ct", "sc", "st", "r"};
    std::string output = "{";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) output += ',';
        const bool numeric = std::any_of(std::begin(kNumericKeys), std::end(kNumericKeys),
            [&](const char* key) { return params[i].first == key; });
        output += "\"" + params[i].first + "\":";
        output += numeric ? params[i].second : "\"" + EscapeJson(params[i].second) + "\"";
    }
    return output + "}";
}

bool IsEmptyJsonObject(const std::string& body) {
    return std::all_of(body.begin(), body.end(), [](unsigned char c) {
        return std::isspace(c) || c == '{' || c == '}';
    }) && body.find('{') != std::string::npos && body.find('}') != std::string::npos;
}

std::string ExtractPsvts(const std::string& html) {
    size_t key = 0;
    while ((key = html.find("\"psvts\"", key)) != std::string::npos) {
        const size_t colon = html.find(':', key + 7);
        if (colon == std::string::npos) return {};
        size_t value = colon + 1;
        while (value < html.size() && std::isspace(static_cast<unsigned char>(html[value]))) ++value;
        // The reader page can contain an earlier non-value use of the psvts
        // key. Only accept an actual JSON string value, otherwise keep looking.
        if (value < html.size() && html[value] == '"') {
            const size_t end = html.find('"', value + 1);
            if (end != std::string::npos && end > value + 1) {
                return html.substr(value + 1, end - value - 1);
            }
        }
        key += 7;
    }
    return {};
}

std::string WebAppId() {
    std::string prefix;
    int words = 0;
    size_t start = 0;
    const std::string ua = kUserAgent;
    for (size_t i = 0; i <= ua.size() && words < 12; ++i) {
        if (i == ua.size() || ua[i] == ' ' || ua[i] == '\t') {
            if (i > start) { prefix += std::to_string((i - start) % 10); ++words; }
            start = i + 1;
        }
    }
    uint32_t hash = 0;
    for (uint8_t c : ua) hash = static_cast<uint32_t>((0x83ULL * hash + c) & 0x7fffffff);
    return "wb" + prefix + "h" + std::to_string(hash);
}

}  // namespace

WereadClient::~WereadClient() { CloseSession(); }

void WereadClient::CloseSession() {
    if (session_ != nullptr) {
        esp_http_client_cleanup(static_cast<esp_http_client_handle_t>(session_));
        session_ = nullptr;
    }
    active_response_ = nullptr;
}

WereadClient::Response WereadClient::RequestOnce(const std::string& method,
                                                  const std::string& url,
                                                  const std::string& body,
                                                  const std::string& referer,
                                                  bool absorb_cookies) {
    Response response;
    response.body.reserve(url.find("/web/reader/") != std::string::npos ? 1024 * 1024 : 64 * 1024);
    if (session_ == nullptr) {
        esp_http_client_config_t config = {};
        config.url = kHost;
        config.user_agent = kUserAgent;
        config.timeout_ms = 25000;
        config.buffer_size = 4096;
        config.buffer_size_tx = 4096;
        config.keep_alive_enable = true;
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.user_data = this;
        config.event_handler = [](esp_http_client_event_t* event) -> esp_err_t {
        auto* self = static_cast<WereadClient*>(event->user_data);
        auto* out = self != nullptr ? self->active_response_ : nullptr;
        if (out == nullptr) return ESP_OK;
        if (event->event_id == HTTP_EVENT_ON_HEADER && event->header_key && event->header_value) {
            std::string key(event->header_key);
            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
            if (key == "set-cookie") out->set_cookies.emplace_back(event->header_value);
        } else if (event->event_id == HTTP_EVENT_ON_DATA && event->data && event->data_len > 0) {
            if (out->body.size() + event->data_len <= kMaxResponseBytes) {
                out->body.append(static_cast<const char*>(event->data), event->data_len);
            } else {
                out->overflow = true;
            }
        }
        return ESP_OK;
        };
        session_ = esp_http_client_init(&config);
    }

    esp_http_client_handle_t client = static_cast<esp_http_client_handle_t>(session_);
    if (client == nullptr) return response;
    active_response_ = &response;
    esp_http_client_set_url(client, url.c_str());
    esp_http_client_set_header(client, "Accept", "application/json, text/plain, */*");
    esp_http_client_set_header(client, "Origin", kHost);
    const std::string cookie = CookieHeader();
    if (!cookie.empty()) esp_http_client_set_header(client, "Cookie", cookie.c_str());
    else esp_http_client_delete_header(client, "Cookie");
    if (!referer.empty()) esp_http_client_set_header(client, "Referer", referer.c_str());
    else esp_http_client_delete_header(client, "Referer");
    if (method == "POST") {
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Content-Type", "application/json;charset=UTF-8");
        esp_http_client_set_post_field(client, body.data(), static_cast<int>(body.size()));
    } else {
        esp_http_client_set_method(client, HTTP_METHOD_GET);
        esp_http_client_delete_header(client, "Content-Type");
        esp_http_client_set_post_field(client, nullptr, 0);
    }
    const esp_err_t result = esp_http_client_perform(client);
    active_response_ = nullptr;
    if (result == ESP_OK && !response.overflow) {
        response.status = esp_http_client_get_status_code(client);
        if (absorb_cookies) AbsorbCookies(response);
    } else {
        ESP_LOGW(kTag, "HTTP failed url=%s err=%s overflow=%d",
                 url.c_str(), esp_err_to_name(result), response.overflow ? 1 : 0);
        response.body.clear();
        CloseSession();
    }
    return response;
}

WereadClient::Response WereadClient::Request(const std::string& method,
                                              const std::string& url,
                                              const std::string& body,
                                              const std::string& referer,
                                              bool absorb_cookies) {
    Response response = RequestOnce(method, url, body, referer, absorb_cookies);
    if (response.status >= 0) return response;
    ESP_LOGW(kTag, "connection failed; rebuilding and retrying once");
    vTaskDelay(pdMS_TO_TICKS(400));
    return RequestOnce(method, url, body, referer, absorb_cookies);
}

void WereadClient::AbsorbCookies(const Response& response) {
    if (auth_ == nullptr) return;
    for (const auto& header : response.set_cookies) {
        const size_t equals = header.find('=');
        if (equals == std::string::npos) continue;
        const std::string name = header.substr(0, equals);
        const size_t end = header.find(';', equals + 1);
        const std::string value = header.substr(equals + 1, end - equals - 1);
        if (name == "wr_vid") auth_->vid = value;
        else if (name == "wr_skey") auth_->skey = value;
        else if (name == "wr_gid") auth_->gid = value;
        else if (name == "wr_rt") auth_->refresh_token = value;
    }
}

std::string WereadClient::CookieHeader() const {
    if (auth_ == nullptr) return {};
    std::string result;
    if (!auth_->vid.empty()) result += "wr_vid=" + auth_->vid;
    if (!auth_->skey.empty()) result += (result.empty() ? "" : "; ") + std::string("wr_skey=") + auth_->skey;
    if (!auth_->gid.empty()) result += (result.empty() ? "" : "; ") + std::string("wr_gid=") + auth_->gid;
    if (!auth_->refresh_token.empty()) result += (result.empty() ? "" : "; ") + std::string("wr_rt=") + auth_->refresh_token;
    return result;
}

bool WereadClient::BeginQrLogin(std::string* uid, std::string* qr_url, std::string* error) {
    Response response = Request("GET", std::string(kHost) + "/api/auth/getLoginUid");
    Json root(cJSON_ParseWithLength(response.body.data(), response.body.size()));
    if (response.status / 100 != 2 || !root) {
        *error = "无法获取登录二维码";
        return false;
    }
    *uid = JsonString(root.get(), "uid");
    if (uid->empty()) *uid = JsonString(cJSON_GetObjectItem(root.get(), "data"), "uid");
    if (uid->empty()) { *error = "登录接口未返回 uid"; return false; }
    *qr_url = std::string(kHost) + "/web/confirm?uid=" + *uid;
    return true;
}

bool WereadClient::PollQrLogin(const std::string& uid,
                               const std::string& otp,
                               QrLoginState* state,
                               std::string* error) {
    if (state == nullptr) return false;
    *state = QrLoginState::kPending;
    std::string url = std::string(kHost) + "/api/auth/getLoginInfo?uid=" + uid + "&otp";
    if (!otp.empty()) url += "=" + otp;
    Response response = Request("GET", url);
    Json root(cJSON_ParseWithLength(response.body.data(), response.body.size()));
    if (response.status / 100 != 2 || !root) return true;
    cJSON* data = cJSON_GetObjectItem(root.get(), "data");
    if (!cJSON_IsObject(data)) data = root.get();
    cJSON* succeed = cJSON_GetObjectItem(data, "succeed");
    if (cJSON_IsTrue(succeed)) {
        if (auth_->vid.empty()) auth_->vid = JsonString(data, "webLoginVid");
        if (auth_->vid.empty()) auth_->vid = JsonString(data, "vid");
        if (auth_->skey.empty()) auth_->skey = JsonString(data, "accessToken");
        if (!auth_->IsLoggedIn()) {
            *error = "扫码已确认，但登录凭据不完整";
            return false;
        }
        *state = QrLoginState::kComplete;
        return true;
    }
    const std::string logic = JsonString(data, "logicCode");
    if (logic == "NEED_OTP") *state = QrLoginState::kNeedOtp;
    else if (logic == "OTP_NOT_MATCH") *state = QrLoginState::kOtpMismatch;
    else if (logic == "LOGIN_TIMEOUT" || logic == "OTP_EXPIRED") *state = QrLoginState::kExpired;
    else if (!logic.empty()) {
        *error = "扫码登录失败: " + logic;
        return false;
    }
    return true;
}

bool WereadClient::RenewSession(std::string* error) {
    if (auth_ == nullptr || !auth_->IsLoggedIn()) {
        if (error) *error = "尚未登录";
        return false;
    }
    const std::string body = "{\"rq\":\"%2Fweb%2Fbook%2Fread\",\"ql\":false}";
    Response response = Request("POST", std::string(kHost) + "/web/login/renewal",
                                body, std::string(kHost) + "/web/", false);
    Json root(cJSON_ParseWithLength(response.body.data(), response.body.size()));
    if (response.status / 100 != 2 || !root || JsonInt(root.get(), "succ") != 1) {
        if (error) *error = "登录续期失败";
        return false;
    }
    AbsorbCookies(response);
    return true;
}

bool WereadClient::FetchShelf(std::vector<WereadBook>* books, std::string* error) {
    books->clear();
    Response response = Request("GET", std::string(kHost) + "/web/shelf/sync", {}, kHost);
    Json root(cJSON_ParseWithLength(response.body.data(), response.body.size()));
    if (response.status / 100 != 2 || !root) { *error = "书架加载失败"; return false; }
    if (JsonErrorCode(root.get()) == -2012) {
        *error = "登录已过期";
        return false;
    }
    cJSON* list = cJSON_GetObjectItem(root.get(), "books");
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, list) {
        WereadBook book;
        book.id = JsonString(item, "bookId");
        book.title = JsonString(item, "title");
        book.author = JsonString(item, "author");
        if (!book.id.empty()) books->push_back(std::move(book));
    }
    cJSON* progress = cJSON_GetObjectItem(root.get(), "bookProgress");
    cJSON_ArrayForEach(item, progress) {
        const std::string id = JsonString(item, "bookId");
        auto found = std::find_if(books->begin(), books->end(), [&](const WereadBook& book) { return book.id == id; });
        if (found != books->end()) {
            found->progress = JsonInt(item, "progress");
            found->progress_chapter_uid = JsonString(item, "chapterUid");
            found->progress_chapter_offset = std::max(0, JsonInt(item, "chapterOffset"));
            found->update_time = static_cast<uint32_t>(JsonInt(item, "updateTime"));
        }
    }
    std::stable_sort(books->begin(), books->end(), [](const WereadBook& a, const WereadBook& b) {
        return a.update_time > b.update_time;
    });
    if (books->empty()) { *error = "微信读书书架为空"; return false; }
    return true;
}

bool WereadClient::FetchChapters(const std::string& book_id,
                                  std::vector<WereadChapter>* chapters,
                                  std::string* error) {
    chapters->clear();
    const std::string body = "{\"bookIds\":[\"" + EscapeJson(book_id) + "\"]}";
    Response response = Request("POST", std::string(kHost) + "/web/book/chapterInfos", body, kHost);
    ESP_LOGI(kTag, "FetchChapters book_id=%s http_status=%d bytes=%u",
             book_id.c_str(), response.status, static_cast<unsigned>(response.body.size()));
    if (response.status / 100 != 2) {
        *error = "目录加载失败(HTTP " + std::to_string(response.status) + ")";
        return false;
    }
    Json root(cJSON_ParseWithLength(response.body.data(), response.body.size()));
    if (!root) {
        *error = "目录数据解析失败";
        return false;
    }
    if (JsonErrorCode(root.get()) == -2012) {
        *error = "登录已过期";
        return false;
    }
    cJSON* data = cJSON_GetObjectItem(root.get(), "data");
    cJSON* book = nullptr;
    const int data_count = cJSON_IsArray(data) ? cJSON_GetArraySize(data) : 0;
    cJSON_ArrayForEach(book, data) {
        std::string found_id = JsonString(book, "bookId");
        if (found_id.empty()) found_id = JsonString(cJSON_GetObjectItem(book, "book"), "bookId");
        if (data_count > 1 && !found_id.empty() && found_id != book_id) {
            continue;
        }
        cJSON* list = cJSON_GetObjectItem(book, "updated");
        if (!cJSON_IsArray(list)) list = cJSON_GetObjectItem(book, "chapterInfos");
        if (!cJSON_IsArray(list)) list = cJSON_GetObjectItem(book, "chapters");
        cJSON* item = nullptr;
        cJSON_ArrayForEach(item, list) {
            WereadChapter chapter;
            chapter.uid = JsonString(item, "chapterUid");
            if (chapter.uid.empty()) chapter.uid = JsonString(item, "uid");
            if (chapter.uid.empty()) chapter.uid = JsonString(item, "chapterId");
            chapter.title = JsonString(item, "title");
            if (chapter.title.empty()) chapter.title = JsonString(item, "chapterTitle");
            chapter.index = JsonInt(item, "chapterIdx", JsonInt(item, "index", 0));
            chapter.word_count = JsonInt(item, "wordCount", 0);
            if (!chapter.uid.empty()) {
                chapters->push_back(std::move(chapter));
            }
        }
        if (!chapters->empty()) {
            break;
        }
    }
    ESP_LOGI(kTag, "FetchChapters parsed %u chapters for book_id=%s",
             static_cast<unsigned>(chapters->size()), book_id.c_str());
    if (chapters->empty()) {
        *error = "这本书没有可读章节";
        return false;
    }
    return true;
}

bool WereadClient::FetchReaderToken(const std::string& reader_url,
                                    std::string* psvts,
                                    int* status,
                                    size_t* bytes_read,
                                    std::string* error) {
    if (psvts == nullptr || status == nullptr || bytes_read == nullptr || error == nullptr) {
        return false;
    }
    psvts->clear();
    *status = -1;
    *bytes_read = 0;

    // Reader pages can be several megabytes for large or media-heavy books,
    // while the only value needed here is the short psvts token. Buffering the
    // full HTML used to hit kMaxResponseBytes and then repeat the whole request.
    // Read it as a stream and stop as soon as the token appears instead.
    esp_http_client_config_t config = {};
    config.url = reader_url.c_str();
    config.user_agent = kUserAgent;
    config.timeout_ms = 25000;
    config.buffer_size = 4096;
    config.buffer_size_tx = 4096;
    config.keep_alive_enable = true;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        *error = "无法创建阅读页请求";
        return false;
    }
    const auto cleanup = [&]() {
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
    };

    esp_http_client_set_header(client, "Accept", "text/html,application/xhtml+xml,application/json;q=0.9,*/*;q=0.8");
    esp_http_client_set_header(client, "Origin", kHost);
    esp_http_client_set_header(client, "Referer", reader_url.c_str());
    const std::string cookie = CookieHeader();
    if (!cookie.empty()) esp_http_client_set_header(client, "Cookie", cookie.c_str());
    ESP_LOGI(kTag, "Opening reader_url: %s, cookie: %s", reader_url.c_str(), cookie.c_str());

    const int64_t started_us = esp_timer_get_time();
    esp_err_t result = esp_http_client_open(client, 0);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "reader stream open failed err=%s", esp_err_to_name(result));
        *error = "阅读页连接失败";
        cleanup();
        return false;
    }
    const int64_t content_length = esp_http_client_fetch_headers(client);
    *status = esp_http_client_get_status_code(client);
    if (content_length < 0 || *status / 100 != 2) {
        ESP_LOGW(kTag, "reader stream headers failed status=%d length=%lld",
                 *status, static_cast<long long>(content_length));
        *error = "阅读页加载失败";
        cleanup();
        return false;
    }

    std::string probe;
    probe.reserve(8192);
    char buffer[4096];
    while (psvts->empty()) {
        const int read = esp_http_client_read(client, buffer, sizeof(buffer));
        if (read < 0) {
            ESP_LOGW(kTag, "reader stream read failed after=%u",
                     static_cast<unsigned>(*bytes_read));
            *error = "阅读页读取失败";
            cleanup();
            return false;
        }
        if (read == 0) break;
        *bytes_read += static_cast<size_t>(read);
        probe.append(buffer, static_cast<size_t>(read));
        *psvts = ExtractPsvts(probe);
        if (probe.size() > 8192 && psvts->empty()) {
            probe.erase(0, probe.size() - 256);
        }
    }
    cleanup();
    ESP_LOGI(kTag, "reader stream status=%d token_at=%u content_length=%lld elapsed_ms=%lld psvts_len=%u",
             *status, static_cast<unsigned>(*bytes_read),
             static_cast<long long>(content_length),
             static_cast<long long>((esp_timer_get_time() - started_us) / 1000),
             static_cast<unsigned>(psvts->size()));
    if (psvts->empty()) {
        *error = "阅读令牌缺失";
        return false;
    }
    return true;
}

bool WereadClient::ReaderTokens(const std::string& book_id,
                                 const std::string& chapter_uid,
                                 std::string* reader_url,
                                 std::string* psvts,
                                 std::string* error,
                                 bool force_refresh) {
    *reader_url = std::string(kHost) + "/web/reader/" + weread_crypto::EncodeId(book_id);
    ESP_LOGI(kTag, "ReaderTokens book_id=%s, url=%s", book_id.c_str(), reader_url->c_str());
    if (!force_refresh) {
        std::lock_guard<std::mutex> lock(g_reader_token_mutex);
        auto cached = std::find_if(g_reader_token_cache.begin(), g_reader_token_cache.end(),
            [&](const ReaderTokenCacheEntry& entry) { return entry.book_id == book_id; });
        if (cached != g_reader_token_cache.end() && !cached->psvts.empty()) {
            *psvts = cached->psvts;
            ReaderTokenCacheEntry recent = std::move(*cached);
            g_reader_token_cache.erase(cached);
            g_reader_token_cache.push_back(std::move(recent));
            ESP_LOGI(kTag, "reader token cache hit book=%s", book_id.c_str());
            return true;
        }
    }
    int status = -1;
    size_t bytes_read = 0;
    if (!FetchReaderToken(*reader_url, psvts, &status, &bytes_read, error)) return false;
    {
        std::lock_guard<std::mutex> lock(g_reader_token_mutex);
        auto old = std::find_if(g_reader_token_cache.begin(), g_reader_token_cache.end(),
            [&](const ReaderTokenCacheEntry& entry) { return entry.book_id == book_id; });
        if (old != g_reader_token_cache.end()) g_reader_token_cache.erase(old);
        g_reader_token_cache.push_back({book_id, *psvts});
        if (g_reader_token_cache.size() > kMaxReaderTokenBooks) {
            g_reader_token_cache.erase(g_reader_token_cache.begin());
        }
    }
    return true;
}

bool WereadClient::FetchChapterText(const std::string& book_id,
                                     const WereadChapter& chapter,
                                     std::string* text,
                                     std::string* error,
                                     const CancelCheck& cancelled) {
    const int64_t chapter_start_us = esp_timer_get_time();
    ESP_LOGI(kTag, "chapter memory begin internal=%u largest=%u psram=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    std::string referer;
    std::string psvts;
    const auto stop_if_cancelled = [&]() {
        if (cancelled && cancelled()) {
            *error = "请求已取消";
            return true;
        }
        return false;
    };
    if (stop_if_cancelled()) return false;
    if (!ReaderTokens(book_id, chapter.uid, &referer, &psvts, error)) return false;
    if (stop_if_cancelled()) return false;
    int64_t last_shard_request_us = 0;
    auto fetch = [&](const char* path) {
        const int64_t now_us = esp_timer_get_time();
        const int64_t remaining_us = last_shard_request_us == 0
            ? 0 : 400000 - (now_us - last_shard_request_us);
        if (remaining_us > 0) vTaskDelay(pdMS_TO_TICKS((remaining_us + 999) / 1000));
        auto params = weread_crypto::ContentParams(book_id, chapter.uid, psvts);
        const std::string body = ParamsJson(params);
        Response response = Request("POST", std::string(kHost) + path, body, referer);
        last_shard_request_us = esp_timer_get_time();
        return response;
    };
    Response first = fetch("/web/book/chapter/e_0");
    if (stop_if_cancelled()) return false;
    if (first.status / 100 != 2 || IsEmptyJsonObject(first.body)) {
        ESP_LOGW(kTag, "chapter e_0 rejected; refreshing reader token once");
        if (ReaderTokens(book_id, chapter.uid, &referer, &psvts, error, true)) {
            first = fetch("/web/book/chapter/e_0");
        }
    }
    ESP_LOGI(kTag, "chapter e_0 status=%d bytes=%u book=%s chapter=%s",
             first.status, static_cast<unsigned>(first.body.size()),
             book_id.c_str(), chapter.uid.c_str());
    if (BodyHasExpiredLogin(first.body)) {
        *error = "登录已过期";
        return false;
    }
    if (first.status / 100 != 2 || IsEmptyJsonObject(first.body)) {
        ESP_LOGW(kTag, "chapter e_0 rejected status=%d bytes=%u",
                 first.status, static_cast<unsigned>(first.body.size()));
        *error = "正文分片加载失败";
        return false;
    }
    if (first.body.size() >= 4 && first.body.compare(0, 4, "PK\x03\x04") == 0) {
        *error = "暂不支持整包 EPUB 书籍";
        return false;
    }
    const bool text_book = first.body.empty() ||
        (first.body.front() == '{' && first.body.find("\"bookId\"") != std::string::npos);
    std::vector<std::string> shards;
    if (text_book) {
        Response a = fetch("/web/book/chapter/t_0");
        if (stop_if_cancelled()) return false;
        Response b = fetch("/web/book/chapter/t_1");
        if (stop_if_cancelled()) return false;
        ESP_LOGI(kTag, "chapter TXT shards t_0=%d/%u t_1=%d/%u",
                 a.status, static_cast<unsigned>(a.body.size()),
                 b.status, static_cast<unsigned>(b.body.size()));
        if (a.status / 100 != 2 || b.status / 100 != 2 || IsEmptyJsonObject(a.body)) {
            *error = "TXT 正文分片加载失败";
            return false;
        }
        shards = {std::move(a.body), std::move(b.body)};
    } else {
        Response a = fetch("/web/book/chapter/e_1");
        if (stop_if_cancelled()) return false;
        Response b = fetch("/web/book/chapter/e_3");
        if (stop_if_cancelled()) return false;
        ESP_LOGI(kTag, "chapter EPUB shards e_0=%u e_1=%d/%u e_3=%d/%u",
                 static_cast<unsigned>(first.body.size()),
                 a.status, static_cast<unsigned>(a.body.size()),
                 b.status, static_cast<unsigned>(b.body.size()));
        if (a.status / 100 != 2 || b.status / 100 != 2) {
            *error = "EPUB 正文分片加载失败";
            return false;
        }
        shards = {std::move(first.body), std::move(a.body), std::move(b.body)};
    }
    std::string decoded;
    if (!weread_crypto::DecodeShards(shards, &decoded)) { *error = "正文解密失败"; return false; }
    *text = text_book ? std::move(decoded) : weread_crypto::StripMarkup(decoded);
    ESP_LOGI(kTag, "chapter decoded type=%s decoded=%u text=%u",
             text_book ? "txt" : "epub", static_cast<unsigned>(decoded.size()),
             static_cast<unsigned>(text->size()));
    if (text->empty()) { *error = "章节正文为空"; return false; }
    ESP_LOGI(kTag, "chapter ready bytes=%u elapsed_ms=%lld",
             static_cast<unsigned>(text->size()),
             static_cast<long long>((esp_timer_get_time() - chapter_start_us) / 1000));
    ESP_LOGI(kTag, "chapter memory ready internal=%u largest=%u psram=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    return true;
}

bool WereadClient::UploadProgress(const std::string& book_id,
                                   const WereadChapter& chapter,
                                   int chapter_offset,
                                   int percent,
                                   int read_seconds,
                                   std::string* error) {
    std::string referer;
    std::string psvts;
    if (!ReaderTokens(book_id, chapter.uid, &referer, &psvts, error)) return false;
    const long now = static_cast<long>(std::time(nullptr));
    const uint64_t ts = static_cast<uint64_t>(now) * 1000 + esp_random() % 1000;
    const uint32_t rn = esp_random() % 1000;
    std::vector<std::pair<std::string, std::string>> params = {
        {"appId", WebAppId()}, {"b", weread_crypto::EncodeId(book_id)},
        {"c", weread_crypto::EncodeId(chapter.uid)}, {"ci", std::to_string(chapter.index)},
        {"co", std::to_string(std::max(0, chapter_offset))}, {"sm", ""},
        {"pr", std::to_string(std::clamp(percent, 0, 100))},
        {"rt", std::to_string(std::clamp(read_seconds, 0, 3600))},
        {"ts", std::to_string(ts)}, {"rn", std::to_string(rn)},
        {"sg", weread_crypto::Sha256Hex(std::to_string(ts) + std::to_string(rn) + kReaderToken)},
        {"ct", std::to_string(now)}, {"ps", psvts},
        {"pc", weread_crypto::EncodeId(std::to_string(now))},
    };
    params.emplace_back("s", weread_crypto::Sign(weread_crypto::SortedQuery(params)));
    static const std::vector<std::string> numbers = {"ci", "co", "pr", "rt", "ts", "rn", "ct"};
    std::string body = "{";
    for (size_t i = 0; i < params.size(); ++i) {
        if (i) body += ',';
        const bool numeric = std::find(numbers.begin(), numbers.end(), params[i].first) != numbers.end();
        body += "\"" + params[i].first + "\":";
        body += numeric ? params[i].second : "\"" + EscapeJson(params[i].second) + "\"";
    }
    body += '}';
    Response response = Request("POST", std::string(kHost) + "/web/book/read", body, referer);
    Json root(cJSON_ParseWithLength(response.body.data(), response.body.size()));
    if (response.status / 100 != 2 || !root || JsonInt(root.get(), "succ") != 1) {
        *error = "阅读进度同步失败";
        return false;
    }
    return true;
}
