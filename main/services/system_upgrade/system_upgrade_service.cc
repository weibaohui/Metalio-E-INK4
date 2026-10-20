#include "system_upgrade_service.h"
#include "upgrade_web_index.h"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_app_format.h>
#include <esp_http_server.h>
#include <esp_netif.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <cJSON.h>

#include "application.h"
#include "board.h"
#include "SdCardManager.hpp"
#include "sd_paths.h"
#include "ota_upgrade_screen/ota_upgrade_screen.h"
#include "services/wifi_file/wifi_file_service.h"

namespace system_upgrade {

namespace {

constexpr const char* TAG = "SysUpgradeSvc";
constexpr size_t kMinHeaderSize = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
constexpr const char* kSdFirmwareDir = SD_APP_ROOT "/firmware";

// HTTP Handler: GET / (返回 Web 升级网页)
esp_err_t HandleGetUpgradeIndex(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, kUpgradeWebHtml, HTTPD_RESP_USE_STRLEN);
}

// HTTP Handler: GET /api/upgrade_info (返回当前系统与分区状态 JSON)
esp_err_t HandleGetUpgradeInfo(httpd_req_t* req) {
    httpd_resp_set_type(req, "application/json; charset=utf-8");
    cJSON* root = cJSON_CreateObject();
    const esp_app_desc_t* desc = esp_app_get_description();
    if (desc != nullptr) {
        cJSON_AddStringToObject(root, "project_name", desc->project_name);
        cJSON_AddStringToObject(root, "version", desc->version);
        cJSON_AddStringToObject(root, "build_date", desc->date);
        cJSON_AddStringToObject(root, "build_time", desc->time);
        cJSON_AddStringToObject(root, "idf_ver", desc->idf_ver);
    }
    const esp_partition_t* running = esp_ota_get_running_partition();
    const esp_partition_t* next = esp_ota_get_next_update_partition(NULL);
    cJSON_AddStringToObject(root, "running_partition", running ? running->label : "unknown");
    cJSON_AddStringToObject(root, "next_partition", next ? next->label : "unknown");

    char* str = cJSON_PrintUnformatted(root);
    esp_err_t ret = httpd_resp_send(req, str, HTTPD_RESP_USE_STRLEN);
    free(str);
    cJSON_Delete(root);
    return ret;
}

// HTTP Handler: POST /upload_ota (流式接收 .bin 写入 OTA 分区)
esp_err_t HandlePostOtaUpload(httpd_req_t* req) {
    ESP_LOGI(TAG, "收到 Web OTA 固件上传请求，总大小: %d 字节", req->content_len);
    if (req->content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid content length");
        return ESP_FAIL;
    }

    const esp_partition_t* update_part = esp_ota_get_next_update_partition(NULL);
    if (update_part == nullptr) {
        ESP_LOGE(TAG, "无法找到目标 OTA 分区");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition available");
        return ESP_FAIL;
    }

    if (static_cast<size_t>(req->content_len) > update_part->size) {
        ESP_LOGE(TAG, "固件大小 (%d) 超出目标分区容量 (%lu)", req->content_len, update_part->size);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Firmware size exceeds partition");
        return ESP_FAIL;
    }

    // 唤起全屏升级 UI 并锁定按键
    const esp_app_desc_t* cur_desc = esp_app_get_description();
    const char* cur_ver_str = (cur_desc != nullptr) ? cur_desc->version : "—";
    OtaUpgradeScreen::Show(cur_ver_str, "Web 上传固件");
    OtaUpgradeScreen::SetProgress(0);

    // 申请 16KB 接收缓冲 (优先 PSRAM)
    constexpr size_t kChunkCap = 16384;
    void* raw_buf = heap_caps_malloc(kChunkCap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t chunk_size = kChunkCap;
    if (raw_buf == nullptr) {
        raw_buf = malloc(4096);
        chunk_size = 4096;
    }
    if (raw_buf == nullptr) {
        OtaUpgradeScreen::Dismiss();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
        return ESP_FAIL;
    }
    std::unique_ptr<char[], decltype(&free)> chunk(static_cast<char*>(raw_buf), &free);

    esp_ota_handle_t ota_handle = 0;
    bool ota_started = false;
    size_t total_recv = 0;
    int remaining = req->content_len;
    std::vector<uint8_t> header_accum;

    int64_t last_ui_time = esp_timer_get_time();

    while (remaining > 0) {
        int to_read = (remaining < static_cast<int>(chunk_size)) ? remaining : static_cast<int>(chunk_size);
        int ret = httpd_req_recv(req, chunk.get(), to_read);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "httpd_req_recv 失败: %d", ret);
            break;
        }

        total_recv += ret;
        remaining -= ret;

        // 首段解析并校验 Magic Word
        if (!ota_started) {
            header_accum.insert(header_accum.end(), reinterpret_cast<uint8_t*>(chunk.get()),
                                reinterpret_cast<uint8_t*>(chunk.get()) + ret);
            if (header_accum.size() >= kMinHeaderSize) {
                const auto* desc = reinterpret_cast<const esp_app_desc_t*>(
                    header_accum.data() + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));
                if (desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
                    ESP_LOGE(TAG, "无效的 ESP32 固件镜像 (Magic 错误: 0x%08lx)", desc->magic_word);
                    OtaUpgradeScreen::Dismiss();
                    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid ESP32 firmware image");
                    return ESP_FAIL;
                }

                ESP_LOGI(TAG, "识别到合法固件: 项目=%s, 版本=%s, 编译=%s %s",
                         desc->project_name, desc->version, desc->date, desc->time);

                esp_err_t ota_err = esp_ota_begin(update_part, req->content_len, &ota_handle);
                if (ota_err != ESP_OK) {
                    ESP_LOGE(TAG, "esp_ota_begin 失败: %s", esp_err_to_name(ota_err));
                    OtaUpgradeScreen::Dismiss();
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
                    return ESP_FAIL;
                }
                ota_started = true;

                // 写入头部累积数据
                ota_err = esp_ota_write(ota_handle, header_accum.data(), header_accum.size());
                if (ota_err != ESP_OK) {
                    ESP_LOGE(TAG, "esp_ota_write 头部写入失败: %s", esp_err_to_name(ota_err));
                    esp_ota_abort(ota_handle);
                    OtaUpgradeScreen::Dismiss();
                    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
                    return ESP_FAIL;
                }
                header_accum.clear();
                header_accum.shrink_to_fit();
            }
            continue;
        }

        // 持续写入 OTA 分区
        esp_err_t wr_err = esp_ota_write(ota_handle, chunk.get(), ret);
        if (wr_err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write 失败: %s", esp_err_to_name(wr_err));
            esp_ota_abort(ota_handle);
            OtaUpgradeScreen::Dismiss();
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
            return ESP_FAIL;
        }

        // 节流刷新屏幕进度条 (每 300ms 或达到 100%)
        int64_t now = esp_timer_get_time();
        if (now - last_ui_time >= 300000 || remaining == 0) {
            int pct = static_cast<int>(total_recv * 100 / req->content_len);
            OtaUpgradeScreen::SetProgress(pct);
            last_ui_time = now;
        }

        // 让步调度，保障 IDLE 任务与看门狗正常运行
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    if (total_recv != static_cast<size_t>(req->content_len) || !ota_started) {
        ESP_LOGE(TAG, "传输未完整 (已收 %u / 总共 %d)", (unsigned)total_recv, req->content_len);
        if (ota_started) esp_ota_abort(ota_handle);
        OtaUpgradeScreen::Dismiss();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload incomplete");
        return ESP_FAIL;
    }

    esp_err_t end_err = esp_ota_end(ota_handle);
    if (end_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end 校验失败: %s", esp_err_to_name(end_err));
        OtaUpgradeScreen::Dismiss();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA validation failed");
        return ESP_FAIL;
    }

    esp_err_t boot_err = esp_ota_set_boot_partition(update_part);
    if (boot_err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition 失败: %s", esp_err_to_name(boot_err));
        OtaUpgradeScreen::Dismiss();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Set boot partition failed");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Web 固件烧录成功！引导分区已更新为 %s，准备自动重启", update_part->label);
    OtaUpgradeScreen::SetProgress(100);
    OtaUpgradeScreen::SetHint("固件写入成功！设备即将自动重启...");

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_sendstr(req, "{\"success\":true,\"message\":\"Firmware flashed successfully, rebooting...\"}");

    // 1.5 秒后安全重启进入新固件
    xTaskCreate([](void*) {
        vTaskDelay(pdMS_TO_TICKS(1500));
        Application::GetInstance().Reboot();
        vTaskDelete(nullptr);
    }, "ota_reboot", 2048, nullptr, 5, nullptr);

    return ESP_OK;
}

}  // namespace

SystemUpgradeService& SystemUpgradeService::GetInstance() {
    static SystemUpgradeService s_instance;
    return s_instance;
}

SystemUpgradeService::~SystemUpgradeService() {
    StopWebServer();
}

std::string SystemUpgradeService::GetCurrentVersion() const {
    const esp_app_desc_t* desc = esp_app_get_description();
    return (desc != nullptr) ? desc->version : "未知";
}

std::string SystemUpgradeService::GetRunningPartition() const {
    const esp_partition_t* p = esp_ota_get_running_partition();
    return (p != nullptr) ? p->label : "未知";
}

std::string SystemUpgradeService::GetNextPartition() const {
    const esp_partition_t* p = esp_ota_get_next_update_partition(NULL);
    return (p != nullptr) ? p->label : "未知";
}

std::string SystemUpgradeService::GetBuildTime() const {
    const esp_app_desc_t* desc = esp_app_get_description();
    if (desc != nullptr) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%s %s", desc->date, desc->time);
        return buf;
    }
    return "未知";
}

std::string SystemUpgradeService::GetProjectName() const {
    const esp_app_desc_t* desc = esp_app_get_description();
    return (desc != nullptr) ? desc->project_name : "xiaozhi";
}

bool SystemUpgradeService::IsSdMounted() const {
    return SdCardManager::GetInstance().IsMounted();
}

bool SystemUpgradeService::ParseFirmwareHeader(const std::string& path, FirmwareInfo& out_info) {
    FILE* fp = fopen(path.c_str(), "rb");
    if (fp == nullptr) {
        return false;
    }

    uint8_t buf[kMinHeaderSize];
    size_t read_bytes = fread(buf, 1, sizeof(buf), fp);
    fclose(fp);

    if (read_bytes < sizeof(buf)) {
        return false;
    }

    const auto* desc = reinterpret_cast<const esp_app_desc_t*>(
        buf + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t));

    if (desc->magic_word == ESP_APP_DESC_MAGIC_WORD) {
        out_info.is_valid_esp32_bin = true;
        out_info.project_name = desc->project_name;
        out_info.version = desc->version;
        out_info.build_date = desc->date;
        out_info.build_time = desc->time;
        out_info.idf_ver = desc->idf_ver;
        return true;
    }

    return false;
}

std::vector<FirmwareInfo> SystemUpgradeService::ScanSdFirmwares() {
    std::vector<FirmwareInfo> results;
    if (!IsSdMounted()) {
        return results;
    }

    // 扫描路径 1: metalio/e-ink/firmware
    // 扫描路径 2: /sdcard (卡根目录)
    const std::vector<std::string> scan_dirs = {
        kSdFirmwareDir,
        SD_MOUNT_POINT
    };

    SdEnsureDir(kSdFirmwareDir);

    for (const auto& dir_path : scan_dirs) {
        DIR* d = opendir(dir_path.c_str());
        if (d == nullptr) continue;

        struct dirent* ent = nullptr;
        while ((ent = readdir(d)) != nullptr) {
            if (ent->d_type == DT_DIR) continue;

            const char* name = ent->d_name;
            size_t len = strlen(name);
            if (len < 5) continue;

            // 检查扩展名是否为 .bin / .BIN
            if (strcasecmp(name + len - 4, ".bin") != 0) {
                continue;
            }

            std::string full_path = dir_path + "/" + name;
            struct stat st;
            if (stat(full_path.c_str(), &st) != 0 || st.st_size <= static_cast<off_t>(kMinHeaderSize)) {
                continue;
            }

            FirmwareInfo info;
            info.filename = name;
            info.full_path = full_path;
            info.file_size = st.st_size;
            info.mod_time = st.st_mtime;

            ParseFirmwareHeader(full_path, info);

            // 避免重复加入相同路径
            bool exists = false;
            for (const auto& ex : results) {
                if (ex.full_path == full_path) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                results.push_back(info);
            }
        }
        closedir(d);
    }

    // 优先展示合法的 ESP32 固件，其次按修改时间倒序排列
    std::sort(results.begin(), results.end(), [](const FirmwareInfo& a, const FirmwareInfo& b) {
        if (a.is_valid_esp32_bin != b.is_valid_esp32_bin) {
            return a.is_valid_esp32_bin > b.is_valid_esp32_bin;
        }
        return a.mod_time > b.mod_time;
    });

    return results;
}

UpgradeResult SystemUpgradeService::FlashFromSd(const std::string& path,
                                               std::function<void(int progress, size_t speed)> progress_cb) {
    if (!IsSdMounted()) {
        return { false, "SD 卡未挂载" };
    }

    FirmwareInfo info;
    info.filename = path;
    info.full_path = path;
    if (!ParseFirmwareHeader(path, info) || !info.is_valid_esp32_bin) {
        return { false, "目标文件不是合法的 ESP32 固件镜像 (.bin)" };
    }

    FILE* fp = fopen(path.c_str(), "rb");
    if (fp == nullptr) {
        return { false, "打开固件文件失败" };
    }

    fseek(fp, 0, SEEK_END);
    size_t file_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    const esp_partition_t* update_part = esp_ota_get_next_update_partition(NULL);
    if (update_part == nullptr) {
        fclose(fp);
        return { false, "无可用的 OTA 目标分区" };
    }

    if (file_size > update_part->size) {
        fclose(fp);
        return { false, "固件体积超出分区限制" };
    }

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(update_part, file_size, &handle);
    if (err != ESP_OK) {
        fclose(fp);
        return { false, std::string("初始化 OTA 分区失败: ") + esp_err_to_name(err) };
    }

    constexpr size_t kBufSize = 16384;
    void* raw_buf = heap_caps_malloc(kBufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    size_t chunk_cap = kBufSize;
    if (raw_buf == nullptr) {
        raw_buf = malloc(4096);
        chunk_cap = 4096;
    }
    if (raw_buf == nullptr) {
        esp_ota_abort(handle);
        fclose(fp);
        return { false, "内存不足" };
    }
    std::unique_ptr<char[], decltype(&free)> buffer(static_cast<char*>(raw_buf), &free);

    size_t total_written = 0;
    size_t recent_read = 0;
    int64_t last_time = esp_timer_get_time();

    while (total_written < file_size) {
        size_t to_read = (file_size - total_written < chunk_cap) ? (file_size - total_written) : chunk_cap;
        size_t r = fread(buffer.get(), 1, to_read, fp);
        if (r == 0) {
            break;
        }

        err = esp_ota_write(handle, buffer.get(), r);
        if (err != ESP_OK) {
            esp_ota_abort(handle);
            fclose(fp);
            return { false, std::string("写入 Flash 失败: ") + esp_err_to_name(err) };
        }

        total_written += r;
        recent_read += r;

        int64_t now = esp_timer_get_time();
        if (now - last_time >= 500000 || total_written == file_size) {
            int pct = static_cast<int>(total_written * 100 / file_size);
            size_t speed = (now > last_time) ? (recent_read * 1000000 / (now - last_time)) : 0;
            if (progress_cb) {
                progress_cb(pct, speed);
            }
            last_time = now;
            recent_read = 0;
        }

        // 让步调度，保障 IDLE 任务与看门狗正常运行
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    fclose(fp);

    if (total_written != file_size) {
        esp_ota_abort(handle);
        return { false, "文件读取未完整" };
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        return { false, std::string("固件校验失败: ") + esp_err_to_name(err) };
    }

    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        return { false, std::string("设置启动引导分区失败: ") + esp_err_to_name(err) };
    }

    ESP_LOGI(TAG, "从 SD 卡烧录固件成功！分区: %s", update_part->label);
    return { true, "" };
}

bool SystemUpgradeService::DeleteFirmwareFile(const std::string& path) {
    if (path.empty()) return false;
    return unlink(path.c_str()) == 0;
}

bool SystemUpgradeService::StartWebServer(uint16_t port) {
    if (http_server_handle_ != nullptr) {
        return true;
    }

    // 确保与 WiFi 传输(传书/文件管理)不冲突——两者都占用 80 端口
    wifi_file::WifiFileService::GetInstance().Stop();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port;
    config.ctrl_port = port + 1;
    config.max_open_sockets = 4;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    httpd_handle_t server = nullptr;
    esp_err_t ret = httpd_start(&server, &config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "启动 Web OTA 服务失败: %s", esp_err_to_name(ret));
        return false;
    }

    httpd_uri_t get_index = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = HandleGetUpgradeIndex,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(server, &get_index);

    httpd_uri_t get_info = {
        .uri = "/api/upgrade_info",
        .method = HTTP_GET,
        .handler = HandleGetUpgradeInfo,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(server, &get_info);

    httpd_uri_t post_upload = {
        .uri = "/upload_ota",
        .method = HTTP_POST,
        .handler = HandlePostOtaUpload,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(server, &post_upload);

    http_server_handle_ = server;
    ESP_LOGI(TAG, "Web OTA 服务已就绪，端口: %d", port);
    return true;
}

void SystemUpgradeService::StopWebServer() {
    if (http_server_handle_ != nullptr) {
        httpd_stop(static_cast<httpd_handle_t>(http_server_handle_));
        http_server_handle_ = nullptr;
        ESP_LOGI(TAG, "Web OTA 服务已完全停止并释放内存");
    }
}

bool SystemUpgradeService::IsWebServerRunning() const {
    return http_server_handle_ != nullptr;
}

std::string SystemUpgradeService::GetWebServerUrl() const {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    if (netif != nullptr && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        char ip_str[32] = {};
        esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
        return std::string("http://") + ip_str;
    }
    return "";
}

void SystemUpgradeService::SetWebUploadCallback(WebUploadCallback cb) {
    web_cb_ = cb;
}

}  // namespace system_upgrade
