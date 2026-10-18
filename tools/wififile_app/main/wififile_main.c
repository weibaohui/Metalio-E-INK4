// tools/wififile_app/main/wififile_main.c
// WiFi 文件 动态应用：.app Bundle 规范版
// 纯 C + LVGL，仅调用固件符号表导出接口。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_btn_lbl = NULL;
static lv_obj_t *s_status_lbl = NULL;
static lv_obj_t *s_url_lbl = NULL;
static lv_obj_t *s_tip_lbl = NULL;
static lv_timer_t *s_timer = NULL;

static void *wififile_create(void);

static const lv_font_t *get_font(int size, int weight) {
    const lv_font_t *f = fontpack_lv_font_get(size, weight);
    if (f == NULL) f = fontpack_lv_font_ui();
    return f;
}

static void on_back_clicked(lv_event_t *e) {
    (void)e;
    dynapp_screen_go_home();
}

static int wififile_on_vk_key(const char* key) {
    if (key != NULL && (strcmp(key, "esc") == 0 || strcmp(key, "back") == 0)) {
        dynapp_screen_go_home();
        return 1;
    }
    return 0;
}

static void update_ui(void) {
    if (!s_scr || !lv_obj_is_valid(s_scr)) return;

    int running = dynapp_wifi_file_is_running();
    char ip[32] = {0};
    int has_ip = dynapp_get_ip_address(ip, sizeof(ip));
    int port = dynapp_wifi_file_get_port();
    if (port <= 0) port = 8080;

    if (running) {
        lv_label_set_text(s_btn_lbl, "停止服务");
        if (has_ip && ip[0] != '\0') {
            char sbuf[128], ubuf[128];
            snprintf(sbuf, sizeof(sbuf), "服务状态: 运行中 (IP: %s)", ip);
            snprintf(ubuf, sizeof(ubuf), "http://%s:%d/file", ip, port);
            lv_label_set_text(s_status_lbl, sbuf);
            lv_label_set_text(s_url_lbl, ubuf);
            lv_label_set_text(s_tip_lbl, "请在电脑或手机浏览器中打开上方网址\n即可无线管理、上传、下载 SD 卡中的文件。");
        } else {
            lv_label_set_text(s_status_lbl, "服务状态: 正在等待 WiFi 连接分配 IP...");
            lv_label_set_text(s_url_lbl, "等待网络就绪...");
            lv_label_set_text(s_tip_lbl, "请确保设备已连入 WiFi 网络。");
        }
    } else {
        lv_label_set_text(s_btn_lbl, "启动服务");
        lv_label_set_text(s_status_lbl, "服务状态: 已停止");
        lv_label_set_text(s_url_lbl, "点击右上角「启动服务」开始");
        lv_label_set_text(s_tip_lbl, "启动服务后，可在局域网网页中直接访问本设备文件系统。");
    }
}

static void on_toggle_clicked(lv_event_t *e) {
    (void)e;
    if (dynapp_wifi_file_is_running()) {
        dynapp_wifi_file_stop();
        dynapp_toast_show("WiFi 文件服务已停止", 1500);
    } else {
        dynapp_wifi_file_start();
        dynapp_toast_show("WiFi 文件服务已启动", 1500);
    }
    update_ui();
}

static void timer_cb(lv_timer_t *tmr) {
    (void)tmr;
    update_ui();
}

static void on_screen_deleted(lv_event_t *e) {
    (void)e;
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    s_scr = NULL;
    s_btn_lbl = NULL;
    s_status_lbl = NULL;
    s_url_lbl = NULL;
    s_tip_lbl = NULL;
    dynapp_wifi_file_stop();
}

static void *wififile_create(void) {
    dynapp_wifi_file_start();

    lv_obj_t *scr = lv_obj_create(NULL);
    s_scr = scr;
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 800);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(scr, on_screen_deleted, LV_EVENT_DELETE, NULL);

    dynapp_vk_attach(scr, "wififile", wififile_create, wififile_on_vk_key);

    // 顶部 Header
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
    const lv_font_t *f22 = get_font(22, 2);
    if (f22) lv_obj_set_style_text_font(back_lbl, f22, 0);
    lv_obj_set_style_text_color(back_lbl, lv_color_black(), 0);
    lv_obj_center(back_lbl);

    lv_obj_t *title_lbl = lv_label_create(header);
    lv_label_set_text(title_lbl, "WiFi 文件");
    const lv_font_t *f26 = get_font(26, 2);
    if (f26) lv_obj_set_style_text_font(title_lbl, f26, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_black(), 0);
    lv_obj_align(title_lbl, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *tog_btn = lv_obj_create(header);
    lv_obj_remove_style_all(tog_btn);
    lv_obj_set_size(tog_btn, 88, 38);
    lv_obj_align(tog_btn, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_set_style_bg_color(tog_btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(tog_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(tog_btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(tog_btn, 2, 0);
    lv_obj_set_style_radius(tog_btn, 8, 0);
    lv_obj_add_flag(tog_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(tog_btn, on_toggle_clicked, LV_EVENT_CLICKED, NULL);

    s_btn_lbl = lv_label_create(tog_btn);
    lv_label_set_text(s_btn_lbl, "停止服务");
    const lv_font_t *f18 = get_font(18, 0);
    if (f18) lv_obj_set_style_text_font(s_btn_lbl, f18, 0);
    lv_obj_set_style_text_color(s_btn_lbl, lv_color_black(), 0);
    lv_obj_center(s_btn_lbl);

    // 主内容区
    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, 456, 696);
    lv_obj_set_pos(body, 12, 96);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 16, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    // 服务状态卡片
    lv_obj_t *card_status = lv_obj_create(body);
    lv_obj_remove_style_all(card_status);
    lv_obj_set_width(card_status, lv_pct(100));
    lv_obj_set_height(card_status, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card_status, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card_status, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card_status, lv_color_black(), 0);
    lv_obj_set_style_border_width(card_status, 2, 0);
    lv_obj_set_style_radius(card_status, 14, 0);
    lv_obj_set_style_pad_all(card_status, 16, 0);
    lv_obj_set_flex_flow(card_status, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card_status, 8, 0);
    lv_obj_clear_flag(card_status, LV_OBJ_FLAG_SCROLLABLE);

    s_status_lbl = lv_label_create(card_status);
    lv_label_set_text(s_status_lbl, "服务状态: 初始化中...");
    const lv_font_t *f20 = get_font(20, 2);
    if (f20) lv_obj_set_style_text_font(s_status_lbl, f20, 0);

    // 网址卡片
    lv_obj_t *card_url = lv_obj_create(body);
    lv_obj_remove_style_all(card_url);
    lv_obj_set_width(card_url, lv_pct(100));
    lv_obj_set_height(card_url, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card_url, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card_url, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card_url, lv_color_black(), 0);
    lv_obj_set_style_border_width(card_url, 3, 0);
    lv_obj_set_style_radius(card_url, 14, 0);
    lv_obj_set_style_pad_all(card_url, 16, 0);
    lv_obj_set_flex_flow(card_url, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card_url, 8, 0);
    lv_obj_clear_flag(card_url, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *url_title = lv_label_create(card_url);
    lv_label_set_text(url_title, "局域网访问地址：");
    if (f18) lv_obj_set_style_text_font(url_title, f18, 0);

    s_url_lbl = lv_label_create(card_url);
    lv_label_set_text(s_url_lbl, "http://...");
    const lv_font_t *f24 = get_font(24, 2);
    if (f24) lv_obj_set_style_text_font(s_url_lbl, f24, 0);
    lv_obj_set_width(s_url_lbl, lv_pct(100));
    lv_label_set_long_mode(s_url_lbl, LV_LABEL_LONG_WRAP);

    // 说明卡片
    lv_obj_t *card_info = lv_obj_create(body);
    lv_obj_remove_style_all(card_info);
    lv_obj_set_width(card_info, lv_pct(100));
    lv_obj_set_height(card_info, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card_info, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card_info, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card_info, lv_color_black(), 0);
    lv_obj_set_style_border_width(card_info, 2, 0);
    lv_obj_set_style_radius(card_info, 14, 0);
    lv_obj_set_style_pad_all(card_info, 16, 0);
    lv_obj_set_flex_flow(card_info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card_info, 8, 0);
    lv_obj_clear_flag(card_info, LV_OBJ_FLAG_SCROLLABLE);

    s_tip_lbl = lv_label_create(card_info);
    lv_label_set_text(s_tip_lbl, "请在电脑或手机浏览器中打开上方网址\n即可无线管理、上传、下载 SD 卡中的文件。");
    const lv_font_t *f16 = get_font(16, 0);
    if (f16) lv_obj_set_style_text_font(s_tip_lbl, f16, 0);
    lv_obj_set_width(s_tip_lbl, lv_pct(100));
    lv_label_set_long_mode(s_tip_lbl, LV_LABEL_LONG_WRAP);

    update_ui();
    s_timer = lv_timer_create(timer_cb, 2000, NULL);

    return scr;
}

void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "wififile";
    out->name = "WiFi 文件";
    out->icon = "icon.png";
    out->create = wififile_create;
}
