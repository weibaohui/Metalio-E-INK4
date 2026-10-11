#include "nvs_backup_service.h"

#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <cJSON.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <map>
#include <vector>
#include <esp_timer.h>

#include "SdCardManager.hpp"
#include "sd_paths.h"
#include "home_screen/home_screen.h"
#include "ff.h"
#include "reader/file_size.h"

namespace backup_restore {

static const char* TAG = "NvsBackupService";

NvsBackupService& NvsBackupService::GetInstance() {
    static NvsBackupService instance;
    return instance;
}

bool NvsBackupService::IsSdMounted() {
    auto& sd = SdCardManager::GetInstance();
    return sd.IsMounted() && sd.GetCard() != nullptr;
}

bool NvsBackupService::GetSdCapacityText(std::string& free_str, std::string& total_str) {
    if (!IsSdMounted()) {
        free_str = "--";
        total_str = "--";
        return false;
    }
    FATFS* fs = nullptr;
    DWORD free_clusters = 0;
    const FRESULT res = f_getfree("0:", &free_clusters, &fs);
    if (res == FR_OK && fs != nullptr) {
        constexpr DWORD kSectorSize = 512;
        uint64_t total_b = static_cast<uint64_t>(fs->n_fatent - 2) * fs->csize * kSectorSize;
        uint64_t free_b = static_cast<uint64_t>(free_clusters) * fs->csize * kSectorSize;
        char tb[32], fb[32];
        reader::FormatFileSize(tb, sizeof(tb), total_b);
        reader::FormatFileSize(fb, sizeof(fb), free_b);
        total_str = tb;
        free_str = fb;
        return true;
    }
    free_str = "未知";
    total_str = "未知";
    return false;
}

int NvsBackupService::GetLocalNvsEntryCount() {
    nvs_stats_t stats = {};
    if (nvs_get_stats(nullptr, &stats) == ESP_OK) {
        return static_cast<int>(stats.used_entries);
    }
    return 0;
}

std::vector<BackupFileInfo> NvsBackupService::ListBackups() {
    std::vector<BackupFileInfo> result;
    if (!IsSdMounted()) {
        return result;
    }

    SdEnsureAppLayout();
    (void)SdEnsureDir(SD_PATH_BACKUP);

    DIR* dir = opendir(SD_PATH_BACKUP);
    if (dir == nullptr) {
        ESP_LOGW(TAG, "opendir(%s) failed", SD_PATH_BACKUP);
        return result;
    }

    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        const char* name = entry->d_name;
        size_t len = strlen(name);
        if (len < 5 || strcasecmp(name + len - 5, ".json") != 0) {
            continue;
        }

        std::string full_path = std::string(SD_PATH_BACKUP) + "/" + name;
        struct stat st = {};
        if (stat(full_path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
            continue;
        }

        BackupFileInfo info;
        info.filename = name;
        info.full_path = full_path;
        info.size_bytes = static_cast<uint32_t>(st.st_size);
        info.mod_time = st.st_mtime;
        info.entry_count = -1;

        // 快速读取前 512 字节提取 count 字段
        FILE* f = fopen(full_path.c_str(), "r");
        if (f != nullptr) {
            char header_buf[512] = {};
            size_t n = fread(header_buf, 1, sizeof(header_buf) - 1, f);
            fclose(f);
            if (n > 0) {
                header_buf[n] = '\0';
                const char* p = strstr(header_buf, "\"count\"");
                if (p != nullptr) {
                    p = strchr(p, ':');
                    if (p != nullptr) {
                        int cnt = 0;
                        if (sscanf(p + 1, " %d", &cnt) == 1) {
                            info.entry_count = cnt;
                        }
                    }
                }
            }
        }

        result.push_back(info);
    }
    closedir(dir);

    // 按修改时间倒序排列（最新备份在最前）
    std::sort(result.begin(), result.end(), [](const BackupFileInfo& a, const BackupFileInfo& b) {
        return a.mod_time > b.mod_time;
    });

    return result;
}

ExportResult NvsBackupService::ExportToSd() {
    ExportResult res;
    if (!IsSdMounted()) {
        res.error_msg = "SD 卡未挂载或未插入";
        return res;
    }

    SdEnsureAppLayout();
    if (!SdEnsureDir(SD_PATH_BACKUP)) {
        res.error_msg = "创建备份目录失败";
        return res;
    }

    nvs_iterator_t it = nullptr;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, nullptr, NVS_TYPE_ANY, &it);
    if (err != ESP_OK || it == nullptr) {
        res.error_msg = "未找到 NVS 条目";
        return res;
    }

    cJSON* root = cJSON_CreateObject();
    if (root == nullptr) {
        nvs_release_iterator(it);
        res.error_msg = "内存不足，创建 JSON 失败";
        return res;
    }

    cJSON_AddStringToObject(root, "device", "metalio-e-ink4");
    cJSON_AddNumberToObject(root, "version", 1);

    time_t now = time(nullptr);
    cJSON_AddNumberToObject(root, "timestamp", static_cast<double>(now));

    struct tm tm_info = {};
    localtime_r(&now, &tm_info);
    char time_buf[40] = {};
    if (tm_info.tm_year + 1900 >= 2025) {
        snprintf(time_buf, sizeof(time_buf), "%04d-%02d-%02d %02d:%02d:%02d",
                 tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                 tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
    } else {
        snprintf(time_buf, sizeof(time_buf), "uptime_%llds", static_cast<long long>(esp_timer_get_time() / 1000000));
    }
    cJSON_AddStringToObject(root, "created_at", time_buf);

    cJSON* entries_arr = cJSON_CreateArray();
    cJSON_AddItemToObject(root, "entries", entries_arr);

    int total_exported = 0;
    std::string current_ns;
    nvs_handle_t current_handle = 0;

    while (err == ESP_OK && it != nullptr) {
        nvs_entry_info_t info = {};
        if (nvs_entry_info(it, &info) == ESP_OK) {
            // 排除硬件级校准命名空间 phy（避免不同设备恢复导致 RF 校准被冲掉）
            if (strcmp(info.namespace_name, "phy") != 0) {
                if (current_handle == 0 || current_ns != info.namespace_name) {
                    if (current_handle != 0) {
                        nvs_close(current_handle);
                        current_handle = 0;
                    }
                    if (nvs_open(info.namespace_name, NVS_READONLY, &current_handle) == ESP_OK) {
                        current_ns = info.namespace_name;
                    } else {
                        current_handle = 0;
                    }
                }

                if (current_handle != 0) {
                    cJSON* item = cJSON_CreateObject();
                    cJSON_AddStringToObject(item, "ns", info.namespace_name);
                    cJSON_AddStringToObject(item, "key", info.key);

                    bool item_ok = false;
                    switch (info.type) {
                        case NVS_TYPE_U8: {
                            uint8_t val = 0;
                            if (nvs_get_u8(current_handle, info.key, &val) == ESP_OK) {
                                cJSON_AddStringToObject(item, "type", "u8");
                                cJSON_AddNumberToObject(item, "val", val);
                                item_ok = true;
                            }
                            break;
                        }
                        case NVS_TYPE_I8: {
                            int8_t val = 0;
                            if (nvs_get_i8(current_handle, info.key, &val) == ESP_OK) {
                                cJSON_AddStringToObject(item, "type", "i8");
                                cJSON_AddNumberToObject(item, "val", val);
                                item_ok = true;
                            }
                            break;
                        }
                        case NVS_TYPE_U16: {
                            uint16_t val = 0;
                            if (nvs_get_u16(current_handle, info.key, &val) == ESP_OK) {
                                cJSON_AddStringToObject(item, "type", "u16");
                                cJSON_AddNumberToObject(item, "val", val);
                                item_ok = true;
                            }
                            break;
                        }
                        case NVS_TYPE_I16: {
                            int16_t val = 0;
                            if (nvs_get_i16(current_handle, info.key, &val) == ESP_OK) {
                                cJSON_AddStringToObject(item, "type", "i16");
                                cJSON_AddNumberToObject(item, "val", val);
                                item_ok = true;
                            }
                            break;
                        }
                        case NVS_TYPE_U32: {
                            uint32_t val = 0;
                            if (nvs_get_u32(current_handle, info.key, &val) == ESP_OK) {
                                cJSON_AddStringToObject(item, "type", "u32");
                                cJSON_AddNumberToObject(item, "val", val);
                                item_ok = true;
                            }
                            break;
                        }
                        case NVS_TYPE_I32: {
                            int32_t val = 0;
                            if (nvs_get_i32(current_handle, info.key, &val) == ESP_OK) {
                                cJSON_AddStringToObject(item, "type", "i32");
                                cJSON_AddNumberToObject(item, "val", val);
                                item_ok = true;
                            }
                            break;
                        }
                        case NVS_TYPE_U64: {
                            uint64_t val = 0;
                            if (nvs_get_u64(current_handle, info.key, &val) == ESP_OK) {
                                cJSON_AddStringToObject(item, "type", "u64");
                                cJSON_AddStringToObject(item, "val", std::to_string(val).c_str());
                                item_ok = true;
                            }
                            break;
                        }
                        case NVS_TYPE_I64: {
                            int64_t val = 0;
                            if (nvs_get_i64(current_handle, info.key, &val) == ESP_OK) {
                                cJSON_AddStringToObject(item, "type", "i64");
                                cJSON_AddStringToObject(item, "val", std::to_string(val).c_str());
                                item_ok = true;
                            }
                            break;
                        }
                        case NVS_TYPE_STR: {
                            size_t req = 0;
                            if (nvs_get_str(current_handle, info.key, nullptr, &req) == ESP_OK && req > 0) {
                                std::vector<char> buf(req);
                                if (nvs_get_str(current_handle, info.key, buf.data(), &req) == ESP_OK) {
                                    cJSON_AddStringToObject(item, "type", "str");
                                    cJSON_AddStringToObject(item, "val", buf.data());
                                    item_ok = true;
                                }
                            }
                            break;
                        }
                        case NVS_TYPE_BLOB: {
                            size_t req = 0;
                            if (nvs_get_blob(current_handle, info.key, nullptr, &req) == ESP_OK && req > 0) {
                                std::vector<uint8_t> buf(req);
                                if (nvs_get_blob(current_handle, info.key, buf.data(), &req) == ESP_OK) {
                                    std::string hex_str;
                                    hex_str.reserve(req * 2);
                                    static const char hex_chars[] = "0123456789abcdef";
                                    for (size_t b = 0; b < req; ++b) {
                                        hex_str.push_back(hex_chars[(buf[b] >> 4) & 0x0F]);
                                        hex_str.push_back(hex_chars[buf[b] & 0x0F]);
                                    }
                                    cJSON_AddStringToObject(item, "type", "blob");
                                    cJSON_AddStringToObject(item, "val", hex_str.c_str());
                                    item_ok = true;
                                }
                            }
                            break;
                        }
                        default:
                            break;
                    }

                    if (item_ok) {
                        cJSON_AddItemToArray(entries_arr, item);
                        total_exported++;
                    } else {
                        cJSON_Delete(item);
                    }
                }
            }
        }
        err = nvs_entry_next(&it);
    }

    if (current_handle != 0) {
        nvs_close(current_handle);
        current_handle = 0;
    }
    nvs_release_iterator(it);

    cJSON_AddNumberToObject(root, "count", total_exported);

    char* json_str = cJSON_Print(root);
    cJSON_Delete(root);

    if (json_str == nullptr) {
        res.error_msg = "序列化 JSON 失败";
        return res;
    }

    char filename[64];
    if (tm_info.tm_year + 1900 >= 2025) {
        snprintf(filename, sizeof(filename), "backup_%04d%02d%02d_%02d%02d%02d.json",
                 tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                 tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
    } else {
        int64_t uptime_s = esp_timer_get_time() / 1000000;
        snprintf(filename, sizeof(filename), "backup_uptime_%llds.json", static_cast<long long>(uptime_s));
    }

    std::string full_path = std::string(SD_PATH_BACKUP) + "/" + filename;
    FILE* f = fopen(full_path.c_str(), "w");
    if (f == nullptr) {
        cJSON_free(json_str);
        res.error_msg = "无法创建备份文件";
        return res;
    }

    size_t str_len = strlen(json_str);
    size_t written = fwrite(json_str, 1, str_len, f);
    fclose(f);
    cJSON_free(json_str);

    if (written != str_len) {
        res.error_msg = "写入文件不完整";
        return res;
    }

    res.success = true;
    res.filename = filename;
    res.full_path = full_path;
    res.count = total_exported;
    ESP_LOGI(TAG, "Exported %d NVS items to %s", total_exported, full_path.c_str());
    return res;
}

ImportResult NvsBackupService::ImportFromSd(const std::string& filepath) {
    ImportResult res;
    res.filename = filepath;

    if (!IsSdMounted()) {
        res.error_msg = "SD 卡未挂载或未插入";
        return res;
    }

    FILE* f = fopen(filepath.c_str(), "r");
    if (f == nullptr) {
        res.error_msg = "打开备份文件失败";
        return res;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0 || fsize > 512 * 1024) { // 限制最大 512KB 防内存耗尽
        fclose(f);
        res.error_msg = "备份文件大小无效";
        return res;
    }

    std::vector<char> buffer(fsize + 1, '\0');
    size_t read_bytes = fread(buffer.data(), 1, fsize, f);
    fclose(f);

    if (read_bytes != static_cast<size_t>(fsize)) {
        res.error_msg = "读取文件不完整";
        return res;
    }

    cJSON* root = cJSON_Parse(buffer.data());
    if (root == nullptr) {
        res.error_msg = "解析 JSON 格式失败";
        return res;
    }

    cJSON* entries_arr = cJSON_GetObjectItem(root, "entries");
    if (!cJSON_IsArray(entries_arr)) {
        cJSON_Delete(root);
        res.error_msg = "备份文件中缺少 entries 数组";
        return res;
    }

    int entry_count = cJSON_GetArraySize(entries_arr);
    if (entry_count <= 0) {
        cJSON_Delete(root);
        res.error_msg = "备份内容为空";
        return res;
    }

    // 按 namespace 分组写入
    struct EntryData {
        std::string key;
        std::string type;
        const cJSON* val_item = nullptr;
    };
    std::map<std::string, std::vector<EntryData>> ns_map;

    for (int i = 0; i < entry_count; ++i) {
        cJSON* item = cJSON_GetArrayItem(entries_arr, i);
        if (!cJSON_IsObject(item)) continue;

        cJSON* ns_item = cJSON_GetObjectItem(item, "ns");
        cJSON* key_item = cJSON_GetObjectItem(item, "key");
        cJSON* type_item = cJSON_GetObjectItem(item, "type");
        cJSON* val_item = cJSON_GetObjectItem(item, "val");

        if (!cJSON_IsString(ns_item) || !cJSON_IsString(key_item) || !cJSON_IsString(type_item) || val_item == nullptr) {
            continue;
        }

        std::string ns = ns_item->valuestring;
        if (ns == "phy") {
            continue; // 跳过硬件校准
        }

        EntryData ed;
        ed.key = key_item->valuestring;
        ed.type = type_item->valuestring;
        ed.val_item = val_item;
        ns_map[ns].push_back(ed);
    }

    int total_applied = 0;
    int total_failed = 0;

    for (auto& pair : ns_map) {
        const std::string& ns = pair.first;
        const auto& entries = pair.second;

        nvs_handle_t handle = 0;
        esp_err_t open_err = nvs_open(ns.c_str(), NVS_READWRITE, &handle);
        if (open_err != ESP_OK) {
            ESP_LOGE(TAG, "nvs_open(%s) for write failed: %s", ns.c_str(), esp_err_to_name(open_err));
            total_failed += static_cast<int>(entries.size());
            continue;
        }

        bool ns_dirty = false;
        for (const auto& entry : entries) {
            esp_err_t set_err = ESP_FAIL;
            if (entry.type == "u8") {
                uint8_t val = static_cast<uint8_t>(cJSON_GetNumberValue(entry.val_item));
                set_err = nvs_set_u8(handle, entry.key.c_str(), val);
            } else if (entry.type == "i8") {
                int8_t val = static_cast<int8_t>(cJSON_GetNumberValue(entry.val_item));
                set_err = nvs_set_i8(handle, entry.key.c_str(), val);
            } else if (entry.type == "u16") {
                uint16_t val = static_cast<uint16_t>(cJSON_GetNumberValue(entry.val_item));
                set_err = nvs_set_u16(handle, entry.key.c_str(), val);
            } else if (entry.type == "i16") {
                int16_t val = static_cast<int16_t>(cJSON_GetNumberValue(entry.val_item));
                set_err = nvs_set_i16(handle, entry.key.c_str(), val);
            } else if (entry.type == "u32") {
                uint32_t val = static_cast<uint32_t>(cJSON_GetNumberValue(entry.val_item));
                set_err = nvs_set_u32(handle, entry.key.c_str(), val);
            } else if (entry.type == "i32") {
                int32_t val = static_cast<int32_t>(cJSON_GetNumberValue(entry.val_item));
                set_err = nvs_set_i32(handle, entry.key.c_str(), val);
            } else if (entry.type == "u64") {
                if (cJSON_IsString(entry.val_item)) {
                    uint64_t val = strtoull(entry.val_item->valuestring, nullptr, 10);
                    set_err = nvs_set_u64(handle, entry.key.c_str(), val);
                } else {
                    uint64_t val = static_cast<uint64_t>(cJSON_GetNumberValue(entry.val_item));
                    set_err = nvs_set_u64(handle, entry.key.c_str(), val);
                }
            } else if (entry.type == "i64") {
                if (cJSON_IsString(entry.val_item)) {
                    int64_t val = strtoll(entry.val_item->valuestring, nullptr, 10);
                    set_err = nvs_set_i64(handle, entry.key.c_str(), val);
                } else {
                    int64_t val = static_cast<int64_t>(cJSON_GetNumberValue(entry.val_item));
                    set_err = nvs_set_i64(handle, entry.key.c_str(), val);
                }
            } else if (entry.type == "str") {
                if (cJSON_IsString(entry.val_item)) {
                    set_err = nvs_set_str(handle, entry.key.c_str(), entry.val_item->valuestring);
                }
            } else if (entry.type == "blob") {
                if (cJSON_IsString(entry.val_item)) {
                    const char* hex = entry.val_item->valuestring;
                    size_t hex_len = strlen(hex);
                    if (hex_len % 2 == 0) {
                        size_t blob_len = hex_len / 2;
                        std::vector<uint8_t> blob_buf(blob_len);
                        bool hex_ok = true;
                        for (size_t b = 0; b < blob_len; ++b) {
                            unsigned int byte_val = 0;
                            if (sscanf(hex + b * 2, "%02x", &byte_val) == 1) {
                                blob_buf[b] = static_cast<uint8_t>(byte_val);
                            } else {
                                hex_ok = false;
                                break;
                            }
                        }
                        if (hex_ok) {
                            set_err = nvs_set_blob(handle, entry.key.c_str(), blob_buf.data(), blob_len);
                        }
                    }
                }
            }

            if (set_err == ESP_OK) {
                total_applied++;
                ns_dirty = true;
            } else {
                total_failed++;
                ESP_LOGW(TAG, "nvs set failed for %s/%s (%s): %s",
                         ns.c_str(), entry.key.c_str(), entry.type.c_str(), esp_err_to_name(set_err));
            }
        }

        if (ns_dirty) {
            esp_err_t commit_err = nvs_commit(handle);
            if (commit_err != ESP_OK) {
                ESP_LOGE(TAG, "nvs_commit failed for ns %s: %s", ns.c_str(), esp_err_to_name(commit_err));
            }
        }
        nvs_close(handle);
    }

    cJSON_Delete(root);

    res.success = (total_applied > 0);
    res.count = total_applied;
    res.failed_count = total_failed;
    ESP_LOGI(TAG, "Import completed: applied=%d failed=%d", total_applied, total_failed);
    return res;
}

bool NvsBackupService::DeleteBackup(const std::string& filepath) {
    if (!IsSdMounted()) {
        return false;
    }
    int ret = unlink(filepath.c_str());
    if (ret == 0) {
        ESP_LOGI(TAG, "Deleted backup file: %s", filepath.c_str());
        return true;
    }
    ESP_LOGW(TAG, "Failed to delete backup file: %s, errno=%d", filepath.c_str(), errno);
    return false;
}

void NvsBackupService::ReloadRuntimeCaches() {
    // 重新载入主屏 APP 启用开关
    HomeScreen::InitAppToggles();
    // 重新载入主屏卡片样式
    HomeScreen::LoadCardStyle();
    ESP_LOGI(TAG, "Runtime caches reloaded");
}

}  // namespace backup_restore
