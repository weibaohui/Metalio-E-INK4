// tools/wallpaper_app/main/wallpaper_main.c
// 待机壁纸 动态应用:2 列大图网格 + 全宽预览(via dynapp_image_decode_l8)
// 设计语言:黑白灰、8px 间距栅格、卡片圆角 12、层级 标题>卡片>标签
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

#define ITEMS_PER_PAGE 4     // 2 列 x 2 行:大缩略图更利于挑选
#define THUMB_W 195
#define THUMB_H 240
#define PREVIEW_W 440
#define PREVIEW_H 540

typedef enum {
    WP_MODE_GRID = 0,
    WP_MODE_PREVIEW = 1,
} wp_mode_t;

static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_content_area = NULL;
static lv_obj_t *s_page_lbl = NULL;

static wp_mode_t s_mode = WP_MODE_GRID;
static int s_cur_page = 0;
static int s_preview_idx = -1;

static void *wallpaper_create(void);
static void render_current_view(void);
static void wallpaper_hop_preview(int delta);

static const lv_font_t *get_font(int size, int weight) {
    if (size >= 30) {
        const lv_font_t *f = fontpack_lv_font_get(30, 2);
        if (f) return f;
    }
    return fontpack_lv_font_ui();
}

// ---- L8 解码图:lv_image 挂自带缓冲,删除时统一释放 ----
typedef struct {
    lv_image_dsc_t dsc;
    uint8_t *pixels;
} wp_img_t;

static void wp_img_delete_cb(lv_event_t *e) {
    wp_img_t *im = (wp_img_t *)lv_event_get_user_data(e);
    if (im) {
        free(im->pixels);
        free(im);
    }
}

// 解码 path 为 L8 图并作为 lv_image 挂到 parent(等比缩放到 max 框内,居中)。
// 失败返回 NULL。像素缓冲随组件删除释放。须在 LVGL 线程调用(解码百毫秒级)。
static lv_obj_t *make_l8_image(lv_obj_t *parent, const char *path, int max_w, int max_h) {
    uint8_t *pix = (uint8_t *)malloc((size_t)max_w * max_h);
    if (!pix) return NULL;
    int w = 0, h = 0;
    if (!dynapp_image_decode_l8(path, max_w, max_h, pix, (size_t)max_w * max_h, &w, &h) ||
        w <= 0 || h <= 0) {
        free(pix);
        return NULL;
    }
    wp_img_t *im = (wp_img_t *)malloc(sizeof(wp_img_t));
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
    lv_obj_add_event_cb(img, wp_img_delete_cb, LV_EVENT_DELETE, im);
    lv_obj_center(img);
    return img;
}

static void on_back_clicked(lv_event_t *e) {
    (void)e;
    if (s_mode == WP_MODE_PREVIEW) {
        s_mode = WP_MODE_GRID;
        s_preview_idx = -1;
        render_current_view();
    } else {
        dynapp_screen_go_home();
    }
}

static void wallpaper_hop_preview(int delta) {
    int total = dynapp_wallpaper_list_count();
    if (total <= 0) return;
    s_preview_idx = (s_preview_idx + delta + total) % total;
    render_current_view();
}

static int wallpaper_on_vk_key(const char* key) {
    if (key != NULL) {
        if (strcmp(key, "esc") == 0 || strcmp(key, "back") == 0 || strcmp(key, "vk_home") == 0 || strcmp(key, "home") == 0) {
            if (s_mode == WP_MODE_PREVIEW) {
                s_mode = WP_MODE_GRID;
                s_preview_idx = -1;
                render_current_view();
                return 1;
            }
            dynapp_screen_go_home();
            return 1;
        }
        if (s_mode == WP_MODE_PREVIEW) {
            // 预览态:上一张/下一张循环
            if (strcmp(key, "vk_prev") == 0) {
                wallpaper_hop_preview(-1);
                return 1;
            }
            if (strcmp(key, "vk_next") == 0) {
                wallpaper_hop_preview(1);
                return 1;
            }
            return 0;
        }
        int total_items = dynapp_wallpaper_list_count();
        int total_pages = (total_items + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE;
        if (total_pages <= 0) total_pages = 1;

        if (strcmp(key, "vk_prev") == 0) {
            if (s_cur_page > 0) {
                s_cur_page--;
                render_current_view();
            }
            return 1;
        }
        if (strcmp(key, "vk_next") == 0) {
            if (s_cur_page + 1 < total_pages) {
                s_cur_page++;
                render_current_view();
            }
            return 1;
        }
    }
    return 0;
}

static void on_card_clicked(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    s_preview_idx = idx;
    s_mode = WP_MODE_PREVIEW;
    render_current_view();
}

static void on_set_standby_clicked(lv_event_t *e) {
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    dynapp_wallpaper_info_t item;
    if (dynapp_wallpaper_get_item(idx, &item)) {
        if (dynapp_wallpaper_set_standby(item.path)) {
            dynapp_toast_show("已设为待机壁纸", 2000);
        } else {
            dynapp_toast_show("设置待机壁纸失败", 2000);
        }
    }
    s_mode = WP_MODE_GRID;
    s_preview_idx = -1;
    render_current_view();
}

static void on_clear_standby_clicked(lv_event_t *e) {
    (void)e;
    dynapp_wallpaper_clear_standby();
    dynapp_toast_show("已恢复系统默认待机画面", 2000);
    render_current_view();
}

static void on_prev_page(lv_event_t *e) {
    (void)e;
    if (s_cur_page > 0) {
        s_cur_page--;
        render_current_view();
    }
}

static void on_next_page(lv_event_t *e) {
    (void)e;
    int total_items = dynapp_wallpaper_list_count();
    int total_pages = (total_items + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE;
    if (s_cur_page + 1 < total_pages) {
        s_cur_page++;
        render_current_view();
    }
}

static void format_size(size_t bytes, char* out, size_t out_len) {
    if (bytes >= 1024 * 1024) {
        unsigned mb = (unsigned)(bytes / (1024 * 1024));
        unsigned rem = (unsigned)(((bytes % (1024 * 1024)) * 10) / (1024 * 1024));
        snprintf(out, out_len, "%u.%u MB", mb, rem);
    } else {
        snprintf(out, out_len, "%u KB", (unsigned)(bytes / 1024));
    }
}

// 通用圆角描边按钮
static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, lv_coord_t w, lv_coord_t h,
                          lv_event_cb_t cb, void *user_data, int invert) {
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, invert ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn, invert ? 0 : 2, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(lbl, invert ? lv_color_white() : lv_color_black(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

static void render_grid_view(void) {
    lv_obj_clean(s_content_area);

    int total_items = dynapp_wallpaper_list_count();
    char cur_standby[128] = {0};
    dynapp_wallpaper_get_standby(cur_standby, sizeof(cur_standby));

    const lv_font_t *f_ui = fontpack_lv_font_ui();
    const lv_font_t *f24 = get_font(24, 2);

    // 标题行:待机壁纸 ····· N 张
    lv_obj_t *title_row = lv_obj_create(s_content_area);
    lv_obj_remove_style_all(title_row);
    lv_obj_set_size(title_row, lv_pct(100), 34);
    lv_obj_clear_flag(title_row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(title_row);
    lv_label_set_text(title, "待机壁纸");
    if (f24) lv_obj_set_style_text_font(title, f24, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_align(title, LV_ALIGN_LEFT_MID, 0, 0);

    if (total_items > 0) {
        lv_obj_t *cnt = lv_label_create(title_row);
        char cbuf[32];
        snprintf(cbuf, sizeof(cbuf), "%d 张", total_items);
        lv_label_set_text(cnt, cbuf);
        if (f_ui) lv_obj_set_style_text_font(cnt, f_ui, 0);
        lv_obj_set_style_text_color(cnt, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
        lv_obj_align(cnt, LV_ALIGN_RIGHT_MID, 0, 0);
    }

    if (total_items <= 0) {
        lv_obj_t *tip = lv_label_create(s_content_area);
        lv_label_set_text(tip, "SD 卡暂无壁纸\n\n请将图片文件存入\n/sdcard/metalio/e-ink/wallpaper 目录\n(支持 PNG / JPG / A2I1)");
        const lv_font_t *f18 = get_font(18, 0);
        if (f18) lv_obj_set_style_text_font(tip, f18, 0);
        lv_obj_set_width(tip, lv_pct(100));
        lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(tip, LV_ALIGN_CENTER, 0, 0);
        return;
    }

    int total_pages = (total_items + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE;
    if (total_pages <= 0) total_pages = 1;
    if (s_cur_page >= total_pages) s_cur_page = total_pages - 1;
    if (s_cur_page < 0) s_cur_page = 0;

    // 2 列网格:大缩略图更利于挑选(高度=2 行卡片+余量,保证底部按钮不越界)
    lv_obj_t *grid = lv_obj_create(s_content_area);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, lv_pct(100), 596);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(grid, 14, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    int start = s_cur_page * ITEMS_PER_PAGE;
    int end = start + ITEMS_PER_PAGE;
    if (end > total_items) end = total_items;

    for (int i = start; i < end; ++i) {
        dynapp_wallpaper_info_t item;
        if (!dynapp_wallpaper_get_item(i, &item)) continue;

        int is_standby = (cur_standby[0] != '\0' && strstr(item.path, cur_standby) != NULL);

        lv_obj_t *card = lv_obj_create(grid);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, 219, 296);
        lv_obj_set_style_bg_color(card, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(card, lv_color_black(), 0);
        lv_obj_set_style_border_width(card, is_standby ? 3 : 2, 0);
        lv_obj_set_style_radius(card, 12, 0);
        lv_obj_set_style_pad_all(card, 12, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(card, 8, 0);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, on_card_clicked, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        // 缩略图框:真实图片(解码失败给文字占位)
        lv_obj_t *img_box = lv_obj_create(card);
        lv_obj_remove_style_all(img_box);
        lv_obj_set_size(img_box, THUMB_W, THUMB_H);
        lv_obj_set_style_bg_color(img_box, lv_color_white(), 0);
        lv_obj_set_style_border_color(img_box, lv_color_black(), 0);
        lv_obj_set_style_border_width(img_box, 1, 0);
        lv_obj_set_style_radius(img_box, 8, 0);
        lv_obj_set_style_pad_all(img_box, 0, 0);
        lv_obj_clear_flag(img_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *thumb = make_l8_image(img_box, item.path, THUMB_W, THUMB_H);
        if (thumb) {
            lv_obj_align(thumb, LV_ALIGN_CENTER, 0, 0);
            lv_obj_clear_flag(thumb, LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_obj_t *ph = lv_label_create(img_box);
            lv_label_set_text(ph, "无预览");
            if (f_ui) lv_obj_set_style_text_font(ph, f_ui, 0);
            lv_obj_set_style_text_color(ph, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
            lv_obj_center(ph);
        }

        // 待机徽标盖在缩略图左上
        if (is_standby) {
            lv_obj_t *tag = lv_label_create(img_box);
            lv_label_set_text(tag, "待机中");
            lv_obj_set_style_bg_color(tag, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(tag, LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(tag, lv_color_white(), 0);
            if (f_ui) lv_obj_set_style_text_font(tag, f_ui, 0);
            lv_obj_set_style_pad_hor(tag, 6, 0);
            lv_obj_set_style_pad_ver(tag, 3, 0);
            lv_obj_set_style_radius(tag, 6, 0);
            lv_obj_align(tag, LV_ALIGN_TOP_LEFT, 4, 4);
        }

        // 壁纸名称:单行截断(高度钉死,否则 DOT 退化为换行溢出卡片)
        lv_obj_t *name_lbl = lv_label_create(card);
        lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name_lbl, THUMB_W);
        lv_obj_set_height(name_lbl, 22);
        lv_label_set_text(name_lbl, item.name);
        if (f_ui) lv_obj_set_style_text_font(name_lbl, f_ui, 0);
        lv_obj_set_style_text_color(name_lbl, lv_color_black(), 0);
        lv_obj_set_style_text_align(name_lbl, LV_TEXT_ALIGN_CENTER, 0);
    }

    // 翻页行:‹  页码  ›
    lv_obj_t *nav_row = lv_obj_create(s_content_area);
    lv_obj_remove_style_all(nav_row);
    lv_obj_set_size(nav_row, lv_pct(100), 40);
    lv_obj_set_flex_flow(nav_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    make_btn(nav_row, "‹", 70, 40, on_prev_page, NULL, 0);

    s_page_lbl = lv_label_create(nav_row);
    char pbuf[48];
    snprintf(pbuf, sizeof(pbuf), "第 %d / %d 页", s_cur_page + 1, total_pages);
    lv_label_set_text(s_page_lbl, pbuf);
    lv_obj_set_width(s_page_lbl, 170);
    lv_obj_set_height(s_page_lbl, 22);
    lv_label_set_long_mode(s_page_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_page_lbl, LV_TEXT_ALIGN_CENTER, 0);
    if (f_ui) lv_obj_set_style_text_font(s_page_lbl, f_ui, 0);

    make_btn(nav_row, "›", 70, 40, on_next_page, NULL, 0);

    // 恢复默认:整宽次级动作,与翻页分层
    make_btn(s_content_area, "恢复系统默认待机画面", lv_pct(100), 44,
             on_clear_standby_clicked, NULL, 0);
}

static void on_hop_prev(lv_event_t *e) {
    (void)e;
    wallpaper_hop_preview(-1);
}

static void on_hop_next(lv_event_t *e) {
    (void)e;
    wallpaper_hop_preview(1);
}

static void render_preview_view(void) {
    lv_obj_clean(s_content_area);

    dynapp_wallpaper_info_t item;
    if (!dynapp_wallpaper_get_item(s_preview_idx, &item)) {
        s_mode = WP_MODE_GRID;
        render_grid_view();
        return;
    }

    const lv_font_t *f_ui = fontpack_lv_font_ui();
    const lv_font_t *f22 = get_font(22, 2);

    // 大图预览
    lv_obj_t *img_box = lv_obj_create(s_content_area);
    lv_obj_remove_style_all(img_box);
    lv_obj_set_size(img_box, lv_pct(100), 500);
    lv_obj_set_style_bg_color(img_box, lv_color_white(), 0);
    lv_obj_set_style_border_color(img_box, lv_color_black(), 0);
    lv_obj_set_style_border_width(img_box, 2, 0);
    lv_obj_set_style_radius(img_box, 12, 0);
    lv_obj_set_style_pad_all(img_box, 0, 0);
    lv_obj_clear_flag(img_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *big = make_l8_image(img_box, item.path, 440, 498);
    if (big) {
        lv_obj_align(big, LV_ALIGN_CENTER, 0, 0);
        lv_obj_clear_flag(big, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_t *ph = lv_label_create(img_box);
        lv_label_set_text(ph, "无法解码预览");
        if (f_ui) lv_obj_set_style_text_font(ph, f_ui, 0);
        lv_obj_center(ph);
    }

    // 信息行:名称 · 大小 · 状态
    char sz_str[32];
    format_size(item.size_bytes, sz_str, sizeof(sz_str));
    char cur_standby[128] = {0};
    dynapp_wallpaper_get_standby(cur_standby, sizeof(cur_standby));
    int is_cur = (cur_standby[0] != '\0' && strstr(item.path, cur_standby) != NULL);
    lv_obj_t *info = lv_label_create(s_content_area);
    char ibuf[256];
    snprintf(ibuf, sizeof(ibuf), "%s · %s · %s",
             item.name, sz_str, is_cur ? "使用中" : "未启用");
    lv_label_set_text(info, ibuf);
    lv_label_set_long_mode(info, LV_LABEL_LONG_DOT);
    lv_obj_set_width(info, lv_pct(100));
    lv_obj_set_height(info, 22);
    if (f_ui) lv_obj_set_style_text_font(info, f_ui, 0);
    lv_obj_set_style_text_color(info, lv_color_black(), 0);

    // 主操作行:‹  设为待机壁纸  ›
    lv_obj_t *act_row = lv_obj_create(s_content_area);
    lv_obj_remove_style_all(act_row);
    lv_obj_set_size(act_row, lv_pct(100), 48);
    lv_obj_set_flex_flow(act_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(act_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    make_btn(act_row, "‹", 56, 48, on_hop_prev, NULL, 0);

    lv_obj_t *btn_set = lv_obj_create(act_row);
    lv_obj_remove_style_all(btn_set);
    lv_obj_set_size(btn_set, 292, 48);
    lv_obj_set_style_bg_color(btn_set, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(btn_set, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_set, 10, 0);
    lv_obj_add_flag(btn_set, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_set, on_set_standby_clicked, LV_EVENT_CLICKED, (void*)(intptr_t)s_preview_idx);

    lv_obj_t *lset = lv_label_create(btn_set);
    lv_label_set_text(lset, is_cur ? "重新设为待机壁纸" : "设为待机壁纸");
    if (f22) lv_obj_set_style_text_font(lset, f22, 0);
    lv_obj_set_style_text_color(lset, lv_color_white(), 0);
    lv_obj_center(lset);

    make_btn(act_row, "›", 56, 48, on_hop_next, NULL, 0);

    // 次级动作:返回列表(整宽)
    make_btn(s_content_area, "返回壁纸列表", lv_pct(100), 44, on_back_clicked, NULL, 0);
}

static void render_current_view(void) {
    if (!s_scr || !lv_obj_is_valid(s_scr)) return;
    if (s_mode == WP_MODE_PREVIEW) {
        render_preview_view();
    } else {
        render_grid_view();
    }
}

static void on_screen_deleted(lv_event_t *e) {
    (void)e;
    s_scr = NULL;
    s_content_area = NULL;
    s_page_lbl = NULL;
    s_mode = WP_MODE_GRID;
    s_preview_idx = -1;
}

static void *wallpaper_create(void) {
    s_mode = WP_MODE_GRID;
    s_cur_page = 0;
    s_preview_idx = -1;

    lv_obj_t *scr = lv_obj_create(NULL);
    s_scr = scr;
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 800);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(scr, on_screen_deleted, LV_EVENT_DELETE, NULL);

    dynapp_screen_status_bar(scr);
    dynapp_vk_attach(scr, "wallpaper", wallpaper_create, wallpaper_on_vk_key);

    // 主内容区 (无返回顶栏，由底部 vk_home 键统一控制返回)
    s_content_area = lv_obj_create(scr);
    lv_obj_remove_style_all(s_content_area);
    lv_obj_set_size(s_content_area, 452, 750);
    lv_obj_set_pos(s_content_area, 14, 42);
    lv_obj_set_flex_flow(s_content_area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_content_area, 10, 0);
    lv_obj_clear_flag(s_content_area, LV_OBJ_FLAG_SCROLLABLE);

    render_current_view();
    return scr;
}

void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "wallpaper";
    out->name = "待机壁纸";
    out->icon = "icon.png";
    out->create = wallpaper_create;
}
