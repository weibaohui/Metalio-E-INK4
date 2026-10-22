// tools/qrcard_app/main/qrcard_main.c
// 电子名片 动态应用：一张能递出去的名片——身份区 + 多个「码入口」。
//
// 设计参考（2026-10 调研）：
//   - 企业微信「对外名片」：头像/姓名/企业/职务/联系方式，再挂 6~7 个自定义对外
//     信息（文字 / 链接 / 小程序三种形式）——对应到墨水屏就是**多个二维码入口**。
//   - 二维彩虹「社交媒体二维码」：把公众号/官网/各渠道链接汇到一页，点哪个跳哪个。
//   墨水屏上就做成：上半身份、下半一个大码，中间一行标签（微信/企微/飞书/群/主页），
//   点标签换码。够大才扫得出来，所以一屏只放一个码。
//
// 两种码：
//   - 文本码（链接/微信号/群链接…）→ LVGL 自带 lv_qrcode 生成；
//   - 图片码（上传的微信个人码等）→ 解码成 L8 位图显示。
//
// 配置见 tools/_shared/card_kit.h 的说明（app 自己的 data/profile.txt）。
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

#include "../../_shared/card_kit.h"

// ================= 布局 =================
#define CARD_SCR_W 480
#define CARD_SCR_H 800
#define CARD_MARGIN 16
#define CARD_CONTENT_W (CARD_SCR_W - CARD_MARGIN * 2) // 448

#define CARD_AVATAR 84
#define CARD_QR_SIZE 280
#define CARD_TAB_H 34
#define CARD_TAB_MAX 8

// ================= 状态 =================
static CardSet s_set;
static int s_qr_idx = 0;

static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_avatar_holder = NULL;
static lv_obj_t *s_name_lbl = NULL;
static lv_obj_t *s_title_lbl = NULL;
static lv_obj_t *s_company_lbl = NULL;
static lv_obj_t *s_contact_lbl = NULL;
static lv_obj_t *s_status_lbl = NULL;
static lv_obj_t *s_qr_holder = NULL;
static lv_obj_t *s_hint_lbl = NULL;
static lv_obj_t *s_tab_lbl[CARD_TAB_MAX] = {NULL};
static lv_obj_t *s_tab_underline = NULL;

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
static lv_obj_t *mk_rect(lv_obj_t *parent, int x, int y, int w, int h, int radius) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(o, 0, 0);
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *mk_label(lv_obj_t *parent, const char *text, const lv_font_t *f, int x, int y,
                          lv_color_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    if (f != NULL) lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, color, 0);
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
    return &s_set.cards[0];
}

// 当前名片上「默认那个码」：没配 [qr] 段时，用 vCard（手机扫了直接存联系人）
static void default_qr_of(const CardProfile *c, CardQr *out) {
    memset(out, 0, sizeof(*out));
    card_copy(out->label, sizeof(out->label), "名片");
    out->is_image = 0;
    if (c != NULL && c->url[0] != '\0') {
        card_copy(out->content, sizeof(out->content), c->url);
    } else if (c != NULL) {
        char mini[512];
        int n = snprintf(mini, sizeof(mini),
                         "BEGIN:VCARD\r\nVERSION:3.0\r\nN:%s;;;;\r\nFN:%s\r\n", c->name, c->name);
        if (c->phone[0] != '\0')
            n += snprintf(mini + n, sizeof(mini) - n, "TEL;TYPE=CELL:%s\r\n", c->phone);
        if (c->email[0] != '\0')
            n += snprintf(mini + n, sizeof(mini) - n, "EMAIL:%s\r\n", c->email);
        snprintf(mini + n, sizeof(mini) - n, "END:VCARD\r\n");
        card_copy(out->content, sizeof(out->content), mini);
    }
}

static int qr_count(const CardProfile *c) {
    return (c != NULL && c->nqr > 0) ? c->nqr : 1;
}

static void get_qr(int idx, CardQr *out) {
    const CardProfile *c = current_card();
    if (c != NULL && c->nqr > 0) {
        if (idx < 0) idx = 0;
        if (idx >= c->nqr) idx = c->nqr - 1;
        *out = c->qrs[idx];
    } else {
        default_qr_of(c, out);
    }
}

// ================= 渲染 =================
static void render_identity(void) {
    const CardProfile *c = current_card();
    if (c == NULL) return;

    if (s_avatar_holder != NULL) {
        lv_obj_clean(s_avatar_holder);
        card_avatar_render(s_avatar_holder, c->avatar, c->name, CARD_AVATAR, title_font());
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

    if (s_contact_lbl != NULL) {
        char b[96];
        if (c->phone[0] != '\0' && c->email[0] != '\0')
            snprintf(b, sizeof(b), "%s  ·  %s", c->phone, c->email);
        else if (c->phone[0] != '\0')
            snprintf(b, sizeof(b), "%s", c->phone);
        else
            snprintf(b, sizeof(b), "%s", c->email);
        lv_label_set_text(s_contact_lbl, b);
    }
    if (s_status_lbl != NULL) lv_label_set_text(s_status_lbl, c->status);
}

static void render_qr(void) {
    const CardProfile *c = current_card();
    const int n = qr_count(c);
    if (s_qr_idx >= n) s_qr_idx = 0;

    if (s_qr_holder != NULL) {
        lv_obj_clean(s_qr_holder);
        CardQr q;
        get_qr(s_qr_idx, &q);
        // 码外面套一圈细边框：让它看起来是「一张卡」而不是浮在白底上
        const int frame = CARD_QR_SIZE + 16;
        lv_obj_t *frame_obj = mk_rect(s_qr_holder, 0, 0, frame, frame, 10);
        lv_obj_set_style_bg_opa(frame_obj, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(frame_obj, lv_color_black(), 0);
        lv_obj_set_style_border_width(frame_obj, 2, 0);
        card_qr_render(frame_obj, &q, CARD_QR_SIZE);
    }

    if (s_hint_lbl != NULL) {
        CardQr q;
        get_qr(s_qr_idx, &q);
        char b[80];
        if (q.is_image)
            snprintf(b, sizeof(b), "%s · 识别图中二维码", q.label);
        else if (strncmp(q.content, "BEGIN:VCARD", 11) == 0)
            snprintf(b, sizeof(b), "%s · 扫码存入通讯录", q.label);
        else if (strncmp(q.content, "http", 4) == 0)
            snprintf(b, sizeof(b), "%s · 扫码打开链接", q.label);
        else
            snprintf(b, sizeof(b), "%s · 扫码查看", q.label);
        lv_label_set_text(s_hint_lbl, b);
    }
}

static void render_tabs(void) {
    const CardProfile *c = current_card();
    const int n = qr_count(c);
    const int cell = CARD_CONTENT_W / (n > 0 ? n : 1);
    const lv_font_t *f = ui_font();
    for (int i = 0; i < CARD_TAB_MAX; ++i) {
        if (s_tab_lbl[i] == NULL) continue;
        if (i >= n) {
            lv_obj_add_flag(s_tab_lbl[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(s_tab_lbl[i], LV_OBJ_FLAG_HIDDEN);
        CardQr q;
        get_qr(i, &q);
        lv_label_set_text(s_tab_lbl[i], q.label);
        lv_obj_set_style_text_color(s_tab_lbl[i], (i == s_qr_idx) ? lv_color_black()
                                                                  : lv_palette_darken(LV_PALETTE_GREY, 2),
                                   0);
        lv_obj_set_width(s_tab_lbl[i], cell);
        lv_obj_set_style_text_align(s_tab_lbl[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(s_tab_lbl[i], CARD_MARGIN + i * cell, 288);
    }
    // 选中项下面一条黑线（1bpp 下没有粗体，用下划线表达"当前"）
    if (s_tab_underline != NULL) {
        if (s_qr_idx < n) {
            lv_obj_set_pos(s_tab_underline, CARD_MARGIN + s_qr_idx * cell + cell / 2 - 18, CARD_TAB_H - 4);
            lv_obj_clear_flag(s_tab_underline, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_tab_underline, LV_OBJ_FLAG_HIDDEN);
        }
    }
    (void)f;
}

static void render_all(void) {
    render_identity();
    render_qr();
    render_tabs();
}

// ================= 交互 =================
static void on_tab_clicked(lv_event_t *e) {
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const CardProfile *c = current_card();
    const int n = qr_count(c);
    if (idx < 0 || idx >= n) return;
    s_qr_idx = idx;
    render_qr();
    render_tabs();
}

static void on_reload(lv_event_t *e) {
    (void)e;
    load_profile();
    s_qr_idx = 0;
    render_all();
    dynapp_toast_show("已重新读取资料", 1200);
}

static void on_help(lv_event_t *e) {
    (void)e;
    dynapp_modal_alert("怎么改",
                       "把 profile.txt 和图片放进 app 自己的 data 目录，\n"
                       "然后点「重载」。\n\n"
                       "profile.txt 里 [card] 段写资料，\n"
                       "[qr] 段每行一个码：标签|内容\n"
                       "（内容是链接/微信号，或图片文件名）\n\n"
                       "文件用设备 HTTP 接口推：\n"
                       "curl -X POST -H \"Authorization: Bearer <PIN>\"\n"
                       "  -H \"X-Filename: profile.txt\" --data-binary @profile.txt\n"
                       "  \"http://<设备IP>:8080/fs/upload?path=apps/qrcard.app/data\"",
                       "知道了");
}

static int qrcard_on_vk_key(const char *key) {
    if (key == NULL) return 0;
    if (strcmp(key, "vk_home") == 0 || strcmp(key, "esc") == 0 || strcmp(key, "back") == 0) {
        dynapp_screen_go_home();
        return 1;
    }
    const CardProfile *c = current_card();
    const int n = qr_count(c);
    if (strcmp(key, "vk_prev") == 0) {
        s_qr_idx = (s_qr_idx - 1 + n) % n;
        render_qr();
        render_tabs();
        return 1;
    }
    if (strcmp(key, "vk_next") == 0) {
        s_qr_idx = (s_qr_idx + 1) % n;
        render_qr();
        render_tabs();
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
    s_contact_lbl = NULL;
    s_status_lbl = NULL;
    s_qr_holder = NULL;
    s_hint_lbl = NULL;
    s_tab_underline = NULL;
    for (int i = 0; i < CARD_TAB_MAX; ++i) s_tab_lbl[i] = NULL;
}

static void *qrcard_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    s_scr = scr;
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, CARD_SCR_W, CARD_SCR_H);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_screen_deleted, LV_EVENT_DELETE, NULL);

    dynapp_screen_status_bar(scr);
    dynapp_vk_attach(scr, "qrcard", qrcard_create, qrcard_on_vk_key);

    const lv_font_t *f_ui = ui_font();
    const lv_font_t *f_title = title_font();
    const lv_color_t grey = lv_palette_darken(LV_PALETTE_GREY, 2);

    load_profile();

    // ---- 标题行 ----
    mk_label(scr, "电子名片", f_title, CARD_MARGIN, 44, lv_color_black());

    // ---- 身份区：头像 + 三行字 ----
    s_avatar_holder = lv_obj_create(scr);
    lv_obj_remove_style_all(s_avatar_holder);
    lv_obj_set_pos(s_avatar_holder, CARD_MARGIN, 92);
    lv_obj_set_size(s_avatar_holder, CARD_AVATAR, CARD_AVATAR);
    lv_obj_clear_flag(s_avatar_holder, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    const int tx = CARD_MARGIN + CARD_AVATAR + 16; // 116
    s_name_lbl = mk_label(scr, "", f_title, tx, 90, lv_color_black());
    s_title_lbl = mk_label(scr, "", f_ui, tx, 132, lv_color_black());
    s_company_lbl = mk_label(scr, "", f_ui, tx, 166, grey);

    // ---- 分隔线 + 联系方式 ----
    mk_rect(scr, CARD_MARGIN, 194, CARD_CONTENT_W, 1, 0);
    s_contact_lbl = mk_label(scr, "", f_ui, CARD_MARGIN, 210, lv_color_black());
    s_status_lbl = mk_label(scr, "", f_ui, CARD_MARGIN, 244, grey);

    // ---- 码标签行 ----
    for (int i = 0; i < CARD_TAB_MAX; ++i) {
        s_tab_lbl[i] = lv_label_create(scr);
        if (f_ui != NULL) lv_obj_set_style_text_font(s_tab_lbl[i], f_ui, 0);
        lv_obj_set_style_text_color(s_tab_lbl[i], grey, 0);
        lv_label_set_long_mode(s_tab_lbl[i], LV_LABEL_LONG_CLIP);
        lv_obj_add_flag(s_tab_lbl[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_tab_lbl[i], on_tab_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_set_pos(s_tab_lbl[i], CARD_MARGIN + i * 56, 288);
    }
    s_tab_underline = mk_rect(scr, CARD_MARGIN, 288 + CARD_TAB_H - 4, 36, 3, 0);

    // ---- 大码 ----
    s_qr_holder = lv_obj_create(scr);
    lv_obj_remove_style_all(s_qr_holder);
    lv_obj_set_pos(s_qr_holder, CARD_MARGIN, 336);
    lv_obj_set_size(s_qr_holder, CARD_CONTENT_W, CARD_QR_SIZE + 16);
    lv_obj_clear_flag(s_qr_holder, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    // ---- 说明 ----
    s_hint_lbl = mk_label(scr, "", f_ui, CARD_MARGIN, 632, lv_color_black());
    lv_obj_set_width(s_hint_lbl, CARD_CONTENT_W);
    lv_obj_set_style_text_align(s_hint_lbl, LV_TEXT_ALIGN_CENTER, 0);

    // ---- 底部按钮 ----
    mk_button(scr, "重载", CARD_MARGIN, 676, 140, 52, on_reload, NULL);
    mk_button(scr, "怎么改", CARD_MARGIN + 148, 676, 140, 52, on_help, NULL);
    {
        // 右侧放张来源角标（拉丁字体，小字）
        lv_obj_t *src = mk_label(scr, "Metalio", latin_font(), CARD_MARGIN + 300, 692, grey);
        lv_obj_set_width(src, 148);
        lv_obj_set_style_text_align(src, LV_TEXT_ALIGN_RIGHT, 0);
    }

    render_all();
    return scr;
}

void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "qrcard";
    out->name = "电子名片";
    out->icon = "icon.png";
    out->create = qrcard_create;
}
