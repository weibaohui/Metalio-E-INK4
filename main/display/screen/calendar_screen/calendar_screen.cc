// main/display/screen/calendar_screen/calendar_screen.cc —— 日历 APP(需求一.4)。
// 月视图/周视图切换,显示每天日程;日程经远端 MCP 推送同步(calendar_store)。
// 列表与首页同规矩:不滚动,整页重画。离开页面不残留常驻资源(纯 UI,无后台任务)。
#include "calendar_screen/calendar_screen.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <esp_log.h>

#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "screen_common.h"
#include "services/calendar/calendar_store.h"
#include "vk_key_handler.h"

namespace {

constexpr const char* TAG = "CalendarScreen";
constexpr const char* kScreenId = "calendar";

constexpr lv_coord_t kPad = 12;
constexpr int kCols = 7;        // 周一..周日
constexpr int kMonthRows = 6;   // 月视图最多 6 行

const char* kWdayNames[kCols] = {"一", "二", "三", "四", "五", "六", "日"};

struct CalUi {
    lv_obj_t* screen = nullptr;
    lv_obj_t* month_lbl = nullptr;   // 月视图:"2026年10月"
    lv_obj_t* grid = nullptr;        // 网格容器
    lv_obj_t* panel_lbl = nullptr;   // 底部日程区
    lv_obj_t* view_btn_lbl = nullptr;
    lv_obj_t* footer_lbl = nullptr;
};
CalUi s_ui;
bool s_alive = false;

bool s_week_view = false;   // false=月视图 true=周视图
time_t s_cursor = 0;        // 当前浏览锚点(某天正午);月视图用其年月,周视图用其所在周
std::string s_selected;     // 选中日 "YYYY-MM-DD"(默认今天);底部日程区显示它

const lv_font_t* UiFont() { return fontpack_lv_font_ui(); }
void DisableScroll(lv_obj_t* o) { lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE); }

// ---- 日期工具 ----
time_t NowEpoch() { return time(nullptr); }

void EpochToYmd(time_t t, int& y, int& m, int& d) {
    struct tm tmv = {};
    localtime_r(&t, &tmv);
    y = tmv.tm_year + 1900; m = tmv.tm_mon + 1; d = tmv.tm_mday;
}
time_t YmdToNoonEpoch(int y, int m, int d) {
    struct tm tmv = {};
    tmv.tm_year = y - 1900; tmv.tm_mon = m - 1; tmv.tm_mday = d; tmv.tm_hour = 12;
    return mktime(&tmv);   // TZ=GMT+0:无 DST 边缘问题
}
std::string YmdToStr(int y, int m, int d) {
    char b[40]; snprintf(b, sizeof(b), "%04d-%02d-%02d", y, m, d); return b;   // 40:年份字段理论 11 字节,-Werror 防截断
}
int DaysInMonth(int y, int m) {
    static const int dm[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m == 2) { const bool leap = (y%4==0 && y%100!=0) || (y%400==0); return leap ? 29 : 28; }
    return dm[m-1];
}
// 0=周一 .. 6=周日
int WeekdayMonFirst(int y, int m, int d) {
    struct tm tmv = {};
    tmv.tm_year = y-1900; tmv.tm_mon = m-1; tmv.tm_mday = d; tmv.tm_hour = 12;
    mktime(&tmv);            // tm_wday 与时区无关
    return (tmv.tm_wday + 6) % 7;
}

// ---- UI 元素 ----
lv_obj_t* MakeLabel(lv_obj_t* parent, const char* text, int font_size = 25) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text != nullptr ? text : "");
    const lv_font_t* font = font_size > 25 ? fontpack_lv_font_get(font_size, 2) : UiFont();
    lv_obj_set_style_text_font(lbl, font != nullptr ? font : UiFont(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return lbl;
}

lv_obj_t* MakeBtn(lv_obj_t* parent, const char* text, lv_coord_t w, lv_event_cb_t cb, void* ud) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, 44);
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, UiFont(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return lbl;
}

// 选中某天 → 底部日程区显示
void ShowDayPanel(const std::string& date) {
    if (s_ui.panel_lbl == nullptr) return;
    if (date.empty()) {
        lv_label_set_text(s_ui.panel_lbl, "未对时,无法确定今天");
        return;
    }
    const auto events = Calendar_GetForDate(date);
    std::string text = date + " 日程(" + std::to_string(events.size()) + ")";
    if (events.empty()) {
        text += "\n  无";
    } else {
        for (const auto& e : events) {
            text += "\n  " + (e.time.empty() ? std::string("全天") : e.time) + " " + e.title;
        }
    }
    lv_label_set_text(s_ui.panel_lbl, text.c_str());
}

void OnDayClicked(lv_event_t* e) {
    const uintptr_t packed = reinterpret_cast<uintptr_t>(lv_event_get_user_data(e));
    // packed = (y-2000)<<16 | m<<8 | d  (天数>0 才有点击回调)
    const int y = 2000 + (int)(packed >> 16);
    const int m = (int)((packed >> 8) & 0xff);
    const int d = (int)(packed & 0xff);
    s_selected = YmdToStr(y, m, d);
    ShowDayPanel(s_selected);
    HapticPulseIfEnabled();
}

void RebuildGrid();  // 前向声明

void OnPrevNext(lv_event_t* e) {
    const bool next = reinterpret_cast<intptr_t>(lv_event_get_user_data(e)) != 0;
    HapticPulseIfEnabled();
    if (s_week_view) {
        s_cursor += (next ? 7 : -7) * 86400;   // 周视图:±一周
    } else {
        int y, m, d; EpochToYmd(s_cursor, y, m, d);
        m += next ? 1 : -1;                    // 月视图:±一月
        if (m > 12) { m = 1; y++; }
        if (m < 1) { m = 12; y--; }
        s_cursor = YmdToNoonEpoch(y, m, 1);
    }
    RebuildGrid();
}

void OnToggleView(lv_event_t*) {
    HapticPulseIfEnabled();
    s_week_view = !s_week_view;
    if (s_ui.view_btn_lbl != nullptr) {
        lv_label_set_text(s_ui.view_btn_lbl, s_week_view ? "月" : "周");
    }
    RebuildGrid();
}

// 月视图:周一开头的 6x7 网格
void BuildMonthGrid(lv_coord_t cell_w) {
    int cy, cm, cd; EpochToYmd(s_cursor, cy, cm, cd);
    if (s_ui.month_lbl != nullptr) {
        char buf[24]; snprintf(buf, sizeof(buf), "%04d年%02d月", cy, cm);
        lv_label_set_text(s_ui.month_lbl, buf);
    }
    const int first_wd = WeekdayMonFirst(cy, cm, 1);
    const int ndays = DaysInMonth(cy, cm);
    const std::string today = Calendar_Today();

    // 周名行
    lv_obj_t* head = lv_obj_create(s_ui.grid);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    DisableScroll(head); lv_obj_clear_flag(head, LV_OBJ_FLAG_CLICKABLE);
    for (int c = 0; c < kCols; ++c) {
        lv_obj_t* w = MakeLabel(head, kWdayNames[c]);
        lv_obj_set_width(w, cell_w);
        lv_obj_set_style_text_align(w, LV_TEXT_ALIGN_CENTER, 0);
    }

    int day = 1;
    for (int r = 0; r < kMonthRows && day <= ndays; ++r) {
        lv_obj_t* row = lv_obj_create(s_ui.grid);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        DisableScroll(row); lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        for (int c = 0; c < kCols; ++c) {
            const bool empty_cell = (r == 0 && c < first_wd) || (day > ndays);
            lv_obj_t* cell = lv_obj_create(row);
            lv_obj_remove_style_all(cell);
            lv_obj_set_size(cell, cell_w, 44);
            lv_obj_set_style_radius(cell, 6, 0);
            DisableScroll(cell);
            if (empty_cell) {
                lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
                lv_obj_clear_flag(cell, LV_OBJ_FLAG_CLICKABLE);
                continue;
            }
            const std::string dstr = YmdToStr(cy, cm, day);
            const bool is_today = (dstr == today);
            const bool has_ev = Calendar_HasEvent(dstr);
            char txt[24]; snprintf(txt, sizeof(txt), "%d%s", day, has_ev ? "·" : "");   // 24:日号(理论 11)+·(3)+NUL,-Werror 防截断
            if (is_today) {
                lv_obj_set_style_bg_color(cell, lv_color_black(), 0);
                lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            } else {
                lv_obj_set_style_bg_color(cell, lv_color_white(), 0);
                lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
                lv_obj_set_style_border_color(cell, lv_color_black(), 0);
                lv_obj_set_style_border_width(cell, has_ev ? 2 : 1, 0);
            }
            lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(cell, OnDayClicked, LV_EVENT_CLICKED,
                reinterpret_cast<void*>((uintptr_t)(((cy-2000)<<16) | (cm<<8) | day)));
            lv_obj_t* lbl = lv_label_create(cell);
            lv_label_set_text(lbl, txt);
            lv_obj_set_style_text_font(lbl, UiFont(), 0);
            lv_obj_set_style_text_color(lbl, is_today ? lv_color_white() : lv_color_black(), 0);
            lv_obj_center(lbl);
            lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
            ++day;
        }
    }
}

// 周视图:本周(周一起)7 列,各列显示日期 + 最多 2 条日程
void BuildWeekGrid(lv_coord_t cell_w) {
    int cy, cm, cd; EpochToYmd(s_cursor, cy, cm, cd);
    const int wd = WeekdayMonFirst(cy, cm, cd);
    const time_t monday = YmdToNoonEpoch(cy, cm, cd) - (time_t)wd * 86400;
    if (s_ui.month_lbl != nullptr) {
        int y2, m2, d2; EpochToYmd(monday, y2, m2, d2);
        char buf[28]; snprintf(buf, sizeof(buf), "%02d月%02d日 起一周", m2, d2);
        lv_label_set_text(s_ui.month_lbl, buf);
    }
    const std::string today = Calendar_Today();

    lv_obj_t* row = lv_obj_create(s_ui.grid);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    DisableScroll(row); lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

    for (int c = 0; c < kCols; ++c) {
        const time_t t = monday + (time_t)c * 86400;
        int y, m, d; EpochToYmd(t, y, m, d);
        const std::string dstr = YmdToStr(y, m, d);
        const bool is_today = (dstr == today);

        lv_obj_t* cell = lv_obj_create(row);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, cell_w, LV_SIZE_CONTENT);
        lv_obj_set_style_border_color(cell, lv_color_black(), 0);
        lv_obj_set_style_border_width(cell, 1, 0);
        lv_obj_set_style_radius(cell, 6, 0);
        lv_obj_set_style_pad_all(cell, 4, 0);
        lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(cell, 2, 0);
        DisableScroll(cell);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(cell, OnDayClicked, LV_EVENT_CLICKED,
            reinterpret_cast<void*>((uintptr_t)(((y-2000)<<16) | (m<<8) | d)));
        if (is_today) {
            lv_obj_set_style_bg_color(cell, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        }
        lv_obj_t* dl = MakeLabel(cell, (std::string(kWdayNames[c]) + " " + std::to_string(d)).c_str());
        lv_obj_set_style_text_color(dl, is_today ? lv_color_white() : lv_color_black(), 0);
        const auto events = Calendar_GetForDate(dstr);
        int shown = 0;
        for (const auto& e : events) {
            if (shown >= 2) break;
            std::string line = (e.time.empty() ? std::string("·") : e.time + " ") + e.title;
            lv_obj_t* el = MakeLabel(cell, line.c_str());
            lv_label_set_long_mode(el, LV_LABEL_LONG_DOT);
            lv_obj_set_width(el, cell_w - 8);
            lv_obj_set_style_text_color(el, is_today ? lv_color_white() : lv_color_black(), 0);
            ++shown;
        }
    }
}

void RebuildGrid() {
    if (s_ui.grid == nullptr) return;
    lv_obj_clean(s_ui.grid);
    const lv_coord_t cell_w = (LV_HOR_RES - 2 * kPad) / kCols - 4;
    if (s_week_view) BuildWeekGrid(cell_w); else BuildMonthGrid(cell_w);
    if (s_selected.empty()) s_selected = Calendar_Today();
    ShowDayPanel(s_selected);
}

void OnScreenUnloaded(lv_event_t*) {
    s_alive = false;
    s_ui = {};
}

}  // namespace

lv_obj_t* CalendarScreen::Create() {
    s_ui = {};
    s_alive = true;
    s_week_view = false;
    // 浏览锚点 = 今天正午(月视图取其年月,周视图取其所在周)
    {
        int y, m, d; EpochToYmd(NowEpoch(), y, m, d);
        s_cursor = YmdToNoonEpoch(y, m, d);
    }
    s_selected = Calendar_Today();
    ScreenSetIsHome(false);

    CalendarStore_EnsureLoaded();   // 载入 SD 缓存(幂等)

    lv_obj_t* screen = lv_obj_create(nullptr);
    s_ui.screen = screen;
    lv_obj_remove_style_all(screen);
    lv_obj_set_size(screen, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);
    lv_obj_set_style_text_font(screen, UiFont(), 0);
    DisableScroll(screen);
    lv_obj_add_event_cb(screen, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);

    const EpdStatusBar status = ScreenCreateStatusBar(screen);
    lv_obj_t* body = lv_obj_create(screen);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, LV_VER_RES - status.height);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, status.height);
    lv_obj_set_style_pad_all(body, kPad, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 8, 0);
    DisableScroll(body);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);

    // 头部:翻页 + 月份/周标签 + 视图切换
    lv_obj_t* head = lv_obj_create(body);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, lv_pct(100), 48);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    DisableScroll(head); lv_obj_clear_flag(head, LV_OBJ_FLAG_CLICKABLE);
    MakeBtn(head, "‹", 70, OnPrevNext, (void*)0);
    s_ui.month_lbl = MakeLabel(head, "日历", 28);
    lv_obj_set_flex_grow(s_ui.month_lbl, 1);
    lv_obj_set_style_text_align(s_ui.month_lbl, LV_TEXT_ALIGN_CENTER, 0);
    s_ui.view_btn_lbl = MakeBtn(head, "周", 70, OnToggleView, nullptr);
    MakeBtn(head, "›", 70, OnPrevNext, (void*)1);

    // 网格
    s_ui.grid = lv_obj_create(body);
    lv_obj_remove_style_all(s_ui.grid);
    lv_obj_set_size(s_ui.grid, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_ui.grid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_ui.grid, 2, 0);
    DisableScroll(s_ui.grid);
    lv_obj_clear_flag(s_ui.grid, LV_OBJ_FLAG_CLICKABLE);

    // 底部:选中日/今日日程
    lv_obj_t* panel = lv_obj_create(body);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_border_color(panel, lv_color_black(), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_set_style_pad_all(panel, 10, 0);
    lv_obj_set_flex_grow(panel, 1);
    DisableScroll(panel); lv_obj_clear_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    s_ui.panel_lbl = MakeLabel(panel, "");
    lv_obj_set_width(s_ui.panel_lbl, lv_pct(100));
    lv_label_set_long_mode(s_ui.panel_lbl, LV_LABEL_LONG_WRAP);

    RebuildGrid();
    VkKey_AttachScreen(screen, kScreenId, VkKeyScreenDesc{CalendarScreen::Create, nullptr});
    return screen;
}
