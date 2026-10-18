// tools/cal_app/main/cal_main.c
// 日历 动态应用：.app Bundle 规范版
// 纯 C + LVGL，仅调用固件符号表导出接口。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

#define COLS 7
#define MONTH_ROWS 6
#define PAD 12

static const char* k_wday_names[COLS] = {"一", "二", "三", "四", "五", "六", "日"};

static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_month_lbl = NULL;
static lv_obj_t *s_grid = NULL;
static lv_obj_t *s_panel_lbl = NULL;

static int s_cur_year = 2026;
static int s_cur_month = 10;
static char s_selected[32] = "";
static char s_today[32] = "";

static void *cal_create(void);

static const lv_font_t *get_font(int size, int weight) {
    const lv_font_t *f = fontpack_lv_font_get(size, weight);
    if (f == NULL) f = fontpack_lv_font_ui();
    return f;
}

static int days_in_month(int y, int m) {
    static const int dm[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (m == 2) {
        int leap = (y % 4 == 0 && y % 100 != 0) || (y % 400 == 0);
        return leap ? 29 : 28;
    }
    if (m >= 1 && m <= 12) return dm[m - 1];
    return 30;
}

static int weekday_mon_first(int y, int m, int d) {
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = y - 1900;
    tmv.tm_mon = m - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = 12;
    mktime(&tmv);
    return (tmv.tm_wday + 6) % 7;
}

static void update_panel(const char* date) {
    if (!s_panel_lbl) return;
    if (!date || date[0] == '\0') {
        lv_label_set_text(s_panel_lbl, "未对时，无法确定当前日期");
        return;
    }
    char buf[1024];
    int count = dynapp_cal_get_events_text(date, buf, sizeof(buf));
    if (count <= 0 && buf[0] == '\0') {
        snprintf(buf, sizeof(buf), "%s 日程(0)\n  无", date);
    }
    lv_label_set_text(s_panel_lbl, buf);
}

static void rebuild_grid(void);

static void async_rebuild_cb(void* user_data) {
    (void)user_data;
    if (s_scr != NULL && s_grid != NULL && lv_obj_is_valid(s_grid)) {
        rebuild_grid();
    }
}

static void on_day_clicked(lv_event_t* e) {
    uintptr_t packed = (uintptr_t)lv_event_get_user_data(e);
    int y = 2000 + (int)(packed >> 16);
    int m = (int)((packed >> 8) & 0xff);
    int d = (int)(packed & 0xff);

    snprintf(s_selected, sizeof(s_selected), "%04d-%02d-%02d", y, m, d);
    lv_async_call(async_rebuild_cb, NULL);
}

static void on_prev_next_internal(int next) {
    s_cur_month += next ? 1 : -1;
    if (s_cur_month > 12) {
        s_cur_month = 1;
        s_cur_year++;
    } else if (s_cur_month < 1) {
        s_cur_month = 12;
        s_cur_year--;
    }
    rebuild_grid();
}

static void on_prev_next(lv_event_t* e) {
    intptr_t next = (intptr_t)lv_event_get_user_data(e);
    on_prev_next_internal((int)next);
}

static void rebuild_grid(void) {
    if (!s_grid) return;
    lv_obj_clean(s_grid);

    if (s_month_lbl) {
        char buf[64];
        snprintf(buf, sizeof(buf), "%04d年%02d月", s_cur_year, s_cur_month);
        lv_label_set_text(s_month_lbl, buf);
    }

    const lv_coord_t cell_w = (LV_HOR_RES - 2 * PAD) / COLS - 4;
    const int first_wd = weekday_mon_first(s_cur_year, s_cur_month, 1);
    const int ndays = days_in_month(s_cur_year, s_cur_month);

    // 星期名行
    lv_obj_t* head = lv_obj_create(s_grid);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    for (int c = 0; c < COLS; ++c) {
        lv_obj_t* w = lv_label_create(head);
        lv_label_set_text(w, k_wday_names[c]);
        const lv_font_t *f = get_font(20, 0);
        if (f) lv_obj_set_style_text_font(w, f, 0);
        lv_obj_set_width(w, cell_w);
        lv_obj_set_style_text_align(w, LV_TEXT_ALIGN_CENTER, 0);
    }

    int day = 1;
    for (int r = 0; r < MONTH_ROWS && day <= ndays; ++r) {
        lv_obj_t* row = lv_obj_create(s_grid);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

        for (int c = 0; c < COLS; ++c) {
            int empty_cell = (r == 0 && c < first_wd) || (day > ndays);
            lv_obj_t* cell = lv_obj_create(row);
            lv_obj_remove_style_all(cell);
            lv_obj_set_size(cell, cell_w, 44);
            lv_obj_set_style_radius(cell, 6, 0);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

            if (empty_cell) {
                lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
                lv_obj_clear_flag(cell, LV_OBJ_FLAG_CLICKABLE);
                continue;
            }

            char dstr[32];
            snprintf(dstr, sizeof(dstr), "%04d-%02d-%02d", s_cur_year, s_cur_month, day);
            int is_today = (strcmp(dstr, s_today) == 0);
            int is_sel = (strcmp(dstr, s_selected) == 0);
            int has_ev = dynapp_cal_has_event(dstr);

            char txt[32];
            snprintf(txt, sizeof(txt), "%d%s", day, has_ev ? "·" : "");

            if (is_today) {
                lv_obj_set_style_bg_color(cell, lv_color_black(), 0);
                lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
                if (is_sel) {
                    lv_obj_set_style_border_color(cell, lv_color_white(), 0);
                    lv_obj_set_style_border_width(cell, 2, 0);
                }
            } else {
                lv_obj_set_style_bg_color(cell, lv_color_white(), 0);
                lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
                lv_obj_set_style_border_color(cell, lv_color_black(), 0);
                lv_obj_set_style_border_width(cell, is_sel ? 3 : (has_ev ? 2 : 1), 0);
            }

            lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
            uintptr_t user_val = ((s_cur_year - 2000) << 16) | (s_cur_month << 8) | day;
            lv_obj_add_event_cb(cell, on_day_clicked, LV_EVENT_CLICKED, (void*)user_val);

            lv_obj_t* lbl = lv_label_create(cell);
            lv_label_set_text(lbl, txt);
            const lv_font_t *f = get_font(20, 0);
            if (f) lv_obj_set_style_text_font(lbl, f, 0);
            lv_obj_set_style_text_color(lbl, is_today ? lv_color_white() : lv_color_black(), 0);
            lv_obj_center(lbl);
            lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

            day++;
        }
    }

    if (s_selected[0] == '\0') {
        strncpy(s_selected, s_today, sizeof(s_selected) - 1);
    }
    update_panel(s_selected);
}

static void on_screen_deleted(lv_event_t* e) {
    (void)e;
    s_scr = NULL;
    s_month_lbl = NULL;
    s_grid = NULL;
    s_panel_lbl = NULL;
}

static void on_back_clicked(lv_event_t *e) {
    (void)e;
    dynapp_screen_go_home();
}

static int cal_on_vk_key(const char* key) {
    if (key != NULL) {
        if (strcmp(key, "esc") == 0 || strcmp(key, "back") == 0 || strcmp(key, "vk_home") == 0 || strcmp(key, "home") == 0) {
            dynapp_screen_go_home();
            return 1;
        }
        if (strcmp(key, "vk_prev") == 0) {
            on_prev_next_internal(0);
            return 1;
        }
        if (strcmp(key, "vk_next") == 0) {
            on_prev_next_internal(1);
            return 1;
        }
    }
    return 0;
}

static void *cal_create(void) {
    dynapp_cal_auto_refresh();

    dynapp_cal_today(s_today, sizeof(s_today));
    if (s_today[0] != '\0') {
        int y = 0, m = 0, d = 0;
        if (sscanf(s_today, "%d-%d-%d", &y, &m, &d) == 3) {
            s_cur_year = y;
            s_cur_month = m;
        }
        strncpy(s_selected, s_today, sizeof(s_selected) - 1);
    } else {
        time_t now = time(NULL);
        struct tm tmv;
        localtime_r(&now, &tmv);
        s_cur_year = tmv.tm_year + 1900;
        s_cur_month = tmv.tm_mon + 1;
        snprintf(s_today, sizeof(s_today), "%04d-%02d-%02d", s_cur_year, s_cur_month, tmv.tm_mday);
        strncpy(s_selected, s_today, sizeof(s_selected) - 1);
    }

    lv_obj_t *scr = lv_obj_create(NULL);
    s_scr = scr;
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 800);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(scr, on_screen_deleted, LV_EVENT_DELETE, NULL);

    dynapp_screen_status_bar(scr);
    dynapp_vk_attach(scr, "cal", cal_create, cal_on_vk_key);

    // 内容区 (无返回顶栏，由底部 vk_home 键统一控制返回)
    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, 456, 750);
    lv_obj_set_pos(body, 12, 42);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 8, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    // 顶部月份与翻页控制栏
    lv_obj_t* head = lv_obj_create(body);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, lv_pct(100), 48);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // 上个月按钮
    lv_obj_t* btn_prev = lv_obj_create(head);
    lv_obj_remove_style_all(btn_prev);
    lv_obj_set_size(btn_prev, 70, 40);
    lv_obj_set_style_bg_color(btn_prev, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn_prev, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn_prev, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn_prev, 2, 0);
    lv_obj_set_style_radius(btn_prev, 6, 0);
    lv_obj_add_flag(btn_prev, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_prev, on_prev_next, LV_EVENT_CLICKED, (void*)0);

    lv_obj_t* lbl_prev = lv_label_create(btn_prev);
    lv_label_set_text(lbl_prev, "‹");
    const lv_font_t *f24 = get_font(24, 2);
    if (f24) lv_obj_set_style_text_font(lbl_prev, f24, 0);
    lv_obj_set_style_text_color(lbl_prev, lv_color_black(), 0);
    lv_obj_center(lbl_prev);

    // 月份标题
    s_month_lbl = lv_label_create(head);
    lv_obj_set_flex_grow(s_month_lbl, 1);
    lv_obj_set_style_text_align(s_month_lbl, LV_TEXT_ALIGN_CENTER, 0);
    const lv_font_t *f20 = get_font(20, 2);
    if (f20) lv_obj_set_style_text_font(s_month_lbl, f20, 0);

    // 下个月按钮
    lv_obj_t* btn_next = lv_obj_create(head);
    lv_obj_remove_style_all(btn_next);
    lv_obj_set_size(btn_next, 70, 40);
    lv_obj_set_style_bg_color(btn_next, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn_next, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn_next, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn_next, 2, 0);
    lv_obj_set_style_radius(btn_next, 6, 0);
    lv_obj_add_flag(btn_next, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_next, on_prev_next, LV_EVENT_CLICKED, (void*)1);

    lv_obj_t* lbl_next = lv_label_create(btn_next);
    lv_label_set_text(lbl_next, "›");
    if (f24) lv_obj_set_style_text_font(lbl_next, f24, 0);
    lv_obj_set_style_text_color(lbl_next, lv_color_black(), 0);
    lv_obj_center(lbl_next);

    // 月历网格容器
    s_grid = lv_obj_create(body);
    lv_obj_remove_style_all(s_grid);
    lv_obj_set_size(s_grid, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_grid, 2, 0);
    lv_obj_clear_flag(s_grid, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // 底部日程面板
    lv_obj_t* panel = lv_obj_create(body);
    lv_obj_remove_style_all(panel);
    lv_obj_set_size(panel, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_border_color(panel, lv_color_black(), 0);
    lv_obj_set_style_border_width(panel, 2, 0);
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_set_style_pad_all(panel, 10, 0);
    lv_obj_set_flex_grow(panel, 1);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_panel_lbl = lv_label_create(panel);
    lv_obj_set_width(s_panel_lbl, lv_pct(100));
    lv_label_set_long_mode(s_panel_lbl, LV_LABEL_LONG_WRAP);
    const lv_font_t *f18 = get_font(18, 0);
    if (f18) lv_obj_set_style_text_font(s_panel_lbl, f18, 0);

    rebuild_grid();
    return scr;
}

void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "cal";
    out->name = "日历";
    out->icon = "icon.png";
    out->create = cal_create;
}
