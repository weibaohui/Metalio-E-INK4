// tools/paycode_app/main/paycode_main.c
// 收款码 动态应用：把微信 / 支付宝收款码摆在墨水屏上。
//
// 为什么单独做一个（而不是塞进电子名片）：
//   收款码的诉求完全不同——名片是"我是谁"，收款码是"收钱"。场景也不同：
//   摊主/小店把设备立在台面上，客人扫码付款，屏幕要**大、清楚、只有一个码**。
//   塞进名片里跟一堆身份信息挤在一起，反而两边都看不清。
//
// 借鉴市面上的做法（2026-10 调研）：
//   - 「双码并存」是痛点 → 聚合码（微信+支付宝二合一）；聚合码的原理是
//     **一个 URL 码，扫码后按 User-Agent 判断是微信还是支付宝再跳对应收款码**。
//     但那需要一台公网服务器做跳转——墨水屏设备是纯离线的，做不了。
//     所以这里用最诚实的形式：**多张收款码摆出来，点标签切换**。
//   - 「支付活码」：多张码轮换（商家换号也不影响收款）→ 这里支持配多张同名/备用码。
//   - 收款码被偷换的防盗思路（码中间加 logo、品牌色、边框）→ 整块界面做成
//     规整的"码卡"，看一眼就知道是自己的。
//
// 配置（app 自己的 data/pay.txt）：
//   [code]
//   微信收款|wechat.png|13800000000
//   支付宝|alipay.png|商户名
//   备用微信|wechat2.png
// 图片放 app 自己的 data 目录，格式 PNG/JPG 都行（自动灰度 + Bayer 抖动适配 1bpp）。
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

#include "../../_shared/card_kit.h"

#define PAY_SCR_W 480
#define PAY_SCR_H 800
#define PAY_MARGIN 16
#define PAY_CONTENT_W (PAY_SCR_W - PAY_MARGIN * 2)
#define PAY_IMG_SIZE 400
#define PAY_TAB_H 34
#define PAY_MAX_CODES 8

typedef struct {
    char label[20];
    char file[64];
    char note[48];
} PayCode;

static PayCode s_codes[PAY_MAX_CODES];
static int s_n = 0;
static int s_idx = 0;

static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_img_holder = NULL;
static lv_obj_t *s_full_holder = NULL;
static lv_obj_t *s_full_img_holder = NULL;
static lv_obj_t *s_label_lbl = NULL;
static lv_obj_t *s_note_lbl = NULL;
static lv_obj_t *s_tab_lbl[PAY_MAX_CODES] = {NULL};
static lv_obj_t *s_tab_underline = NULL;
static lv_obj_t *s_empty_lbl = NULL;

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

static lv_obj_t *mk_label(lv_obj_t *parent, const char *text, const lv_font_t *f, int x, int y, int w,
                          lv_color_t color, lv_text_align_t align) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    if (f != NULL) lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_width(l, w);
    lv_obj_set_style_text_align(l, align, 0);
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

// ================= 配置 =================
static void load_codes(void) {
    s_n = 0;
    FILE *fp = (FILE *)dynapp_fopen_data("pay.txt", "r");
    if (fp == NULL) return;
    char line[256];
    while (fgets(line, sizeof(line), fp) != NULL && s_n < PAY_MAX_CODES) {
        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '\0' || *s == '\n' || *s == '#' || *s == '[') continue;
        card_rtrim(s);
        // 标签|图片文件名|备注（备注可省）
        char *bar1 = strchr(s, '|');
        if (bar1 == NULL) continue;
        *bar1 = '\0';
        char *bar2 = strchr(bar1 + 1, '|');
        if (bar2 != NULL) *bar2 = '\0';
        PayCode *c = &s_codes[s_n];
        card_copy(c->label, sizeof(c->label), s);
        card_rtrim(c->label);
        card_copy(c->file, sizeof(c->file), bar1 + 1);
        card_rtrim(c->file);
        if (bar2 != NULL) {
            card_copy(c->note, sizeof(c->note), bar2 + 1);
            card_rtrim(c->note);
        }
        if (c->file[0] == '\0') continue;
        if (c->label[0] == '\0') card_copy(c->label, sizeof(c->label), c->file);
        s_n++;
    }
    fclose(fp);
}

// ================= 渲染 =================
static void render_image(lv_obj_t *holder, int size) {
    if (holder == NULL) return;
    lv_obj_clean(holder);
    if (s_n <= 0) return;
    if (s_idx >= s_n) s_idx = 0;
    char path[192];
    card_data_path(path, sizeof(path), s_codes[s_idx].file);
    lv_obj_t *img = card_make_l8_image(holder, path, size, size);
    if (img == NULL) {
        // 图片缺失/解码失败：给个明确的空态，而不是白屏
        mk_label(holder, "图片读不到", ui_font(), 0, size / 2 - 16, size, lv_color_black(),
                 LV_TEXT_ALIGN_CENTER);
    }
}

static void render_tabs(void) {
    const int cell = PAY_CONTENT_W / (s_n > 0 ? s_n : 1);
    for (int i = 0; i < PAY_MAX_CODES; ++i) {
        if (s_tab_lbl[i] == NULL) continue;
        if (i >= s_n) {
            lv_obj_add_flag(s_tab_lbl[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_clear_flag(s_tab_lbl[i], LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_tab_lbl[i], s_codes[i].label);
        lv_obj_set_style_text_color(s_tab_lbl[i], (i == s_idx) ? lv_color_black()
                                                               : lv_palette_darken(LV_PALETTE_GREY, 2),
                                   0);
        lv_obj_set_width(s_tab_lbl[i], cell);
        lv_obj_set_style_text_align(s_tab_lbl[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(s_tab_lbl[i], PAY_MARGIN + i * cell, 92);
    }
    if (s_tab_underline != NULL) {
        if (s_idx < s_n) {
            lv_obj_set_pos(s_tab_underline, PAY_MARGIN + s_idx * cell + cell / 2 - 18, PAY_TAB_H - 4);
            lv_obj_clear_flag(s_tab_underline, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_tab_underline, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void render_all(void) {
    if (s_n <= 0) {
        if (s_empty_lbl != NULL) lv_obj_clear_flag(s_empty_lbl, LV_OBJ_FLAG_HIDDEN);
        if (s_label_lbl != NULL) lv_label_set_text(s_label_lbl, "还没有收款码");
        if (s_note_lbl != NULL) lv_label_set_text(s_note_lbl, "");
        render_image(s_img_holder, PAY_IMG_SIZE);
        for (int i = 0; i < PAY_MAX_CODES; ++i) {
            if (s_tab_lbl[i] != NULL) lv_obj_add_flag(s_tab_lbl[i], LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    if (s_empty_lbl != NULL) lv_obj_add_flag(s_empty_lbl, LV_OBJ_FLAG_HIDDEN);
    render_image(s_img_holder, PAY_IMG_SIZE);
    render_tabs();
    if (s_label_lbl != NULL) lv_label_set_text(s_label_lbl, s_codes[s_idx].label);
    if (s_note_lbl != NULL) {
        char b[80];
        snprintf(b, sizeof(b), "%s", s_codes[s_idx].note);
        lv_label_set_text(s_note_lbl, b);
    }
}

// ================= 交互 =================
static void on_tab_clicked(lv_event_t *e) {
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= s_n) return;
    s_idx = idx;
    render_all();
}

static void on_img_clicked(lv_event_t *e) {
    (void)e;
    if (s_n <= 0) return;
    if (s_full_holder == NULL) return;
    // 全屏展示：递给客人时屏幕只剩一个码，最清楚
    render_image(s_full_img_holder, PAY_SCR_W - 40);
    lv_obj_clear_flag(s_full_holder, LV_OBJ_FLAG_HIDDEN);
    dynapp_epd_full_refresh();
}

static void on_full_clicked(lv_event_t *e) {
    (void)e;
    if (s_full_holder != NULL) lv_obj_add_flag(s_full_holder, LV_OBJ_FLAG_HIDDEN);
    dynapp_epd_full_refresh();
}

static void on_reload(lv_event_t *e) {
    (void)e;
    load_codes();
    s_idx = 0;
    render_all();
    dynapp_toast_show(s_n > 0 ? "已重新读取收款码" : "没有找到收款码", 1400);
}

static void on_help(lv_event_t *e) {
    (void)e;
    dynapp_modal_alert("怎么改",
                       "1. 把收款码图片（微信/支付宝）放进\n"
                       "   app 自己的 data 目录，\n"
                       "   例如 apps/paycode.app/data/wechat.png\n\n"
                       "2. 在同一个目录写 pay.txt：\n"
                       "   [code]\n"
                       "   微信收款|wechat.png|13800000000\n"
                       "   支付宝|alipay.png|商户名\n\n"
                       "3. 点「重载」生效。\n\n"
                       "点一下码可以全屏展示，递给客人看。",
                       "知道了");
}

static int paycode_on_vk_key(const char *key) {
    if (key == NULL) return 0;
    if (strcmp(key, "vk_home") == 0 || strcmp(key, "esc") == 0 || strcmp(key, "back") == 0) {
        if (s_full_holder != NULL && !lv_obj_has_flag(s_full_holder, LV_OBJ_FLAG_HIDDEN)) {
            lv_obj_add_flag(s_full_holder, LV_OBJ_FLAG_HIDDEN);
            return 1;
        }
        dynapp_screen_go_home();
        return 1;
    }
    if (strcmp(key, "vk_prev") == 0 && s_n > 0) {
        s_idx = (s_idx - 1 + s_n) % s_n;
        render_all();
        return 1;
    }
    if (strcmp(key, "vk_next") == 0 && s_n > 0) {
        s_idx = (s_idx + 1) % s_n;
        render_all();
        return 1;
    }
    return 0;
}

// ================= 生命周期 =================
static void on_screen_deleted(lv_event_t *e) {
    (void)e;
    s_scr = NULL;
    s_img_holder = NULL;
    s_full_holder = NULL;
    s_full_img_holder = NULL;
    s_label_lbl = NULL;
    s_note_lbl = NULL;
    s_tab_underline = NULL;
    s_empty_lbl = NULL;
    for (int i = 0; i < PAY_MAX_CODES; ++i) s_tab_lbl[i] = NULL;
}

static void *paycode_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    s_scr = scr;
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, PAY_SCR_W, PAY_SCR_H);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_screen_deleted, LV_EVENT_DELETE, NULL);

    dynapp_screen_status_bar(scr);
    dynapp_vk_attach(scr, "paycode", paycode_create, paycode_on_vk_key);

    const lv_font_t *f_ui = ui_font();
    const lv_font_t *f_title = title_font();
    const lv_color_t grey = lv_palette_darken(LV_PALETTE_GREY, 2);

    load_codes();

    // ---- 标题 ----
    mk_label(scr, "收款码", f_title, PAY_MARGIN, 44, PAY_CONTENT_W, lv_color_black(), LV_TEXT_ALIGN_LEFT);

    // ---- 标签行 ----
    for (int i = 0; i < PAY_MAX_CODES; ++i) {
        s_tab_lbl[i] = lv_label_create(scr);
        if (f_ui != NULL) lv_obj_set_style_text_font(s_tab_lbl[i], f_ui, 0);
        lv_obj_set_style_text_color(s_tab_lbl[i], grey, 0);
        lv_label_set_long_mode(s_tab_lbl[i], LV_LABEL_LONG_CLIP);
        lv_obj_add_flag(s_tab_lbl[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_tab_lbl[i], on_tab_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_set_pos(s_tab_lbl[i], PAY_MARGIN + i * 56, 92);
    }
    s_tab_underline = mk_rect(scr, PAY_MARGIN, 92 + PAY_TAB_H - 4, 36, 3);

    // ---- 大码 ----
    s_img_holder = lv_obj_create(scr);
    lv_obj_remove_style_all(s_img_holder);
    lv_obj_set_pos(s_img_holder, (PAY_SCR_W - PAY_IMG_SIZE) / 2, 136);
    lv_obj_set_size(s_img_holder, PAY_IMG_SIZE, PAY_IMG_SIZE);
    lv_obj_add_flag(s_img_holder, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_img_holder, on_img_clicked, LV_EVENT_CLICKED, NULL);

    // ---- 空态提示 ----
    s_empty_lbl = mk_label(scr,
                           "把收款码图片放进 data 目录\n"
                           "并在 pay.txt 里写：\n"
                           "标签|图片文件名|备注",
                           f_ui, PAY_MARGIN, 300, PAY_CONTENT_W, grey, LV_TEXT_ALIGN_CENTER);

    // ---- 说明 ----
    s_label_lbl = mk_label(scr, "", f_title, PAY_MARGIN, 556, PAY_CONTENT_W, lv_color_black(),
                           LV_TEXT_ALIGN_CENTER);
    s_note_lbl = mk_label(scr, "", f_ui, PAY_MARGIN, 600, PAY_CONTENT_W, grey, LV_TEXT_ALIGN_CENTER);

    // ---- 底部 ----
    mk_button(scr, "重载", PAY_MARGIN, 700, 140, 52, on_reload, NULL);
    mk_button(scr, "怎么改", PAY_MARGIN + 148, 700, 140, 52, on_help, NULL);
    {
        lv_obj_t *hint = mk_label(scr, "点码全屏", latin_font(), PAY_MARGIN + 300, 716, 148, grey,
                                  LV_TEXT_ALIGN_RIGHT);
        (void)hint;
    }

    // ---- 全屏展示层（默认隐藏，盖在最上面）----
    s_full_holder = lv_obj_create(scr);
    lv_obj_remove_style_all(s_full_holder);
    lv_obj_set_pos(s_full_holder, 0, 0);
    lv_obj_set_size(s_full_holder, PAY_SCR_W, PAY_SCR_H);
    lv_obj_set_style_bg_color(s_full_holder, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_full_holder, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_full_holder, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_full_holder, on_full_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_full_holder, LV_OBJ_FLAG_HIDDEN);

    s_full_img_holder = lv_obj_create(s_full_holder);
    lv_obj_remove_style_all(s_full_img_holder);
    lv_obj_set_pos(s_full_img_holder, 20, (PAY_SCR_H - (PAY_SCR_W - 40)) / 2);
    lv_obj_set_size(s_full_img_holder, PAY_SCR_W - 40, PAY_SCR_W - 40);
    lv_obj_clear_flag(s_full_img_holder, LV_OBJ_FLAG_CLICKABLE);

    render_all();
    return scr;
}

void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "paycode";
    out->name = "收款码";
    out->icon = "icon.png";
    out->create = paycode_create;
}
