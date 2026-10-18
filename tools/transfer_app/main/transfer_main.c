// tools/transfer_app/main/transfer_main.c
// WiFi 传书 动态应用：严格对齐 2739acf wifi_transfer_screen 完整界面与逻辑
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_wifi_status_lbl = NULL;
static lv_obj_t *s_url_lbl = NULL;
static lv_obj_t *s_switch_btn_lbl = NULL;
static lv_obj_t *s_qr_box = NULL;
static lv_obj_t *s_qr_obj = NULL;
static lv_obj_t *s_qr_tip_lbl = NULL;
static lv_obj_t *s_storage_lbl = NULL;
static lv_obj_t *s_log_lbl = NULL;
static lv_timer_t *s_timer = NULL;

static char s_recent_log[256] = "服务就绪，等待文件传输...";

static void *transfer_create(void);

static const lv_font_t *get_font(int size, int weight) {
    if (size >= 30) {
        const lv_font_t *f = fontpack_lv_font_get(30, 2);
        if (f) return f;
    }
    return fontpack_lv_font_ui();
}

static void on_back_clicked(lv_event_t *e) {
    (void)e;
    dynapp_screen_go_home();
}

static int transfer_on_vk_key(const char* key) {
    if (key != NULL && (strcmp(key, "esc") == 0 || strcmp(key, "back") == 0 || strcmp(key, "vk_home") == 0 || strcmp(key, "home") == 0)) {
        dynapp_screen_go_home();
        return 1;
    }
    return 0;
}

static lv_obj_t *make_card(lv_obj_t *parent, lv_coord_t pad) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, pad, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return card;
}

static void format_size(size_t bytes, char* out, size_t out_len) {
    if (bytes >= 1024 * 1024 * 1024) {
        unsigned gb = (unsigned)(bytes / (1024 * 1024 * 1024));
        unsigned rem = (unsigned)(((bytes % (1024 * 1024 * 1024)) * 10) / (1024 * 1024 * 1024));
        snprintf(out, out_len, "%u.%u GB", gb, rem);
    } else if (bytes >= 1024 * 1024) {
        unsigned mb = (unsigned)(bytes / (1024 * 1024));
        unsigned rem = (unsigned)(((bytes % (1024 * 1024)) * 10) / (1024 * 1024));
        snprintf(out, out_len, "%u.%u MB", mb, rem);
    } else {
        snprintf(out, out_len, "%u KB", (unsigned)(bytes / 1024));
    }
}

static void refresh_ui(void) {
    if (!s_scr || !lv_obj_is_valid(s_scr)) return;

    int is_net = dynapp_is_network_ready();
    int running = dynapp_wifi_transfer_is_running();
    char ip[32] = {0};
    int has_ip = dynapp_get_ip_address(ip, sizeof(ip));
    char url[128] = {0};
    dynapp_wifi_transfer_get_url(url, sizeof(url));

    // 1. WiFi 状态与 URL
    if (s_wifi_status_lbl) {
        if (is_net && has_ip && ip[0] != '\0') {
            char buf[64];
            snprintf(buf, sizeof(buf), "WiFi 状态: 已连接 (IP: %s)", ip);
            lv_label_set_text(s_wifi_status_lbl, buf);
        } else {
            lv_label_set_text(s_wifi_status_lbl, "WiFi 状态: 未连接网络 (请在设置中连接 WiFi)");
        }
    }

    if (s_url_lbl) {
        if (running && url[0] != '\0') {
            lv_label_set_text(s_url_lbl, url);
        } else if (!is_net) {
            lv_label_set_text(s_url_lbl, "未连接 WiFi");
        } else {
            lv_label_set_text(s_url_lbl, "服务未运行 (请点击下方启动)");
        }
    }

    if (s_switch_btn_lbl) {
        lv_label_set_text(s_switch_btn_lbl, running ? "停止传书服务" : "启动传书服务");
    }

    // 2. 二维码展示与更新
    if (s_qr_box && s_qr_obj) {
        if (running && url[0] != '\0') {
            lv_qrcode_update(s_qr_obj, url, strlen(url));
            lv_obj_clear_flag(s_qr_box, LV_OBJ_FLAG_HIDDEN);
            if (s_qr_tip_lbl) {
                lv_label_set_text(s_qr_tip_lbl, "手机微信 / 相机扫码打开");
            }
        } else {
            lv_obj_add_flag(s_qr_box, LV_OBJ_FLAG_HIDDEN);
            if (s_qr_tip_lbl) {
                lv_label_set_text(s_qr_tip_lbl, "请先启动传书服务以生成直达二维码");
            }
        }
    }

    // 3. 存储容量
    if (s_storage_lbl) {
        char free_str[32] = {0}, total_str[32] = {0};
        if (dynapp_backup_get_sd_capacity(free_str, sizeof(free_str), total_str, sizeof(total_str))) {
            char sbuf[96];
            snprintf(sbuf, sizeof(sbuf), "存储卡可用空间: %s (总容量: %s)", free_str, total_str);
            lv_label_set_text(s_storage_lbl, sbuf);
        } else {
            lv_label_set_text(s_storage_lbl, "存储卡可用空间: --");
        }
    }

    // 4. 动态日志
    if (s_log_lbl) {
        lv_label_set_text(s_log_lbl, s_recent_log);
    }
}

static void on_toggle_clicked(lv_event_t *e) {
    (void)e;
    if (dynapp_wifi_transfer_is_running()) {
        dynapp_wifi_transfer_stop();
        snprintf(s_recent_log, sizeof(s_recent_log), "传书服务已停止");
        dynapp_toast_show("WiFi 传书服务已停止", 1500);
    } else {
        if (!dynapp_is_network_ready()) {
            dynapp_modal_alert("提示", "当前设备未连接 WiFi，请先进入设置连接 WiFi 后再启动传书服务。", "我知道了");
            return;
        }
        if (dynapp_wifi_transfer_start()) {
            snprintf(s_recent_log, sizeof(s_recent_log), "服务已启动，请在浏览器中输入网址访问");
            dynapp_toast_show("WiFi 传书服务已启动", 1500);
        } else {
            dynapp_modal_alert("错误", "启动传书服务失败，端口可能已被占用", "确定");
        }
    }
    refresh_ui();
}

static void on_upload_done_cb(const char* filename, size_t size_bytes, void* user_data) {
    (void)user_data;
    char sz_str[32];
    format_size(size_bytes, sz_str, sizeof(sz_str));
    snprintf(s_recent_log, sizeof(s_recent_log), "✓ 成功接收: %s (%s)", filename ? filename : "未知", sz_str);
    if (s_log_lbl && lv_obj_is_valid(s_log_lbl)) {
        lv_label_set_text(s_log_lbl, s_recent_log);
    }
    char toast[64];
    snprintf(toast, sizeof(toast), "已收到新书: %s", filename ? filename : "");
    dynapp_toast_show(toast, 2500);
}

static void timer_cb(lv_timer_t *tmr) {
    (void)tmr;
    refresh_ui();
}

static void on_screen_deleted(lv_event_t *e) {
    (void)e;
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    dynapp_wifi_transfer_set_callback(NULL, NULL);
    dynapp_wifi_transfer_stop();

    s_scr = NULL;
    s_wifi_status_lbl = NULL;
    s_url_lbl = NULL;
    s_switch_btn_lbl = NULL;
    s_qr_box = NULL;
    s_qr_obj = NULL;
    s_qr_tip_lbl = NULL;
    s_storage_lbl = NULL;
    s_log_lbl = NULL;
}

static void *transfer_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    s_scr = scr;
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 800);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(scr, on_screen_deleted, LV_EVENT_DELETE, NULL);

    dynapp_screen_status_bar(scr);
    dynapp_vk_attach(scr, "transfer", transfer_create, transfer_on_vk_key);

    const lv_font_t *f_ui = fontpack_lv_font_ui();
    const lv_font_t *f22 = get_font(22, 2);
    const lv_font_t *f24 = get_font(24, 2);
    const lv_font_t *f26 = get_font(26, 2);
    const lv_font_t *f28 = get_font(28, 2);

    // 主滚动容器 (无返回顶栏，由底部 vk_home 键统一控制返回)
    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_pos(body, 14, 42);
    lv_obj_set_size(body, 452, 750);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 10, 0);

    // 1. 顶部卡片：电脑/手机浏览器访问
    lv_obj_t *card1 = make_card(body, 12);

    lv_obj_t *c1_title = lv_label_create(card1);
    lv_label_set_text(c1_title, "电脑 / 手机浏览器访问");
    if (f24) lv_obj_set_style_text_font(c1_title, f24, 0);
    lv_obj_set_style_text_color(c1_title, lv_color_black(), 0);

    s_wifi_status_lbl = lv_label_create(card1);
    lv_obj_set_width(s_wifi_status_lbl, lv_pct(100));
    lv_label_set_long_mode(s_wifi_status_lbl, LV_LABEL_LONG_WRAP);
    if (f_ui) lv_obj_set_style_text_font(s_wifi_status_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_wifi_status_lbl, lv_color_black(), 0);

    lv_obj_t *tip_h = lv_label_create(card1);
    lv_label_set_text(tip_h, "同局域网浏览器输入下方网址即可传书:");
    lv_obj_set_width(tip_h, lv_pct(100));
    lv_label_set_long_mode(tip_h, LV_LABEL_LONG_WRAP);
    if (f_ui) lv_obj_set_style_text_font(tip_h, f_ui, 0);
    lv_obj_set_style_text_color(tip_h, lv_color_black(), 0);

    s_url_lbl = lv_label_create(card1);
    lv_obj_set_width(s_url_lbl, lv_pct(100));
    lv_label_set_long_mode(s_url_lbl, LV_LABEL_LONG_WRAP);
    if (f28) lv_obj_set_style_text_font(s_url_lbl, f28, 0);
    lv_obj_set_style_text_color(s_url_lbl, lv_color_black(), 0);

    lv_obj_t *btn = lv_obj_create(card1);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, lv_pct(100), 42);
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, on_toggle_clicked, LV_EVENT_CLICKED, NULL);

    s_switch_btn_lbl = lv_label_create(btn);
    lv_label_set_text(s_switch_btn_lbl, "启动传书服务");
    if (f_ui) lv_obj_set_style_text_font(s_switch_btn_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_switch_btn_lbl, lv_color_black(), 0);
    lv_obj_center(s_switch_btn_lbl);

    s_storage_lbl = lv_label_create(card1);
    lv_obj_set_width(s_storage_lbl, lv_pct(100));
    lv_label_set_long_mode(s_storage_lbl, LV_LABEL_LONG_WRAP);
    if (f_ui) lv_obj_set_style_text_font(s_storage_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_storage_lbl, lv_color_black(), 0);

    lv_obj_t *fmt_lbl = lv_label_create(card1);
    lv_label_set_text(fmt_lbl, "支持格式: .txt / .epub / .ebook / .pdf (直存图书库)");
    lv_obj_set_width(fmt_lbl, lv_pct(100));
    lv_label_set_long_mode(fmt_lbl, LV_LABEL_LONG_WRAP);
    if (f_ui) lv_obj_set_style_text_font(fmt_lbl, f_ui, 0);
    lv_obj_set_style_text_color(fmt_lbl, lv_color_black(), 0);

    // 2. 中部卡片：手机扫码直达
    lv_obj_t *card2 = make_card(body, 12);
    lv_obj_set_flex_align(card2, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *qr_title = lv_label_create(card2);
    lv_label_set_text(qr_title, "手机扫码直达");
    lv_obj_set_width(qr_title, lv_pct(100));
    lv_obj_set_style_text_align(qr_title, LV_TEXT_ALIGN_CENTER, 0);
    if (f24) lv_obj_set_style_text_font(qr_title, f24, 0);
    lv_obj_set_style_text_color(qr_title, lv_color_black(), 0);

    s_qr_tip_lbl = lv_label_create(card2);
    lv_label_set_text(s_qr_tip_lbl, "手机微信 / 相机扫码打开");
    lv_obj_set_width(s_qr_tip_lbl, lv_pct(100));
    lv_obj_set_style_text_align(s_qr_tip_lbl, LV_TEXT_ALIGN_CENTER, 0);
    if (f_ui) lv_obj_set_style_text_font(s_qr_tip_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_qr_tip_lbl, lv_color_black(), 0);

    s_qr_box = lv_obj_create(card2);
    lv_obj_remove_style_all(s_qr_box);
    lv_obj_set_size(s_qr_box, 200, 200);
    lv_obj_set_style_bg_color(s_qr_box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_qr_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_qr_box, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_qr_box, 2, 0);
    lv_obj_set_style_radius(s_qr_box, 8, 0);
    lv_obj_clear_flag(s_qr_box, LV_OBJ_FLAG_SCROLLABLE);

    s_qr_obj = lv_qrcode_create(s_qr_box);
    lv_qrcode_set_size(s_qr_obj, 186);
    lv_qrcode_set_dark_color(s_qr_obj, lv_color_black());
    lv_qrcode_set_light_color(s_qr_obj, lv_color_white());
    lv_obj_center(s_qr_obj);

    lv_obj_t *qr_subtip = lv_label_create(card2);
    lv_label_set_text(qr_subtip, "免输网址 · 手机直接选书上传");
    lv_obj_set_width(qr_subtip, lv_pct(100));
    lv_obj_set_style_text_align(qr_subtip, LV_TEXT_ALIGN_CENTER, 0);
    if (f_ui) lv_obj_set_style_text_font(qr_subtip, f_ui, 0);
    lv_obj_set_style_text_color(qr_subtip, lv_color_black(), 0);

    // 3. 底部卡片：实时动态记录
    lv_obj_t *card3 = make_card(body, 10);

    lv_obj_t *l_title = lv_label_create(card3);
    lv_label_set_text(l_title, "实时传输动态");
    if (f24) lv_obj_set_style_text_font(l_title, f24, 0);
    lv_obj_set_style_text_color(l_title, lv_color_black(), 0);

    s_log_lbl = lv_label_create(card3);
    lv_obj_set_width(s_log_lbl, lv_pct(100));
    lv_label_set_long_mode(s_log_lbl, LV_LABEL_LONG_WRAP);
    if (f_ui) lv_obj_set_style_text_font(s_log_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_log_lbl, lv_color_black(), 0);

    lv_obj_t *hint = lv_label_create(card3);
    lv_label_set_text(hint, "提示: 退出本界面将自动关闭 Web 传书服务，零常驻后台内存。");
    lv_obj_set_width(hint, lv_pct(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    if (f_ui) lv_obj_set_style_text_font(hint, f_ui, 0);
    lv_obj_set_style_text_color(hint, lv_color_black(), 0);

    // 注册文件上传回调
    dynapp_wifi_transfer_set_callback(on_upload_done_cb, NULL);

    refresh_ui();

    s_timer = lv_timer_create(timer_cb, 2000, NULL);
    return scr;
}

void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "transfer";
    out->name = "WiFi 传书";
    out->icon = "icon.png";
    out->create = transfer_create;
}
