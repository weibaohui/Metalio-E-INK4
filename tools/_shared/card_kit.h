// tools/_shared/card_kit.h —— 电子名片 / 电子工牌 / 收款码 共用的「名片套件」
//
// 干什么用的：
//   1. 解析名片资料（profile.txt）+ 一张名片下的多个「码入口」；
//   2. 统一渲染两种码：
//        - 文本码（链接/微信号/群链接…）→ LVGL 自带 lv_qrcode 生成；
//        - 图片码（用户上传的微信个人码、支付宝收款码…）→ 解码成 L8 位图显示。
//
// 为什么要支持「多个码」：
//   企业微信的「对外名片」支持 6~7 个自定义对外信息，每条可以是文字 / 链接 / 小程序；
//   市面上的一码通收（微信+支付宝聚合码）也是同一个思路——一个入口承载多个去向。
//   墨水屏上正好适合：上屏一个标签行（微信 / 企微 / 飞书 / 群 / 主页…），
//   下屏一个大码，点标签换码。
//
// 配置文件（放在 app 自己的 data/ 目录，profile.txt）：
//
//   [card]
//   name=张三
//   title=产品设计师
//   company=示例科技
//   dept=产品部
//   id=EMP-1024
//   phone=13800000000
//   email=zhangsan@example.com
//   url=https://example.com
//   status=今天也在搬砖
//   avatar=avatar.png
//   qr=1
//   [qr]
//   微信|mywechatid
//   企微|https://work.weixin.qq.com/qr/xxxx
//   飞书|https://feishu.cn/qr/xxxx
//   群聊|https://example.com/group
//   主页|https://example.com
//   微信码|wechat_qr.png      ← 图片型码（后缀是图片就当图片）
//   [card]
//   ...第二张名片
//
// 说明：
//   - 动态应用写权限只在自身 data/，读权限也只有自身 data/ 与 bundle，
//     所以配置和图片都必须放进 app 自己的 data 目录。
//   - 设备提供 /fs/upload（路径相对 metalio/e-ink），用它把文件推进对应 app 的 data 目录。
#ifndef CARD_KIT_H
#define CARD_KIT_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "dynamic_app_shims.h"

#define CARD_MAX_CARDS 4
#define CARD_MAX_QRS 8
#define CARD_LINE_MAX 256
#define CARD_QR_TEXT_MAX 160

/* ---------------- 数据结构 ---------------- */

typedef struct {
    char label[20];                /* 标签：微信 / 企微 / 飞书 / 群聊 / 主页 … */
    char content[CARD_QR_TEXT_MAX]; /* 文本码内容（URL/微信号…）或图片文件名 */
    int is_image;                  /* 1 = 图片型码（用户上传的码图） */
} CardQr;

typedef struct {
    char name[32];
    char title[32];
    char company[40];
    char dept[24];
    char empid[24];
    char phone[24];
    char email[48];
    char url[96];
    char status[48];
    char avatar[32];
    int show_qr;
    CardQr qrs[CARD_MAX_QRS];
    int nqr;
} CardProfile;

typedef struct {
    CardProfile cards[CARD_MAX_CARDS];
    int n;
} CardSet;

/* ---------------- 小工具 ---------------- */

static void card_copy(char *dst, size_t cap, const char *src) {
    if (dst == NULL || cap == 0) return;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void card_rtrim(char *s) {
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
}

static void card_ltrim(const char **s) {
    while (**s == ' ' || **s == '\t') (*s)++;
}

/* 内容是不是图片文件（按扩展名判断，大小写不敏感） */
static int card_is_image_name(const char *s) {
    if (s == NULL) return 0;
    static const char *kExt[] = {".png", ".jpg", ".jpeg", ".bmp", ".webp"};
    const size_t n = strlen(s);
    for (int i = 0; i < (int)(sizeof(kExt) / sizeof(kExt[0])); ++i) {
        const size_t e = strlen(kExt[i]);
        if (n < e) continue;
        const char *tail = s + (n - e);
        int match = 1;
        for (size_t j = 0; j < e; ++j) {
            char c = tail[j];
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (c != kExt[i][j]) {
                match = 0;
                break;
            }
        }
        if (match) return 1;
    }
    return 0;
}

/* 解析一行「标签|内容」；返回 1 成功 */
static int card_qr_parse_line(const char *line, CardQr *out) {
    if (line == NULL || out == NULL) return 0;
    memset(out, 0, sizeof(*out));
    const char *bar = strchr(line, '|');
    if (bar == NULL) return 0;
    char label[20];
    const size_t ll = (size_t)(bar - line);
    const size_t cl = (ll >= sizeof(label)) ? sizeof(label) - 1 : ll;
    memcpy(label, line, cl);
    label[cl] = '\0';
    card_rtrim(label);
    card_copy(out->content, sizeof(out->content), bar + 1);
    card_rtrim(out->content);
    if (out->content[0] == '\0') return 0;
    if (label[0] == '\0') {
        /* 没写标签就用内容前几个字顶上 */
        snprintf(label, sizeof(label), "%s", out->content);
    }
    card_copy(out->label, sizeof(out->label), label);
    out->is_image = card_is_image_name(out->content);
    return 1;
}

/* ---------------- 配置解析 ---------------- */

static void card_set_field(CardProfile *c, const char *key, const char *val) {
    if (strcmp(key, "name") == 0) card_copy(c->name, sizeof(c->name), val);
    else if (strcmp(key, "title") == 0) card_copy(c->title, sizeof(c->title), val);
    else if (strcmp(key, "company") == 0) card_copy(c->company, sizeof(c->company), val);
    else if (strcmp(key, "dept") == 0) card_copy(c->dept, sizeof(c->dept), val);
    else if (strcmp(key, "id") == 0) card_copy(c->empid, sizeof(c->empid), val);
    else if (strcmp(key, "phone") == 0) card_copy(c->phone, sizeof(c->phone), val);
    else if (strcmp(key, "email") == 0) card_copy(c->email, sizeof(c->email), val);
    else if (strcmp(key, "url") == 0) card_copy(c->url, sizeof(c->url), val);
    else if (strcmp(key, "status") == 0) card_copy(c->status, sizeof(c->status), val);
    else if (strcmp(key, "avatar") == 0) card_copy(c->avatar, sizeof(c->avatar), val);
    else if (strcmp(key, "qr") == 0) c->show_qr = (atoi(val) != 0);
}

/* 解析 profile 文本；返回卡片数 */
static int card_parse(const char *text, CardSet *set) {
    if (set == NULL) return 0;
    memset(set, 0, sizeof(*set));
    if (text == NULL) return 0;
    CardProfile *cur = NULL;
    int in_qr = 0;
    char line[CARD_LINE_MAX];
    const char *p = text;
    while (*p != '\0') {
        size_t i = 0;
        while (*p != '\0' && *p != '\n' && i + 1 < sizeof(line)) line[i++] = *p++;
        line[i] = '\0';
        while (*p == '\n' || *p == '\r') p++;

        char *s = line;
        card_ltrim((const char **)&s);
        if (*s == '\0' || *s == '#') continue;
        if (*s == '[') {
            char *end = strchr(s, ']');
            if (end != NULL) *end = '\0';
            const char *sec = s + 1;
            if (strcmp(sec, "qr") == 0) {
                in_qr = 1;
            } else {
                /* 其它段（card 之类）→ 开始一张新名片 */
                in_qr = 0;
                if (set->n >= CARD_MAX_CARDS) break;
                cur = &set->cards[set->n++];
                memset(cur, 0, sizeof(*cur));
                cur->show_qr = 1;
            }
            continue;
        }
        if (in_qr && cur != NULL) {
            CardQr q;
            if (card_qr_parse_line(s, &q) && cur->nqr < CARD_MAX_QRS) cur->qrs[cur->nqr++] = q;
            continue;
        }
        char *eq = strchr(s, '=');
        if (eq == NULL || cur == NULL) continue;
        *eq = '\0';
        char *val = eq + 1;
        card_rtrim(val);
        card_set_field(cur, s, val);
    }
    return set->n;
}

/* 兜底默认资料：没配 profile.txt 也能看到效果 */
static void card_default(CardSet *set) {
    if (set == NULL) return;
    memset(set, 0, sizeof(*set));
    set->n = 1;
    CardProfile *c = &set->cards[0];
    card_copy(c->name, sizeof(c->name), "你的名字");
    card_copy(c->title, sizeof(c->title), "职位");
    card_copy(c->company, sizeof(c->company), "公司");
    card_copy(c->phone, sizeof(c->phone), "13800000000");
    card_copy(c->email, sizeof(c->email), "you@example.com");
    card_copy(c->status, sizeof(c->status), "一句话介绍自己");
    c->show_qr = 1;
    c->nqr = 1;
    card_copy(c->qrs[0].label, sizeof(c->qrs[0].label), "名片");
    card_copy(c->qrs[0].content, sizeof(c->qrs[0].content), "https://example.com");
    c->qrs[0].is_image = 0;
}

/* ---------------- 路径 ---------------- */

/* app 自己的 data 目录绝对路径（用于 dynapp_image_decode_l8） */
static const char *card_data_dir(void) {
    const char *dir = dynapp_get_data_path();
    return (dir != NULL && dir[0] != '\0') ? dir : ".";
}

/* 拼出 data 目录下的文件绝对路径 */
static void card_data_path(char *out, size_t cap, const char *name) {
    if (out == NULL || cap == 0) return;
    snprintf(out, cap, "%s/%s", card_data_dir(), (name != NULL && name[0] != '\0') ? name : "");
}

/* ---------------- 渲染 ---------------- */

/* 图片型码：解码成 L8 位图挂成 lv_image。
   缓冲区用 malloc —— 固件配了 CONFIG_SPIRAM_USE_MALLOC，16 字节以上的分配走 PSRAM，
   几百 KB 的位图也不占内部 RAM。 */
typedef struct {
    lv_image_dsc_t dsc;
    uint8_t *pixels;
} card_img_t;

static void card_img_free_cb(lv_event_t *e) {
    card_img_t *im = (card_img_t *)lv_event_get_user_data(e);
    if (im != NULL) {
        if (im->pixels != NULL) free(im->pixels);
        free(im);
    }
}

/* 解码 L8 图并挂成 lv_image（等比缩放居中）；失败返回 NULL */
static lv_obj_t *card_make_l8_image(lv_obj_t *parent, const char *path, int max_w, int max_h) {
    if (parent == NULL || path == NULL) return NULL;
    uint8_t *pix = (uint8_t *)malloc((size_t)max_w * max_h);
    if (pix == NULL) return NULL;
    int w = 0, h = 0;
    if (dynapp_image_decode_l8(path, max_w, max_h, pix, (size_t)max_w * max_h, &w, &h) == 0 ||
        w <= 0 || h <= 0) {
        free(pix);
        return NULL;
    }
    card_img_t *im = (card_img_t *)malloc(sizeof(card_img_t));
    if (im == NULL) {
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
    lv_obj_add_event_cb(img, card_img_free_cb, LV_EVENT_DELETE, im);
    lv_obj_center(img);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    return img;
}

/* 占位头像：圆圈 + 姓名首字（没有头像文件时的兜底） */
static lv_obj_t *card_avatar_placeholder(lv_obj_t *parent, const char *name, int size,
                                         const lv_font_t *font) {
    if (parent == NULL) return NULL;
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, size, size);
    lv_obj_set_style_bg_color(box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, lv_color_black(), 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_radius(box, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* 名字首字（按 UTF-8 取一个字符） */
    char ch[8] = {0};
    const char *p = (name != NULL) ? name : "";
    while (*p == ' ') p++;
    if (*p != '\0') {
        const unsigned char c0 = (unsigned char)p[0];
        size_t n = 1;
        if (c0 >= 0xF0) n = 4;
        else if (c0 >= 0xE0) n = 3;
        else if (c0 >= 0xC0) n = 2;
        for (size_t i = 0; i < n && p[i] != '\0'; ++i) ch[i] = p[i];
        ch[n] = '\0';
    }
    lv_obj_t *lbl = lv_label_create(box);
    lv_label_set_text(lbl, ch);
    if (font != NULL) lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return box;
}

/* 头像：有文件就解码显示，没有就画占位。返回创建的对象 */
static lv_obj_t *card_avatar_render(lv_obj_t *parent, const char *avatar_name, const char *person_name,
                                    int size, const lv_font_t *font) {
    if (parent == NULL) return NULL;
    const char *fn = (avatar_name != NULL && avatar_name[0] != '\0') ? avatar_name : "avatar.png";
    char path[192];
    card_data_path(path, sizeof(path), fn);
    lv_obj_t *img = card_make_l8_image(parent, path, size, size);
    if (img != NULL) return img;
    return card_avatar_placeholder(parent, person_name, size, font);
}

/* 画一个码条目：
     文本码 → lv_qrcode（内容太长时自动降级，扫不出来就没意义了）
     图片码 → 解码成 L8 位图
   返回创建的对象（NULL = 失败），位置由调用方摆。 */
static lv_obj_t *card_qr_render(lv_obj_t *parent, const CardQr *q, int size) {
    if (parent == NULL || q == NULL) return NULL;
    if (q->is_image) {
        char path[192];
        card_data_path(path, sizeof(path), q->content);
        lv_obj_t *img = card_make_l8_image(parent, path, size, size);
        if (img != NULL) return img;
        return NULL;
    }
    if (q->content[0] == '\0') return NULL;
    lv_obj_t *qr = lv_qrcode_create(parent);
    if (qr == NULL) return NULL;
    lv_qrcode_set_size(qr, size);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, q->content, strlen(q->content));
    lv_obj_clear_flag(qr, LV_OBJ_FLAG_CLICKABLE);
    return qr;
}

/* 拼一份 vCard：手机扫码后可直接存成联系人 */
static int card_build_vcard(const CardProfile *c, char *out, size_t cap) {
    int n = snprintf(out, cap,
                     "BEGIN:VCARD\r\n"
                     "VERSION:3.0\r\n"
                     "N:%s;;;;\r\n"
                     "FN:%s\r\n"
                     "ORG:%s\r\n"
                     "TITLE:%s\r\n"
                     "NOTE:%s\r\n",
                     c->name, c->name, c->company, c->title, c->status);
    if (c->dept[0] != '\0')
        n += snprintf(out + n, cap > (size_t)n ? cap - n : 0, "DEPARTMENT:%s\r\n", c->dept);
    if (c->phone[0] != '\0')
        n += snprintf(out + n, cap > (size_t)n ? cap - n : 0, "TEL;TYPE=CELL:%s\r\n", c->phone);
    if (c->email[0] != '\0')
        n += snprintf(out + n, cap > (size_t)n ? cap - n : 0, "EMAIL:%s\r\n", c->email);
    if (c->url[0] != '\0')
        n += snprintf(out + n, cap > (size_t)n ? cap - n : 0, "URL:%s\r\n", c->url);
    n += snprintf(out + n, cap > (size_t)n ? cap - n : 0, "END:VCARD\r\n");
    return n;
}

#endif /* CARD_KIT_H */
