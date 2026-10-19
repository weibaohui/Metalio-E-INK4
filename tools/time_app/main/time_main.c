// tools/time_app/main/time_main.c
// 时间校准 动态应用：.app Bundle 规范版
// 纯 C + LVGL，仅调用固件符号表导出接口。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

static lv_obj_t *s_scr = NULL;
static lv_timer_t *s_timer = NULL;
static int s_alive = 0;

static lv_obj_t *s_clock_lbl = NULL;
static lv_obj_t *s_date_lbl = NULL;
static lv_obj_t *s_status_lbl = NULL;
static lv_obj_t *s_last_sync_lbl = NULL;

static lv_obj_t *s_tz_lbl = NULL;
static lv_obj_t *s_man_date_lbl = NULL;
static lv_obj_t *s_man_time_lbl = NULL;

static int s_year = 2026, s_mon = 1, s_day = 1, s_hour = 12, s_min = 0;

static void *time_create(void);

static void adjust_manual_date(int delta_days, int delta_hours, int delta_mins) {
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_year = s_year - 1900;
    t.tm_mon = s_mon - 1;
    t.tm_mday = s_day + delta_days;
    t.tm_hour = s_hour + delta_hours;
    t.tm_min = s_min + delta_mins;
    time_t sec = mktime(&t);
    struct tm res;
    localtime_r(&sec, &res);
    s_year = res.tm_year + 1900;
    s_mon = res.tm_mon + 1;
    s_day = res.tm_mday;
    s_hour = res.tm_hour;
    s_min = res.tm_min;
}

static void refresh_manual_display(void) {
    if (s_man_date_lbl != NULL) {
        char b[32];
        snprintf(b, sizeof(b), "%04d-%02d-%02d", s_year, s_mon, s_day);
        lv_label_set_text(s_man_date_lbl, b);
    }
    if (s_man_time_lbl != NULL) {
        char b[16];
        snprintf(b, sizeof(b), "%02d:%02d", s_hour, s_min);
        lv_label_set_text(s_man_time_lbl, b);
    }
}

static void refresh_tz_display(void) {
    if (s_tz_lbl == NULL) return;
    int minutes = dynapp_time_get_tz_offset_min();
    char buf[48];
    if (minutes == 480) {
        snprintf(buf, sizeof(buf), "UTC+8 (北京时间)");
    } else if (minutes == 540) {
        snprintf(buf, sizeof(buf), "UTC+9 (东京时间)");
    } else if (minutes == 0) {
        snprintf(buf, sizeof(buf), "UTC+0 (伦敦时间)");
    } else if (minutes == -300) {
        snprintf(buf, sizeof(buf), "UTC-5 (纽约时间)");
    } else {
        char sign = minutes >= 0 ? '+' : '-';
        int a = abs(minutes);
        snprintf(buf, sizeof(buf), "UTC%c%d:%02d", sign, a / 60, a % 60);
    }
    lv_label_set_text(s_tz_lbl, buf);
}

static void refresh_clock(void) {
    if (!s_alive || s_clock_lbl == NULL) return;

    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);

    if (local.tm_year + 1900 >= 2025) {
        char tb[16];
        snprintf(tb, sizeof(tb), "%02d:%02d:%02d", local.tm_hour, local.tm_min, local.tm_sec);
        lv_label_set_text(s_clock_lbl, tb);

        static const char *const kWdays[] = {"星期日", "星期一", "星期二", "星期三", "星期四", "星期五", "星期六"};
        const char *wday_str = (local.tm_wday >= 0 && local.tm_wday <= 6) ? kWdays[local.tm_wday] : "";
        char db[64];
        snprintf(db, sizeof(db), "%04d年%02d月%02d日  %s",
                 local.tm_year + 1900, local.tm_mon + 1, local.tm_mday, wday_str);
        lv_label_set_text(s_date_lbl, db);
    } else {
        lv_label_set_text(s_clock_lbl, "--:--:--");
        lv_label_set_text(s_date_lbl, "未初始化时钟");
    }

    if (s_status_lbl != NULL) {
        int net = dynapp_is_network_ready();
        int synced = dynapp_time_is_synced();
        if (synced) {
            lv_label_set_text(s_status_lbl, "● 对时状态: 已完成网络同步");
        } else if (net) {
            lv_label_set_text(s_status_lbl, "◌ 对时状态: 正在向SNTP同步…");
        } else {
            lv_label_set_text(s_status_lbl, "○ 对时状态: 未联网(本地RTC运行)");
        }
    }

    if (s_last_sync_lbl != NULL) {
        long last_epoch = dynapp_time_last_sync_epoch();
        if (last_epoch > 0) {
            time_t t = (time_t)last_epoch;
            struct tm tm_last;
            localtime_r(&t, &tm_last);
            char b[64];
            snprintf(b, sizeof(b), "上次对时: %04d-%02d-%02d %02d:%02d",
                     tm_last.tm_year + 1900, tm_last.tm_mon + 1, tm_last.tm_mday,
                     tm_last.tm_hour, tm_last.tm_min);
            lv_label_set_text(s_last_sync_lbl, b);
        } else {
            lv_label_set_text(s_last_sync_lbl, "上次对时: 尚未联网对时");
        }
    }
}

static void on_timer_tick(lv_timer_t *t) {
    refresh_clock();
}

static void on_sync_now(lv_event_t *e) {
    dynapp_time_sync_now();
    if (s_status_lbl != NULL) {
        lv_label_set_text(s_status_lbl, "◌ 对时状态: 正在向SNTP同步…");
    }
    dynapp_toast_show("已触发网络对时请求", 1500);
}

static void on_tz_step(lv_event_t *e) {
    intptr_t delta = (intptr_t)lv_event_get_user_data(e);
    int cur = dynapp_time_get_tz_offset_min();
    dynapp_time_set_tz_offset_min(cur + (int)delta);
    refresh_tz_display();
    refresh_clock();
}

static void on_tz_default(lv_event_t *e) {
    dynapp_time_set_tz_offset_min(480);
    refresh_tz_display();
    refresh_clock();
}

static void on_adjust_date(lv_event_t *e) {
    intptr_t delta = (intptr_t)lv_event_get_user_data(e);
    adjust_manual_date((int)delta, 0, 0);
    refresh_manual_display();
}

static void on_adjust_hour(lv_event_t *e) {
    intptr_t delta = (intptr_t)lv_event_get_user_data(e);
    adjust_manual_date(0, (int)delta, 0);
    refresh_manual_display();
}

static void on_adjust_min(lv_event_t *e) {
    intptr_t delta = (intptr_t)lv_event_get_user_data(e);
    adjust_manual_date(0, 0, (int)delta);
    refresh_manual_display();
}

static void on_manual_save(lv_event_t *e) {
    if (dynapp_time_set_manual(s_year, s_mon, s_day, s_hour, s_min)) {
        refresh_clock();
        dynapp_toast_show("时间已校准并同步至RTC", 2000);
    } else {
        dynapp_toast_show("时间参数无效", 2000);
    }
}

static void on_back_clicked(lv_event_t *e) {
    dynapp_screen_go_home();
}

static int time_on_vk_key(const char *key) {
    if (key == NULL) return 0;
    if (strcmp(key, "home") == 0 || strcmp(key, "vk_home") == 0 || strcmp(key, "prev") == 0 || strcmp(key, "vk_prev") == 0) {
        dynapp_screen_go_home();
        return 1;
    }
    return 0;
}

static void time_screen_delete_cb(lv_event_t *e) {
    s_alive = 0;
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    s_scr = NULL;
    s_clock_lbl = NULL;
    s_date_lbl = NULL;
    s_status_lbl = NULL;
    s_last_sync_lbl = NULL;
    s_tz_lbl = NULL;
    s_man_date_lbl = NULL;
    s_man_time_lbl = NULL;
}

static lv_obj_t *make_card(lv_obj_t *parent, lv_coord_t w, lv_coord_t h) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, w, h);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    return card;
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, lv_coord_t w, lv_coord_t h,
                          lv_event_cb_t cb, void *user_data, int invert) {
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, invert ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    const lv_font_t *f = fontpack_lv_font_get(18, 0);
    if (f == NULL) f = fontpack_lv_font_ui();
    if (f != NULL) lv_obj_set_style_text_font(lbl, f, 0);
    lv_obj_set_style_text_color(lbl, invert ? lv_color_white() : lv_color_black(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

static void *time_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 800);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_scr = scr;
    s_alive = 1;

    lv_obj_add_event_cb(scr, time_screen_delete_cb, LV_EVENT_DELETE, NULL);

    // 1. 系统顶部状态栏
    dynapp_screen_status_bar(scr);

    // 2. 虚拟键
    dynapp_vk_attach(scr, "time", time_create, time_on_vk_key);

    // 日期/时区小标签字体(部分字号缺字形时返回 NULL,下方使用均带保护)
    const lv_font_t *f22 = fontpack_lv_font_get(22, 2);

    // 3. 内容列容器 (无返回顶栏，由底部 vk_home 键统一控制返回)
    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, 448, 750);
    lv_obj_set_pos(body, 16, 42);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(body, 10, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    // ==================== 卡片 1: 系统时钟与对时状态 ====================
    lv_obj_t *c1 = make_card(body, 448, 270);
    lv_obj_set_flex_flow(c1, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c1, 4, 0);

    lv_obj_t *c1_tag = lv_label_create(c1);
    lv_label_set_text(c1_tag, "当前系统时间 (RTC)");
    const lv_font_t *f18 = fontpack_lv_font_get(18, 0);
    if (f18 == NULL) f18 = fontpack_lv_font_ui();
    if (f18 != NULL) lv_obj_set_style_text_font(c1_tag, f18, 0);
    lv_obj_set_style_text_color(c1_tag, lv_palette_darken(LV_PALETTE_GREY, 3), 0);
    lv_obj_set_width(c1_tag, 420);
    lv_obj_set_style_text_align(c1_tag, LV_TEXT_ALIGN_CENTER, 0);

    s_clock_lbl = lv_label_create(c1);
    lv_label_set_text(s_clock_lbl, "--:--:--");
    const lv_font_t *f56 = fontpack_lv_font_get(56, 2);
    if (f56 == NULL) f56 = fontpack_lv_font_ui();
    if (f56 != NULL) lv_obj_set_style_text_font(s_clock_lbl, f56, 0);
    lv_obj_set_style_text_color(s_clock_lbl, lv_color_black(), 0);
    lv_obj_set_width(s_clock_lbl, 420);
    lv_obj_set_style_text_align(s_clock_lbl, LV_TEXT_ALIGN_CENTER, 0);

    s_date_lbl = lv_label_create(c1);
    lv_label_set_text(s_date_lbl, "----年--月--日");
    if (f22 != NULL) lv_obj_set_style_text_font(s_date_lbl, f22, 0);
    lv_obj_set_style_text_color(s_date_lbl, lv_color_black(), 0);
    lv_obj_set_width(s_date_lbl, 420);
    lv_obj_set_style_text_align(s_date_lbl, LV_TEXT_ALIGN_CENTER, 0);

    lv_obj_t *div = lv_obj_create(c1);
    lv_obj_remove_style_all(div);
    lv_obj_set_size(div, 420, 1);
    lv_obj_set_style_bg_color(div, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(div, LV_OPA_COVER, 0);
    lv_obj_set_style_margin_ver(div, 4, 0);

    s_status_lbl = lv_label_create(c1);
    lv_label_set_text(s_status_lbl, "○ 对时状态: 检测中…");
    if (f18 != NULL) lv_obj_set_style_text_font(s_status_lbl, f18, 0);
    lv_obj_set_style_text_color(s_status_lbl, lv_color_black(), 0);

    s_last_sync_lbl = lv_label_create(c1);
    lv_label_set_text(s_last_sync_lbl, "上次对时: 尚未对时");
    if (f18 != NULL) lv_obj_set_style_text_font(s_last_sync_lbl, f18, 0);
    lv_obj_set_style_text_color(s_last_sync_lbl, lv_color_black(), 0);

    lv_obj_t *sntp_btn = make_btn(c1, "立即联网对时 (SNTP)", 420, 42, on_sync_now, NULL, 1);
    lv_obj_set_style_margin_top(sntp_btn, 6, 0);

    // ==================== 卡片 2: 时区配置 ====================
    lv_obj_t *c2 = make_card(body, 448, 102);
    lv_obj_set_flex_flow(c2, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c2, 6, 0);

    lv_obj_t *c2_row1 = lv_obj_create(c2);
    lv_obj_remove_style_all(c2_row1);
    lv_obj_set_size(c2_row1, 420, 24);
    lv_obj_set_flex_flow(c2_row1, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c2_row1, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *tz_tag = lv_label_create(c2_row1);
    lv_label_set_text(tz_tag, "时区配置");
    if (f22 != NULL) lv_obj_set_style_text_font(tz_tag, f22, 0);
    lv_obj_set_style_text_color(tz_tag, lv_color_black(), 0);

    s_tz_lbl = lv_label_create(c2_row1);
    lv_label_set_text(s_tz_lbl, "UTC+8 (北京时间)");
    if (f18 != NULL) lv_obj_set_style_text_font(s_tz_lbl, f18, 0);
    lv_obj_set_style_text_color(s_tz_lbl, lv_color_black(), 0);

    lv_obj_t *c2_row2 = lv_obj_create(c2);
    lv_obj_remove_style_all(c2_row2);
    lv_obj_set_size(c2_row2, 420, 42);
    lv_obj_set_flex_flow(c2_row2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c2_row2, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    make_btn(c2_row2, "- 1小时", 124, 40, on_tz_step, (void *)(intptr_t)-60, 0);
    make_btn(c2_row2, "默认(UTC+8)", 148, 40, on_tz_default, NULL, 0);
    make_btn(c2_row2, "+ 1小时", 124, 40, on_tz_step, (void *)(intptr_t)60, 0);

    // ==================== 卡片 3: 手动校准 ====================
    lv_obj_t *c3 = make_card(body, 448, 200);
    lv_obj_set_flex_flow(c3, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c3, 8, 0);

    lv_obj_t *c3_tag = lv_label_create(c3);
    lv_label_set_text(c3_tag, "手动校准 (无网络时纠正时钟)");
    if (f22 != NULL) lv_obj_set_style_text_font(c3_tag, f22, 0);
    lv_obj_set_style_text_color(c3_tag, lv_color_black(), 0);

    // 日期行
    lv_obj_t *d_row = lv_obj_create(c3);
    lv_obj_remove_style_all(d_row);
    lv_obj_set_size(d_row, 420, 36);
    lv_obj_set_flex_flow(d_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(d_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_man_date_lbl = lv_label_create(d_row);
    lv_label_set_text(s_man_date_lbl, "2026-10-18");
    if (f18 != NULL) lv_obj_set_style_text_font(s_man_date_lbl, f18, 0);
    lv_obj_set_style_text_color(s_man_date_lbl, lv_color_black(), 0);

    lv_obj_t *d_btns = lv_obj_create(d_row);
    lv_obj_remove_style_all(d_btns);
    lv_obj_set_size(d_btns, 180, 36);
    lv_obj_set_flex_flow(d_btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(d_btns, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    make_btn(d_btns, "-1天", 84, 34, on_adjust_date, (void *)(intptr_t)-1, 0);
    make_btn(d_btns, "+1天", 84, 34, on_adjust_date, (void *)(intptr_t)1, 0);

    // 时间行
    lv_obj_t *t_row = lv_obj_create(c3);
    lv_obj_remove_style_all(t_row);
    lv_obj_set_size(t_row, 420, 36);
    lv_obj_set_flex_flow(t_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(t_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_man_time_lbl = lv_label_create(t_row);
    lv_label_set_text(s_man_time_lbl, "12:00");
    if (f18 != NULL) lv_obj_set_style_text_font(s_man_time_lbl, f18, 0);
    lv_obj_set_style_text_color(s_man_time_lbl, lv_color_black(), 0);

    lv_obj_t *t_btns = lv_obj_create(t_row);
    lv_obj_remove_style_all(t_btns);
    lv_obj_set_size(t_btns, 240, 36);
    lv_obj_set_flex_flow(t_btns, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(t_btns, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    make_btn(t_btns, "-1时", 54, 34, on_adjust_hour, (void *)(intptr_t)-1, 0);
    make_btn(t_btns, "+1时", 54, 34, on_adjust_hour, (void *)(intptr_t)1, 0);
    make_btn(t_btns, "-1分", 54, 34, on_adjust_min, (void *)(intptr_t)-1, 0);
    make_btn(t_btns, "+1分", 54, 34, on_adjust_min, (void *)(intptr_t)1, 0);

    // 保存按钮
    make_btn(c3, "保存并写入硬件 RTC", 420, 40, on_manual_save, NULL, 0);

    // 初始化手动时间变量
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    s_year = local.tm_year + 1900;
    s_mon = local.tm_mon + 1;
    s_day = local.tm_mday;
    s_hour = local.tm_hour;
    s_min = local.tm_min;

    // 初始刷新
    refresh_clock();
    refresh_tz_display();
    refresh_manual_display();

    // 启动 1 秒定时器
    s_timer = lv_timer_create(on_timer_tick, 1000, NULL);

    return scr;
}

void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "time";
    out->name = "时间校准";
    out->icon = "icon.png";
    out->create = time_create;
}
