// main/services/dynamic_apps/dynamic_app_shims.h
// 动态应用 C ABI 垫片：把 C++ 链接的固件设施（屏骨架/虚拟键/异步投递/返回桌面/
// 全刷）包成 extern "C" 函数，经符号表导出给 .so 应用。
// 只做参数转发，不放业务逻辑。
#pragma once

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// 屏幕状态栏（电量/时钟），返回状态栏根对象（可 NULL）
void* dynapp_screen_status_bar(void* scr);

// 挂接页面虚拟键：factory=重建本屏的工厂（返回 lv_obj_t*），on_key=按键处理
// （esp_timer 上下文回调，内部只允许投递，不允许直接碰 LVGL）
void dynapp_vk_attach(void* scr, const char* name,
                      void* (*factory)(void), int (*on_key)(const char* key));

// 非 LVGL 线程投递 UI 任务（线程安全版异步）
int dynapp_lv_async(void (*cb)(void*), void* user_data);

// 返回桌面（内部自行处理线程投递）
void dynapp_screen_go_home(void);

// 请求下一次墨水屏全刷（清残影）
void dynapp_epd_full_refresh(void);

// 获取当前动态应用的 Bundle 根目录（例如 "/sdcard/metalio/e-ink/apps/fangfang.app"）
const char* dynapp_get_bundle_path(void);

// 获取当前动态应用的私有数据目录（例如 "/sdcard/metalio/e-ink/apps/fangfang.app/data"），
// 首次调用若目录不存在会自动在 SD 卡上创建。配置文件/存档必须写入此目录。
const char* dynapp_get_data_path(void);

// 安全打开当前应用 data/ 目录下的文件（如 "config.json"、"hiscore.txt"）。
// 严格防穿越（拒绝包含 ".."、绝对路径或反斜杠），返回 FILE*（fopen 同款用法）。
void* dynapp_fopen_data(const char* rel_filename, const char* mode);

// 沙箱路径合法性检查：
//   write=1: 仅允许写入当前应用的 data 目录内；
//   write=0: 允许读取应用自身 bundle、自身 data，以及公共字体/系统资源。
int dynapp_is_path_allowed(const char* path, int write);

// 电源管理与系统控制
void dynapp_power_sleep(void);
void dynapp_power_reboot(void);
void dynapp_power_shutdown(void);

// 网络状态与时间服务
int dynapp_is_network_ready(void);
int dynapp_time_is_synced(void);
long dynapp_time_last_sync_epoch(void);
int dynapp_time_get_tz_offset_min(void);
void dynapp_time_set_tz_offset_min(int minutes);
void dynapp_time_sync_now(void);
int dynapp_time_set_manual(int year, int month, int day, int hour, int minute);

// 提示小浮窗 (EpdToast)
void dynapp_toast_show(const char* message, uint32_t duration_ms);

// 系统信息查询结构
typedef struct {
    size_t ram_free;
    size_t ram_total;
    size_t ram_largest;
    size_t ram_min_free;
    size_t psram_free;
    size_t psram_total;
    size_t psram_largest;
    size_t psram_min_free;
    uint64_t flash_size;
    uint32_t app_part_size;
    char app_part_label[16];
    int sd_mounted;
    uint64_t sd_total_bytes;
    uint64_t sd_free_bytes;
    uint32_t nvs_used;
    uint32_t nvs_total;
    uint32_t nvs_ns_count;
    char chip_model[32];
    char idf_ver[32];
    char mac[20];
    int64_t uptime_seconds;
} dynapp_sysinfo_t;

int dynapp_sysinfo_get(dynapp_sysinfo_t* out);

// 日历日程服务
int dynapp_cal_today(char* out_buf, size_t max_len);
int dynapp_cal_has_event(const char* date);
int dynapp_cal_get_events_text(const char* date, char* out_buf, size_t max_len);
void dynapp_cal_auto_refresh(void);

// GLM 用量服务
typedef struct {
    int configured;
    int busy;
    int discovering;
    int last_ok;
    int active_context;
    int context_count;
    char context_name[64];
    char level[32];
    int tokens_5h_pct;
    int tokens_5h_remaining;
    int tokens_week_pct;
    int tokens_week_remaining;
    int five_hour_resets_left;
    int week_resets_left;
    char error[64];
} dynapp_glm_snapshot_t;

int dynapp_glm_get_snapshot(dynapp_glm_snapshot_t* out);
int dynapp_glm_get_context_name(int index, char* out_name, size_t max_len);
void dynapp_glm_request_refresh(void);
void dynapp_glm_select_context(int index);
void dynapp_glm_configure(const char* api_key, const char* jwt_token);
void dynapp_glm_start(void);
void dynapp_glm_stop(void);

// 备份恢复服务
typedef struct {
    char filename[64];
    char full_path[128];
    uint32_t size_bytes;
    int64_t mod_time;
    int entry_count;
} dynapp_backup_info_t;

int dynapp_backup_is_sd_mounted(void);
int dynapp_backup_get_sd_capacity(char* out_free, size_t free_len, char* out_total, size_t total_len);
int dynapp_backup_list_count(void);
int dynapp_backup_get_item(int index, dynapp_backup_info_t* out);
int dynapp_backup_export(char* out_filename, size_t max_len);
int dynapp_backup_import(const char* filepath);
int dynapp_backup_delete(const char* filepath);
int dynapp_backup_get_nvs_count(void);

// 壁纸设置服务
typedef struct {
    char name[64];
    char path[128];
    size_t size_bytes;
} dynapp_wallpaper_info_t;

int dynapp_wallpaper_list_count(void);
int dynapp_wallpaper_get_item(int index, dynapp_wallpaper_info_t* out);
int dynapp_wallpaper_get_standby(char* out_name, size_t max_len);
int dynapp_wallpaper_set_standby(const char* path_or_name);
int dynapp_wallpaper_clear_standby(void);

// WiFi 局域网服务
int dynapp_get_ip_address(char* out_ip, size_t max_len);

int dynapp_wifi_file_start(void);
void dynapp_wifi_file_stop(void);
int dynapp_wifi_file_is_running(void);
int dynapp_wifi_file_get_port(void);

int dynapp_wifi_transfer_start(void);
void dynapp_wifi_transfer_stop(void);
int dynapp_wifi_transfer_is_running(void);
int dynapp_wifi_transfer_get_port(void);

#ifdef __cplusplus
}

// 固件内部调用：设置/清除当前活跃应用的 Bundle 路径
void dynapp_set_active_bundle(const char* bundle_path);
void dynapp_clear_active_bundle(void);
#endif

