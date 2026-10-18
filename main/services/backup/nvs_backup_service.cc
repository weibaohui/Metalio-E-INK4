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
#include <vector>
#include <string>
#include <esp_timer.h>

#include "SdCardManager.hpp"
#include "sd_paths.h"
#include "home_screen/home_screen.h"
#include "app_manager.h"
#include "services/dynamic_apps/dynamic_app_loader.h"
#include "ff.h"
#include "reader_core/file_size.h"
#include "mcp_server.h"

namespace backup_restore {

static const char* TAG = "BackupService";

namespace {

bool IsExcludedNamespace(const char* ns) {
    if (ns == nullptr) return true;
    return (strcmp(ns, "phy") == 0 || strcmp(ns, "nvs.net80211") == 0);
}

bool IsSystemNamespace(const char* ns) {
    if (ns == nullptr) return false;
    static const char* kSystemNs[] = {
        "wifi", "settings", "syslog", "dbg", "power", "upgrade",
        "sys_upgrade", "mcp", "local_mcp", "board"
    };
    for (const char* s : kSystemNs) {
        if (strcmp(ns, s) == 0) return true;
    }
    return false;
}

bool CopyFile(const std::string& src, const std::string& dst) {
    FILE* in = fopen(src.c_str(), "rb");
    if (!in) return false;
    FILE* out = fopen(dst.c_str(), "wb");
    if (!out) {
        fclose(in);
        return false;
    }
    char buf[4096];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        if (fwrite(buf, 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    fclose(in);
    fclose(out);
    return ok;
}

bool CopyDirRecursive(const std::string& src, const std::string& dst) {
    SdEnsureDir(dst.c_str());
    DIR* dir = opendir(src.c_str());
    if (!dir) return false;

    struct dirent* ent;
    bool ok = true;
    while ((ent = readdir(dir)) != nullptr) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        const std::string s_child = src + "/" + ent->d_name;
        const std::string d_child = dst + "/" + ent->d_name;
        struct stat st = {};
        if (stat(s_child.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            ok = CopyDirRecursive(s_child, d_child) && ok;
        } else {
            ok = CopyFile(s_child, d_child) && ok;
        }
    }
    closedir(dir);
    return ok;
}

bool RemoveDirRecursive(const std::string& path) {
    DIR* dir = opendir(path.c_str());
    if (!dir) return false;

    struct dirent* ent;
    bool ok = true;
    while ((ent = readdir(dir)) != nullptr) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        const std::string child = path + "/" + ent->d_name;
        struct stat st = {};
        if (stat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
            ok = RemoveDirRecursive(child) && ok;
        } else {
            ok = (unlink(child.c_str()) == 0) && ok;
        }
    }
    closedir(dir);
    return (rmdir(path.c_str()) == 0) && ok;
}

// 序列化单条 NVS 项为 cJSON
cJSON* SerializeNvsEntry(nvs_handle_t handle, const nvs_entry_info_t& info) {
    cJSON* item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "ns", info.namespace_name);
    cJSON_AddStringToObject(item, "key", info.key);

    bool item_ok = false;
    switch (info.type) {
        case NVS_TYPE_U8: {
            uint8_t val = 0;
            if (nvs_get_u8(handle, info.key, &val) == ESP_OK) {
                cJSON_AddStringToObject(item, "type", "u8");
                cJSON_AddNumberToObject(item, "val", val);
                item_ok = true;
            }
            break;
        }
        case NVS_TYPE_I8: {
            int8_t val = 0;
            if (nvs_get_i8(handle, info.key, &val) == ESP_OK) {
                cJSON_AddStringToObject(item, "type", "i8");
                cJSON_AddNumberToObject(item, "val", val);
                item_ok = true;
            }
            break;
        }
        case NVS_TYPE_U16: {
            uint16_t val = 0;
            if (nvs_get_u16(handle, info.key, &val) == ESP_OK) {
                cJSON_AddStringToObject(item, "type", "u16");
                cJSON_AddNumberToObject(item, "val", val);
                item_ok = true;
            }
            break;
        }
        case NVS_TYPE_I16: {
            int16_t val = 0;
            if (nvs_get_i16(handle, info.key, &val) == ESP_OK) {
                cJSON_AddStringToObject(item, "type", "i16");
                cJSON_AddNumberToObject(item, "val", val);
                item_ok = true;
            }
            break;
        }
        case NVS_TYPE_U32: {
            uint32_t val = 0;
            if (nvs_get_u32(handle, info.key, &val) == ESP_OK) {
                cJSON_AddStringToObject(item, "type", "u32");
                cJSON_AddNumberToObject(item, "val", val);
                item_ok = true;
            }
            break;
        }
        case NVS_TYPE_I32: {
            int32_t val = 0;
            if (nvs_get_i32(handle, info.key, &val) == ESP_OK) {
                cJSON_AddStringToObject(item, "type", "i32");
                cJSON_AddNumberToObject(item, "val", val);
                item_ok = true;
            }
            break;
        }
        case NVS_TYPE_U64: {
            uint64_t val = 0;
            if (nvs_get_u64(handle, info.key, &val) == ESP_OK) {
                cJSON_AddStringToObject(item, "type", "u64");
                cJSON_AddStringToObject(item, "val", std::to_string(val).c_str());
                item_ok = true;
            }
            break;
        }
        case NVS_TYPE_I64: {
            int64_t val = 0;
            if (nvs_get_i64(handle, info.key, &val) == ESP_OK) {
                cJSON_AddStringToObject(item, "type", "i64");
                cJSON_AddStringToObject(item, "val", std::to_string(val).c_str());
                item_ok = true;
            }
            break;
        }
        case NVS_TYPE_STR: {
            size_t req = 0;
            if (nvs_get_str(handle, info.key, nullptr, &req) == ESP_OK && req > 0) {
                std::vector<char> buf(req);
                if (nvs_get_str(handle, info.key, buf.data(), &req) == ESP_OK) {
                    cJSON_AddStringToObject(item, "type", "str");
                    cJSON_AddStringToObject(item, "val", buf.data());
                    item_ok = true;
                }
            }
            break;
        }
        case NVS_TYPE_BLOB: {
            size_t req = 0;
            if (nvs_get_blob(handle, info.key, nullptr, &req) == ESP_OK && req > 0) {
                std::vector<uint8_t> buf(req);
                if (nvs_get_blob(handle, info.key, buf.data(), &req) == ESP_OK) {
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

    if (!item_ok) {
        cJSON_Delete(item);
        return nullptr;
    }
    return item;
}

// 恢复单个 NVS JSON 文件到 NVS
int RestoreNvsJsonFile(const std::string& filepath) {
    FILE* fp = fopen(filepath.c_str(), "rb");
    if (!fp) return 0;

    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz <= 0 || sz > 512 * 1024) {
        fclose(fp);
        return 0;
    }

    char* buf = (char*)malloc(sz + 1);
    if (!buf) {
        fclose(fp);
        return 0;
    }
    size_t nr = fread(buf, 1, sz, fp);
    buf[nr] = '\0';
    fclose(fp);

    cJSON* root = cJSON_Parse(buf);
    free(buf);
    if (!root) return 0;

    cJSON* entries = cJSON_GetObjectItem(root, "entries");
    if (!cJSON_IsArray(entries)) {
        cJSON_Delete(root);
        return 0;
    }

    int success_count = 0;
    std::string current_ns;
    nvs_handle_t current_handle = 0;

    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, entries) {
        cJSON* j_ns = cJSON_GetObjectItem(item, "ns");
        cJSON* j_key = cJSON_GetObjectItem(item, "key");
        cJSON* j_type = cJSON_GetObjectItem(item, "type");
        cJSON* j_val = cJSON_GetObjectItem(item, "val");

        if (!cJSON_IsString(j_ns) || !cJSON_IsString(j_key) || !cJSON_IsString(j_type) || !j_val) {
            continue;
        }

        const char* ns = j_ns->valuestring;
        const char* key = j_key->valuestring;
        const char* type = j_type->valuestring;

        if (IsExcludedNamespace(ns)) continue;

        if (current_handle == 0 || current_ns != ns) {
            if (current_handle != 0) {
                nvs_commit(current_handle);
                nvs_close(current_handle);
                current_handle = 0;
            }
            if (nvs_open(ns, NVS_READWRITE, &current_handle) == ESP_OK) {
                current_ns = ns;
            } else {
                current_handle = 0;
                continue;
            }
        }

        esp_err_t set_err = ESP_FAIL;
        if (strcmp(type, "u8") == 0 && cJSON_IsNumber(j_val)) {
            set_err = nvs_set_u8(current_handle, key, static_cast<uint8_t>(j_val->valuedouble));
        } else if (strcmp(type, "i8") == 0 && cJSON_IsNumber(j_val)) {
            set_err = nvs_set_i8(current_handle, key, static_cast<int8_t>(j_val->valuedouble));
        } else if (strcmp(type, "u16") == 0 && cJSON_IsNumber(j_val)) {
            set_err = nvs_set_u16(current_handle, key, static_cast<uint16_t>(j_val->valuedouble));
        } else if (strcmp(type, "i16") == 0 && cJSON_IsNumber(j_val)) {
            set_err = nvs_set_i16(current_handle, key, static_cast<int16_t>(j_val->valuedouble));
        } else if (strcmp(type, "u32") == 0 && cJSON_IsNumber(j_val)) {
            set_err = nvs_set_u32(current_handle, key, static_cast<uint32_t>(j_val->valuedouble));
        } else if (strcmp(type, "i32") == 0 && cJSON_IsNumber(j_val)) {
            set_err = nvs_set_i32(current_handle, key, static_cast<int32_t>(j_val->valuedouble));
        } else if (strcmp(type, "u64") == 0 && cJSON_IsString(j_val)) {
            uint64_t v = strtoull(j_val->valuestring, nullptr, 10);
            set_err = nvs_set_u64(current_handle, key, v);
        } else if (strcmp(type, "i64") == 0 && cJSON_IsString(j_val)) {
            int64_t v = strtoll(j_val->valuestring, nullptr, 10);
            set_err = nvs_set_i64(current_handle, key, v);
        } else if (strcmp(type, "str") == 0 && cJSON_IsString(j_val)) {
            set_err = nvs_set_str(current_handle, key, j_val->valuestring);
        } else if (strcmp(type, "blob") == 0 && cJSON_IsString(j_val)) {
            const char* hex = j_val->valuestring;
            size_t hex_len = strlen(hex);
            if (hex_len % 2 == 0) {
                std::vector<uint8_t> blob(hex_len / 2);
                for (size_t b = 0; b < blob.size(); ++b) {
                    char hbuf[3] = { hex[b * 2], hex[b * 2 + 1], '\0' };
                    blob[b] = static_cast<uint8_t>(strtoul(hbuf, nullptr, 16));
                }
                set_err = nvs_set_blob(current_handle, key, blob.data(), blob.size());
            }
        }

        if (set_err == ESP_OK) {
            success_count++;
        }
    }

    if (current_handle != 0) {
        nvs_commit(current_handle);
        nvs_close(current_handle);
        current_handle = 0;
    }
    cJSON_Delete(root);
    return success_count;
}

} // namespace

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

bool NvsBackupService::GetStats(int& out_system_nvs, int& out_app_nvs, int& out_installed_apps) {
    out_system_nvs = 0;
    out_app_nvs = 0;
    out_installed_apps = 0;

    nvs_iterator_t it = nullptr;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, nullptr, NVS_TYPE_ANY, &it);
    while (err == ESP_OK && it != nullptr) {
        nvs_entry_info_t info = {};
        if (nvs_entry_info(it, &info) == ESP_OK) {
            if (!IsExcludedNamespace(info.namespace_name)) {
                if (IsSystemNamespace(info.namespace_name)) {
                    out_system_nvs++;
                } else {
                    out_app_nvs++;
                }
            }
        }
        err = nvs_entry_next(&it);
    }
    if (it != nullptr) nvs_release_iterator(it);

    if (IsSdMounted()) {
        DIR* dir = opendir(SD_PATH_APPS);
        if (dir) {
            struct dirent* ent;
            while ((ent = readdir(dir)) != nullptr) {
                if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
                std::string p = std::string(SD_PATH_APPS) + "/" + ent->d_name;
                struct stat st = {};
                if (stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                    out_installed_apps++;
                }
            }
            closedir(dir);
        }
    }
    return true;
}

std::vector<BackupFileInfo> NvsBackupService::ListBackups() {
    std::vector<BackupFileInfo> result;
    if (!IsSdMounted()) return result;

    SdEnsureAppLayout();
    SdEnsureDir(SD_PATH_BACKUP);

    DIR* dir = opendir(SD_PATH_BACKUP);
    if (!dir) return result;

    struct dirent* entry = nullptr;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        const std::string dir_path = std::string(SD_PATH_BACKUP) + "/" + entry->d_name;
        struct stat st = {};
        if (stat(dir_path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;

        // 检查 backup_meta.json
        const std::string meta_path = dir_path + "/backup_meta.json";
        FILE* mf = fopen(meta_path.c_str(), "rb");
        if (!mf) continue;

        fseek(mf, 0, SEEK_END);
        long msz = ftell(mf);
        fseek(mf, 0, SEEK_SET);
        if (msz <= 0 || msz > 16384) {
            fclose(mf);
            continue;
        }

        std::vector<char> mbuf(msz + 1);
        size_t nr = fread(mbuf.data(), 1, msz, mf);
        mbuf[nr] = '\0';
        fclose(mf);

        cJSON* meta = cJSON_Parse(mbuf.data());
        if (!meta) continue;

        BackupFileInfo info;
        info.filename = entry->d_name;
        info.full_path = dir_path;
        info.mod_time = st.st_mtime;

        cJSON* j_type = cJSON_GetObjectItem(meta, "backup_type");
        if (cJSON_IsNumber(j_type)) info.backup_type = j_type->valueint;

        cJSON* j_created = cJSON_GetObjectItem(meta, "created_at");
        if (cJSON_IsString(j_created)) info.created_at = j_created->valuestring;

        cJSON* j_hsys = cJSON_GetObjectItem(meta, "has_system");
        if (cJSON_IsBool(j_hsys)) info.has_system = cJSON_IsTrue(j_hsys);

        cJSON* j_happ = cJSON_GetObjectItem(meta, "has_app_data");
        if (cJSON_IsBool(j_happ)) info.has_app_data = cJSON_IsTrue(j_happ);

        cJSON* j_hbun = cJSON_GetObjectItem(meta, "has_app_bundles");
        if (cJSON_IsBool(j_hbun)) info.has_app_bundles = cJSON_IsTrue(j_hbun);

        cJSON* j_sys_cnt = cJSON_GetObjectItem(meta, "system_nvs_count");
        if (cJSON_IsNumber(j_sys_cnt)) info.system_nvs_count = j_sys_cnt->valueint;

        cJSON* j_app_cnt = cJSON_GetObjectItem(meta, "app_nvs_count");
        if (cJSON_IsNumber(j_app_cnt)) info.app_nvs_count = j_app_cnt->valueint;

        cJSON* j_data_cnt = cJSON_GetObjectItem(meta, "app_data_count");
        if (cJSON_IsNumber(j_data_cnt)) info.app_data_count = j_data_cnt->valueint;

        cJSON* j_bun_cnt = cJSON_GetObjectItem(meta, "app_bundle_count");
        if (cJSON_IsNumber(j_bun_cnt)) info.app_bundle_count = j_bun_cnt->valueint;

        cJSON* j_sz = cJSON_GetObjectItem(meta, "size_bytes");
        if (cJSON_IsNumber(j_sz)) info.size_bytes = static_cast<uint32_t>(j_sz->valuedouble);

        cJSON_Delete(meta);
        result.push_back(info);
    }
    closedir(dir);

    // 依修改时间倒序排列
    std::sort(result.begin(), result.end(), [](const BackupFileInfo& a, const BackupFileInfo& b) {
        return a.mod_time > b.mod_time;
    });

    return result;
}

ExportResult NvsBackupService::ExportToSd(int backup_type) {
    ExportResult res;
    if (!IsSdMounted()) {
        res.error_msg = "SD 卡未挂载";
        return res;
    }

    SdEnsureAppLayout();
    SdEnsureDir(SD_PATH_BACKUP);

    time_t now = time(nullptr);
    struct tm tm_info = {};
    localtime_r(&now, &tm_info);

    char time_str[64] = {};
    char dir_name[128] = {};
    const char* suffix = (backup_type == BACKUP_TYPE_SYSTEM) ? "sys"
                         : (backup_type == BACKUP_TYPE_APP_DATA) ? "app" : "full";

    if (tm_info.tm_year + 1900 >= 2025) {
        snprintf(time_str, sizeof(time_str), "%04d-%02d-%02d %02d:%02d:%02d",
                 tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                 tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec);
        snprintf(dir_name, sizeof(dir_name), "%04d%02d%02d_%02d%02d%02d_%s",
                 tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                 tm_info.tm_hour, tm_info.tm_min, tm_info.tm_sec, suffix);
    } else {
        uint32_t uptime_s = static_cast<uint32_t>(esp_timer_get_time() / 1000000);
        snprintf(time_str, sizeof(time_str), "uptime_%lus", (unsigned long)uptime_s);
        snprintf(dir_name, sizeof(dir_name), "backup_%lus_%s", (unsigned long)uptime_s, suffix);
    }

    const std::string snapshot_dir = std::string(SD_PATH_BACKUP) + "/" + dir_name;
    if (!SdEnsureDir(snapshot_dir.c_str())) {
        res.error_msg = "创建快照目录失败";
        return res;
    }

    // 1. 分类提取 NVS
    cJSON* sys_root = cJSON_CreateObject();
    cJSON* sys_entries = cJSON_CreateArray();
    cJSON_AddItemToObject(sys_root, "entries", sys_entries);

    cJSON* app_root = cJSON_CreateObject();
    cJSON* app_entries = cJSON_CreateArray();
    cJSON_AddItemToObject(app_root, "entries", app_entries);

    int sys_count = 0;
    int app_count = 0;

    nvs_iterator_t it = nullptr;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, nullptr, NVS_TYPE_ANY, &it);

    std::string cur_ns;
    nvs_handle_t cur_h = 0;

    while (err == ESP_OK && it != nullptr) {
        nvs_entry_info_t info = {};
        if (nvs_entry_info(it, &info) == ESP_OK) {
            if (!IsExcludedNamespace(info.namespace_name)) {
                if (cur_h == 0 || cur_ns != info.namespace_name) {
                    if (cur_h != 0) {
                        nvs_close(cur_h);
                        cur_h = 0;
                    }
                    if (nvs_open(info.namespace_name, NVS_READONLY, &cur_h) == ESP_OK) {
                        cur_ns = info.namespace_name;
                    } else {
                        cur_h = 0;
                    }
                }

                if (cur_h != 0) {
                    cJSON* item = SerializeNvsEntry(cur_h, info);
                    if (item) {
                        if (IsSystemNamespace(info.namespace_name)) {
                            cJSON_AddItemToArray(sys_entries, item);
                            sys_count++;
                        } else {
                            cJSON_AddItemToArray(app_entries, item);
                            app_count++;
                        }
                    }
                }
            }
        }
        err = nvs_entry_next(&it);
    }

    if (cur_h != 0) {
        nvs_close(cur_h);
        cur_h = 0;
    }
    if (it != nullptr) {
        nvs_release_iterator(it);
    }

    // 保存系统 NVS
    const bool save_sys = (backup_type == BACKUP_TYPE_FULL || backup_type == BACKUP_TYPE_SYSTEM);
    if (save_sys) {
        cJSON_AddNumberToObject(sys_root, "count", sys_count);
        char* sys_str = cJSON_Print(sys_root);
        if (sys_str) {
            FILE* f = fopen((snapshot_dir + "/system_config.json").c_str(), "wb");
            if (f) {
                fwrite(sys_str, 1, strlen(sys_str), f);
                fclose(f);
            }
            cJSON_free(sys_str);
        }
    }
    cJSON_Delete(sys_root);

    // 保存应用 NVS
    const bool save_app = (backup_type == BACKUP_TYPE_FULL || backup_type == BACKUP_TYPE_APP_DATA);
    if (save_app) {
        cJSON_AddNumberToObject(app_root, "count", app_count);
        char* app_str = cJSON_Print(app_root);
        if (app_str) {
            FILE* f = fopen((snapshot_dir + "/app_config.json").c_str(), "wb");
            if (f) {
                fwrite(app_str, 1, strlen(app_str), f);
                fclose(f);
            }
            cJSON_free(app_str);
        }
    }
    cJSON_Delete(app_root);

    // 2. 备份应用私有数据目录 (apps/<id>.app/data)
    int backed_app_data = 0;
    if (save_app) {
        DIR* apps_dir = opendir(SD_PATH_APPS);
        if (apps_dir) {
            struct dirent* ent;
            while ((ent = readdir(apps_dir)) != nullptr) {
                if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
                std::string app_data_src = std::string(SD_PATH_APPS) + "/" + ent->d_name + "/data";
                struct stat st = {};
                if (stat(app_data_src.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                    std::string app_data_dst = snapshot_dir + "/app_data/" + ent->d_name;
                    if (CopyDirRecursive(app_data_src, app_data_dst)) {
                        backed_app_data++;
                    }
                }
            }
            closedir(apps_dir);
        }
    }

    // 3. 全量备份时，备份已安装的应用包本体 (apps/<id>.app/)
    int backed_bundles = 0;
    if (backup_type == BACKUP_TYPE_FULL) {
        DIR* apps_dir = opendir(SD_PATH_APPS);
        if (apps_dir) {
            struct dirent* ent;
            while ((ent = readdir(apps_dir)) != nullptr) {
                if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
                std::string bundle_src = std::string(SD_PATH_APPS) + "/" + ent->d_name;
                std::string manifest_file = bundle_src + "/manifest.json";
                struct stat st = {};
                if (stat(manifest_file.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                    std::string bundle_dst = snapshot_dir + "/app_bundles/" + ent->d_name;
                    SdEnsureDir(bundle_dst.c_str());
                    CopyFile(manifest_file, bundle_dst + "/manifest.json");
                    if (stat((bundle_src + "/icon.png").c_str(), &st) == 0) {
                        CopyFile(bundle_src + "/icon.png", bundle_dst + "/icon.png");
                    }
                    if (stat((bundle_src + "/README.md").c_str(), &st) == 0) {
                        CopyFile(bundle_src + "/README.md", bundle_dst + "/README.md");
                    }
                    if (stat((bundle_src + "/lib/app.so").c_str(), &st) == 0) {
                        SdEnsureDir((bundle_dst + "/lib").c_str());
                        CopyFile(bundle_src + "/lib/app.so", bundle_dst + "/lib/app.so");
                    }
                    backed_bundles++;
                }
            }
            closedir(apps_dir);
        }
    }

    // 4. 写入 backup_meta.json
    cJSON* meta = cJSON_CreateObject();
    cJSON_AddStringToObject(meta, "device", "metalio-e-ink4");
    cJSON_AddNumberToObject(meta, "version", 2);
    cJSON_AddNumberToObject(meta, "backup_type", backup_type);
    cJSON_AddNumberToObject(meta, "timestamp", static_cast<double>(now));
    cJSON_AddStringToObject(meta, "created_at", time_str);
    cJSON_AddBoolToObject(meta, "has_system", save_sys);
    cJSON_AddBoolToObject(meta, "has_app_data", save_app);
    cJSON_AddBoolToObject(meta, "has_app_bundles", backup_type == BACKUP_TYPE_FULL);
    cJSON_AddNumberToObject(meta, "system_nvs_count", sys_count);
    cJSON_AddNumberToObject(meta, "app_nvs_count", app_count);
    cJSON_AddNumberToObject(meta, "app_data_count", backed_app_data);
    cJSON_AddNumberToObject(meta, "app_bundle_count", backed_bundles);

    char* meta_str = cJSON_Print(meta);
    cJSON_Delete(meta);
    if (meta_str) {
        FILE* mf = fopen((snapshot_dir + "/backup_meta.json").c_str(), "wb");
        if (mf) {
            fwrite(meta_str, 1, strlen(meta_str), mf);
            fclose(mf);
        }
        cJSON_free(meta_str);
    }

    res.success = true;
    res.filename = dir_name;
    res.full_path = snapshot_dir;
    res.system_count = sys_count;
    res.app_count = app_count;
    ESP_LOGI(TAG, "backup exported: %s (sys=%d, app_nvs=%d, app_data=%d, bundles=%d)",
             dir_name, sys_count, app_count, backed_app_data, backed_bundles);
    return res;
}

ImportResult NvsBackupService::ImportFromSd(const std::string& snapshot_path, int restore_mask) {
    ImportResult res;
    res.filename = snapshot_path;

    if (!IsSdMounted()) {
        res.error_msg = "SD 卡未挂载";
        return res;
    }

    const std::string meta_path = snapshot_path + "/backup_meta.json";
    FILE* mf = fopen(meta_path.c_str(), "rb");
    if (!mf) {
        res.error_msg = "未找到 backup_meta.json";
        return res;
    }
    fclose(mf);

    int restored_items = 0;

    // 1. 恢复系统配置
    if (restore_mask & RESTORE_MASK_SYSTEM) {
        std::string sys_json = snapshot_path + "/system_config.json";
        int cnt = RestoreNvsJsonFile(sys_json);
        restored_items += cnt;
        ESP_LOGI(TAG, "system config restored: %d items", cnt);
    }

    // 2. 恢复应用状态与私有数据
    if (restore_mask & RESTORE_MASK_APP_DATA) {
        // 恢复应用 NVS
        std::string app_json = snapshot_path + "/app_config.json";
        int cnt = RestoreNvsJsonFile(app_json);
        restored_items += cnt;

        // 恢复各应用 data/
        std::string app_data_src = snapshot_path + "/app_data";
        DIR* ad_dir = opendir(app_data_src.c_str());
        if (ad_dir) {
            struct dirent* ent;
            while ((ent = readdir(ad_dir)) != nullptr) {
                if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
                std::string s_app_dir = app_data_src + "/" + ent->d_name;
                std::string d_app_dir = std::string(SD_PATH_APPS) + "/" + ent->d_name + "/data";
                CopyDirRecursive(s_app_dir, d_app_dir);
                restored_items++;
            }
            closedir(ad_dir);
        }
        ESP_LOGI(TAG, "app data and states restored");
    }

    // 3. 恢复应用安装包本体
    if (restore_mask & RESTORE_MASK_APP_BUNDLES) {
        std::string bundles_src = snapshot_path + "/app_bundles";
        DIR* b_dir = opendir(bundles_src.c_str());
        if (b_dir) {
            struct dirent* ent;
            while ((ent = readdir(b_dir)) != nullptr) {
                if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
                std::string s_bundle = bundles_src + "/" + ent->d_name;
                std::string d_bundle = std::string(SD_PATH_APPS) + "/" + ent->d_name;
                CopyDirRecursive(s_bundle, d_bundle);
                restored_items++;
            }
            closedir(b_dir);
        }
        ESP_LOGI(TAG, "app bundles restored");
    }

    ReloadRuntimeCaches();

    res.success = true;
    res.restored_items = restored_items;
    return res;
}

bool NvsBackupService::DeleteBackup(const std::string& snapshot_path) {
    if (!IsSdMounted() || snapshot_path.empty()) return false;
    return RemoveDirRecursive(snapshot_path);
}

void NvsBackupService::ReloadRuntimeCaches() {
    AppManager::GetInstance().Init();
    dynamic_apps::ScanAndRegister();
    HomeScreenRequestAppsRefresh();
}

void Backup_RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool("self.backup.create",
        "Create a backup snapshot to SD card. Args: type ('full', 'system', 'app_data', default 'full').",
        PropertyList({Property("type", kPropertyTypeString, std::string("full"))}),
        [](const PropertyList& p) -> ReturnValue {
            std::string t = p["type"].value<std::string>();
            int btype = BACKUP_TYPE_FULL;
            if (t == "system") btype = BACKUP_TYPE_SYSTEM;
            else if (t == "app_data") btype = BACKUP_TYPE_APP_DATA;
            auto res = NvsBackupService::GetInstance().ExportToSd(btype);
            cJSON* obj = cJSON_CreateObject();
            cJSON_AddBoolToObject(obj, "success", res.success);
            cJSON_AddStringToObject(obj, "filename", res.filename.c_str());
            cJSON_AddStringToObject(obj, "path", res.full_path.c_str());
            cJSON_AddNumberToObject(obj, "system_count", res.system_count);
            cJSON_AddNumberToObject(obj, "app_count", res.app_count);
            if (!res.error_msg.empty()) {
                cJSON_AddStringToObject(obj, "error", res.error_msg.c_str());
            }
            return obj;
        });

    mcp.AddTool("self.backup.list",
        "List all structured backup snapshots on the SD card.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            auto list = NvsBackupService::GetInstance().ListBackups();
            cJSON* arr = cJSON_CreateArray();
            for (const auto& item : list) {
                cJSON* obj = cJSON_CreateObject();
                cJSON_AddStringToObject(obj, "name", item.filename.c_str());
                cJSON_AddStringToObject(obj, "created_at", item.created_at.c_str());
                cJSON_AddNumberToObject(obj, "type", item.backup_type);
                cJSON_AddBoolToObject(obj, "has_system", item.has_system);
                cJSON_AddBoolToObject(obj, "has_app_data", item.has_app_data);
                cJSON_AddBoolToObject(obj, "has_app_bundles", item.has_app_bundles);
                cJSON_AddNumberToObject(obj, "system_nvs", item.system_nvs_count);
                cJSON_AddNumberToObject(obj, "app_nvs", item.app_nvs_count);
                cJSON_AddNumberToObject(obj, "app_bundles", item.app_bundle_count);
                cJSON_AddNumberToObject(obj, "size_bytes", item.size_bytes);
                cJSON_AddItemToArray(arr, obj);
            }
            return arr;
        });

    mcp.AddTool("self.backup.restore",
        "Restore from a backup snapshot. Args: snapshot (directory name or path), mask ('all', 'system', 'app_data', default 'all').",
        PropertyList({Property("snapshot", kPropertyTypeString),
                      Property("mask", kPropertyTypeString, std::string("all"))}),
        [](const PropertyList& p) -> ReturnValue {
            std::string snap = p["snapshot"].value<std::string>();
            std::string mask_str = p["mask"].value<std::string>();
            int mask = RESTORE_MASK_ALL;
            if (mask_str == "system") mask = RESTORE_MASK_SYSTEM;
            else if (mask_str == "app_data") mask = RESTORE_MASK_APP_DATA;
            std::string full_path = snap;
            if (snap.rfind("/sdcard", 0) != 0) {
                full_path = std::string(SD_PATH_BACKUP) + "/" + snap;
            }
            auto res = NvsBackupService::GetInstance().ImportFromSd(full_path, mask);
            cJSON* obj = cJSON_CreateObject();
            cJSON_AddBoolToObject(obj, "success", res.success);
            cJSON_AddNumberToObject(obj, "restored_items", res.restored_items);
            if (!res.error_msg.empty()) {
                cJSON_AddStringToObject(obj, "error", res.error_msg.c_str());
            }
            return obj;
        });

    mcp.AddTool("self.backup.delete",
        "Delete a backup snapshot directory from SD card. Arg: snapshot (directory name or full path).",
        PropertyList({Property("snapshot", kPropertyTypeString)}),
        [](const PropertyList& p) -> ReturnValue {
            std::string snap = p["snapshot"].value<std::string>();
            std::string full_path = snap;
            if (snap.rfind("/sdcard", 0) != 0) {
                full_path = std::string(SD_PATH_BACKUP) + "/" + snap;
            }
            bool ok = NvsBackupService::GetInstance().DeleteBackup(full_path);
            return ok;
        });

    ESP_LOGI(TAG, "backup MCP tools registered");
}

}  // namespace backup_restore
