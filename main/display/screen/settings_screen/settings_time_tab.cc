#include "settings_time_tab.h"

#include <cstdio>
#include <ctime>
#include <string>

#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "services/time_service.h"
#include "settings_common.h"

namespace {

constexpr lv_coord_t kStepBtnW = 64;
constexpr lv_coord_t kRowH = 56;

struct TimeUi {
    lv_obj_t* now_lbl = nullptr;      // 当前时间 + 对时状态
    lv_obj_t* tz_val = nullptr;       // 时区偏移显示
    lv_obj_t* man_val = nullptr;      // 手动校准值显示
};
TimeUi s_ui;
lv_timer_t* s_timer = nullptr;

// 手动校准的暂存字段(进页时用当前时间初始化)
int s_year, s_mon, s_day, s_hour, s_min;

const lv_font_t* UiFont() { return fontpack_lv_font_ui(); }

lv_obj_t* MakeText(lv_obj_t* parent, const char* text, int font_size = 25) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text != nullptr ? text : "");
    const lv_font_t* font = font_size > 25 ? fontpack_lv_font_get(font_size, 2) : UiFont();
    lv_obj_set_style_text_font(lbl, font != nullptr ? font : UiFont(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, lv_pct(100));
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return lbl;
}

lv_obj_t* MakeSmallBtn(lv_obj_t* parent, const char* text, lv_event_cb_t cb, intptr_t ud) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, kStepBtnW, kRowH - 12);
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, reinterpret_cast<void*>(ud));
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, UiFont(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

// 一行:标题  [-]  值  [+]
lv_obj_t* MakeStepperRow(lv_obj_t* parent, const char* title, lv_event_cb_t cb,
                         intptr_t ud_minus, intptr_t ud_plus, lv_obj_t** val_out) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), kRowH);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* t = lv_label_create(row);
    lv_label_set_text(t, title);
    lv_obj_set_style_text_font(t, UiFont(), 0);
    lv_obj_set_style_text_color(t, lv_color_black(), 0);
    lv_obj_set_flex_grow(t, 1);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_CLICKABLE);

    MakeSmallBtn(row, "-", cb, ud_minus);
    lv_obj_t* val = lv_label_create(row);
    lv_obj_set_style_text_font(val, UiFont(), 0);
    lv_obj_set_style_text_color(val, lv_color_black(), 0);
    lv_obj_set_style_text_align(val, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(val, 90);
    lv_obj_clear_flag(val, LV_OBJ_FLAG_CLICKABLE);
    *val_out = val;
    MakeSmallBtn(row, "+", cb, ud_plus);
    return row;
}

std::string TzText(int minutes) {
    const char sign = minutes < 0 ? '-' : '+';
    int a = minutes < 0 ? -minutes : minutes;
    char buf[24];
    snprintf(buf, sizeof(buf), "UTC%c%d:%02d", sign, a / 60, a % 60);
    return buf;
}

void RefreshNow() {
    if (s_ui.now_lbl == nullptr) return;
    const bool synced = TimeService_IsSynced();
    std::string s = "当前时间 " + TimeService_NowText();
    s += synced ? " · 已对时" : " · 未对时";
    lv_label_set_text(s_ui.now_lbl, s.c_str());
}

void RefreshTz() {
    if (s_ui.tz_val != nullptr) {
        lv_label_set_text(s_ui.tz_val, TzText(TimeService_GetTzOffsetMin()).c_str());
    }
}

void RefreshManual() {
    if (s_ui.man_val != nullptr) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%02d:%02d", s_hour, s_min);
        lv_label_set_text(s_ui.man_val, buf);
    }
}

void OnTimer(lv_timer_t*) { RefreshNow(); }

// 时区步进:±30 分钟,范围 UTC-12 ~ UTC+14
void OnTzStep(lv_event_t* e) {
    const int delta = (reinterpret_cast<intptr_t>(lv_event_get_user_data(e)) != 0) ? 30 : -30;
    HapticPulseIfEnabled();
    TimeService_SetTzOffsetMin(TimeService_GetTzOffsetMin() + delta);
    RefreshTz();
}

void OnSyncNowClicked(lv_event_t*) {
    HapticPulseIfEnabled();
    TimeService_SyncNow();
    RefreshNow();
}

void OnManualStep(lv_event_t* e) {
    const int delta = (reinterpret_cast<intptr_t>(lv_event_get_user_data(e)) != 0) ? 1 : -1;
    // 简化:只调"分",60 进位到时
    HapticPulseIfEnabled();
    s_min += delta;
    while (s_min >= 60) { s_min -= 60; s_hour = (s_hour + 1) % 24; }
    while (s_min < 0) { s_min += 60; s_hour = (s_hour + 23) % 24; }
    RefreshManual();
}

void OnManualSaveClicked(lv_event_t*) {
    HapticPulseIfEnabled();
    if (TimeService_SetManual(s_year, s_mon, s_day, s_hour, s_min)) {
        RefreshNow();
    }
}

}  // namespace

void SettingsTimeTab_Reset() {
    if (s_timer != nullptr) {
        lv_timer_delete(s_timer);
        s_timer = nullptr;
    }
    s_ui = {};
}

void SettingsTimeTab_Build(lv_obj_t* page) {
    s_ui = {};

    MakeText(page, "时间 / 对时", 30);
    s_ui.now_lbl = MakeText(page, "当前时间 --");
    MakeText(page, "联网后自动对时(SNTP);时区偏移用于把 UTC 换算成本地时间。");

    // 时区选择
    MakeStepperRow(page, "时区", OnTzStep, 0, 1, &s_ui.tz_val);

    // 立即对时
    lv_obj_t* sync_btn = SettingsCreateSelectableOption(page, "立即对时", OnSyncNowClicked, 0);
    (void)sync_btn;

    // 手动校准(时:分),用当前时间初始化
    {
        time_t now = time(nullptr);
        struct tm local = {};
        localtime_r(&now, &local);
        if (local.tm_year + 1900 >= 2025) {
            s_year = local.tm_year + 1900; s_mon = local.tm_mon + 1; s_day = local.tm_mday;
            s_hour = local.tm_hour; s_min = local.tm_min;
        } else {
            s_year = 2026; s_mon = 1; s_day = 1; s_hour = 12; s_min = 0;
        }
    }
    MakeText(page, "手动校准(时:分)", 25);
    MakeStepperRow(page, "校准", OnManualStep, 0, 1, &s_ui.man_val);
    SettingsCreateSelectableOption(page, "保存校准时间", OnManualSaveClicked, 0);

    RefreshTz();
    RefreshManual();
    RefreshNow();
    s_timer = lv_timer_create(OnTimer, 1000, nullptr);
}
