// main/display/screen/time_sync_screen/time_sync_screen.cc —— 见 time_sync_screen.h。
#include "time_sync_screen/time_sync_screen.h"

#include <cstdio>
#include <ctime>
#include <string>

#include <esp_log.h>

#include "board.h"
#include "display/screen/epd_widget_kit.h"
#include "display/status_bar_manager.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "screen_common.h"
#include "services/time_service.h"
#include "vk_key_handler.h"

namespace {

constexpr const char* TAG = "TimeSyncScreen";
constexpr const char* kScreenId = "time_sync";

struct TsUi {
    lv_obj_t* screen = nullptr;
    lv_obj_t* clock_lbl = nullptr;   // 大字 HH:MM:SS
    lv_obj_t* date_lbl = nullptr;    // 日期 + 星期
    lv_obj_t* status_lbl = nullptr;  // 对时状态
    lv_obj_t* tz_lbl = nullptr;      // 时区显示
    lv_obj_t* man_lbl = nullptr;     // 手动校准值
    lv_obj_t* footer_lbl = nullptr;
};
TsUi s_ui;
bool s_alive = false;
lv_timer_t* s_timer = nullptr;
int s_hour = 12, s_min = 0, s_year = 2026, s_mon = 1, s_day = 1;  // 手动校准暂存

void DisableScroll(lv_obj_t* o) { lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE); }

lv_obj_t* MakeRow(lv_obj_t* parent, lv_coord_t h) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), h);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    DisableScroll(row);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    return row;
}

std::string TzText(int minutes) {
    const char sign = minutes < 0 ? '-' : '+';
    const int a = minutes < 0 ? -minutes : minutes;
    char b[24]; snprintf(b, sizeof(b), "UTC%c%d:%02d", sign, a/60, a%60); return b;
}

void RefreshClock() {
    if (!s_alive || s_ui.clock_lbl == nullptr) return;
    time_t now = time(nullptr);
    struct tm local = {};
    localtime_r(&now, &local);
    const bool valid = (local.tm_year + 1900) >= 2025;
    if (valid) {
        char tb[16];
        snprintf(tb, sizeof(tb), "%02d:%02d:%02d", local.tm_hour, local.tm_min, local.tm_sec);
        lv_label_set_text(s_ui.clock_lbl, tb);
        char db[40];
        snprintf(db, sizeof(db), "%04d-%02d-%02d", local.tm_year+1900, local.tm_mon+1, local.tm_mday);
        lv_label_set_text(s_ui.date_lbl, db);
    } else {
        lv_label_set_text(s_ui.clock_lbl, "--:--:--");
        lv_label_set_text(s_ui.date_lbl, "未对时");
    }
    if (s_ui.status_lbl != nullptr) {
        const bool net = Board::GetInstance().IsNetworkReady();
        const bool synced = TimeService_IsSynced();
        std::string s;
        if (synced) s = "已对时 · " + std::string(TimeService_NowText());
        else if (net) s = "对时中…(SNTP)";
        else s = "未联网,无法对时";
        lv_label_set_text(s_ui.status_lbl, s.c_str());
    }
}

void RefreshTz() {
    if (s_ui.tz_lbl != nullptr) {
        lv_label_set_text(s_ui.tz_lbl, TzText(TimeService_GetTzOffsetMin()).c_str());
    }
}
void RefreshManual() {
    if (s_ui.man_lbl != nullptr) {
        char b[12]; snprintf(b, sizeof(b), "%02d:%02d", s_hour, s_min);
        lv_label_set_text(s_ui.man_lbl, b);
    }
}

void OnTimer(lv_timer_t*) { RefreshClock(); }

void OnSyncNow(lv_event_t*) {
    HapticPulseIfEnabled();
    TimeService_SyncNow();
    if (s_ui.status_lbl != nullptr) lv_label_set_text(s_ui.status_lbl, "对时中…(SNTP)");
}

void OnTzStep(lv_event_t* e) {
    const int d = (reinterpret_cast<intptr_t>(lv_event_get_user_data(e)) != 0) ? 30 : -30;
    HapticPulseIfEnabled();
    TimeService_SetTzOffsetMin(TimeService_GetTzOffsetMin() + d);
    RefreshTz();
}

void OnManualStep(lv_event_t* e) {
    const int d = (reinterpret_cast<intptr_t>(lv_event_get_user_data(e)) != 0) ? 1 : -1;
    HapticPulseIfEnabled();
    s_min += d;
    while (s_min >= 60) { s_min -= 60; s_hour = (s_hour + 1) % 24; }
    while (s_min < 0) { s_min += 60; s_hour = (s_hour + 23) % 24; }
    RefreshManual();
}

void OnManualSave(lv_event_t*) {
    HapticPulseIfEnabled();
    if (TimeService_SetManual(s_year, s_mon, s_day, s_hour, s_min)) {
        RefreshClock();
        StatusBarManager::GetInstance().ShowToast("校准时间已生效", 2000);
    }
}

void OnScreenUnloaded(lv_event_t*) {
    s_alive = false;
    if (s_timer != nullptr) { lv_timer_delete(s_timer); s_timer = nullptr; }
    s_ui = {};
}

}  // namespace

lv_obj_t* TimeSyncScreen::Create() {
    s_ui = {};
    s_alive = true;
    ScreenSetIsHome(false);

    auto* scaffold = EpdWidgetKit::EpdPageScaffold::Create({
        .title = "时间与对时",
        .show_status_bar = true,
        .show_back_btn = true,
        .pad_h = 16,
        .pad_v = 12,
        .row_gap = 10,
    });

    s_ui.screen = scaffold->GetScreen();
    lv_obj_t* body = scaffold->GetBody();
    lv_obj_add_event_cb(s_ui.screen, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);

    // 大字时钟
    s_ui.clock_lbl = ScreenMakeLabel(body, "--:--:--", 64, false);
    lv_obj_set_width(s_ui.clock_lbl, lv_pct(100));
    lv_obj_set_style_text_align(s_ui.clock_lbl, LV_TEXT_ALIGN_CENTER, 0);
    // 日期
    s_ui.date_lbl = ScreenMakeLabel(body, "未对时", 28, false);
    lv_obj_set_width(s_ui.date_lbl, lv_pct(100));
    lv_obj_set_style_text_align(s_ui.date_lbl, LV_TEXT_ALIGN_CENTER, 0);
    // 对时状态
    s_ui.status_lbl = ScreenMakeLabel(body, "对时中…(SNTP)", 25, false);
    lv_obj_set_width(s_ui.status_lbl, lv_pct(100));
    lv_obj_set_style_text_align(s_ui.status_lbl, LV_TEXT_ALIGN_CENTER, 0);

    // 时区
    lv_obj_t* tzrow = MakeRow(body, 46);
    lv_obj_t* tztitle = ScreenMakeLabel(tzrow, "时区", 25, false);
    lv_obj_set_flex_grow(tztitle, 1);
    ScreenMakeBtn(tzrow, "-", 60, 46, OnTzStep, (void*)0);
    s_ui.tz_lbl = ScreenMakeLabel(tzrow, "UTC+8:00", 25, false);
    lv_obj_set_width(s_ui.tz_lbl, 110);
    lv_obj_set_style_text_align(s_ui.tz_lbl, LV_TEXT_ALIGN_CENTER, 0);
    ScreenMakeBtn(tzrow, "+", 60, 46, OnTzStep, (void*)1);

    // 手动校准(时:分)
    lv_obj_t* manrow = MakeRow(body, 46);
    lv_obj_t* mantitle = ScreenMakeLabel(manrow, "手动校准", 25, false);
    lv_obj_set_flex_grow(mantitle, 1);
    ScreenMakeBtn(manrow, "-", 60, 46, OnManualStep, (void*)0);
    s_ui.man_lbl = ScreenMakeLabel(manrow, "12:00", 25, false);
    lv_obj_set_width(s_ui.man_lbl, 80);
    lv_obj_set_style_text_align(s_ui.man_lbl, LV_TEXT_ALIGN_CENTER, 0);
    ScreenMakeBtn(manrow, "+", 60, 46, OnManualStep, (void*)1);

    // 操作行
    lv_obj_t* actrow = MakeRow(body, 46);
    ScreenMakeBtn(actrow, "立即对时", 150, 46, OnSyncNow, nullptr);
    ScreenMakeBtn(actrow, "保存校准", 150, 46, OnManualSave, nullptr);

    // 初始化手动校准暂存为当前时间
    {
        time_t now = time(nullptr);
        struct tm local = {};
        localtime_r(&now, &local);
        if (local.tm_year + 1900 >= 2025) {
            s_year = local.tm_year + 1900; s_mon = local.tm_mon + 1; s_day = local.tm_mday;
            s_hour = local.tm_hour; s_min = local.tm_min;
        }
    }

    // 进屏即触发一次 SNTP 对时(异步;状态行随 1s 定时器刷新)
    TimeService_SyncNow();

    RefreshTz();
    RefreshManual();
    RefreshClock();
    s_timer = lv_timer_create(OnTimer, 1000, nullptr);
    VkKey_AttachScreen(s_ui.screen, kScreenId, VkKeyScreenDesc{TimeSyncScreen::Create, nullptr});
    return s_ui.screen;
}
