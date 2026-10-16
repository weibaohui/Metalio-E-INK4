// main/display/screen/time_sync_screen/time_sync_screen.cc —— 独立"时间与网络对时"应用(新设计)。
#include "time_sync_screen/time_sync_screen.h"

#include <cmath>
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
    // 左侧时钟与状态卡片
    lv_obj_t* clock_lbl = nullptr;
    lv_obj_t* date_lbl = nullptr;
    lv_obj_t* status_lbl = nullptr;
    lv_obj_t* last_sync_lbl = nullptr;
    lv_obj_t* rtc_lbl = nullptr;
    lv_obj_t* sntp_btn = nullptr;
    // 右上时区卡片
    lv_obj_t* tz_value_lbl = nullptr;
    // 右下手调卡片
    lv_obj_t* man_date_lbl = nullptr;
    lv_obj_t* man_time_lbl = nullptr;
};

TsUi s_ui;
bool s_alive = false;
lv_timer_t* s_timer = nullptr;
int s_year = 2026, s_mon = 1, s_day = 1, s_hour = 12, s_min = 0;

void AdjustDate(int delta_days, int delta_hours, int delta_mins) {
    struct tm t = {};
    t.tm_year = s_year - 1900;
    t.tm_mon = s_mon - 1;
    t.tm_mday = s_day + delta_days;
    t.tm_hour = s_hour + delta_hours;
    t.tm_min = s_min + delta_mins;
    time_t sec = mktime(&t);
    struct tm res = {};
    localtime_r(&sec, &res);
    s_year = res.tm_year + 1900;
    s_mon = res.tm_mon + 1;
    s_day = res.tm_mday;
    s_hour = res.tm_hour;
    s_min = res.tm_min;
}

std::string GetTzName(int minutes) {
    const char sign = minutes >= 0 ? '+' : '-';
    const int a = std::abs(minutes);
    const int h = a / 60;
    const int m = a % 60;
    char b[48];
    if (minutes == 480) {
        snprintf(b, sizeof(b), "UTC+8 (北京时间)");
    } else if (minutes == 540) {
        snprintf(b, sizeof(b), "UTC+9 (东京时间)");
    } else if (minutes == 0) {
        snprintf(b, sizeof(b), "UTC+0 (伦敦时间)");
    } else if (minutes == -300) {
        snprintf(b, sizeof(b), "UTC-5 (纽约时间)");
    } else {
        snprintf(b, sizeof(b), "UTC%c%d:%02d", sign, h, m);
    }
    return b;
}

lv_obj_t* MakeButton(lv_obj_t* parent, const char* text, lv_coord_t w, lv_coord_t h,
                     lv_event_cb_t cb, void* user_data = nullptr, bool primary = false) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, primary ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(lbl, primary ? lv_color_white() : lv_color_black(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

lv_obj_t* MakeCard(lv_obj_t* parent, lv_coord_t w, lv_coord_t h) {
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

lv_obj_t* MakeRow(lv_obj_t* parent, lv_coord_t h, lv_flex_align_t main_place = LV_FLEX_ALIGN_SPACE_BETWEEN) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), h);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, main_place, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    return row;
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

        static const char* const kWdays[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
        const char* wday_str = (local.tm_wday >= 0 && local.tm_wday <= 6) ? kWdays[local.tm_wday] : "";
        char db[64];
        snprintf(db, sizeof(db), "%04d年%02d月%02d日  %s",
                 local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, wday_str);
        lv_label_set_text(s_ui.date_lbl, db);
    } else {
        lv_label_set_text(s_ui.clock_lbl, "--:--:--");
        lv_label_set_text(s_ui.date_lbl, "未初始化时钟");
    }

    if (s_ui.status_lbl != nullptr) {
        const bool net = Board::GetInstance().IsNetworkReady();
        const bool synced = TimeService_IsSynced();
        if (synced) {
            lv_label_set_text(s_ui.status_lbl, "● 对时状态: 已完成网络同步");
        } else if (net) {
            lv_label_set_text(s_ui.status_lbl, "◌ 对时状态: 正在向SNTP同步…");
        } else {
            lv_label_set_text(s_ui.status_lbl, "○ 对时状态: 未联网(本地RTC运行)");
        }
    }

    if (s_ui.last_sync_lbl != nullptr) {
        long last_epoch = TimeService_LastSyncEpoch();
        if (last_epoch > 0) {
            time_t t = static_cast<time_t>(last_epoch);
            struct tm tm_last = {};
            localtime_r(&t, &tm_last);
            char b[64];
            snprintf(b, sizeof(b), "上次对时: %04d-%02d-%02d %02d:%02d",
                     tm_last.tm_year + 1900, tm_last.tm_mon + 1, tm_last.tm_mday,
                     tm_last.tm_hour, tm_last.tm_min);
            lv_label_set_text(s_ui.last_sync_lbl, b);
        } else {
            lv_label_set_text(s_ui.last_sync_lbl, "上次对时: 尚未联网对时");
        }
    }
}

void RefreshTz() {
    if (s_ui.tz_value_lbl != nullptr) {
        lv_label_set_text(s_ui.tz_value_lbl, GetTzName(TimeService_GetTzOffsetMin()).c_str());
    }
}

void RefreshManual() {
    if (s_ui.man_date_lbl != nullptr) {
        char b[32];
        snprintf(b, sizeof(b), "%04d-%02d-%02d", s_year, s_mon, s_day);
        lv_label_set_text(s_ui.man_date_lbl, b);
    }
    if (s_ui.man_time_lbl != nullptr) {
        char b[16];
        snprintf(b, sizeof(b), "%02d : %02d", s_hour, s_min);
        lv_label_set_text(s_ui.man_time_lbl, b);
    }
}

void OnTimer(lv_timer_t*) {
    RefreshClock();
}

void OnSyncNow(lv_event_t*) {
    HapticPulseIfEnabled();
    TimeService_SyncNow();
    if (s_ui.status_lbl != nullptr) {
        lv_label_set_text(s_ui.status_lbl, "◌ 对时状态: 正在向SNTP同步…");
    }
    EpdWidgetKit::EpdToast::Show("已触发网络对时请求", 1500);
}

void OnTzStep(lv_event_t* e) {
    int delta = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    HapticPulseIfEnabled();
    TimeService_SetTzOffsetMin(TimeService_GetTzOffsetMin() + delta);
    RefreshTz();
    RefreshClock();
}

void OnTzDefault(lv_event_t*) {
    HapticPulseIfEnabled();
    TimeService_SetTzOffsetMin(480);
    RefreshTz();
    RefreshClock();
}

void OnAdjustDate(lv_event_t* e) {
    int delta = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    HapticPulseIfEnabled();
    AdjustDate(delta, 0, 0);
    RefreshManual();
}

void OnAdjustHour(lv_event_t* e) {
    int delta = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    HapticPulseIfEnabled();
    AdjustDate(0, delta, 0);
    RefreshManual();
}

void OnAdjustMin(lv_event_t* e) {
    int delta = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    HapticPulseIfEnabled();
    AdjustDate(0, 0, delta);
    RefreshManual();
}

void OnManualSave(lv_event_t*) {
    HapticPulseIfEnabled();
    if (TimeService_SetManual(s_year, s_mon, s_day, s_hour, s_min)) {
        RefreshClock();
        EpdWidgetKit::EpdToast::Show("时间已校准并同步至RTC", 2000);
    } else {
        EpdWidgetKit::EpdToast::Show("时间参数无效", 2000);
    }
}

void OnScreenUnloaded(lv_event_t*) {
    s_alive = false;
    if (s_timer != nullptr) {
        lv_timer_delete(s_timer);
        s_timer = nullptr;
    }
    s_ui = {};
}

}  // namespace

lv_obj_t* TimeSyncScreen::Create() {
    s_ui = {};
    s_alive = true;
    ScreenSetIsHome(false);

    auto* scaffold = EpdWidgetKit::EpdPageScaffold::Create({
        .title = "时间与网络对时",
        .show_header = true,
        .show_status_bar = true,
        .show_back_btn = true,
        .pad_h = 16,
        .pad_v = 10,
        .flex_column = true,
        .row_gap = 12,
    });

    s_ui.screen = scaffold->GetScreen();
    lv_obj_t* body = scaffold->GetBody();
    lv_obj_add_event_cb(s_ui.screen, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);

    // ==================== 卡片 1: 当前系统时间与对时状态 (Hero Card) ====================
    lv_obj_t* clock_card = MakeCard(body, lv_pct(100), 285);
    lv_obj_set_style_pad_all(clock_card, 12, 0);
    lv_obj_set_style_pad_row(clock_card, 4, 0);

    lv_obj_t* tag_lbl = ScreenMakeLabel(clock_card, "当前系统时间 (RTC)", 22, false);
    lv_obj_set_style_text_color(tag_lbl, lv_palette_darken(LV_PALETTE_GREY, 3), 0);
    lv_obj_set_width(tag_lbl, lv_pct(100));
    lv_obj_set_style_text_align(tag_lbl, LV_TEXT_ALIGN_CENTER, 0);

    s_ui.clock_lbl = ScreenMakeLabel(clock_card, "--:--:--", 56, false);
    lv_obj_set_width(s_ui.clock_lbl, lv_pct(100));
    lv_obj_set_style_text_align(s_ui.clock_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_pad_ver(s_ui.clock_lbl, 2, 0);

    s_ui.date_lbl = ScreenMakeLabel(clock_card, "----年--月--日", 25, false);
    lv_obj_set_width(s_ui.date_lbl, lv_pct(100));
    lv_obj_set_style_text_align(s_ui.date_lbl, LV_TEXT_ALIGN_CENTER, 0);

    // 分隔线
    lv_obj_t* div = lv_obj_create(clock_card);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, lv_pct(100), 1);
    lv_obj_set_style_bg_color(div, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_ver(div, 4, 0);

    // 状态信息列表
    s_ui.status_lbl = ScreenMakeLabel(clock_card, "○ 对时状态: 检测中…", 22, false);
    s_ui.last_sync_lbl = ScreenMakeLabel(clock_card, "上次对时: 尚未对时", 22, false);
    s_ui.rtc_lbl = ScreenMakeLabel(clock_card, "硬件时钟: PCF8563 运行正常", 22, false);
    ScreenMakeLabel(clock_card, "授时节点: ntp.aliyun.com", 22, false);

    // 弹性撑开，将对时按钮停靠在卡片底部
    lv_obj_t* spacer = lv_obj_create(clock_card);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_size(spacer, lv_pct(100), 1);
    lv_obj_set_flex_grow(spacer, 1);

    s_ui.sntp_btn = MakeButton(clock_card, "立即联网对时 (SNTP)", lv_pct(100), 42, OnSyncNow, nullptr, true);

    // ==================== 卡片 2: 时区配置 ====================
    lv_obj_t* tz_card = MakeCard(body, lv_pct(100), 108);
    lv_obj_set_style_pad_all(tz_card, 12, 0);
    lv_obj_set_style_pad_row(tz_card, 8, 0);

    lv_obj_t* tz_head_row = MakeRow(tz_card, 26);
    ScreenMakeLabel(tz_head_row, "时区配置", 25, false);
    s_ui.tz_value_lbl = ScreenMakeLabel(tz_head_row, "UTC+8 (北京时间)", 22, false);
    lv_obj_set_style_text_align(s_ui.tz_value_lbl, LV_TEXT_ALIGN_RIGHT, 0);

    lv_obj_t* tz_btn_row = MakeRow(tz_card, 40, LV_FLEX_ALIGN_SPACE_BETWEEN);
    MakeButton(tz_btn_row, "- 1小时", 120, 40, OnTzStep, reinterpret_cast<void*>(-60));
    MakeButton(tz_btn_row, "默认 (UTC+8)", 150, 40, OnTzDefault, nullptr);
    MakeButton(tz_btn_row, "+ 1小时", 120, 40, OnTzStep, reinterpret_cast<void*>(60));

    // ==================== 卡片 3: 手动校准 ====================
    lv_obj_t* man_card = MakeCard(body, lv_pct(100), 215);
    lv_obj_set_style_pad_all(man_card, 12, 0);
    lv_obj_set_style_pad_row(man_card, 8, 0);

    lv_obj_t* man_head_row = MakeRow(man_card, 26);
    ScreenMakeLabel(man_head_row, "手动校准", 25, false);
    lv_obj_t* man_hint = ScreenMakeLabel(man_head_row, "无网时校正RTC", 20, false);
    lv_obj_set_style_text_color(man_hint, lv_palette_darken(LV_PALETTE_GREY, 2), 0);

    // 日期调节行
    lv_obj_t* date_row = MakeRow(man_card, 36, LV_FLEX_ALIGN_SPACE_BETWEEN);
    ScreenMakeLabel(date_row, "日期", 22, false);
    MakeButton(date_row, "- 1天", 80, 36, OnAdjustDate, reinterpret_cast<void*>(-1));
    s_ui.man_date_lbl = ScreenMakeLabel(date_row, "2026-10-16", 24, false);
    lv_obj_set_width(s_ui.man_date_lbl, 130);
    lv_obj_set_style_text_align(s_ui.man_date_lbl, LV_TEXT_ALIGN_CENTER, 0);
    MakeButton(date_row, "+ 1天", 80, 36, OnAdjustDate, reinterpret_cast<void*>(1));

    // 时间调节行
    lv_obj_t* time_row = MakeRow(man_card, 36, LV_FLEX_ALIGN_SPACE_BETWEEN);
    ScreenMakeLabel(time_row, "时间", 22, false);
    MakeButton(time_row, "-1时", 58, 36, OnAdjustHour, reinterpret_cast<void*>(-1));
    MakeButton(time_row, "-5分", 58, 36, OnAdjustMin, reinterpret_cast<void*>(-5));
    s_ui.man_time_lbl = ScreenMakeLabel(time_row, "12 : 00", 24, false);
    lv_obj_set_width(s_ui.man_time_lbl, 80);
    lv_obj_set_style_text_align(s_ui.man_time_lbl, LV_TEXT_ALIGN_CENTER, 0);
    MakeButton(time_row, "+5分", 58, 36, OnAdjustMin, reinterpret_cast<void*>(5));
    MakeButton(time_row, "+1时", 58, 36, OnAdjustHour, reinterpret_cast<void*>(1));

    // 保存按钮
    MakeButton(man_card, "保存并写入硬件RTC芯片", lv_pct(100), 40, OnManualSave, nullptr, false);

    // 初始化暂存为当前时间
    {
        time_t now = time(nullptr);
        struct tm local = {};
        localtime_r(&now, &local);
        if (local.tm_year + 1900 >= 2025) {
            s_year = local.tm_year + 1900;
            s_mon = local.tm_mon + 1;
            s_day = local.tm_mday;
            s_hour = local.tm_hour;
            s_min = local.tm_min;
        }
    }

    // 进屏触发一次对时请求
    TimeService_SyncNow();

    RefreshTz();
    RefreshManual();
    RefreshClock();

    s_timer = lv_timer_create(OnTimer, 1000, nullptr);
    VkKey_AttachScreen(s_ui.screen, kScreenId, VkKeyScreenDesc{TimeSyncScreen::Create, nullptr});
    return s_ui.screen;
}
