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

#ifdef __cplusplus
}
#endif
