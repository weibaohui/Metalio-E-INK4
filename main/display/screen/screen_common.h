#pragma once

#include "lvgl.h"

#include <atomic>

// 通用状态栏信息，包含网络、电量和通知文本。
struct EpdStatusBar {
    lv_obj_t* bar = nullptr;
    lv_obj_t* overlay = nullptr;
    lv_obj_t* network_label = nullptr;
    lv_obj_t* mute_label = nullptr;
    lv_obj_t* volume_label = nullptr;   // 喇叭图标后的音量数字(需求:音量数值提示)
    lv_obj_t* battery_pct_label = nullptr;
    lv_obj_t* battery_label = nullptr;
    lv_obj_t* status_label = nullptr;
    lv_obj_t* notification_label = nullptr;
    lv_obj_t* low_battery_popup = nullptr;
    lv_coord_t height = 0;
};

// 通用顶栏和占位页辅助接口。
EpdStatusBar ScreenCreateStatusBar(lv_obj_t* scr);
lv_obj_t* ScreenCreatePlaceholder(const char* screen_id, const char* title);
void ScreenLoadReplace(lv_obj_t* new_scr);
void ScreenNavigateTo(lv_obj_t* (*create)());
void ScreenNavigateBack();
void ScreenSetIsHome(bool is_home);
bool ScreenIsHome();
void ScreenGoHome();
void ScreenRequestHome();
void ScreenRequestBack();

// 非 LVGL 线程投递 UI 任务，避免在触摸/墨水刷屏链路上直接持锁。
bool ScreenLvAsync(void (*cb)(void*), void* user_data = nullptr);
bool ScreenLvAsyncUrgent(void (*cb)(void*), void* user_data = nullptr);

// 多次重绘请求合并，减少无效刷新。
struct ScreenPaintCoalesce {
    std::atomic<uint32_t> seq{0};
    std::atomic<uint32_t> done{0};
    std::atomic<bool> queued{false};
    void (*paint)() = nullptr;
    void* defer_timer = nullptr;
};

void ScreenPaintCoalesceRequest(ScreenPaintCoalesce* c);
void ScreenPaintCoalesceRequestDebounced(ScreenPaintCoalesce* c, uint64_t delay_us);
void ScreenPaintCoalesceReset(ScreenPaintCoalesce* c);

// 页面外层的点状装饰底纹。
void ScreenApplyDotBackdrop(lv_obj_t* obj);

// ---- 屏幕骨架共享件(公共功能沉淀,docs/code-review-optimization-2026-10.md §3.3) ----
// 各业务屏原先各自 static 重定义同形 MakeLabel/MakeBtn,统一收拢到这里。

// 黑字标签:UI 字体;font_size>25 时取大字体(字库缺该规格会回退 UI 字体,
// NULL 字体会让 lv_text 测量死循环,必须空守卫)。wrap=true:宽 100% + 长文折行。
lv_obj_t* ScreenMakeLabel(lv_obj_t* parent, const char* text, int font_size = 25, bool wrap = false);

// 白底黑框圆角按钮(带触觉反馈),cb 挂 LV_EVENT_CLICKED;返回内部 label 指针
// (可用于改字),不需要时传 nullptr。
lv_obj_t* ScreenMakeBtn(lv_obj_t* parent, const char* text, lv_coord_t w, lv_coord_t h,
                        lv_event_cb_t cb, void* user_data = nullptr, lv_obj_t** lbl_out = nullptr);

// 屏幕/模态状态感知接口（解耦板级按键及电源策略，避免硬件驱动层直接反向依赖具体 Screen）
bool ScreenIsCriticalModalActive();
bool ScreenIsReadingActive();
bool ScreenIsStandbyActive();

