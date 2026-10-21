// tools/qrcard_app/main/qrcard_main.c
// 二维码名片 动态应用：把个人资料编成 vCard 二维码，手机扫一下直接存联系人。
//
// 设计要点（墨水屏 480x800 / 1bpp 单色）：
//   - 二维码用 LVGL 自带的 lv_qrcode（画在 I1 单色 canvas 上，天然适配墨水屏），
//     不依赖任何外部库；SDK 已把 lv_qrcode_* 导出给动态应用用。
//   - 资料从 app 自己的 data/profile.txt 读（动态应用只能读自己的目录），
//     没配就用内置默认资料，开机即用。
//   - 一份资料里可写多张名片（[card] 分段），上下键切换。
//   - 墨水屏刷新慢：整页只在切换名片/二维码内容变化时刷一次，不做动画。
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

#include "../../_shared/badge_profile.h"

#define QR_SIZE 240

static ProfileSet s_set;
static int s_idx = 0;

static lv_obj_t *s_qr = NULL;
static lv_obj_t *s_title_lbl = NULL;
static lv_obj_t *s_idx_lbl = NULL;
static lv_obj_t *s_name_lbl = NULL;
static lv_obj_t *s_org_lbl = NULL;
static lv_obj_t *s_phone_lbl = NULL;
static lv_obj_t *s_email_lbl = NULL;
static lv_obj_t *s_status_lbl = NULL;
static lv_obj_t *s_help_lbl = NULL;
static lv_obj_t *s_help_obj = NULL;
static int s_help_shown = 0;

static lv_obj_t *s_scr = NULL;

// ---- 字体 ----
static const lv_font_t *ui_font(void) {
    const lv_font_t *f = fontpack_lv_font_ui();
    return f != NULL ? f : fontpack_lv_font_get(25, 2);
}
static const lv_font_t *title_font(void) {
    const lv_font_t *f = fontpack_lv_font_get(30, 2);
    return f != NULL ? f : ui_font();
}

// ================= 资料载入 =================
static void load_profile(void) {
    FILE *fp = (FILE *)dynapp_fopen_data("profile.txt", "r");
    if (fp == NULL) {
        profile_default(&s_set);
        return;
    }
    static char buf[4096];
    size_t total = 0;
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    total = n;
    fclose(fp);
    buf[total] = '\0';
    if (profile_parse(buf, &s_set) <= 0) profile_default(&s_set);
}

// ================= 渲染 =================
static void apply_card(void) {
    if (s_set.n <= 0) profile_default(&s_set);
    if (s_idx >= s_set.n) s_idx = 0;
    const ProfileCard *c = &s_set.cards[s_idx];

    // 二维码内容：vCard，手机扫了能直接存联系人
    static char vcard[1024];
    profile_build_vcard(c, vcard, sizeof(vcard));
    if (s_qr != NULL) lv_qrcode_update(s_qr, vcard, strlen(vcard));

    if (s_title_lbl != NULL) lv_label_set_text(s_title_lbl, "名片");
    if (s_idx_lbl != NULL) {
        char b[16];
        snprintf(b, sizeof(b), "%d/%d", s_idx + 1, s_set.n);
        lv_label_set_text(s_idx_lbl, b);
    }
    if (s_name_lbl != NULL) lv_label_set_text(s_name_lbl, c->name);
    if (s_org_lbl != NULL) {
        char b[96];
        if (c->title[0] != '\0' && c->company[0] != '\0')
            snprintf(b, sizeof(b), "%s · %s", c->title, c->company);
        else if (c->title[0] != '\0')
            snprintf(b, sizeof(b), "%s", c->title);
        else
            snprintf(b, sizeof(b), "%s", c->company);
        lv_label_set_text(s_org_lbl, b);
    }
    if (s_phone_lbl != NULL) lv_label_set_text(s_phone_lbl, c->phone);
    if (s_email_lbl != NULL) lv_label_set_text(s_email_lbl, c->email);
    if (s_status_lbl != NULL) {
        const char *t = "扫二维码存联系人 · 上下键换名片";
        if (c->status[0] != '\0') {
            char b[80];
            snprintf(b, sizeof(b), "%s", c->status);
            lv_label_set_text(s_status_lbl, b);
        } else {
            lv_label_set_text(s_status_lbl, t);
        }
    }
}

static void show_help(int show) {
    s_help_shown = show;
    if (s_help_obj == NULL) return;
    if (show) {
        lv_obj_clear_flag(s_help_obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_help_obj, LV_OBJ_FLAG_HIDDEN);
    }
}

// ================= 交互 =================
static void on_prev_clicked(lv_event_t *e) {
    (void)e;
    if (s_set.n <= 0) return;
    s_idx = (s_idx + s_set.n - 1) % s_set.n;
    apply_card();
    dynapp_epd_full_refresh();
}

static void on_next_clicked(lv_event_t *e) {
    (void)e;
    if (s_set.n <= 0) return;
    s_idx = (s_idx + 1) % s_set.n;
    apply_card();
    dynapp_epd_full_refresh();
}

static void on_reload_clicked(lv_event_t *e) {
    (void)e;
    // 动态应用的实例是缓存复用的：改完 profile.txt 不会自动重新读取，
    // 必须在这里手动重载（否则得重启设备才能看到新资料）。
    load_profile();
    apply_card();
    dynapp_toast_show("资料已重新载入", 1400);
    dynapp_epd_full_refresh();
}

static void on_help_clicked(lv_event_t *e) {
    (void)e;
    show_help(!s_help_shown);
    dynapp_epd_full_refresh();
}

static int qrcard_on_vk_key(const char *key) {
    if (key == NULL) return 0;
    if (strcmp(key, "vk_home") == 0 || strcmp(key, "esc") == 0 || strcmp(key, "back") == 0) {
        dynapp_screen_go_home();
        return 1;
    }
    if (strcmp(key, "vk_prev") == 0) {
        on_prev_clicked(NULL);
        return 1;
    }
    if (strcmp(key, "vk_next") == 0) {
        on_next_clicked(NULL);
        return 1;
    }
    return 0;
}

// ================= 界面 =================
static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                          lv_coord_t h, lv_event_cb_t cb, void *user_data) {
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
    if (cb != NULL) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    const lv_font_t *f = ui_font();
    if (f != NULL) lv_obj_set_style_text_font(lbl, f, 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

static void on_screen_deleted(lv_event_t *e) {
    (void)e;
    s_scr = NULL;
    s_qr = NULL;
    s_title_lbl = NULL;
    s_idx_lbl = NULL;
    s_name_lbl = NULL;
    s_org_lbl = NULL;
    s_phone_lbl = NULL;
    s_email_lbl = NULL;
    s_status_lbl = NULL;
    s_help_lbl = NULL;
    s_help_obj = NULL;
}

static void *qrcard_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    s_scr = scr;
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 800);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_screen_deleted, LV_EVENT_DELETE, NULL);

    dynapp_screen_status_bar(scr);
    dynapp_vk_attach(scr, "qrcard", qrcard_create, qrcard_on_vk_key);

    load_profile();

    const lv_font_t *f_ui = ui_font();
    const lv_font_t *f_title = title_font();

    // 1. 标题
    s_title_lbl = lv_label_create(scr);
    lv_label_set_text(s_title_lbl, "名片");
    if (f_title != NULL) lv_obj_set_style_text_font(s_title_lbl, f_title, 0);
    lv_obj_set_style_text_color(s_title_lbl, lv_color_black(), 0);
    lv_obj_set_pos(s_title_lbl, 16, 48);

    s_idx_lbl = lv_label_create(scr);
    if (f_ui != NULL) lv_obj_set_style_text_font(s_idx_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_idx_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_align(s_idx_lbl, LV_ALIGN_TOP_RIGHT, -16, 56);

    // 2. 二维码
    s_qr = lv_qrcode_create(scr);
    lv_qrcode_set_size(s_qr, QR_SIZE);
    lv_qrcode_set_dark_color(s_qr, lv_color_black());
    lv_qrcode_set_light_color(s_qr, lv_color_white());
    lv_obj_set_pos(s_qr, (480 - QR_SIZE) / 2, 92);

    // 3. 资料
    s_name_lbl = lv_label_create(scr);
    if (f_title != NULL) lv_obj_set_style_text_font(s_name_lbl, f_title, 0);
    lv_obj_set_style_text_color(s_name_lbl, lv_color_black(), 0);
    lv_obj_set_pos(s_name_lbl, 16, 348);

    s_org_lbl = lv_label_create(scr);
    if (f_ui != NULL) lv_obj_set_style_text_font(s_org_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_org_lbl, lv_color_black(), 0);
    lv_obj_set_pos(s_org_lbl, 16, 390);

    s_phone_lbl = lv_label_create(scr);
    if (f_ui != NULL) lv_obj_set_style_text_font(s_phone_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_phone_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_pos(s_phone_lbl, 16, 428);

    s_email_lbl = lv_label_create(scr);
    if (f_ui != NULL) lv_obj_set_style_text_font(s_email_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_email_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_pos(s_email_lbl, 16, 464);

    s_status_lbl = lv_label_create(scr);
    if (f_ui != NULL) lv_obj_set_style_text_font(s_status_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_status_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_pos(s_status_lbl, 16, 508);

    // 4. 帮助浮层（怎么改资料）
    s_help_obj = lv_obj_create(scr);
    lv_obj_remove_style_all(s_help_obj);
    lv_obj_set_pos(s_help_obj, 16, 96);
    lv_obj_set_size(s_help_obj, 448, 450);
    lv_obj_set_style_bg_color(s_help_obj, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_help_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_help_obj, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_help_obj, 2, 0);
    lv_obj_clear_flag(s_help_obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    {
        const char *lines[] = {
            "怎么改资料",
            "",
            "① 写一个 profile.txt：",
            "   [card]",
            "   name=你的名字",
            "   phone=13800000000",
            "   email=you@example.com",
            "",
            "② 传到设备（网页或 curl）：",
            "   apps/qrcard.app/data/",
            "",
            "③ 回到 app 点「重载」生效",
            "",
            "写多段 [card] 就是多张名片",
        };
        for (int i = 0; i < (int)(sizeof(lines) / sizeof(lines[0])); ++i) {
            lv_obj_t *l = lv_label_create(s_help_obj);
            lv_label_set_text(l, lines[i]);
            if (f_ui != NULL) lv_obj_set_style_text_font(l, f_ui, 0);
            lv_obj_set_style_text_color(l, lv_color_black(), 0);
            lv_obj_set_pos(l, 12, 12 + i * 30);
        }
    }
    lv_obj_add_flag(s_help_obj, LV_OBJ_FLAG_HIDDEN);

    // 5. 按钮
    const lv_coord_t bw = 107, bh = 56, bgap = 6;
    const lv_coord_t by = 600;
    make_btn(scr, "上一张", 16, by, bw, bh, on_prev_clicked, NULL);
    make_btn(scr, "下一张", 16 + bw + bgap, by, bw, bh, on_next_clicked, NULL);
    make_btn(scr, "重载", 16 + 2 * (bw + bgap), by, bw, bh, on_reload_clicked, NULL);
    make_btn(scr, "怎么改", 16 + 3 * (bw + bgap), by, bw, bh, on_help_clicked, NULL);

    apply_card();
    return scr;
}

void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "qrcard";
    out->name = "二维码名片";
    out->icon = "icon.png";
    out->create = qrcard_create;
}
