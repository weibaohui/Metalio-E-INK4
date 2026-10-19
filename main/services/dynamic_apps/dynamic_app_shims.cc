// main/services/dynamic_apps/dynamic_app_shims.cc
// 动态应用 C ABI 垫片实现：转发到 C++ 固件设施（见 dynamic_app_shims.h）。
#include "dynamic_app_shims.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <esp_log.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "display/lv_adapter_display.h"
#include "display/screen/screen_common.h"
#include "display/screen/vk_key_handler.h"
#include "reader/image_util.h"
#include "sd_paths.h"

// PSRAM 栈任务(含 LVGL worker,动态应用 create/按键回调都在其中)禁止任何
// NVS/flash 访问:关 cache 窗口内跑 PSRAM 栈任务触发
// spi_flash_disable_interrupts_caches_and_other_cpu 的
// esp_task_stack_is_sane_cache_disabled() 断言崩机(实测 backup 统计入口)。
// backup 系 shim 是同步语义:起内部栈临时任务执行,阻塞等结果。
// SD 卡 I/O 走 SDMMC 控制器不关 cache,无需此包装。
namespace {
struct FlashSyncJob {
    void (*fn)(void*);
    void* arg;
    SemaphoreHandle_t done;
};

void FlashSyncTrampoline(void* p) {
    auto* job = static_cast<FlashSyncJob*>(p);
    job->fn(job->arg);
    xSemaphoreGive(job->done);
    vTaskDelete(nullptr);
}

bool RunOnInternalStack(void (*fn)(void*), void* arg) {
    FlashSyncJob job{fn, arg, xSemaphoreCreateBinary()};
    if (job.done == nullptr) {
        return false;
    }
    // 栈 12KB 内部 RAM:覆盖 NVS 遍历/快照 JSON/文件拷贝;失败即按 shim 失败
    // 返回,由应用界面提示。
    if (xTaskCreate(FlashSyncTrampoline, "dynapp_flash", 12288, &job,
                    tskIDLE_PRIORITY + 3, nullptr) != pdPASS) {
        vSemaphoreDelete(job.done);
        return false;
    }
    // 整机快照含应用包克隆,慢 SD 上可能数分钟,等待上限放宽到 3 分钟。
    xSemaphoreTake(job.done, pdMS_TO_TICKS(180000));
    vSemaphoreDelete(job.done);
    return true;
}
}  // namespace

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
#include "audio_codec.h"
#include "boot_key_handler.h"
#include "display/modal_manager.h"
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
    xTaskCreate([](void*) {
        vTaskDelay(pdMS_TO_TICKS(300));
        Application::GetInstance().Reboot();
        vTaskDelete(nullptr);
    }, "reboot_task", 2048, nullptr, 5, nullptr);
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
    ESP_LOGI("CalShim", "get_events_text %s task=%s", date, pcTaskGetName(nullptr));
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
    Application::GetInstance().Schedule([]() {
        GlmUsageService::GetInstance().Stop();
    });
}

// 备份恢复服务
int dynapp_backup_is_sd_mounted(void) {
    return backup_restore::NvsBackupService::GetInstance().IsSdMounted() ? 1 : 0;
}

int dynapp_backup_get_sd_capacity(char* out_free, size_t free_len, char* out_total, size_t total_len) {
    std::string f, t;
    if (backup_restore::NvsBackupService::GetInstance().GetSdCapacityText(f, t)) {
        if (out_free && free_len > 0) { strncpy(out_free, f.c_str(), free_len - 1); out_free[free_len - 1] = '\0'; }
        if (out_total && total_len > 0) { strncpy(out_total, t.c_str(), total_len - 1); out_total[total_len - 1] = '\0'; }
        return 1;
    }
    return 0;
}

namespace {

struct GetStatsArgs {
    int s = 0, a = 0, i = 0;
    bool ok = false;
};

void GetStatsJob(void* p) {
    auto* arg = static_cast<GetStatsArgs*>(p);
    arg->ok = backup_restore::NvsBackupService::GetInstance().GetStats(arg->s, arg->a, arg->i);
}

struct ExportArgs {
    int backup_type = 0;
    backup_restore::ExportResult res;
};

void ExportJob(void* p) {
    auto* arg = static_cast<ExportArgs*>(p);
    arg->res = backup_restore::NvsBackupService::GetInstance().ExportToSd(arg->backup_type);
}

struct ImportArgs {
    std::string snapshot_path;
    int restore_mask = 0;
    backup_restore::ImportResult res;
};

void ImportJob(void* p) {
    auto* arg = static_cast<ImportArgs*>(p);
    arg->res = backup_restore::NvsBackupService::GetInstance().ImportFromSd(arg->snapshot_path, arg->restore_mask);
}

}  // namespace

int dynapp_backup_get_stats(int* out_system_nvs, int* out_app_nvs, int* out_installed_apps) {
    GetStatsArgs args;
    // NVS 遍历要走 flash 读,必须离开 PSRAM 栈任务(见 RunOnInternalStack)。
    if (!RunOnInternalStack(GetStatsJob, &args)) {
        ESP_LOGE("DynAppShim", "backup_get_stats: spawn internal-stack worker failed");
        return 0;
    }
    if (args.ok) {
        if (out_system_nvs) *out_system_nvs = args.s;
        if (out_app_nvs) *out_app_nvs = args.a;
        if (out_installed_apps) *out_installed_apps = args.i;
        return 1;
    }
    return 0;
}

static std::vector<backup_restore::BackupFileInfo> s_backup_cache;

int dynapp_backup_list_count(void) {
    s_backup_cache = backup_restore::NvsBackupService::GetInstance().ListBackups();
    return static_cast<int>(s_backup_cache.size());
}

int dynapp_backup_get_item(int index, dynapp_backup_info_t* out) {
    if (out == nullptr || index < 0 || static_cast<size_t>(index) >= s_backup_cache.size()) return 0;
    const auto& b = s_backup_cache[index];
    memset(out, 0, sizeof(*out));
    strncpy(out->name, b.filename.c_str(), sizeof(out->name) - 1);
    strncpy(out->full_path, b.full_path.c_str(), sizeof(out->full_path) - 1);
    strncpy(out->created_at, b.created_at.c_str(), sizeof(out->created_at) - 1);
    out->backup_type = b.backup_type;
    out->has_system = b.has_system ? 1 : 0;
    out->has_app_data = b.has_app_data ? 1 : 0;
    out->has_app_bundles = b.has_app_bundles ? 1 : 0;
    out->system_nvs_count = b.system_nvs_count;
    out->app_nvs_count = b.app_nvs_count;
    out->app_data_count = b.app_data_count;
    out->app_bundle_count = b.app_bundle_count;
    out->size_bytes = b.size_bytes;
    out->mod_time = static_cast<int64_t>(b.mod_time);
    return 1;
}

int dynapp_backup_export(int backup_type, char* out_name, size_t max_len) {
    ExportArgs args;
    args.backup_type = backup_type;
    if (!RunOnInternalStack(ExportJob, &args)) {
        ESP_LOGE("DynAppShim", "backup_export: spawn internal-stack worker failed");
        return 0;
    }
    if (args.res.success && out_name != nullptr && max_len > 0) {
        strncpy(out_name, args.res.filename.c_str(), max_len - 1);
        out_name[max_len - 1] = '\0';
    }
    return args.res.success ? 1 : 0;
}

int dynapp_backup_import(const char* snapshot_path, int restore_mask) {
    if (snapshot_path == nullptr) return 0;
    ImportArgs args;
    args.snapshot_path = snapshot_path;
    args.restore_mask = restore_mask;
    if (!RunOnInternalStack(ImportJob, &args)) {
        ESP_LOGE("DynAppShim", "backup_import: spawn internal-stack worker failed");
        return 0;
    }
    return args.res.success ? 1 : 0;
}

int dynapp_backup_delete(const char* snapshot_path) {
    if (snapshot_path == nullptr) return 0;
    return backup_restore::NvsBackupService::GetInstance().DeleteBackup(snapshot_path) ? 1 : 0;
}

// 壁纸设置服务
#include <dirent.h>
#include <sys/stat.h>

struct WallpaperFileItem {
    std::string name;
    std::string path;
    size_t size_bytes = 0;
};
static std::vector<WallpaperFileItem> s_wallpaper_cache;

int dynapp_wallpaper_list_count(void) {
    s_wallpaper_cache.clear();
    const char* dir_path = "/sdcard/metalio/e-ink/wallpaper";
    DIR* dir = opendir(dir_path);
    if (!dir) return 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        const char* dot = strrchr(entry->d_name, '.');
        if (!dot) continue;
        if (strcasecmp(dot, ".png") == 0 || strcasecmp(dot, ".jpg") == 0 ||
            strcasecmp(dot, ".jpeg") == 0 || strcasecmp(dot, ".a2i1") == 0 ||
            strcasecmp(dot, ".bin") == 0) {
            WallpaperFileItem item;
            item.name = entry->d_name;
            item.path = std::string(dir_path) + "/" + entry->d_name;
            struct stat st;
            if (stat(item.path.c_str(), &st) == 0) {
                item.size_bytes = st.st_size;
            }
            s_wallpaper_cache.push_back(item);
        }
    }
    closedir(dir);
    return static_cast<int>(s_wallpaper_cache.size());
}

int dynapp_wallpaper_get_item(int index, dynapp_wallpaper_info_t* out) {
    if (out == nullptr || index < 0 || static_cast<size_t>(index) >= s_wallpaper_cache.size()) return 0;
    const auto& w = s_wallpaper_cache[index];
    strncpy(out->name, w.name.c_str(), sizeof(out->name) - 1);
    out->name[sizeof(out->name) - 1] = '\0';
    strncpy(out->path, w.path.c_str(), sizeof(out->path) - 1);
    out->path[sizeof(out->path) - 1] = '\0';
    out->size_bytes = w.size_bytes;
    return 1;
}

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

int dynapp_image_decode_l8(const char* path, int max_w, int max_h,
                           uint8_t* out_pixels, size_t cap,
                           int* out_w, int* out_h) {
    if (path == nullptr || out_pixels == nullptr || out_w == nullptr || out_h == nullptr ||
        max_w <= 0 || max_h <= 0) {
        return 0;
    }
    reader::RasterImage img;
    if (!reader::DecodeImageFileToL8(path, max_w, max_h, img) || img.empty()) {
        return 0;
    }
    const size_t need = static_cast<size_t>(img.width) * static_cast<size_t>(img.height);
    if (need > cap) return 0;
    memcpy(out_pixels, img.pixels.data(), need);
    *out_w = img.width;
    *out_h = img.height;
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
    return wifi_file::WifiFileService::GetInstance().Start(80) ? 1 : 0;
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

int dynapp_wifi_file_get_url(char* out_url, size_t max_len) {
    std::string url = wifi_file::WifiFileService::GetInstance().GetServerUrl();
    if (out_url != nullptr && max_len > 0) {
        strncpy(out_url, url.c_str(), max_len - 1);
        out_url[max_len - 1] = '\0';
    }
    return url.empty() ? 0 : 1;
}

int dynapp_wifi_transfer_start(void) {
    return wifi_transfer::WifiTransferService::GetInstance().Start(80) ? 1 : 0;
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

int dynapp_wifi_transfer_get_url(char* out_url, size_t max_len) {
    std::string url = wifi_transfer::WifiTransferService::GetInstance().GetServerUrl();
    if (out_url != nullptr && max_len > 0) {
        strncpy(out_url, url.c_str(), max_len - 1);
        out_url[max_len - 1] = '\0';
    }
    return url.empty() ? 0 : 1;
}

static dynapp_upload_cb_t s_user_upload_cb = nullptr;
static void* s_user_upload_ud = nullptr;

void dynapp_wifi_transfer_set_callback(dynapp_upload_cb_t cb, void* user_data) {
    s_user_upload_cb = cb;
    s_user_upload_ud = user_data;
    if (cb != nullptr) {
        wifi_transfer::WifiTransferService::GetInstance().SetUploadCallback([](const wifi_transfer::UploadEvent& ev) {
            if (s_user_upload_cb != nullptr) {
                struct EvMsg {
                    std::string fn;
                    size_t sz;
                };
                auto* msg = new EvMsg{ev.filename, ev.size_bytes};
                ScreenLvAsync([](void* p) {
                    auto* m = static_cast<EvMsg*>(p);
                    if (s_user_upload_cb != nullptr) {
                        s_user_upload_cb(m->fn.c_str(), m->sz, s_user_upload_ud);
                    }
                    delete m;
                }, msg);
            }
        });
    } else {
        wifi_transfer::WifiTransferService::GetInstance().SetUploadCallback(nullptr);
    }
}

void dynapp_modal_alert(const char* title, const char* message, const char* ok_text) {
    ModalManager::GetInstance().ShowAlert(title ? title : "提示", message ? message : "", ok_text ? ok_text : "我知道了");
}

void dynapp_modal_dialog(const char* title, const char* message, const char* ok_text, const char* cancel_text,
                         void (*cb)(int confirmed, void* user_data), void* user_data) {
    ModalManager::GetInstance().ShowDialog(
        title ? title : "确认",
        message ? message : "",
        ok_text ? ok_text : "确定",
        cancel_text ? cancel_text : "取消",
        [cb, user_data](bool confirmed) {
            if (cb) {
                cb(confirmed ? 1 : 0, user_data);
            }
        });
}

int dynapp_radio_get_volume(void) {
    auto* codec = Board::GetInstance().GetAudioCodec();
    return codec != nullptr ? codec->output_volume() : 70;
}

void dynapp_radio_set_volume(int volume) {
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec != nullptr) {
        if (volume < 0) volume = 0;
        if (volume > 100) volume = 100;
        codec->SetOutputVolume(volume);
    }
}



