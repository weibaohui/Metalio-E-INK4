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
static lv_obj_t *s_footer_lbl = NULL;

static void *power_create(void);

static void on_sleep_clicked(lv_event_t *e) {
    (void)e;
    dynapp_power_sleep();
}

static void on_reboot_confirmed(int confirmed, void *user_data) {
    (void)user_data;
    if (confirmed) {
        if (s_footer_lbl != NULL) lv_label_set_text(s_footer_lbl, "正在重启…");
        dynapp_toast_show("正在重启…", 2000);
        dynapp_power_reboot();
    }
}

static void on_reboot_clicked(lv_event_t *e) {
    (void)e;
    dynapp_modal_dialog(
        "重启设备",
        "确定要重启设备吗？未保存的数据可能会丢失。",
        "确定重启",
        "取消",
        on_reboot_confirmed,
        NULL);
}

static void on_shutdown_confirmed(int confirmed, void *user_data) {
    (void)user_data;
    if (confirmed) {
        if (s_footer_lbl != NULL) lv_label_set_text(s_footer_lbl, "正在关机…");
        dynapp_toast_show("正在关机…", 2000);
        dynapp_power_shutdown();
    }
}

static void on_shutdown_clicked(lv_event_t *e) {
    (void)e;
    dynapp_modal_dialog(
        "关闭设备",
        "确定要关机吗？关机后需长按电源键开机。",
        "确定关机",
        "取消",
        on_shutdown_confirmed,
        NULL);
}

static void on_back_clicked(lv_event_t *e) {
    dynapp_screen_go_home();
}

static int power_on_vk_key(const char *key) {
    if (key == NULL) return 0;
    if (strcmp(key, "home") == 0 || strcmp(key, "vk_home") == 0 || strcmp(key, "prev") == 0 ||
        strcmp(key, "vk_prev") == 0 || strcmp(key, "esc") == 0 || strcmp(key, "back") == 0) {
        dynapp_screen_go_home();
        return 1;
    }
    return 0;
}

static void power_screen_delete_cb(lv_event_t *e) {
    (void)e;
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

    lv_obj_add_event_cb(scr, power_screen_delete_cb, LV_EVENT_DELETE, NULL);

    // 1. 系统顶部状态栏
    dynapp_screen_status_bar(scr);

    // 2. 虚拟键
    dynapp_vk_attach(scr, "power", power_create, power_on_vk_key);

    // 3. 内容容器 (无返回顶栏，由底部 vk_home 键统一控制返回)
    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, 448, 720);
    lv_obj_set_pos(body, 16, 52);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(body, 28, 0);
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
