// tools/power_app/main/power_main.c
// 电源管理 动态应用：.app Bundle 规范版
// 纯 C + LVGL，仅调用固件符号表导出接口。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_modal_mask = NULL;
static lv_obj_t *s_footer_lbl = NULL;
static int s_pending_action = 0; // 1=reboot, 2=shutdown

static void *power_create(void);

static void close_modal(void) {
    if (s_modal_mask != NULL) {
        lv_obj_del(s_modal_mask);
        s_modal_mask = NULL;
    }
}

static void modal_confirm_cb(lv_event_t *e) {
    int action = s_pending_action;
    close_modal();

    if (action == 1) {
        if (s_footer_lbl != NULL) lv_label_set_text(s_footer_lbl, "正在重启系统…");
        dynapp_toast_show("正在重启系统…", 2000);
        dynapp_power_reboot();
    } else if (action == 2) {
        if (s_footer_lbl != NULL) lv_label_set_text(s_footer_lbl, "正在关闭设备…");
        dynapp_toast_show("正在关闭设备…", 2000);
        dynapp_power_shutdown();
    }
}

static void modal_cancel_cb(lv_event_t *e) {
    close_modal();
}

static void show_confirm_dialog(const char *title, const char *msg, int action) {
    if (s_scr == NULL || s_modal_mask != NULL) return;
    s_pending_action = action;

    lv_obj_t *mask = lv_obj_create(s_scr);
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, 480, 800);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_50, 0);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *card = lv_obj_create(mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 380, 240);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 3, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 16, 0);

    lv_obj_t *t_lbl = lv_label_create(card);
    lv_label_set_text(t_lbl, title);
    const lv_font_t *f24 = fontpack_lv_font_get(24, 2);
    if (f24 == NULL) f24 = fontpack_lv_font_ui();
    if (f24 != NULL) lv_obj_set_style_text_font(t_lbl, f24, 0);
    lv_obj_set_style_text_color(t_lbl, lv_color_black(), 0);
    lv_obj_align(t_lbl, LV_ALIGN_TOP_MID, 0, 6);

    lv_obj_t *m_lbl = lv_label_create(card);
    lv_label_set_text(m_lbl, msg);
    const lv_font_t *f18 = fontpack_lv_font_get(18, 0);
    if (f18 == NULL) f18 = fontpack_lv_font_ui();
    if (f18 != NULL) lv_obj_set_style_text_font(m_lbl, f18, 0);
    lv_obj_set_style_text_color(m_lbl, lv_palette_darken(LV_PALETTE_GREY, 3), 0);
    lv_obj_set_style_text_align(m_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(m_lbl, 340);
    lv_obj_align(m_lbl, LV_ALIGN_CENTER, 0, -10);

    // 底部确定与取消按钮
    lv_obj_t *btn_row = lv_obj_create(card);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, 340, 52);
    lv_obj_align(btn_row, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // 取消按钮
    lv_obj_t *cancel_btn = lv_obj_create(btn_row);
    lv_obj_remove_style_all(cancel_btn);
    lv_obj_set_size(cancel_btn, 160, 48);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(cancel_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(cancel_btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(cancel_btn, 2, 0);
    lv_obj_set_style_radius(cancel_btn, 10, 0);
    lv_obj_add_flag(cancel_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cancel_btn, modal_cancel_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *c_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(c_lbl, "取消");
    if (f18 != NULL) lv_obj_set_style_text_font(c_lbl, f18, 0);
    lv_obj_set_style_text_color(c_lbl, lv_color_black(), 0);
    lv_obj_center(c_lbl);

    // 确定按钮（黑底反白）
    lv_obj_t *ok_btn = lv_obj_create(btn_row);
    lv_obj_remove_style_all(ok_btn);
    lv_obj_set_size(ok_btn, 160, 48);
    lv_obj_set_style_bg_color(ok_btn, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ok_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(ok_btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(ok_btn, 2, 0);
    lv_obj_set_style_radius(ok_btn, 10, 0);
    lv_obj_add_flag(ok_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(ok_btn, modal_confirm_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ok_lbl = lv_label_create(ok_btn);
    lv_label_set_text(ok_lbl, "确定");
    if (f18 != NULL) lv_obj_set_style_text_font(ok_lbl, f18, 0);
    lv_obj_set_style_text_color(ok_lbl, lv_color_white(), 0);
    lv_obj_center(ok_lbl);

    s_modal_mask = mask;
}

static void on_sleep_clicked(lv_event_t *e) {
    dynapp_power_sleep();
}

static void on_reboot_clicked(lv_event_t *e) {
    show_confirm_dialog("重启设备", "确定要重启设备吗？\n未保存的数据可能会丢失。", 1);
}

static void on_shutdown_clicked(lv_event_t *e) {
    show_confirm_dialog("关闭设备", "确定要关机吗？\n关机后需长按电源键开机。", 2);
}

static void on_back_clicked(lv_event_t *e) {
    dynapp_screen_go_home();
}

static int power_on_vk_key(const char *key) {
    if (key == NULL) return 0;
    if (s_modal_mask != NULL) {
        close_modal();
        return 1;
    }
    if (strcmp(key, "home") == 0 || strcmp(key, "prev") == 0 || strcmp(key, "vk_prev") == 0) {
        dynapp_screen_go_home();
        return 1;
    }
    return 0;
}

static void power_screen_delete_cb(lv_event_t *e) {
    s_modal_mask = NULL;
    s_scr = NULL;
    s_footer_lbl = NULL;
}

static lv_obj_t *make_big_btn(lv_obj_t *parent, const char *text, lv_event_cb_t cb) {
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, lv_pct(100), 96);
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn, 3, 0);
    lv_obj_set_style_radius(btn, 18, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    const lv_font_t *big_font = fontpack_lv_font_get(34, 2);
    if (big_font == NULL) big_font = fontpack_lv_font_ui();
    if (big_font != NULL) lv_obj_set_style_text_font(lbl, big_font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

static void *power_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 800);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_scr = scr;
    s_modal_mask = NULL;

    lv_obj_add_event_cb(scr, power_screen_delete_cb, LV_EVENT_DELETE, NULL);

    // 1. 系统顶部状态栏
    dynapp_screen_status_bar(scr);

    // 2. 虚拟键
    dynapp_vk_attach(scr, "power", power_create, power_on_vk_key);

    // 3. 顶部 Header (返回按钮 + 标题)
    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, 480, 56);
    lv_obj_set_pos(header, 0, 36);

    lv_obj_t *back_btn = lv_obj_create(header);
    lv_obj_remove_style_all(back_btn);
    lv_obj_set_size(back_btn, 64, 48);
    lv_obj_set_pos(back_btn, 12, 4);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back_btn, on_back_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, "‹ 返回");
    const lv_font_t *f22 = fontpack_lv_font_get(22, 2);
    if (f22 == NULL) f22 = fontpack_lv_font_ui();
    if (f22 != NULL) lv_obj_set_style_text_font(back_lbl, f22, 0);
    lv_obj_set_style_text_color(back_lbl, lv_color_black(), 0);
    lv_obj_center(back_lbl);

    lv_obj_t *title_lbl = lv_label_create(header);
    lv_label_set_text(title_lbl, "电源管理");
    const lv_font_t *f28 = fontpack_lv_font_get(28, 2);
    if (f28 == NULL) f28 = fontpack_lv_font_ui();
    if (f28 != NULL) lv_obj_set_style_text_font(title_lbl, f28, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_black(), 0);
    lv_obj_align(title_lbl, LV_ALIGN_CENTER, 0, 0);

    // 4. 内容容器
    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, 448, 680);
    lv_obj_set_pos(body, 16, 100);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(body, 20, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    make_big_btn(body, "息屏待机", on_sleep_clicked);
    make_big_btn(body, "重启系统", on_reboot_clicked);
    make_big_btn(body, "关机", on_shutdown_clicked);

    // 5. 提示说明
    s_footer_lbl = lv_label_create(body);
    lv_label_set_text(s_footer_lbl, "息屏后按 BOOT/POWER 键唤醒\n关机需长按电源键开机");
    const lv_font_t *f20 = fontpack_lv_font_get(20, 0);
    if (f20 == NULL) f20 = fontpack_lv_font_ui();
    if (f20 != NULL) lv_obj_set_style_text_font(s_footer_lbl, f20, 0);
    lv_obj_set_style_text_color(s_footer_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_style_text_align(s_footer_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_footer_lbl, 420);
    lv_obj_set_style_pad_top(s_footer_lbl, 24, 0);

    return scr;
}

void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "power";
    out->name = "电源管理";
    out->icon = "icon.png";
    out->create = power_create;
}
