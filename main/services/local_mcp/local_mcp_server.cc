#include "local_mcp_server.h"

#include <condition_variable>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cJSON.h>

#include "application.h"
#include "core/app_manager.h"
#include "display/screen/home_screen/home_screen.h"
#include "mcp_server.h"
#include "sd_paths.h"
#include "services/dynamic_apps/dynamic_app_loader.h"
#include "settings.h"

#define TAG "LocalMCP"

namespace {

constexpr const char* kMcpProtocolVersion = "2024-11-05";
constexpr size_t kMaxRequestBody = 16 * 1024;
constexpr TickType_t kToolCallTimeoutMs = 10000;

struct PendingResponse {
    std::mutex mutex;
    std::condition_variable condition;
    bool done = false;
    std::string payload;
};

using PendingResponsePtr = std::shared_ptr<PendingResponse>;

std::string ReadBody(httpd_req_t* req) {
    const size_t content_length = req->content_len;
    if (content_length == 0 || content_length > kMaxRequestBody) {
        return {};
    }

    std::string body(content_length, '\0');
    size_t received = 0;
    while (received < content_length) {
        const int result = httpd_req_recv(req, body.data() + received,
                                          content_length - received);
        if (result <= 0) {
            if (result == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return {};
        }
        received += static_cast<size_t>(result);
    }
    return body;
}

void SetCommonHeaders(httpd_req_t* req) {
    static const char* kAllowOrigin = "*";
    static const char* kAllowHeaders =
        "Content-Type, Authorization, MCP-Protocol-Version, Mcp-Session-Id";
    static const char* kProtocol = kMcpProtocolVersion;
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", kAllowOrigin);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", kAllowHeaders);
    httpd_resp_set_hdr(req, "MCP-Protocol-Version", kProtocol);
}

void SendJsonRpc(httpd_req_t* req, const std::string& payload) {
    SetCommonHeaders(req);
    httpd_resp_set_type(req, "application/json");
    const esp_err_t result = httpd_resp_send(
        req, payload.data(), static_cast<ssize_t>(payload.size()));
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "response send failed: %s", esp_err_to_name(result));
    }
}

std::string ErrorPayload(const char* code_json_literal, const char* message) {
    std::string payload =
        "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":";
    payload += code_json_literal;
    payload += ",\"message\":\"";
    payload += message;
    payload += "\"}}";
    return payload;
}

bool CheckAuthorization(httpd_req_t* req, const std::string& token) {
    if (token.empty()) {
        return true;
    }

    const size_t header_length = httpd_req_get_hdr_value_len(req, "Authorization");
    if (header_length == 0) {
        return false;
    }
    std::string value(header_length + 1, '\0');
    if (httpd_req_get_hdr_value_str(req, "Authorization", value.data(),
                                    value.size()) != ESP_OK) {
        return false;
    }
    value.resize(header_length);
    return value == "Bearer " + token;
}

// ---- 文件直传：AI 经 curl POST 二进制到 /fs/upload，不经模型、不 base64 ----
// 与 /mcp 同端口(8080)同 Bearer PIN；path 限 /sdcard/metalio/e-ink 下相对目录。
constexpr size_t kFsUploadMaxBytes = 2 * 1024 * 1024;

bool FsCharSafe(char c) {
    return ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') || ('0' <= c && c <= '9') ||
           c == '.' || c == '_' || c == '-' || c == ' ';
}

// 相对目录白名单：拒绝 ".."、绝对路径、反斜杠与非法字符，限 apps/xxx、books 等特性目录。
bool FsRelPathSafe(const char* rel) {
    if (rel == nullptr || rel[0] == '\0' || rel[0] == '/') {
        return false;
    }
    if (strstr(rel, "..") != nullptr || strchr(rel, '\\') != nullptr) {
        return false;
    }
    for (const char* p = rel; *p != '\0'; ++p) {
        if (!FsCharSafe(*p) && *p != '/') {
            return false;
        }
    }
    return true;
}

// 文件名白名单：取 basename，拒绝隐藏文件与非法字符。
std::string FsSanitizeFilename(const char* name) {
    std::string s = (name != nullptr) ? name : "";
    const size_t slash = s.find_last_of('/');
    if (slash != std::string::npos) {
        s = s.substr(slash + 1);
    }
    if (s.empty() || s.front() == '.') {
        return {};
    }
    for (const char c : s) {
        if (!FsCharSafe(c)) {
            return {};
        }
    }
    return s;
}

esp_err_t FsUploadHandler(httpd_req_t* req) {
    const std::string token = LocalMcpServer::GetInstance().GetPin();
    if (!CheckAuthorization(req, token)) {
        SetCommonHeaders(req);
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Authorization bearer token required");
        return ESP_OK;
    }

    char query[256] = {};
    std::string rel_dir;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char pval[192] = {};
        if (httpd_query_key_value(query, "path", pval, sizeof(pval)) == ESP_OK) {
            rel_dir = pval;
        }
    }
    if (!FsRelPathSafe(rel_dir.c_str())) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid path (use <dir> relative to metalio/e-ink)");
        return ESP_FAIL;
    }

    char fn_hdr[160] = {};
    if (httpd_req_get_hdr_value_str(req, "X-Filename", fn_hdr, sizeof(fn_hdr)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing X-Filename header");
        return ESP_FAIL;
    }
    const std::string filename = FsSanitizeFilename(fn_hdr);
    if (filename.empty()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_FAIL;
    }
    if (req->content_len == 0 || req->content_len > kFsUploadMaxBytes) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body must be 1 byte..2MB");
        return ESP_FAIL;
    }

    // 逐级建目录（rel 已过白名单）
    const std::string dir = std::string(SD_APP_ROOT) + "/" + rel_dir;
    for (size_t i = sizeof(SD_APP_ROOT); i <= dir.size(); ++i) {
        if (i == dir.size() || dir[i] == '/') {
            SdEnsureDir(dir.substr(0, i).c_str());
        }
    }

    const std::string target_path = dir + "/" + filename;
    const std::string tmp_path = target_path + ".upload.tmp";
    ESP_LOGI(TAG, "fs upload -> %s (%u bytes)", target_path.c_str(),
             static_cast<unsigned>(req->content_len));

    FILE* fp = fopen(tmp_path.c_str(), "wb");
    if (fp == nullptr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot write on SD");
        return ESP_FAIL;
    }

    constexpr size_t kChunkSize = 16384;
    void* chunk = heap_caps_malloc(kChunkSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t chunk_cap = kChunkSize;
    if (chunk == nullptr) {
        chunk = malloc(4096);
        chunk_cap = 4096;
    }
    if (chunk == nullptr) {
        fclose(fp);
        unlink(tmp_path.c_str());
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }

    size_t total = 0;
    int remaining = static_cast<int>(req->content_len);
    bool ok = true;
    while (remaining > 0) {
        const int to_read = (remaining < static_cast<int>(chunk_cap)) ? remaining
                                                                     : static_cast<int>(chunk_cap);
        const int ret = httpd_req_recv(req, static_cast<char*>(chunk), to_read);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ok = false;
            break;
        }
        if (fwrite(chunk, 1, static_cast<size_t>(ret), fp) != static_cast<size_t>(ret)) {
            ok = false;
            break;
        }
        total += static_cast<size_t>(ret);
        remaining -= ret;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    fclose(fp);
    free(chunk);

    if (!ok) {
        unlink(tmp_path.c_str());
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload interrupted");
        return ESP_FAIL;
    }
    // FatFS rename 对已存在目标返回失败——覆盖语义需先删旧文件
    unlink(target_path.c_str());
    if (rename(tmp_path.c_str(), target_path.c_str()) != 0) {
        unlink(tmp_path.c_str());
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Rename failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "fs upload done: %s (%u bytes)", target_path.c_str(), static_cast<unsigned>(total));

    // 传到 apps/ 的应用包即时重扫注册，免重启
    if (rel_dir.compare(0, 5, "apps/") == 0 || rel_dir == "apps") {
        Application::GetInstance().Schedule([]() {
            dynamic_apps::ScanAndRegister();
        });
    }

    httpd_resp_set_type(req, "text/plain");
    const std::string resp = "OK " + target_path + " " + std::to_string(total) + " bytes\n";
    httpd_resp_sendstr(req, resp.c_str());
    return ESP_OK;
}

// 递归删除目录（FatFS：先清空子项再 rmdir）
bool FsRemoveRecursive(const std::string& path) {
    struct stat st = {};
    if (stat(path.c_str(), &st) != 0) {
        return true;  // 不存在视为已删除
    }
    if (!S_ISDIR(st.st_mode)) {
        return unlink(path.c_str()) == 0;
    }
    DIR* dir = opendir(path.c_str());
    if (dir == nullptr) {
        return false;
    }
    struct dirent* ent;
    bool ok = true;
    while ((ent = readdir(dir)) != nullptr) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        const std::string child = path + "/" + ent->d_name;
        struct stat cst = {};
        if (stat(child.c_str(), &cst) == 0 && S_ISDIR(cst.st_mode)) {
            ok = FsRemoveRecursive(child) && ok;
        } else {
            ok = (unlink(child.c_str()) == 0) && ok;
        }
    }
    closedir(dir);
    return rmdir(path.c_str()) == 0 && ok;
}

// 读 SD 上的 manifest.json 取应用 id（删除 apps/<dir> 前调用，用于注销桌面入口）
std::string FsReadAppId(const std::string& dir_path) {
    FILE* f = fopen((dir_path + "/manifest.json").c_str(), "rb");
    if (f == nullptr) {
        return {};
    }
    std::string text;
    char buf[256];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && text.size() < 1024) {
        text.append(buf, n);
    }
    fclose(f);

    std::string id;
    cJSON* root = cJSON_Parse(text.c_str());
    if (root != nullptr) {
        const cJSON* item = cJSON_GetObjectItem(root, "id");
        if (cJSON_IsString(item) && item->valuestring != nullptr) {
            id = item->valuestring;
        }
        cJSON_Delete(root);
    }
    return id;
}

esp_err_t FsDeleteHandler(httpd_req_t* req) {
    const std::string token = LocalMcpServer::GetInstance().GetPin();
    if (!CheckAuthorization(req, token)) {
        SetCommonHeaders(req);
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Authorization bearer token required");
        return ESP_OK;
    }

    char query[256] = {};
    std::string rel;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char pval[192] = {};
        if (httpd_query_key_value(query, "path", pval, sizeof(pval)) == ESP_OK) {
            rel = pval;
        }
    }
    if (!FsRelPathSafe(rel.c_str()) || rel.size() < 3) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                            "Invalid path (relative file or dir under metalio/e-ink)");
        return ESP_FAIL;
    }

    const std::string target = std::string(SD_APP_ROOT) + "/" + rel;

    // apps/<dir>：删除前取 id，删完注销桌面入口（主循环执行）
    std::string app_id;
    if (rel.compare(0, 5, "apps/") == 0) {
        app_id = FsReadAppId(target);
    }

    if (!FsRemoveRecursive(target)) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Delete failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "fs delete: %s", target.c_str());

    if (!app_id.empty()) {
        const std::string id = app_id;
        Application::GetInstance().Schedule([id]() {
            AppManager::GetInstance().UnregisterApp(id.c_str());
            HomeScreenRequestAppsRefresh();
        });
    }

    httpd_resp_set_type(req, "text/plain");
    const std::string resp = "OK deleted " + target + "\n";
    httpd_resp_sendstr(req, resp.c_str());
    return ESP_OK;
}

esp_err_t PostHandler(httpd_req_t* req) {
    // PIN 即 bearer token(6 位数字),从 RAM 缓存读(避免每次请求都开 NVS)。
    const std::string token = LocalMcpServer::GetInstance().GetPin();
    if (!CheckAuthorization(req, token)) {
        SetCommonHeaders(req);
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Authorization bearer token required");
        return ESP_OK;
    }

    if (req->content_len == 0) {
        SendJsonRpc(req, ErrorPayload("-32700", "Empty request body"));
        return ESP_OK;
    }
    if (req->content_len > kMaxRequestBody) {
        SendJsonRpc(req, ErrorPayload("-32600", "Request body too large"));
        return ESP_OK;
    }

    const std::string body = ReadBody(req);
    if (body.empty()) {
        SendJsonRpc(req, ErrorPayload("-32700", "Failed to read request body"));
        return ESP_OK;
    }

    cJSON* request = cJSON_ParseWithLength(body.c_str(), body.size());
    if (request == nullptr) {
        SendJsonRpc(req, ErrorPayload("-32700", "Invalid JSON"));
        return ESP_OK;
    }

    // If there is no response after a notification, Streamable HTTP requires
    // HTTP 202 with no body. Requests with an invalid shape fall back to an
    // internal JSON-RPC error so clients never hang.
    const cJSON* id = cJSON_GetObjectItem(request, "id");
    const bool expects_response = id != nullptr && !cJSON_IsNull(id);
    const cJSON* method = cJSON_GetObjectItem(request, "method");
    const bool is_notification = !expects_response && cJSON_IsString(method) &&
        strncmp(method->valuestring, "notifications", sizeof("notifications") - 1) == 0;
    auto pending = std::make_shared<PendingResponse>();
    McpServer::ResponseSink sink = [pending](const std::string& payload) {
        {
            std::lock_guard<std::mutex> lock(pending->mutex);
            pending->payload = payload;
            pending->done = true;
        }
        pending->condition.notify_all();
    };

    McpServer::GetInstance().ParseMessage(request, sink);
    cJSON_Delete(request);

    if (!pending->done && is_notification) {
        httpd_resp_set_status(req, "202 Accepted");
        SetCommonHeaders(req);
        httpd_resp_send(req, nullptr, 0);
        return ESP_OK;
    }

    {
        std::unique_lock<std::mutex> lock(pending->mutex);
        const bool finished = pending->condition.wait_for(
            lock, std::chrono::milliseconds(kToolCallTimeoutMs),
            [&pending] { return pending->done; });
        if (finished) {
            SendJsonRpc(req, pending->payload);
            return ESP_OK;
        }
    }

    if (expects_response) {
        SendJsonRpc(req, ErrorPayload("-32603", "MCP tool call timed out"));
    } else {
        httpd_resp_set_status(req, "202 Accepted");
        SetCommonHeaders(req);
        httpd_resp_send(req, nullptr, 0);
    }
    return ESP_OK;
}

esp_err_t OptionsHandler(httpd_req_t* req) {
    static const char* kAllowMethods = "GET, POST, DELETE, OPTIONS";
    SetCommonHeaders(req);
    httpd_resp_set_hdr(req, "Allow", kAllowMethods);
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

esp_err_t GetHandler(httpd_req_t* req) {
    (void)req;
    SetCommonHeaders(req);
    httpd_resp_set_status(req, "405 Method Not Allowed");
    httpd_resp_set_type(req, "application/json");
    static const char* response =
        "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32000,"
        "\"message\":\"GET SSE is not supported; use POST /mcp\"}}";
    httpd_resp_sendstr(req, response);
    return ESP_OK;
}

esp_err_t DeleteHandler(httpd_req_t* req) {
    (void)req;
    SetCommonHeaders(req);
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

}  // namespace

void LocalMcpServer::Start() {
    if (running_.exchange(true)) {
        return;
    }

    // Start 在 boot 的 main 任务(内部栈)执行,可安全读 NVS。把 PIN 载入 RAM 缓存,
    // 之后每个请求从缓存校验,不再每次开 NVS。
    {
        Settings settings("local_mcp", false);
        std::lock_guard<std::mutex> lock(pin_mutex_);
        pin_ = settings.GetString("token");
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8080;
    config.ctrl_port = 32769;
    config.stack_size = 12288;
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;

    httpd_handle_t server = nullptr;
    esp_err_t result = httpd_start(&server, &config);
    if (result != ESP_OK || server == nullptr) {
        running_.store(false);
        ESP_LOGE(TAG, "start failed on :8080: %s", esp_err_to_name(result));
        return;
    }
    server_ = server;

    httpd_uri_t post_uri = {};
    post_uri.uri = "/mcp";
    post_uri.method = HTTP_POST;
    post_uri.handler = PostHandler;
    post_uri.user_ctx = nullptr;
    httpd_uri_t get_uri = {};
    get_uri.uri = "/mcp";
    get_uri.method = HTTP_GET;
    get_uri.handler = GetHandler;
    get_uri.user_ctx = nullptr;
    httpd_uri_t options_uri = {};
    options_uri.uri = "/mcp";
    options_uri.method = HTTP_OPTIONS;
    options_uri.handler = OptionsHandler;
    options_uri.user_ctx = nullptr;
    httpd_uri_t delete_uri = {};
    delete_uri.uri = "/mcp";
    delete_uri.method = HTTP_DELETE;
    delete_uri.handler = DeleteHandler;
    delete_uri.user_ctx = nullptr;

    result = httpd_register_uri_handler(server, &post_uri);
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(server, &get_uri);
    }
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(server, &options_uri);
    }
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(server, &delete_uri);
    }
    if (result != ESP_OK) {
        httpd_stop(server);
        server_ = nullptr;
        running_.store(false);
        ESP_LOGE(TAG, "register /mcp failed");
        return;
    }

    // 文件直传端点：curl POST 二进制（不经模型、不 base64），apps/ 下自动重扫注册
    httpd_uri_t fs_upload_uri = {};
    fs_upload_uri.uri = "/fs/upload";
    fs_upload_uri.method = HTTP_POST;
    fs_upload_uri.handler = FsUploadHandler;
    fs_upload_uri.user_ctx = nullptr;
    if (httpd_register_uri_handler(server, &fs_upload_uri) != ESP_OK) {
        ESP_LOGE(TAG, "register /fs/upload failed");
    }

    // 删除端点：文件或目录（递归）；apps/<dir> 删除后自动注销桌面入口
    httpd_uri_t fs_delete_uri = {};
    fs_delete_uri.uri = "/fs/delete";
    fs_delete_uri.method = HTTP_POST;
    fs_delete_uri.handler = FsDeleteHandler;
    fs_delete_uri.user_ctx = nullptr;
    if (httpd_register_uri_handler(server, &fs_delete_uri) != ESP_OK) {
        ESP_LOGE(TAG, "register /fs/delete failed");
    }

    ESP_LOGI(TAG, "resident HTTP MCP listening on port 8080 at /mcp");
}

void LocalMcpServer::Stop() {
    if (!running_.exchange(false)) {
        return;
    }
    auto server = static_cast<httpd_handle_t>(server_);
    if (server != nullptr) {
        httpd_stop(server);
        server_ = nullptr;
    }
}

// ---- 6 位数字 PIN 认证(需求一.5) ----

bool LocalMcpServer::AuthEnabled() {
    std::lock_guard<std::mutex> lock(pin_mutex_);
    return !pin_.empty();
}

std::string LocalMcpServer::GetPin() {
    std::lock_guard<std::mutex> lock(pin_mutex_);
    return pin_;
}

void LocalMcpServer::SetPin(const std::string& pin) {
    {
        std::lock_guard<std::mutex> lock(pin_mutex_);
        if (pin_ == pin) {
            return;
        }
        pin_ = pin;
    }
    // 落盘必须离开 LVGL worker(PSRAM 栈,写 flash 会关 cache 触发断言),调度到 main。
    // 缓存已先行更新,UI 立即生效;写盘是异步收尾。
    Application::GetInstance().Schedule([pin]() {
        Settings settings("local_mcp", true);
        settings.SetString("token", pin);
    });
}

void LocalMcpServer::SetAuthEnabled(bool enable) {
    if (enable) {
        if (AuthEnabled()) {
            return;   // 已有 PIN,保持
        }
        RegeneratePin();
        return;
    }
    SetPin("");   // 清空 = 关闭认证(放开)
}

void LocalMcpServer::RegeneratePin() {
    // %06u:6 位数字(含前导零),方便局域网内手抄填写,取代长 bearer token。
    char pin[8];
    snprintf(pin, sizeof(pin), "%06u", static_cast<unsigned>(esp_random() % 1000000));
    SetPin(pin);
    ESP_LOGI(TAG, "MCP auth PIN regenerated");
}
