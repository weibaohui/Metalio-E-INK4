// tools/badge_app/main/badge_main.c
// 电子工牌 动态应用：头像 + 姓名/职位/公司 + 状态 + 可扫的二维码名片。
//
// 设计要点（墨水屏 480x800 / 1bpp 单色）：
//   - 头像走 dynapp_image_decode_l8（输出已做 Bayer 抖动，天然适配墨水屏），
//     没有头像就画一个几何占位（圆圈 + 姓名首字），不会开天窗。
//   - 资料从 app 自己的 data/profile.txt 读（动态应用只能读自己的目录），
//     没配就用内置默认资料；一份资料可写多张工牌（[card] 分段），上下键切换。
//   - 二维码用 LVGL 自带 lv_qrcode（I1 单色 canvas），SDK 已导出给动态应用。
//   - 墨水屏刷新慢：只在换工牌/内容变化时整页刷一次，不做动画。
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

#include "../../_shared/badge_profile.h"

#define AVATAR_SIZE 150
#define QR_SIZE 190

static ProfileSet s_set;
static int s_idx = 0;

static lv_obj_t *s_avatar = NULL;     // 头像图片（或占位容器）
static lv_obj_t *s_avatar_holder = NULL;
static lv_obj_t *s_qr = NULL;
static lv_obj_t *s_idx_lbl = NULL;
static lv_obj_t *s_name_lbl = NULL;
static lv_obj_t *s_title_lbl = NULL;
static lv_obj_t *s_company_lbl = NULL;
static lv_obj_t *s_dept_lbl = NULL;
static lv_obj_t *s_status_lbl = NULL;
static lv_obj_t *s_phone_lbl = NULL;
static lv_obj_t *s_email_lbl = NULL;
static lv_obj_t *s_qr_tip_lbl = NULL;
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
    size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    buf[n] = '\0';
    if (profile_parse(buf, &s_set) <= 0) profile_default(&s_set);
}

// ================= 头像 =================
typedef struct {
    lv_image_dsc_t dsc;
    uint8_t *pixels;
} badge_img_t;

static void badge_img_delete_cb(lv_event_t *e) {
    badge_img_t *im = (badge_img_t *)lv_event_get_user_data(e);
    if (im) {
        free(im->pixels);
        free(im);
    }
}

// 解码 L8 图并挂成 lv_image（等比缩放居中）；失败返回 NULL
static lv_obj_t *make_l8_image(lv_obj_t *parent, const char *path, int max_w, int max_h) {
    uint8_t *pix = (uint8_t *)malloc((size_t)max_w * max_h);
    if (!pix) return NULL;
    int w = 0, h = 0;
    if (!dynapp_image_decode_l8(path, max_w, max_h, pix, (size_t)max_w * max_h, &w, &h) ||
        w <= 0 || h <= 0) {
        free(pix);
        return NULL;
    }
    badge_img_t *im = (badge_img_t *)malloc(sizeof(badge_img_t));
    if (!im) {
        free(pix);
        return NULL;
    }
    memset(&im->dsc, 0, sizeof(im->dsc));
    im->dsc.header.w = (uint32_t)w;
    im->dsc.header.h = (uint32_t)h;
    im->dsc.header.cf = LV_COLOR_FORMAT_L8;
    im->dsc.data = pix;
    im->dsc.data_size = (uint32_t)w * h;
    im->pixels = pix;

    lv_obj_t *img = lv_image_create(parent);
    lv_image_set_src(img, &im->dsc);
    lv_obj_add_event_cb(img, badge_img_delete_cb, LV_EVENT_DELETE, im);
    lv_obj_center(img);
    return img;
}

// 占位头像：圆圈 + 姓名首字
static lv_obj_t *make_avatar_placeholder(lv_obj_t *parent, const char *name) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, AVATAR_SIZE, AVATAR_SIZE);
    lv_obj_set_style_bg_color(box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, lv_color_black(), 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_radius(box, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    // 名字首字（汉字或字母）
    char ch[2] = {0};
    const char *p = name;
    while (*p == ' ') p++;
    if (*p != '\0') {
        // 取一个 UTF-8 字符
        ch[0] = (char)(unsigned char)p[0];
        if ((unsigned char)p[0] >= 0xF0 && p[1] && p[2] && p[3]) {
            ch[1] = p[1];
            ch[2] = p[2];
            ch[3] = p[3];
            ch[4] = '\0';
        } else if ((unsigned char)p[0] >= 0xE0 && p[1] && p[2]) {
            ch[1] = p[1];
            ch[2] = p[2];
            ch[3] = '\0';
        } else {
            ch[1] = '\0';
        }
    }
    lv_obj_t *lbl = lv_label_create(box);
    lv_label_set_text(lbl, ch);
    const lv_font_t *f = title_font();
    if (f != NULL) lv_obj_set_style_text_font(lbl, f, 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return box;
}

static void refresh_avatar(void) {
    if (s_avatar_holder == NULL) return;
    // 清掉旧头像
    lv_obj_clean(s_avatar_holder);
    s_avatar = NULL;

    const ProfileCard *c = &s_set.cards[s_idx];
    char path[192];
    const char *dir = dynapp_get_data_path();
    if (dir == NULL || dir[0] == '\0') dir = ".";
    const char *av = (c->avatar[0] != '\0') ? c->avatar : "avatar.png";
    snprintf(path, sizeof(path), "%s/%s", dir, av);
    lv_obj_t *img = make_l8_image(s_avatar_holder, path, AVATAR_SIZE, AVATAR_SIZE);
    if (img != NULL) {
        s_avatar = img;
    } else {
        s_avatar = make_avatar_placeholder(s_avatar_holder, c->name);
    }
}

// ================= 渲染 =================
static void apply_card(void) {
    if (s_set.n <= 0) profile_default(&s_set);
    if (s_idx >= s_set.n) s_idx = 0;
    const ProfileCard *c = &s_set.cards[s_idx];

    // 工牌上的码要"一扫码就能用"，但又不能太长：
    // 载荷越长版本越高、模块越多，190px 装不下就会糊（3px/模块扫不出来）。
    // 所以有网址就只放网址，否则放只含姓名/电话/邮箱的精简 vCard。
    static char vcard[1024];
    if (c->url[0] != '\0') {
        snprintf(vcard, sizeof(vcard), "%s", c->url);
    } else {
        char mini[512];
        int n = snprintf(mini, sizeof(mini),
                         "BEGIN:VCARD\r\nVERSION:3.0\r\nN:%s;;;;\r\nFN:%s\r\n", c->name, c->name);
        if (c->phone[0] != '\0')
            n += snprintf(mini + n, sizeof(mini) - n, "TEL;TYPE=CELL:%s\r\n", c->phone);
        if (c->email[0] != '\0')
            n += snprintf(mini + n, sizeof(mini) - n, "EMAIL:%s\r\n", c->email);
        snprintf(mini + n, sizeof(mini) - n, "END:VCARD\r\n");
        snprintf(vcard, sizeof(vcard), "%s", mini);
    }
    if (s_qr != NULL) {
        lv_qrcode_update(s_qr, vcard, strlen(vcard));
        if (c->show_qr) {
            lv_obj_clear_flag(s_qr, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_qr, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (s_idx_lbl != NULL) {
        char b[16];
        snprintf(b, sizeof(b), "%d/%d", s_idx + 1, s_set.n);
        lv_label_set_text(s_idx_lbl, b);
    }
    if (s_name_lbl != NULL) lv_label_set_text(s_name_lbl, c->name);
    if (s_title_lbl != NULL) lv_label_set_text(s_title_lbl, c->title);
    if (s_company_lbl != NULL) lv_label_set_text(s_company_lbl, c->company);
    if (s_dept_lbl != NULL) {
        char b[64];
        if (c->dept[0] != '\0' && c->empid[0] != '\0')
            snprintf(b, sizeof(b), "%s · %s", c->dept, c->empid);
        else if (c->dept[0] != '\0')
            snprintf(b, sizeof(b), "%s", c->dept);
        else
            snprintf(b, sizeof(b), "%s", c->empid);
        lv_label_set_text(s_dept_lbl, b);
    }
    if (s_status_lbl != NULL) lv_label_set_text(s_status_lbl, c->status);
    if (s_phone_lbl != NULL) lv_label_set_text(s_phone_lbl, c->phone);
    if (s_email_lbl != NULL) lv_label_set_text(s_email_lbl, c->email);
    if (s_qr_tip_lbl != NULL) {
        lv_label_set_text(s_qr_tip_lbl, c->show_qr ? "扫码打开主页" : "二维码已隐藏");
    }
    refresh_avatar();
}

static void show_help(int show) {
    s_help_shown = show;
    if (s_help_obj == NULL) return;
    if (show) lv_obj_clear_flag(s_help_obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_help_obj, LV_OBJ_FLAG_HIDDEN);
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
    // 动态应用的实例是缓存复用的：改完 profile.txt / 换头像不会自动生效，
    // 必须在这里手动重载（否则得重启设备）。
    load_profile();
    if (s_idx >= s_set.n) s_idx = 0;
    apply_card();
    dynapp_toast_show("资料已重新载入", 1400);
    dynapp_epd_full_refresh();
}

static void on_qr_clicked(lv_event_t *e) {
    (void)e;
    ProfileCard *c = &s_set.cards[s_idx];
    c->show_qr = !c->show_qr;
    apply_card();
    dynapp_epd_full_refresh();
}

static void on_help_clicked(lv_event_t *e) {
    (void)e;
    show_help(!s_help_shown);
    dynapp_epd_full_refresh();
}

static int badge_on_vk_key(const char *key) {
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
    s_avatar = NULL;
    s_avatar_holder = NULL;
    s_qr = NULL;
    s_idx_lbl = NULL;
    s_name_lbl = NULL;
    s_title_lbl = NULL;
    s_company_lbl = NULL;
    s_dept_lbl = NULL;
    s_status_lbl = NULL;
    s_phone_lbl = NULL;
    s_email_lbl = NULL;
    s_qr_tip_lbl = NULL;
    s_help_obj = NULL;
}

static void *badge_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    s_scr = scr;
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 800);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_screen_deleted, LV_EVENT_DELETE, NULL);

    dynapp_screen_status_bar(scr);
    dynapp_vk_attach(scr, "badge", badge_create, badge_on_vk_key);

    load_profile();

    const lv_font_t *f_ui = ui_font();
    const lv_font_t *f_title = title_font();

    // 1. 标题
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "工牌");
    if (f_title != NULL) lv_obj_set_style_text_font(title, f_title, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_set_pos(title, 16, 48);

    s_idx_lbl = lv_label_create(scr);
    if (f_ui != NULL) lv_obj_set_style_text_font(s_idx_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_idx_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_align(s_idx_lbl, LV_ALIGN_TOP_RIGHT, -16, 56);

    // 2. 头像
    s_avatar_holder = lv_obj_create(scr);
    lv_obj_remove_style_all(s_avatar_holder);
    lv_obj_set_pos(s_avatar_holder, 16, 96);
    lv_obj_set_size(s_avatar_holder, AVATAR_SIZE, AVATAR_SIZE);
    lv_obj_clear_flag(s_avatar_holder, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    // 3. 文字块
    s_name_lbl = lv_label_create(scr);
    if (f_title != NULL) lv_obj_set_style_text_font(s_name_lbl, f_title, 0);
    lv_obj_set_style_text_color(s_name_lbl, lv_color_black(), 0);
    lv_obj_set_pos(s_name_lbl, 182, 100);

    s_title_lbl = lv_label_create(scr);
    if (f_ui != NULL) lv_obj_set_style_text_font(s_title_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_title_lbl, lv_color_black(), 0);
    lv_obj_set_pos(s_title_lbl, 182, 142);

    s_company_lbl = lv_label_create(scr);
    if (f_ui != NULL) lv_obj_set_style_text_font(s_company_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_company_lbl, lv_color_black(), 0);
    lv_obj_set_pos(s_company_lbl, 182, 178);

    s_dept_lbl = lv_label_create(scr);
    if (f_ui != NULL) lv_obj_set_style_text_font(s_dept_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_dept_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_pos(s_dept_lbl, 182, 214);

    s_status_lbl = lv_label_create(scr);
    if (f_ui != NULL) lv_obj_set_style_text_font(s_status_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_status_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_pos(s_status_lbl, 16, 262);

    // 4. 二维码
    s_qr = lv_qrcode_create(scr);
    lv_qrcode_set_size(s_qr, QR_SIZE);
    lv_qrcode_set_dark_color(s_qr, lv_color_black());
    lv_qrcode_set_light_color(s_qr, lv_color_white());
    lv_obj_set_pos(s_qr, 16, 290);

    s_phone_lbl = lv_label_create(scr);
    if (f_ui != NULL) lv_obj_set_style_text_font(s_phone_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_phone_lbl, lv_color_black(), 0);
    lv_obj_set_pos(s_phone_lbl, 214, 300);

    s_email_lbl = lv_label_create(scr);
    if (f_ui != NULL) lv_obj_set_style_text_font(s_email_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_email_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_pos(s_email_lbl, 214, 336);

    s_qr_tip_lbl = lv_label_create(scr);
    if (f_ui != NULL) lv_obj_set_style_text_font(s_qr_tip_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_qr_tip_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_pos(s_qr_tip_lbl, 214, 372);

    // 5. 帮助浮层
    s_help_obj = lv_obj_create(scr);
    lv_obj_remove_style_all(s_help_obj);
    lv_obj_set_pos(s_help_obj, 16, 96);
    lv_obj_set_size(s_help_obj, 448, 480);
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
            "   title=职位",
            "   phone=13800000000",
            "   avatar=avatar.png",
            "",
            "② 传到设备（用网页或 curl）：",
            "   apps/badge.app/data/",
            "",
            "③ 头像也放同目录",
            "   （PNG/JPG，自动灰度+抖动）",
            "",
            "④ 回到 app 点「重载」生效",
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

    // 6. 状态行
    lv_obj_t *status = lv_label_create(scr);
    lv_label_set_text(status, "上下键换工牌 · 点「怎么改」看配置方法");
    if (f_ui != NULL) lv_obj_set_style_text_font(status, f_ui, 0);
    lv_obj_set_style_text_color(status, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_pos(status, 16, 492);

    // 7. 按钮
    const lv_coord_t bw = 90, bh = 56, bgap = 5;
    const lv_coord_t by = 600;
    make_btn(scr, "上一张", 16, by, bw, bh, on_prev_clicked, NULL);
    make_btn(scr, "下一张", 16 + bw + bgap, by, bw, bh, on_next_clicked, NULL);
    make_btn(scr, "二维码", 16 + 2 * (bw + bgap), by, bw, bh, on_qr_clicked, NULL);
    make_btn(scr, "重载", 16 + 3 * (bw + bgap), by, bw, bh, on_reload_clicked, NULL);
    make_btn(scr, "怎么改", 16 + 4 * (bw + bgap), by, bw, bh, on_help_clicked, NULL);

    apply_card();
    return scr;
}

void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "badge";
    out->name = "电子工牌";
    out->icon = "icon.png";
    out->create = badge_create;
}
