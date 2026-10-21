// tools/cloud_app/main/cloud_main.c
// 云工具 动态应用：验证并使用固件新开的两个云接口。
//   - 朗读：把一段文字用 MiniMax TTS 合成为语音并播放
//           （墨水屏可以熄屏只出声，这是电子墨水的独占能力）
//   - 联网：异步 HTTP GET，回调在 LVGL 线程，可直接刷界面
//                （墨水屏当第二屏：一天刷几次即可）
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

static lv_obj_t *s_status = NULL;
static lv_obj_t *s_body = NULL;
static lv_obj_t *s_tts_lbl = NULL;

// HTTP 回调（在 LVGL 线程执行）
static void on_http_resp(const char *data, int len, int status, void *user_data) {
    (void)user_data;
    char buf[64];
    snprintf(buf, sizeof(buf), "HTTP %d · %d 字节", status, len);
    if (s_status != NULL) lv_label_set_text(s_status, buf);
    if (s_body != NULL) {
        char show[512];
        int n = len;
        if (n > (int)sizeof(show) - 1) n = (int)sizeof(show) - 1;
        memcpy(show, data, n);
        show[n] = '\0';
        for (int i = 0; show[i] != '\0'; ++i) {
            if (show[i] == '\n' || show[i] == '\r') show[i] = ' ';
        }
        lv_label_set_text(s_body, show);
    }
}

// TTS 完成回调（LVGL 线程）
static void on_tts_done(int ok, const char *err, void *user_data) {
    (void)user_data;
    if (ok) {
        if (s_tts_lbl != NULL) lv_label_set_text(s_tts_lbl, "朗读完成");
        dynapp_toast_show("朗读完成", 1200);
    } else {
        char b[96];
        snprintf(b, sizeof(b), "失败：%s", (err && err[0]) ? err : "未知");
        if (s_tts_lbl != NULL) lv_label_set_text(s_tts_lbl, b);
        dynapp_toast_show(b, 1800);
    }
}

static void on_tts_clicked(lv_event_t *e) {
    (void)e;
    if (dynapp_tts_available() == 0) {
        dynapp_toast_show("没配置 MiniMax API Key", 1600);
        return;
    }
    const char *text = "墨水屏可以熄屏只出声。这是不烧屏，不耗刷新电，"
                       "电子墨水相对手机和平板独有的能力。";
    if (dynapp_tts_speak_ex(text, on_tts_done, NULL)) {
        dynapp_toast_show("正在合成语音…", 1400);
        if (s_tts_lbl != NULL) lv_label_set_text(s_tts_lbl, "朗读中…");
    } else {
        dynapp_toast_show("排队失败", 1200);
    }
}

static void on_stop_clicked(lv_event_t *e) {
    (void)e;
    dynapp_tts_stop();
    if (s_tts_lbl != NULL) lv_label_set_text(s_tts_lbl, "已停止");
    dynapp_toast_show("已停止", 1000);
}

static void on_http_clicked(lv_event_t *e) {
    (void)e;
    if (s_status != NULL) lv_label_set_text(s_status, "请求中…");
    if (s_body != NULL) lv_label_set_text(s_body, "");
    if (!dynapp_http_get_async("http://127.0.0.1:8080/api/fs/roots", on_http_resp, NULL)) {
        dynapp_toast_show("请求发起失败", 1200);
    }
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                          lv_coord_t h, lv_event_cb_t cb) {
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_pos(btn, x, y);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    if (cb != NULL) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    const lv_font_t *f = fontpack_lv_font_ui();
    if (f != NULL) lv_obj_set_style_text_font(lbl, f, 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

static int cloud_on_vk_key(const char *key) {
    if (key == NULL) return 0;
    if (strcmp(key, "vk_home") == 0 || strcmp(key, "esc") == 0 || strcmp(key, "back") == 0) {
        dynapp_screen_go_home();
        return 1;
    }
    return 0;
}

static void *cloud_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 800);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    dynapp_screen_status_bar(scr);
    dynapp_vk_attach(scr, "cloud", cloud_create, cloud_on_vk_key);

    const lv_font_t *f_ui = fontpack_lv_font_ui();
    const lv_font_t *f_title = fontpack_lv_font_get(30, 2);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "云工具");
    if (f_title != NULL) lv_obj_set_style_text_font(title, f_title, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_set_pos(title, 16, 48);

    s_status = lv_label_create(scr);
    lv_label_set_text(s_status, dynapp_tts_available() ? "TTS 可用 · 未请求" : "TTS 未配置");
    if (f_ui != NULL) lv_obj_set_style_text_font(s_status, f_ui, 0);
    lv_obj_set_style_text_color(s_status, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_pos(s_status, 16, 92);

    s_body = lv_label_create(scr);
    lv_label_set_text(s_body, "点「朗读」用云端 TTS 合成语音；\n点「联网」异步取一个网页。");
    if (f_ui != NULL) lv_obj_set_style_text_font(s_body, f_ui, 0);
    lv_obj_set_style_text_color(s_body, lv_color_black(), 0);
    lv_obj_set_pos(s_body, 16, 140);
    lv_obj_set_width(s_body, 448);

    s_tts_lbl = lv_label_create(scr);
    lv_label_set_text(s_tts_lbl, "");
    if (f_ui != NULL) lv_obj_set_style_text_font(s_tts_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_tts_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_pos(s_tts_lbl, 16, 420);

    const lv_coord_t bw = 107, bh = 56, bgap = 6;
    make_btn(scr, "朗读", 16, 600, bw, bh, on_tts_clicked);
    make_btn(scr, "停止", 16 + bw + bgap, 600, bw, bh, on_stop_clicked);
    make_btn(scr, "联网", 16 + 2 * (bw + bgap), 600, bw, bh, on_http_clicked);
    return scr;
}

void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "cloud";
    out->name = "云工具";
    out->icon = "icon.png";
    out->create = cloud_create;
}
