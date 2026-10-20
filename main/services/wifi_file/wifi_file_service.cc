#include "services/wifi_file/wifi_file_service.h"
#include "services/wifi_file/web_file_manager_html.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <memory>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <esp_log.h>
#include <esp_http_server.h>
#include <esp_netif.h>
#include <esp_heap_caps.h>
#include <cJSON.h>

#include "board.h"
#include "boards/common/SdCardManager.hpp"
#include "boards/common/sd_paths.h"
#include "reader/book_library.h"
#include "utils/fs_utils.h"
#include "ff.h"

namespace wifi_file {

namespace {

constexpr const char* TAG = "WifiFileSvc";
constexpr const char* kSdMountRoot = "/sdcard";

using fs_utils::IsUnderDir;
using fs_utils::SanitizeFilename;
using fs_utils::UrlDecode;

// 严防路径穿越：确保路径限定在 /sdcard 下
std::string SafeSdPath(const std::string& rel_or_abs) {
    std::string decoded = UrlDecode(rel_or_abs);
    // 过滤控制字符
    std::string clean;
    clean.reserve(decoded.size());
    for (char c : decoded) {
        if (static_cast<unsigned char>(c) >= 0x20 && c != '\\') {
            clean.push_back(c);
        }
    }

    // 分割路径段
    std::vector<std::string> segments;
    size_t start = 0;
    while (start < clean.size()) {
        size_t next = clean.find('/', start);
        if (next == std::string::npos) next = clean.size();
        std::string part = clean.substr(start, next - start);
        if (!part.empty() && part != ".") {
            if (part == "..") {
                if (!segments.empty()) {
                    segments.pop_back();
                }
            } else if (part != "sdcard") {
                segments.push_back(part);
            }
        }
        start = next + 1;
    }

    std::string result = kSdMountRoot;
    for (const auto& s : segments) {
        result.push_back('/');
        result.append(s);
    }
    return result;
}

// 目标是否落在图书库目录内——落在库内才需要让阅读器重新扫描书库
bool IsUnderBooksDir(const std::string& abs_path) {
    return IsUnderDir(abs_path, WifiFileService::GetBooksAbsDir());
}

// 逐级创建目录(如 /sdcard/metalio/e-ink/books)：目标目录可能尚未存在
// (新卡首次传书、或用户新建了多级目录),已存在时 mkdir 返回 EEXIST 可忽略
void EnsureDirTree(const std::string& abs_dir) {
    if (abs_dir.size() <= std::strlen(kSdMountRoot)) return;
    size_t pos = std::strlen(kSdMountRoot);
    while (pos < abs_dir.size()) {
        size_t next = abs_dir.find('/', pos);
        if (next == std::string::npos) next = abs_dir.size();
        mkdir(abs_dir.substr(0, next).c_str(), 0755);
        pos = next + 1;
    }
}

// 递归删除文件/目录树。depth 仅用于防御异常深的目录(FAT32 无符号链接,
// 正常不会触发),避免极端情况下无限递归。
bool RemoveTree(const std::string& path, int depth) {
    if (depth > 32) return false;

    struct stat st {};
    if (stat(path.c_str(), &st) != 0) return false;

    if (!S_ISDIR(st.st_mode)) {
        return unlink(path.c_str()) == 0;
    }

    DIR* d = opendir(path.c_str());
    if (d == nullptr) return false;

    bool ok = true;
    struct dirent* ent = nullptr;
    while ((ent = readdir(d)) != nullptr) {
        if (std::strcmp(ent->d_name, ".") == 0 || std::strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        if (!RemoveTree(path + "/" + ent->d_name, depth + 1)) {
            ok = false;
        }
    }
    closedir(d);

    if (rmdir(path.c_str()) != 0) {
        ok = false;
    }
    return ok;
}

// HTTP Handler: GET /
esp_err_t HandleGetIndex(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, kFileManagerHtml, HTTPD_RESP_USE_STRLEN);
}

// HTTP Handler: GET /api/fs/roots
// 前端用它拿到「传书」页签的图书库路径,避免在页面里硬编码产品目录
esp_err_t HandleGetRoots(httpd_req_t* req) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddStringToObject(root, "books", WifiFileService::GetBooksRelDir());
    cJSON_AddStringToObject(root, "root", "/");
    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t res = httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return res;
}

// HTTP Handler: GET /api/fs/list?path=...
esp_err_t HandleGetList(httpd_req_t* req) {
    char query[512] = {};
    std::string rel_path = "/";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char pval[256] = {};
        if (httpd_query_key_value(query, "path", pval, sizeof(pval)) == ESP_OK) {
            rel_path = pval;
        }
    }

    std::string full_dir = SafeSdPath(rel_path);
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);

    std::string display_path = full_dir.substr(std::strlen(kSdMountRoot));
    if (display_path.empty()) display_path = "/";
    cJSON_AddStringToObject(root, "path", display_path.c_str());

    cJSON* arr = cJSON_AddArrayToObject(root, "items");

    DIR* d = opendir(full_dir.c_str());
    if (d != nullptr) {
        struct ItemInfo {
            std::string name;
            bool is_dir = false;
            size_t size = 0;
            std::string date;
        };
        std::vector<ItemInfo> items;

        dirent* ent = nullptr;
        while ((ent = readdir(d)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            std::string item_full = full_dir + "/" + ent->d_name;
            struct stat st {};
            if (stat(item_full.c_str(), &st) == 0) {
                ItemInfo ii;
                ii.name = ent->d_name;
                ii.is_dir = S_ISDIR(st.st_mode);
                ii.size = static_cast<size_t>(st.st_size);

                struct tm tm_info = {};
                localtime_r(&st.st_mtime, &tm_info);
                char date_buf[64] = {};
                std::snprintf(date_buf, sizeof(date_buf), "%04d-%02d-%02d %02d:%02d",
                              tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                              tm_info.tm_hour, tm_info.tm_min);
                ii.date = date_buf;
                items.push_back(std::move(ii));
            }
        }
        closedir(d);

        // 文件夹排在前面，同类按名称排序
        std::sort(items.begin(), items.end(), [](const ItemInfo& a, const ItemInfo& b) {
            if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
            return a.name < b.name;
        });

        for (const auto& it : items) {
            cJSON* obj = cJSON_CreateObject();
            cJSON_AddStringToObject(obj, "name", it.name.c_str());
            cJSON_AddBoolToObject(obj, "is_dir", it.is_dir);
            cJSON_AddNumberToObject(obj, "size", it.size);
            cJSON_AddStringToObject(obj, "date", it.date.c_str());
            cJSON_AddItemToArray(arr, obj);
        }
    }

    // SD 卡容量
    FATFS* fs = nullptr;
    DWORD free_clusters = 0;
    if (f_getfree("0:", &free_clusters, &fs) == FR_OK && fs != nullptr) {
        constexpr DWORD kSectorSize = 512;
        uint64_t total_bytes = static_cast<uint64_t>(fs->n_fatent - 2) * fs->csize * kSectorSize;
        uint64_t free_bytes = static_cast<uint64_t>(free_clusters) * fs->csize * kSectorSize;
        cJSON_AddNumberToObject(root, "total_mb", total_bytes / (1024 * 1024));
        cJSON_AddNumberToObject(root, "free_mb", free_bytes / (1024 * 1024));
    }

    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t res = httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return res;
}

// HTTP Handler: POST /api/fs/upload?path=...
esp_err_t HandlePostUpload(httpd_req_t* req) {
    char query[512] = {};
    std::string rel_path = "/";
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        char pval[256] = {};
        if (httpd_query_key_value(query, "path", pval, sizeof(pval)) == ESP_OK) {
            rel_path = pval;
        }
    }

    char fn_hdr[256] = {};
    if (httpd_req_get_hdr_value_str(req, "X-Filename", fn_hdr, sizeof(fn_hdr)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing X-Filename header");
        return ESP_FAIL;
    }

    std::string clean_name = SanitizeFilename(UrlDecode(fn_hdr));
    if (clean_name.empty()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_FAIL;
    }

    std::string target_dir = SafeSdPath(rel_path);
    EnsureDirTree(target_dir);

    std::string target_path = target_dir + "/" + clean_name;
    std::string tmp_path = target_path + ".upload.tmp";
    ESP_LOGI(TAG, "uploading to %s (%d bytes)", target_path.c_str(), req->content_len);

    FILE* fp = fopen(tmp_path.c_str(), "wb");
    if (fp == nullptr) {
        ESP_LOGE(TAG, "failed to open %s", tmp_path.c_str());
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot open file on SD");
        return ESP_FAIL;
    }

    constexpr size_t kChunkSize = 16384;
    void* raw_buf = heap_caps_malloc(kChunkSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t chunk_cap = kChunkSize;
    if (raw_buf == nullptr) {
        raw_buf = malloc(4096);
        chunk_cap = 4096;
    }
    if (raw_buf == nullptr) {
        fclose(fp);
        unlink(tmp_path.c_str());
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    std::unique_ptr<char[], decltype(&free)> chunk(static_cast<char*>(raw_buf), &free);

    size_t total_recv = 0;
    int remaining = req->content_len;
    bool write_ok = true;
    while (remaining > 0) {
        int to_read = (remaining < static_cast<int>(chunk_cap)) ? remaining : static_cast<int>(chunk_cap);
        int ret = httpd_req_recv(req, chunk.get(), to_read);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) continue;
            ESP_LOGE(TAG, "recv error: %d", ret);
            write_ok = false;
            break;
        }
        if (fwrite(chunk.get(), 1, ret, fp) != static_cast<size_t>(ret)) {
            ESP_LOGE(TAG, "fwrite error");
            write_ok = false;
            break;
        }
        total_recv += ret;
        remaining -= ret;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    fclose(fp);

    if (!write_ok || remaining > 0) {
        unlink(tmp_path.c_str());
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload interrupted");
        return ESP_FAIL;
    }

    unlink(target_path.c_str());
    if (rename(tmp_path.c_str(), target_path.c_str()) != 0) {
        unlink(tmp_path.c_str());
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to commit upload");
        return ESP_FAIL;
    }

    // 落在图书库内才算「传书」——必须让阅读器重新扫描书库,否则设备上看不到新书
    const bool to_books = IsUnderBooksDir(target_path);
    if (to_books) {
        reader::InvalidateBookLibraryCache();
    }
    WifiFileService::GetInstance().NotifyActivity(to_books ? "传书" : "上传",
                                                 clean_name);

    httpd_resp_set_type(req, "application/json");
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddStringToObject(root, "filename", clean_name.c_str());
    cJSON_AddNumberToObject(root, "size", total_recv);
    char* json_str = cJSON_PrintUnformatted(root);
    esp_err_t res = httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return res;
}

// HTTP Handler: POST /api/fs/delete?path=...
esp_err_t HandlePostDelete(httpd_req_t* req) {
    char query[512] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query");
        return ESP_FAIL;
    }
    char pval[256] = {};
    if (httpd_query_key_value(query, "path", pval, sizeof(pval)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing path param");
        return ESP_FAIL;
    }

    std::string target = SafeSdPath(pval);
    if (target == kSdMountRoot) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Cannot delete root");
        return ESP_FAIL;
    }

    struct stat st {};
    if (stat(target.c_str(), &st) != 0) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Target not found");
        return ESP_FAIL;
    }

    int ret = 0;
    if (S_ISDIR(st.st_mode)) {
        // 目录必须递归删除——Web 端的确认框承诺「彻底删除」,
        // 而 rmdir 只能删空目录,非空目录会直接失败
        ret = RemoveTree(target, 0) ? 0 : -1;
    } else {
        ret = unlink(target.c_str());
    }

    if (ret != 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Delete failed (directory might not be empty)");
        return ESP_FAIL;
    }

    // 图书库内的书要连带清掉分页索引/封面等旁路文件,否则残留垃圾文件
    if (IsUnderBooksDir(target)) {
        unlink((target + ".pos").c_str());
        unlink((target + ".idx").c_str());
        unlink((target + ".upload.tmp").c_str());
        reader::InvalidateBookLibraryCache();
    }

    WifiFileService::GetInstance().NotifyActivity("删除", target.substr(std::strlen(kSdMountRoot)));

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
}

// HTTP Handler: POST /api/fs/mkdir?path=...
esp_err_t HandlePostMkdir(httpd_req_t* req) {
    char query[512] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query");
        return ESP_FAIL;
    }
    char pval[256] = {};
    if (httpd_query_key_value(query, "path", pval, sizeof(pval)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing path param");
        return ESP_FAIL;
    }

    std::string target = SafeSdPath(pval);
    // 父目录可能尚不存在(API 直接指定多级路径),先逐级补齐
    size_t slash = target.rfind('/');
    if (slash != std::string::npos && slash > 0) {
        EnsureDirTree(target.substr(0, slash));
    }
    if (mkdir(target.c_str(), 0755) != 0) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Mkdir failed");
        return ESP_FAIL;
    }

    WifiFileService::GetInstance().NotifyActivity("新建目录", target.substr(std::strlen(kSdMountRoot)));

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
}

// HTTP Handler: GET /api/fs/download?path=...
esp_err_t HandleGetDownload(httpd_req_t* req) {
    char query[512] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query");
        return ESP_FAIL;
    }
    char pval[256] = {};
    if (httpd_query_key_value(query, "path", pval, sizeof(pval)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing path param");
        return ESP_FAIL;
    }

    std::string target = SafeSdPath(pval);
    struct stat st {};
    if (stat(target.c_str(), &st) != 0 || S_ISDIR(st.st_mode)) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found or is directory");
        return ESP_FAIL;
    }

    FILE* fp = fopen(target.c_str(), "rb");
    if (fp == nullptr) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Cannot open file");
        return ESP_FAIL;
    }

    const char* slash = std::strrchr(target.c_str(), '/');
    const char* basename = (slash != nullptr) ? slash + 1 : target.c_str();

    httpd_resp_set_type(req, "application/octet-stream");
    char disp_hdr[300] = {};
    std::snprintf(disp_hdr, sizeof(disp_hdr), "attachment; filename=\"%s\"", basename);
    httpd_resp_set_hdr(req, "Content-Disposition", disp_hdr);

    constexpr size_t kBufSize = 16384;
    void* raw_buf = heap_caps_malloc(kBufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t buf_cap = kBufSize;
    if (raw_buf == nullptr) {
        raw_buf = malloc(4096);
        buf_cap = 4096;
    }
    if (raw_buf == nullptr) {
        fclose(fp);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    std::unique_ptr<char[], decltype(&free)> buffer(static_cast<char*>(raw_buf), &free);

    size_t read_bytes = 0;
    while ((read_bytes = fread(buffer.get(), 1, buf_cap, fp)) > 0) {
        if (httpd_resp_send_chunk(req, buffer.get(), static_cast<ssize_t>(read_bytes)) != ESP_OK) {
            fclose(fp);
            return ESP_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    fclose(fp);
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

}  // namespace

WifiFileService& WifiFileService::GetInstance() {
    static WifiFileService instance;
    return instance;
}

const char* WifiFileService::GetBooksRelDir() {
    // SD_PATH_BOOKS 形如 /sdcard/metalio/e-ink/books,Web 端用相对 SD 根的路径
    static std::string rel;
    if (rel.empty()) {
        std::string abs(SD_PATH_BOOKS);
        const std::string root(kSdMountRoot);
        rel = abs.compare(0, root.size(), root) == 0 ? abs.substr(root.size()) : abs;
        if (rel.empty()) rel = "/";
    }
    return rel.c_str();
}

const char* WifiFileService::GetBooksAbsDir() {
    return SD_PATH_BOOKS;
}

bool WifiFileService::Start(uint16_t port) {
    if (server_ != nullptr) {
        return true;
    }
    port_ = port;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port_;
    config.ctrl_port = port_ + 1;
    config.max_uri_handlers = 12;
    config.stack_size = 10240;
    config.lru_purge_enable = true;

    httpd_handle_t handle = nullptr;
    esp_err_t err = httpd_start(&handle, &config);
    if (err != ESP_OK && port_ == 80) {
        // 80 端口常被 OTA/其他服务占用,回退到 8088(与 WiFi 传书旧行为一致)
        ESP_LOGW(TAG, "httpd_start on port 80 failed (%s), fallback to 8088",
                 esp_err_to_name(err));
        port_ = 8088;
        config.server_port = port_;
        config.ctrl_port = port_ + 1;
        err = httpd_start(&handle, &config);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to start http server on port %u: %s", port_,
                 esp_err_to_name(err));
        return false;
    }
    server_ = handle;

    httpd_uri_t get_index = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = HandleGetIndex,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(handle, &get_index);

    httpd_uri_t get_roots = {
        .uri = "/api/fs/roots",
        .method = HTTP_GET,
        .handler = HandleGetRoots,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(handle, &get_roots);

    httpd_uri_t get_list = {
        .uri = "/api/fs/list",
        .method = HTTP_GET,
        .handler = HandleGetList,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(handle, &get_list);

    httpd_uri_t post_upload = {
        .uri = "/api/fs/upload",
        .method = HTTP_POST,
        .handler = HandlePostUpload,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(handle, &post_upload);

    httpd_uri_t post_delete = {
        .uri = "/api/fs/delete",
        .method = HTTP_POST,
        .handler = HandlePostDelete,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(handle, &post_delete);

    httpd_uri_t post_mkdir = {
        .uri = "/api/fs/mkdir",
        .method = HTTP_POST,
        .handler = HandlePostMkdir,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(handle, &post_mkdir);

    httpd_uri_t get_download = {
        .uri = "/api/fs/download",
        .method = HTTP_GET,
        .handler = HandleGetDownload,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(handle, &get_download);

    ESP_LOGI(TAG, "WiFi file server started on port %u", port_);
    return true;
}

void WifiFileService::Stop() {
    if (server_ != nullptr) {
        httpd_stop(static_cast<httpd_handle_t>(server_));
        server_ = nullptr;
        ESP_LOGI(TAG, "WiFi file server stopped");
    }
}

std::string WifiFileService::GetLocalIp() const {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif == nullptr) {
        return "";
    }
    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        char buf[16];
        esp_ip4addr_ntoa(&ip_info.ip, buf, sizeof(buf));
        return buf;
    }
    return "";
}

std::string WifiFileService::GetServerUrl() const {
    std::string ip = GetLocalIp();
    if (ip.empty()) {
        return "";
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "http://%s:%u/", ip.c_str(), port_);
    return buf;
}

}  // namespace wifi_file
