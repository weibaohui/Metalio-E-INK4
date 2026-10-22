// tools/badge_app/main/badge_main.c
// 电子工牌 动态应用：挂在胸前 / 摆桌上的「身份卡」。
//
// 和「电子名片」分工：
//   - 电子工牌：一张牌一个身份，**大头像 + 大字**，让人隔着半米也能看清你是谁；
//     底部给一张码，别人扫了就能加你。
//   - 电子名片：小头像 + **多个码入口**（微信/企微/飞书/群/主页），递出去交换。
//
// 设计原则（1bpp 墨水屏）：
//   - 大量留白 + 居中排版：工牌是"看"的，不是"读"的。
//   - 层级：姓名 30@2 > 职位 25@2 > 公司/联系方式 灰 25@2，一条细线分段。
//   - 不堆按钮：底部一行小按钮收着，主区域只放身份。
//
// 配置见 tools/_shared/card_kit.h（app 自己的 data/profile.txt）。
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

#include "../../_shared/card_kit.h"

#define BADGE_SCR_W 480
#define BADGE_SCR_H 800
#define BADGE_MARGIN 16
#define BADGE_CONTENT_W (BADGE_SCR_W - BADGE_MARGIN * 2)
#define BADGE_AVATAR 120
#define BADGE_QR_CARD 300   // 码卡（含白边）
#define BADGE_QR_CODE 200   // 二维码本身；四周 50px 白边=静区

static CardSet s_set;
static int s_idx = 0;

static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_avatar_holder = NULL;
static lv_obj_t *s_name_lbl = NULL;
static lv_obj_t *s_title_lbl = NULL;
static lv_obj_t *s_company_lbl = NULL;
static lv_obj_t *s_phone_lbl = NULL;
static lv_obj_t *s_email_lbl = NULL;
static lv_obj_t *s_status_lbl = NULL;
static lv_obj_t *s_qr_holder = NULL;
static lv_obj_t *s_idx_lbl = NULL;

// ================= 字体 =================
static const lv_font_t *ui_font(void) {
    const lv_font_t *f = fontpack_lv_font_ui();
    return f != NULL ? f : fontpack_lv_font_get(25, 2);
}
static const lv_font_t *title_font(void) {
    const lv_font_t *f = fontpack_lv_font_get(30, 2);
    return f != NULL ? f : ui_font();
}
static const lv_font_t *latin_font(void) {
    const lv_font_t *f = fontpack_lv_font_latin(18);
    return f != NULL ? f : ui_font();
}

// ================= 小工具 =================
static lv_obj_t *mk_rect(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *mk_center_label(lv_obj_t *parent, const char *text, const lv_font_t *f, int x, int y,
                                 int w, lv_color_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    if (f != NULL) lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_pos(l, x, y);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static lv_obj_t *mk_button(lv_obj_t *parent, const char *text, int x, int y, int w, int h,
                           lv_event_cb_t cb, void *ud) {
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
    if (cb != NULL) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    const lv_font_t *f = ui_font();
    if (f != NULL) lv_obj_set_style_text_font(lbl, f, 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

// ================= 资料 =================
static void load_profile(void) {
    FILE *fp = (FILE *)dynapp_fopen_data("profile.txt", "r");
    if (fp == NULL) {
        card_default(&s_set);
        return;
    }
    static char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    if (card_parse(buf, &s_set) <= 0) card_default(&s_set);
}

static const CardProfile *current_card(void) {
    if (s_set.n <= 0) return NULL;
    if (s_idx >= s_set.n) s_idx = 0;
    return &s_set.cards[s_idx];
}

// 工牌上那张码：优先用 [qr] 段第一个；没有就 vCard（扫了直接存联系人）
static void badge_qr(const CardProfile *c, CardQr *out) {
    memset(out, 0, sizeof(*out));
    if (c != NULL && c->nqr > 0) {
        *out = c->qrs[0];
        return;
    }
    card_copy(out->label, sizeof(out->label), "名片");
    out->is_image = 0;
    if (c == NULL) return;
    if (c->url[0] != '\0') {
        card_copy(out->content, sizeof(out->content), c->url);
        return;
    }
    char mini[512];
    card_build_mini_vcard(c, mini, sizeof(mini));
    card_copy(out->content, sizeof(out->content), mini);
}

// ================= 渲染 =================
static void render_badge(void) {
    const CardProfile *c = current_card();
    if (c == NULL) return;

    if (s_avatar_holder != NULL) {
        lv_obj_clean(s_avatar_holder);
        card_avatar_render(s_avatar_holder, c->avatar, c->name, BADGE_AVATAR, title_font());
    }
    if (s_name_lbl != NULL) lv_label_set_text(s_name_lbl, c->name);
    if (s_title_lbl != NULL) lv_label_set_text(s_title_lbl, c->title);
    if (s_company_lbl != NULL) {
        char b[72];
        if (c->dept[0] != '\0' && c->empid[0] != '\0')
            snprintf(b, sizeof(b), "%s · %s", c->company, c->empid);
        else if (c->dept[0] != '\0')
            snprintf(b, sizeof(b), "%s · %s", c->company, c->dept);
        else if (c->empid[0] != '\0')
            snprintf(b, sizeof(b), "%s · %s", c->company, c->empid);
        else
            snprintf(b, sizeof(b), "%s", c->company);
        lv_label_set_text(s_company_lbl, b);
    }
    if (s_phone_lbl != NULL) lv_label_set_text(s_phone_lbl, c->phone);
    if (s_email_lbl != NULL) lv_label_set_text(s_email_lbl, c->email);
    if (s_status_lbl != NULL) lv_label_set_text(s_status_lbl, c->status);

    if (s_qr_holder != NULL) {
        lv_obj_clean(s_qr_holder);
        CardQr q;
        badge_qr(c, &q);
        // 码卡：白底 + 细边框，二维码居中留足静区（紧贴码的黑框会让手机扫不出来）
        card_qr_card(s_qr_holder, &q, BADGE_QR_CARD,
                     q.is_image ? BADGE_QR_CARD : BADGE_QR_CODE);
    }

    if (s_idx_lbl != NULL) {
        char b[16];
        snprintf(b, sizeof(b), "%d/%d", s_idx + 1, s_set.n);
        lv_label_set_text(s_idx_lbl, b);
    }
}

// ================= 交互 =================
static void switch_card(int delta) {
    if (s_set.n <= 0) return;
    s_idx = (s_idx + delta + s_set.n) % s_set.n;
    render_badge();
}

static void on_prev(lv_event_t *e) {
    (void)e;
    switch_card(-1);
}

static void on_next(lv_event_t *e) {
    (void)e;
    switch_card(1);
}

static void on_reload(lv_event_t *e) {
    (void)e;
    load_profile();
    s_idx = 0;
    render_badge();
    dynapp_toast_show("已重新读取资料", 1200);
}

static void on_help(lv_event_t *e) {
    (void)e;
    dynapp_modal_alert("怎么改",
                       "把 profile.txt 和头像放进 app 自己的 data 目录，\n"
                       "然后点「重载」。\n\n"
                       "profile.txt 里 [card] 段写资料，\n"
                       "[qr] 段每行一个码：标签|内容\n"
                       "（内容是链接/微信号，或图片文件名）\n\n"
                       "文件用设备 HTTP 接口推：\n"
                       "curl -X POST -H \"Authorization: Bearer <PIN>\"\n"
                       "  -H \"X-Filename: profile.txt\" --data-binary @profile.txt\n"
                       "  \"http://<设备IP>:8080/fs/upload?path=apps/badge.app/data\"",
                       "知道了");
}

static int badge_on_vk_key(const char *key) {
    if (key == NULL) return 0;
    if (strcmp(key, "vk_home") == 0 || strcmp(key, "esc") == 0 || strcmp(key, "back") == 0) {
        dynapp_screen_go_home();
        return 1;
    }
    if (strcmp(key, "vk_prev") == 0) {
        switch_card(-1);
        return 1;
    }
    if (strcmp(key, "vk_next") == 0) {
        switch_card(1);
        return 1;
    }
    return 0;
}

// ================= 生命周期 =================
static void on_screen_deleted(lv_event_t *e) {
    (void)e;
    s_scr = NULL;
    s_avatar_holder = NULL;
    s_name_lbl = NULL;
    s_title_lbl = NULL;
    s_company_lbl = NULL;
    s_phone_lbl = NULL;
    s_email_lbl = NULL;
    s_status_lbl = NULL;
    s_qr_holder = NULL;
    s_idx_lbl = NULL;
}

static void *badge_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    s_scr = scr;
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, BADGE_SCR_W, BADGE_SCR_H);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_screen_deleted, LV_EVENT_DELETE, NULL);

    dynapp_screen_status_bar(scr);
    dynapp_vk_attach(scr, "badge", badge_create, badge_on_vk_key);

    const lv_font_t *f_ui = ui_font();
    const lv_font_t *f_title = title_font();
    const lv_color_t grey = lv_palette_darken(LV_PALETTE_GREY, 2);

    load_profile();

    // ---- 标题行 ----
    mk_center_label(scr, "电子工牌", f_title, BADGE_MARGIN, 36, BADGE_CONTENT_W, lv_color_black());
    s_idx_lbl = mk_center_label(scr, "", latin_font(), BADGE_MARGIN + 300, 46, 148, grey);
    lv_obj_set_style_text_align(s_idx_lbl, LV_TEXT_ALIGN_RIGHT, 0);

    // ---- 头像（居中）----
    s_avatar_holder = lv_obj_create(scr);
    lv_obj_remove_style_all(s_avatar_holder);
    lv_obj_set_pos(s_avatar_holder, (BADGE_SCR_W - BADGE_AVATAR) / 2, 78);
    lv_obj_set_size(s_avatar_holder, BADGE_AVATAR, BADGE_AVATAR);
    lv_obj_clear_flag(s_avatar_holder, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    // ---- 身份三行 ----
    s_name_lbl = mk_center_label(scr, "", f_title, BADGE_MARGIN, 210, BADGE_CONTENT_W, lv_color_black());
    s_title_lbl = mk_center_label(scr, "", f_ui, BADGE_MARGIN, 252, BADGE_CONTENT_W, lv_color_black());
    s_company_lbl = mk_center_label(scr, "", f_ui, BADGE_MARGIN, 288, BADGE_CONTENT_W, grey);

    mk_rect(scr, BADGE_MARGIN + 60, 324, BADGE_CONTENT_W - 120, 1);

    // ---- 码（居中，带细框）----
    s_qr_holder = lv_obj_create(scr);
    lv_obj_remove_style_all(s_qr_holder);
    lv_obj_set_pos(s_qr_holder, (BADGE_SCR_W - BADGE_QR_CARD) / 2, 336);
    lv_obj_set_size(s_qr_holder, BADGE_QR_CARD, BADGE_QR_CARD);
    lv_obj_clear_flag(s_qr_holder, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    // ---- 联系方式 ----
    s_phone_lbl = mk_center_label(scr, "", f_ui, BADGE_MARGIN, 646, BADGE_CONTENT_W, lv_color_black());
    s_email_lbl = mk_center_label(scr, "", f_ui, BADGE_MARGIN, 678, BADGE_CONTENT_W, grey);
    s_status_lbl = mk_center_label(scr, "", f_ui, BADGE_MARGIN, 710, BADGE_CONTENT_W, grey);

    // ---- 底部按钮 ----
    const int bw = 106, gap = 8;
    const int y = 744;
    mk_button(scr, "上一张", BADGE_MARGIN, y, bw, 48, on_prev, NULL);
    mk_button(scr, "下一张", BADGE_MARGIN + (bw + gap), y, bw, 48, on_next, NULL);
    mk_button(scr, "重载", BADGE_MARGIN + (bw + gap) * 2, y, bw, 48, on_reload, NULL);
    mk_button(scr, "怎么改", BADGE_MARGIN + (bw + gap) * 3, y, bw, 48, on_help, NULL);

    render_badge();
    return scr;
}

void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "badge";
    out->name = "电子工牌";
    out->icon = "icon.png";
    out->create = badge_create;
}
