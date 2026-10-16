#include "wifi_transfer_service.h"
#include "transfer_utils.h"
#include "web_index_html.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <memory>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <esp_log.h>
#include <esp_http_server.h>
#include <esp_netif.h>
#include <esp_heap_caps.h>
#include <cJSON.h>

#include "board.h"
#include "reader/book_library.h"
#include "reader_core/file_size.h"
#include "SdCardManager.hpp"
#include "sd_paths.h"
#include "ff.h"

namespace wifi_transfer {

namespace {

constexpr const char* TAG = "WifiTransfer";

// HTTP Handler: GET /
esp_err_t HandleGetIndex(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, kIndexHtml, HTTPD_RESP_USE_STRLEN);
}

// HTTP Handler: POST /upload
esp_err_t HandlePostUpload(httpd_req_t* req) {
    char fn_hdr[256] = {};
    if (httpd_req_get_hdr_value_str(req, "X-Filename", fn_hdr, sizeof(fn_hdr)) != ESP_OK) {
        ESP_LOGW(TAG, "upload header X-Filename missing");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing X-Filename header");
        return ESP_FAIL;
    }

    std::string decoded = UrlDecode(fn_hdr);
    std::string clean_name = SanitizeFilename(decoded);
    if (clean_name.empty()) {
        ESP_LOGW(TAG, "invalid clean filename");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_FAIL;
    }

    const char* books_dir = WifiTransferService::GetBooksDir();
    mkdir(SD_APP_ROOT, 0755);
    mkdir(books_dir, 0755);

    std::string target_path = std::string(books_dir) + "/" + clean_name;
    std::string tmp_path = target_path + ".upload.tmp";
    ESP_LOGI(TAG, "saving upload to %s, total size %d", target_path.c_str(), req->content_len);

    // 先写入临时文件，传输全部成功后再原子改名，杜绝断网产生损坏残缺书籍
    FILE* fp = fopen(tmp_path.c_str(), "wb");
    if (fp == nullptr) {
        ESP_LOGE(TAG, "failed to open %s for write", tmp_path.c_str());
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open destination file on SD");
        return ESP_FAIL;
    }

    // 优先从 PSRAM 申请 16KB 大缓冲区提升 SD 写入吞吐量，同时不在 FreeRTOS 任务栈上消耗内存
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
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "httpd_req_recv failed: %d", ret);
            write_ok = false;
            break;
        }
        if (fwrite(chunk.get(), 1, ret, fp) != static_cast<size_t>(ret)) {
            ESP_LOGE(TAG, "fwrite to SD card failed");
            write_ok = false;
            break;
        }
        total_recv += ret;
        remaining -= ret;

        // 让步调度，保障 IDLE 任务与看门狗正常运行
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    fclose(fp);

    if (!write_ok || remaining > 0) {
        unlink(tmp_path.c_str());
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload stream interrupted or disk write failed");
        return ESP_FAIL;
    }

    // 原子改名生效
    unlink(target_path.c_str());
    if (rename(tmp_path.c_str(), target_path.c_str()) != 0) {
        unlink(tmp_path.c_str());
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to commit uploaded book");
        return ESP_FAIL;
    }

    reader::InvalidateBookLibraryCache();

    auto& svc = WifiTransferService::GetInstance();
    if (svc.IsRunning()) {
        UploadEvent ev{clean_name, total_recv, true, "上传成功"};
        svc.NotifyUpload(ev);
    }

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

// HTTP Handler: GET /api/books
esp_err_t HandleGetBooks(httpd_req_t* req) {
    cJSON* root = cJSON_CreateObject();
    cJSON* arr = cJSON_AddArrayToObject(root, "books");

    const char* dir_path = WifiTransferService::GetBooksDir();
    DIR* d = opendir(dir_path);
    if (d != nullptr) {
        dirent* ent = nullptr;
        while ((ent = readdir(d)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            std::string path = std::string(dir_path) + "/" + ent->d_name;
            struct stat st = {};
            if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                std::string name = ent->d_name;
                // 忽略旁边缓存索引文件
                if (name.size() > 4 && (name.rfind(".pos") == name.size() - 4 ||
                                        name.rfind(".idx") == name.size() - 4 ||
                                        name.rfind(".tmp") == name.size() - 4)) {
                    continue;
                }
                cJSON* item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "name", ent->d_name);
                cJSON_AddNumberToObject(item, "size", st.st_size);
                struct tm tm_info = {};
                localtime_r(&st.st_mtime, &tm_info);
                char date_buf[64] = {};
                snprintf(date_buf, sizeof(date_buf), "%04d-%02d-%02d %02d:%02d",
                         tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                         tm_info.tm_hour, tm_info.tm_min);
                cJSON_AddStringToObject(item, "date", date_buf);
                cJSON_AddItemToArray(arr, item);
            }
        }
        closedir(d);
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

// HTTP Handler: POST /api/delete
esp_err_t HandlePostDelete(httpd_req_t* req) {
    char query[256] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query");
        return ESP_FAIL;
    }

    char fn[128] = {};
    if (httpd_query_key_value(query, "file", fn, sizeof(fn)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file parameter");
        return ESP_FAIL;
    }

    std::string clean = SanitizeFilename(UrlDecode(fn));
    if (clean.empty()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid file name");
        return ESP_FAIL;
    }

    std::string full_path = std::string(WifiTransferService::GetBooksDir()) + "/" + clean;
    unlink(full_path.c_str());
    unlink((full_path + ".pos").c_str());
    unlink((full_path + ".idx").c_str());
    unlink((full_path + ".upload.tmp").c_str());
    reader::InvalidateBookLibraryCache();

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
}

// HTTP Handler: GET /api/download
esp_err_t HandleGetDownload(httpd_req_t* req) {
    char query[256] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query");
        return ESP_FAIL;
    }

    char fn[128] = {};
    if (httpd_query_key_value(query, "file", fn, sizeof(fn)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file parameter");
        return ESP_FAIL;
    }

    std::string clean = SanitizeFilename(UrlDecode(fn));
    std::string full_path = std::string(WifiTransferService::GetBooksDir()) + "/" + clean;
    FILE* fp = fopen(full_path.c_str(), "rb");
    if (fp == nullptr) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/octet-stream");
    char disp[256];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", clean.c_str());
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    // 动态分配下载缓冲，避免 4KB 局部变量占用任务栈
    constexpr size_t kDlChunkSize = 8192;
    void* dl_raw = heap_caps_malloc(kDlChunkSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t dl_cap = kDlChunkSize;
    if (dl_raw == nullptr) {
        dl_raw = malloc(2048);
        dl_cap = 2048;
    }
    if (dl_raw == nullptr) {
        fclose(fp);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    std::unique_ptr<char[], decltype(&free)> chunk(static_cast<char*>(dl_raw), &free);

    size_t n = 0;
    while ((n = fread(chunk.get(), 1, dl_cap, fp)) > 0) {
        if (httpd_resp_send_chunk(req, chunk.get(), n) != ESP_OK) {
            fclose(fp);
            return ESP_FAIL;
        }
    }
    fclose(fp);
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

}  // namespace

WifiTransferService& WifiTransferService::GetInstance() {
    static WifiTransferService instance;
    return instance;
}

const char* WifiTransferService::GetBooksDir() {
    return SD_PATH_BOOKS;
}

std::string WifiTransferService::GetLocalIp() const {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    if (netif != nullptr && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        char ip_str[32] = {};
        esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
        return std::string(ip_str);
    }
    return {};
}

std::string WifiTransferService::GetServerUrl() const {
    std::string ip = GetLocalIp();
    if (ip.empty()) return {};
    char buf[64];
    if (port_ == 80) {
        snprintf(buf, sizeof(buf), "http://%s/", ip.c_str());
    } else {
        snprintf(buf, sizeof(buf), "http://%s:%u/", ip.c_str(), port_);
    }
    return std::string(buf);
}

bool WifiTransferService::Start(uint16_t port) {
    if (server_ != nullptr) {
        ESP_LOGI(TAG, "server already running");
        return true;
    }

    port_ = port;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port_;
    config.ctrl_port = 32770;  // 区别于 LocalMcp 的 32769
    config.max_uri_handlers = 8;
    config.stack_size = 8192;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 15;
    config.send_wait_timeout = 15;

    httpd_handle_t handle = nullptr;
    esp_err_t err = httpd_start(&handle, &config);
    if (err != ESP_OK && port_ == 80) {
        ESP_LOGW(TAG, "httpd_start on port 80 failed (%s), fallback to 8088", esp_err_to_name(err));
        port_ = 8088;
        config.server_port = port_;
        err = httpd_start(&handle, &config);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return false;
    }
    server_ = handle;

    // 注册 URI
    httpd_uri_t uri_index = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = HandleGetIndex,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(handle, &uri_index);

    httpd_uri_t uri_upload = {
        .uri = "/upload",
        .method = HTTP_POST,
        .handler = HandlePostUpload,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(handle, &uri_upload);

    httpd_uri_t uri_books = {
        .uri = "/api/books",
        .method = HTTP_GET,
        .handler = HandleGetBooks,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(handle, &uri_books);

    httpd_uri_t uri_delete = {
        .uri = "/api/delete",
        .method = HTTP_POST,
        .handler = HandlePostDelete,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(handle, &uri_delete);

    httpd_uri_t uri_download = {
        .uri = "/api/download",
        .method = HTTP_GET,
        .handler = HandleGetDownload,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(handle, &uri_download);

    ESP_LOGI(TAG, "WiFi Transfer server started at port %u", port_);
    return true;
}

void WifiTransferService::Stop() {
    if (server_ != nullptr) {
        httpd_stop(static_cast<httpd_handle_t>(server_));
        server_ = nullptr;
        ESP_LOGI(TAG, "WiFi Transfer server stopped");
    }
}

}  // namespace wifi_transfer
