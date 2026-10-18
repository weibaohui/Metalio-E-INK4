// main/services/dynamic_apps/dynamic_app_shims.cc
// 动态应用 C ABI 垫片实现：转发到 C++ 固件设施（见 dynamic_app_shims.h）。
#include "dynamic_app_shims.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <esp_log.h>

#include "display/lv_adapter_display.h"
#include "display/screen/screen_common.h"
#include "display/screen/vk_key_handler.h"
#include "sd_paths.h"

void* dynapp_screen_status_bar(void* scr) {
    if (scr == nullptr) {
        return nullptr;
    }
    return ScreenCreateStatusBar(static_cast<lv_obj_t*>(scr)).bar;
}

void dynapp_vk_attach(void* scr, const char* name,
                      void* (*factory)(void), int (*on_key)(const char* key)) {
    if (scr == nullptr || name == nullptr) {
        return;
    }
    VkKeyScreenDesc desc;
    desc.factory = reinterpret_cast<ScreenFactory>(factory);
    desc.on_key = reinterpret_cast<VkKeyHandler>(on_key);
    VkKey_AttachScreen(static_cast<lv_obj_t*>(scr), name, desc);
}

int dynapp_lv_async(void (*cb)(void*), void* user_data) {
    return ScreenLvAsync(cb, user_data) ? 1 : 0;
}

void dynapp_screen_go_home(void) {
    ScreenRequestHome();
}

void dynapp_epd_full_refresh(void) {
    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->RequestNextFullRefresh();
    }
}

namespace {
std::string s_active_bundle_path;
std::string s_active_data_path;
constexpr const char* kShimTag = "DynAppShim";
}

void dynapp_set_active_bundle(const char* bundle_path) {
    if (bundle_path != nullptr) {
        s_active_bundle_path = bundle_path;
        s_active_data_path = s_active_bundle_path + "/data";
    } else {
        s_active_bundle_path.clear();
        s_active_data_path.clear();
    }
}

void dynapp_clear_active_bundle(void) {
    s_active_bundle_path.clear();
    s_active_data_path.clear();
}

const char* dynapp_get_bundle_path(void) {
    return s_active_bundle_path.c_str();
}

const char* dynapp_get_data_path(void) {
    if (s_active_data_path.empty()) {
        return "";
    }
    // 首次获取或确保 data 目录存在（0755）
    SdEnsureDir(s_active_data_path.c_str());
    return s_active_data_path.c_str();
}

void* dynapp_fopen_data(const char* rel_filename, const char* mode) {
    if (rel_filename == nullptr || rel_filename[0] == '\0' || rel_filename[0] == '/') {
        ESP_LOGE(kShimTag, "fopen_data: invalid relative filename");
        return nullptr;
    }
    if (strstr(rel_filename, "..") != nullptr || strchr(rel_filename, '\\') != nullptr) {
        ESP_LOGE(kShimTag, "fopen_data: path traversal attempt rejected: %s", rel_filename);
        return nullptr;
    }
    const char* data_dir = dynapp_get_data_path();
    if (data_dir == nullptr || data_dir[0] == '\0') {
        ESP_LOGE(kShimTag, "fopen_data: no active app data path");
        return nullptr;
    }
    std::string full_path = std::string(data_dir) + "/" + rel_filename;
    FILE* f = fopen(full_path.c_str(), mode);
    if (f == nullptr) {
        ESP_LOGD(kShimTag, "fopen_data %s (%s) failed", full_path.c_str(), mode ? mode : "");
    }
    return f;
}

int dynapp_is_path_allowed(const char* path, int write) {
    if (path == nullptr || path[0] == '\0') {
        return 0;
    }
    if (strstr(path, "..") != nullptr || strchr(path, '\\') != nullptr) {
        return 0;
    }
    const char* data_dir = dynapp_get_data_path();
    if (write) {
        // 写操作严格限制在自身 data/ 目录下
        if (data_dir != nullptr && data_dir[0] != '\0') {
            const size_t len = strlen(data_dir);
            if (strncmp(path, data_dir, len) == 0 && (path[len] == '/' || path[len] == '\0')) {
                return 1;
            }
        }
        ESP_LOGW(kShimTag, "sandbox: write access denied outside data dir: %s", path);
        return 0;
    }
    // 读操作允许读取自身 data/、自身 bundle 资源、系统公共字体及壁纸
    if (data_dir != nullptr && data_dir[0] != '\0') {
        const size_t len = strlen(data_dir);
        if (strncmp(path, data_dir, len) == 0 && (path[len] == '/' || path[len] == '\0')) {
            return 1;
        }
    }
    const char* bundle_dir = dynapp_get_bundle_path();
    if (bundle_dir != nullptr && bundle_dir[0] != '\0') {
        const size_t len = strlen(bundle_dir);
        if (strncmp(path, bundle_dir, len) == 0 && (path[len] == '/' || path[len] == '\0')) {
            return 1;
        }
    }
    if (strncmp(path, "/spiffs", 7) == 0) return 1;
    if (strncmp(path, "/sdcard/metalio/e-ink/fonts", 27) == 0) return 1;
    if (strncmp(path, "/sdcard/metalio/e-ink/wallpaper", 31) == 0) return 1;
    return 0;
}

#include "application.h"
#include "board.h"
#include "boot_key_handler.h"
#include "display/screen/epd_widget_kit.h"
#include "power_policy.h"
#include "services/time_service.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

void dynapp_power_sleep(void) {
    ESP_LOGI(kShimTag, "dynapp power sleep");
    PowerKey_OnClick();
}

void dynapp_power_reboot(void) {
    ESP_LOGI(kShimTag, "dynapp power reboot");
    Application::GetInstance().Schedule([]() {
        vTaskDelay(pdMS_TO_TICKS(300));
        Application::GetInstance().Reboot();
    });
}

void dynapp_power_shutdown(void) {
    ESP_LOGI(kShimTag, "dynapp power shutdown");
    PowerPolicy::GetInstance().RequestPowerOff();
}

int dynapp_is_network_ready(void) {
    return Board::GetInstance().IsNetworkReady() ? 1 : 0;
}

int dynapp_time_is_synced(void) {
    return TimeService_IsSynced() ? 1 : 0;
}

long dynapp_time_last_sync_epoch(void) {
    return TimeService_LastSyncEpoch();
}

int dynapp_time_get_tz_offset_min(void) {
    return TimeService_GetTzOffsetMin();
}

void dynapp_time_set_tz_offset_min(int minutes) {
    TimeService_SetTzOffsetMin(minutes);
}

void dynapp_time_sync_now(void) {
    TimeService_SyncNow();
}

int dynapp_time_set_manual(int year, int month, int day, int hour, int minute) {
    return TimeService_SetManual(year, month, day, hour, minute) ? 1 : 0;
}

void dynapp_toast_show(const char* message, uint32_t duration_ms) {
    if (message != nullptr && message[0] != '\0') {
        EpdWidgetKit::EpdToast::Show(message, duration_ms);
    }
}

#include <esp_heap_caps.h>
#include <esp_ota_ops.h>
#include <esp_timer.h>
#include <esp_idf_version.h>
#include <nvs.h>
#include <ff.h>
#include "boards/common/SdCardManager.hpp"
#include "system_info.h"

int dynapp_sysinfo_get(dynapp_sysinfo_t* out) {
    if (out == nullptr) return 0;
    memset(out, 0, sizeof(*out));

    // 内部 RAM
    out->ram_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    out->ram_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    out->ram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    out->ram_min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    // PSRAM
    out->psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    out->psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    out->psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    out->psram_min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    // Flash & App 分区
    out->flash_size = SystemInfo::GetFlashSize();
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running != nullptr) {
        out->app_part_size = running->size;
        strncpy(out->app_part_label, running->label, sizeof(out->app_part_label) - 1);
    }

    // SD 卡
    auto& sd = SdCardManager::GetInstance();
    if (sd.IsMounted()) {
        out->sd_mounted = 1;
        FATFS* fs = nullptr;
        DWORD free_clusters = 0;
        if (f_getfree("0:", &free_clusters, &fs) == FR_OK && fs != nullptr) {
            constexpr DWORD kSectorSize = 512;
            out->sd_total_bytes = (uint64_t)(fs->n_fatent - 2) * fs->csize * kSectorSize;
            out->sd_free_bytes = (uint64_t)free_clusters * fs->csize * kSectorSize;
        }
    }

    // NVS
    nvs_stats_t stats = {};
    if (nvs_get_stats(nullptr, &stats) == ESP_OK) {
        out->nvs_used = stats.used_entries;
        out->nvs_total = stats.total_entries;
        out->nvs_ns_count = stats.namespace_count;
    }

    // 系统硬件与运行时间
    strncpy(out->chip_model, SystemInfo::GetChipModelName().c_str(), sizeof(out->chip_model) - 1);
    strncpy(out->idf_ver, esp_get_idf_version(), sizeof(out->idf_ver) - 1);
    strncpy(out->mac, SystemInfo::GetMacAddress().c_str(), sizeof(out->mac) - 1);
    out->uptime_seconds = esp_timer_get_time() / 1000000;

    return 1;
}

#include "services/calendar/calendar_ics.h"
#include "services/calendar/calendar_store.h"
#include "services/glm_usage/glm_usage_service.h"
#include "services/backup/nvs_backup_service.h"
#include "services/wallpaper/wallpaper_active.h"
#include "services/wifi_file/wifi_file_service.h"
#include "services/wifi_transfer/wifi_transfer_service.h"
#include "wifi_station.h"

// 日历日程服务
int dynapp_cal_today(char* out_buf, size_t max_len) {
    std::string today = Calendar_Today();
    if (out_buf != nullptr && max_len > 0) {
        strncpy(out_buf, today.c_str(), max_len - 1);
        out_buf[max_len - 1] = '\0';
    }
    return today.empty() ? 0 : 1;
}

int dynapp_cal_has_event(const char* date) {
    if (date == nullptr) return 0;
    CalendarStore_EnsureLoaded();
    return Calendar_HasEvent(date) ? 1 : 0;
}

int dynapp_cal_get_events_text(const char* date, char* out_buf, size_t max_len) {
    if (date == nullptr || out_buf == nullptr || max_len == 0) return 0;
    CalendarStore_EnsureLoaded();
    auto events = Calendar_GetForDate(date);
    std::string text = std::string(date) + " 日程(" + std::to_string(events.size()) + ")";
    if (events.empty()) {
        text += "\n  无";
    } else {
        for (const auto& e : events) {
            text += "\n  " + (e.time.empty() ? std::string("全天") : e.time) + " " + e.title;
        }
    }
    strncpy(out_buf, text.c_str(), max_len - 1);
    out_buf[max_len - 1] = '\0';
    return static_cast<int>(events.size());
}

void dynapp_cal_auto_refresh(void) {
    CalendarStore_EnsureLoaded();
    CalendarIcs_MaybeAutoRefresh();
}

// GLM 用量服务
int dynapp_glm_get_snapshot(dynapp_glm_snapshot_t* out) {
    if (out == nullptr) return 0;
    memset(out, 0, sizeof(*out));
    auto& svc = GlmUsageService::GetInstance();
    auto st = svc.GetStatus();
    out->configured = st.configured ? 1 : 0;
    out->busy = st.busy ? 1 : 0;
    out->discovering = st.discovering ? 1 : 0;
    out->last_ok = st.last_ok ? 1 : 0;
    out->active_context = static_cast<int>(st.active_context);
    out->context_count = static_cast<int>(st.context_count);
    strncpy(out->error, st.error.c_str(), sizeof(out->error) - 1);

    auto contexts = svc.GetContexts();
    if (st.active_context < contexts.size()) {
        const auto& ctx = contexts[st.active_context];
        strncpy(out->context_name, ctx.name.c_str(), sizeof(out->context_name) - 1);
        if (ctx.has_usage) {
            strncpy(out->level, ctx.usage.level.c_str(), sizeof(out->level) - 1);
            out->tokens_5h_pct = ctx.usage.tokens_5h_used_pct;
            out->tokens_5h_remaining = ctx.usage.tokens_5h_remaining;
            out->tokens_week_pct = ctx.usage.tokens_week_used_pct;
            out->tokens_week_remaining = ctx.usage.tokens_week_remaining;
            out->five_hour_resets_left = ctx.usage.five_hour_resets_left;
            out->week_resets_left = ctx.usage.week_resets_left;
        }
    }
    return 1;
}

int dynapp_glm_get_context_name(int index, char* out_name, size_t max_len) {
    if (out_name == nullptr || max_len == 0 || index < 0) return 0;
    auto contexts = GlmUsageService::GetInstance().GetContexts();
    if (static_cast<size_t>(index) < contexts.size()) {
        strncpy(out_name, contexts[index].name.c_str(), max_len - 1);
        out_name[max_len - 1] = '\0';
        return 1;
    }
    return 0;
}

void dynapp_glm_configure(const char* api_key, const char* jwt_token) {
    if (api_key == nullptr) return;
    GlmUsageService::GetInstance().Configure(api_key, jwt_token != nullptr ? jwt_token : "");
}

void dynapp_glm_request_refresh(void) {
    GlmUsageService::GetInstance().RequestRefresh();
}

void dynapp_glm_select_context(int index) {
    GlmUsageService::GetInstance().SelectContext(static_cast<size_t>(index));
}

void dynapp_glm_start(void) {
    GlmUsageService::GetInstance().Start();
}

void dynapp_glm_stop(void) {
    GlmUsageService::GetInstance().Stop();
}

// 备份恢复服务
int dynapp_backup_export(char* out_filename, size_t max_len) {
    auto res = backup_restore::NvsBackupService::GetInstance().ExportToSd();
    if (res.success && out_filename != nullptr && max_len > 0) {
        strncpy(out_filename, res.filename.c_str(), max_len - 1);
        out_filename[max_len - 1] = '\0';
    }
    return res.success ? 1 : 0;
}

int dynapp_backup_import(const char* filepath) {
    if (filepath == nullptr) return 0;
    auto res = backup_restore::NvsBackupService::GetInstance().ImportFromSd(filepath);
    return res.success ? 1 : 0;
}

int dynapp_backup_delete(const char* filepath) {
    if (filepath == nullptr) return 0;
    return backup_restore::NvsBackupService::GetInstance().DeleteBackup(filepath) ? 1 : 0;
}

int dynapp_backup_get_nvs_count(void) {
    return backup_restore::NvsBackupService::GetInstance().GetLocalNvsEntryCount();
}

// 壁纸设置服务
int dynapp_wallpaper_get_standby(char* out_name, size_t max_len) {
    std::string s = wallpaper::GetStandbyFilename();
    if (out_name != nullptr && max_len > 0) {
        strncpy(out_name, s.c_str(), max_len - 1);
        out_name[max_len - 1] = '\0';
    }
    return s.empty() ? 0 : 1;
}

int dynapp_wallpaper_set_standby(const char* path_or_name) {
    if (path_or_name == nullptr) return 0;
    std::string err;
    return wallpaper::SetStandbyFromFile(path_or_name, err) ? 1 : 0;
}

int dynapp_wallpaper_clear_standby(void) {
    wallpaper::ClearStandbyWallpaper();
    return 1;
}

// WiFi 局域网服务
int dynapp_get_ip_address(char* out_ip, size_t max_len) {
    std::string ip = WifiStation::GetInstance().GetIpAddress();
    if (out_ip != nullptr && max_len > 0) {
        strncpy(out_ip, ip.c_str(), max_len - 1);
        out_ip[max_len - 1] = '\0';
    }
    return ip.empty() ? 0 : 1;
}

int dynapp_wifi_file_start(void) {
    return wifi_file::WifiFileService::GetInstance().Start(8080) ? 1 : 0;
}

void dynapp_wifi_file_stop(void) {
    wifi_file::WifiFileService::GetInstance().Stop();
}

int dynapp_wifi_file_is_running(void) {
    return wifi_file::WifiFileService::GetInstance().IsRunning() ? 1 : 0;
}

int dynapp_wifi_file_get_port(void) {
    return static_cast<int>(wifi_file::WifiFileService::GetInstance().GetPort());
}

int dynapp_wifi_transfer_start(void) {
    return wifi_transfer::WifiTransferService::GetInstance().Start(8080) ? 1 : 0;
}

void dynapp_wifi_transfer_stop(void) {
    wifi_transfer::WifiTransferService::GetInstance().Stop();
}

int dynapp_wifi_transfer_is_running(void) {
    return wifi_transfer::WifiTransferService::GetInstance().IsRunning() ? 1 : 0;
}

int dynapp_wifi_transfer_get_port(void) {
    return static_cast<int>(wifi_transfer::WifiTransferService::GetInstance().GetPort());
}



