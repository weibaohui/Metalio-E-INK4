// main/services/dynamic_apps/dynamic_app_shims.h
// 动态应用 C ABI 垫片：把 C++ 链接的固件设施（屏骨架/虚拟键/异步投递/返回桌面/
// 全刷）包成 extern "C" 函数，经符号表导出给 .so 应用。
// 只做参数转发，不放业务逻辑。
#pragma once

#include <stdint.h>

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

#ifdef __cplusplus
}

// 固件内部调用：设置/清除当前活跃应用的 Bundle 路径
void dynapp_set_active_bundle(const char* bundle_path);
void dynapp_clear_active_bundle(void);
#endif

