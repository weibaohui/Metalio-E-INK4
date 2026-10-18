// tools/wallpaper_app/main/wallpaper_main.c
// 壁纸管理 动态应用：对齐 2739acf wallpaper_screen 3列网格与预览模式
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

#define ITEMS_PER_PAGE 6

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

static const lv_font_t *get_font(int size, int weight) {
    if (size >= 30) {
        const lv_font_t *f = fontpack_lv_font_get(30, 2);
        if (f) return f;
    }
    return fontpack_lv_font_ui();
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
        if (s_mode == WP_MODE_GRID) {
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

static void render_grid_view(void) {
    lv_obj_clean(s_content_area);

    int total_items = dynapp_wallpaper_list_count();
    char cur_standby[64] = {0};
    dynapp_wallpaper_get_standby(cur_standby, sizeof(cur_standby));

    if (total_items <= 0) {
        lv_obj_t *tip = lv_label_create(s_content_area);
        lv_label_set_text(tip, "SD 卡暂无壁纸\n请将图片文件存入\n/sdcard/metalio/e-ink/wallpapers 目录\n（支持 PNG / JPG / A2I1）");
        const lv_font_t *f18 = get_font(18, 0);
        if (f18) lv_obj_set_style_text_font(tip, f18, 0);
        lv_obj_set_width(tip, lv_pct(100));
        lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(tip, LV_ALIGN_CENTER, 0, -40);
        return;
    }

    int total_pages = (total_items + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE;
    if (total_pages <= 0) total_pages = 1;
    if (s_cur_page >= total_pages) s_cur_page = total_pages - 1;
    if (s_cur_page < 0) s_cur_page = 0;

    // 3 列网格容器
    lv_obj_t *grid = lv_obj_create(s_content_area);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, lv_pct(100), 550);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(grid, 10, 0);
    lv_obj_clear_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    int start = s_cur_page * ITEMS_PER_PAGE;
    int end = start + ITEMS_PER_PAGE;
    if (end > total_items) end = total_items;

    const lv_font_t *f_ui = fontpack_lv_font_ui();

    for (int i = start; i < end; ++i) {
        dynapp_wallpaper_info_t item;
        if (!dynapp_wallpaper_get_item(i, &item)) continue;

        int is_standby = (cur_standby[0] != '\0' && strstr(item.path, cur_standby) != NULL);

        lv_obj_t *card = lv_obj_create(grid);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, 138, 220);
        lv_obj_set_style_bg_color(card, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(card, lv_color_black(), 0);
        lv_obj_set_style_border_width(card, is_standby ? 3 : 2, 0);
        lv_obj_set_style_radius(card, 10, 0);
        lv_obj_set_style_pad_all(card, 6, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, on_card_clicked, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        // 缩略图外框
        lv_obj_t *img_box = lv_obj_create(card);
        lv_obj_remove_style_all(img_box);
        lv_obj_set_size(img_box, 126, 160);
        lv_obj_set_style_bg_color(img_box, lv_color_white(), 0);
        lv_obj_set_style_border_color(img_box, lv_color_black(), 0);
        lv_obj_set_style_border_width(img_box, 1, 0);
        lv_obj_set_style_radius(img_box, 6, 0);
        lv_obj_clear_flag(img_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        // 徽标（若是当前待机）
        if (is_standby) {
            lv_obj_t *tag = lv_label_create(img_box);
            lv_label_set_text(tag, "待机");
            lv_obj_set_style_bg_color(tag, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(tag, LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(tag, lv_color_white(), 0);
            if (f_ui) lv_obj_set_style_text_font(tag, f_ui, 0);
            lv_obj_set_style_pad_hor(tag, 4, 0);
            lv_obj_set_style_pad_ver(tag, 2, 0);
            lv_obj_set_style_radius(tag, 4, 0);
            lv_obj_align(tag, LV_ALIGN_TOP_LEFT, 2, 2);
        } else {
            lv_obj_t *icon_lbl = lv_label_create(img_box);
            lv_label_set_text(icon_lbl, "🖼");
            const lv_font_t *f26 = get_font(26, 0);
            if (f26) lv_obj_set_style_text_font(icon_lbl, f26, 0);
            lv_obj_center(icon_lbl);
        }

        // 壁纸名称
        lv_obj_t *name_lbl = lv_label_create(card);
        lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name_lbl, 126);
        lv_label_set_text(name_lbl, item.name);
        if (f_ui) lv_obj_set_style_text_font(name_lbl, f_ui, 0);
        lv_obj_set_style_text_color(name_lbl, lv_color_black(), 0);
        lv_obj_set_style_text_align(name_lbl, LV_TEXT_ALIGN_CENTER, 0);
    }

    // 底部控制行
    lv_obj_t *ctrl_row = lv_obj_create(s_content_area);
    lv_obj_remove_style_all(ctrl_row);
    lv_obj_set_size(ctrl_row, lv_pct(100), 48);
    lv_obj_set_flex_flow(ctrl_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctrl_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *btn_p = lv_obj_create(ctrl_row);
    lv_obj_remove_style_all(btn_p);
    lv_obj_set_size(btn_p, 80, 40);
    lv_obj_set_style_bg_color(btn_p, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn_p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn_p, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn_p, 2, 0);
    lv_obj_set_style_radius(btn_p, 8, 0);
    lv_obj_add_flag(btn_p, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_p, on_prev_page, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lp = lv_label_create(btn_p);
    lv_label_set_text(lp, "‹ 上页");
    if (f_ui) lv_obj_set_style_text_font(lp, f_ui, 0);
    lv_obj_center(lp);

    s_page_lbl = lv_label_create(ctrl_row);
    char pbuf[64];
    snprintf(pbuf, sizeof(pbuf), "第 %d / %d 页 (共 %d 张)", s_cur_page + 1, total_pages, total_items);
    lv_label_set_text(s_page_lbl, pbuf);
    if (f_ui) lv_obj_set_style_text_font(s_page_lbl, f_ui, 0);

    lv_obj_t *btn_n = lv_obj_create(ctrl_row);
    lv_obj_remove_style_all(btn_n);
    lv_obj_set_size(btn_n, 80, 40);
    lv_obj_set_style_bg_color(btn_n, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn_n, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn_n, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn_n, 2, 0);
    lv_obj_set_style_radius(btn_n, 8, 0);
    lv_obj_add_flag(btn_n, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_n, on_next_page, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ln = lv_label_create(btn_n);
    lv_label_set_text(ln, "下页 ›");
    if (f_ui) lv_obj_set_style_text_font(ln, f_ui, 0);
    lv_obj_center(ln);

    lv_obj_t *btn_def = lv_obj_create(ctrl_row);
    lv_obj_remove_style_all(btn_def);
    lv_obj_set_size(btn_def, 100, 40);
    lv_obj_set_style_bg_color(btn_def, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn_def, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn_def, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn_def, 2, 0);
    lv_obj_set_style_radius(btn_def, 8, 0);
    lv_obj_add_flag(btn_def, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_def, on_clear_standby_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ldef = lv_label_create(btn_def);
    lv_label_set_text(ldef, "恢复默认");
    if (f_ui) lv_obj_set_style_text_font(ldef, f_ui, 0);
    lv_obj_center(ldef);
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
    const lv_font_t *f24 = get_font(24, 2);

    // 预览详情卡片
    lv_obj_t *card = lv_obj_create(s_content_area);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 12, 0);

    lv_obj_t *title = lv_label_create(card);
    lv_label_set_text(title, "壁纸详情");
    if (f24) lv_obj_set_style_text_font(title, f24, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);

    lv_obj_t *name = lv_label_create(card);
    char nbuf[128];
    snprintf(nbuf, sizeof(nbuf), "文件名称: %s", item.name);
    lv_label_set_text(name, nbuf);
    if (f_ui) lv_obj_set_style_text_font(name, f_ui, 0);
    lv_obj_set_style_text_color(name, lv_color_black(), 0);

    char sz_str[32];
    format_size(item.size_bytes, sz_str, sizeof(sz_str));
    lv_obj_t *sz = lv_label_create(card);
    char sbuf[128];
    snprintf(sbuf, sizeof(sbuf), "文件大小: %s", sz_str);
    lv_label_set_text(sz, sbuf);
    if (f_ui) lv_obj_set_style_text_font(sz, f_ui, 0);
    lv_obj_set_style_text_color(sz, lv_color_black(), 0);

    char cur_standby[64] = {0};
    dynapp_wallpaper_get_standby(cur_standby, sizeof(cur_standby));
    int is_cur = (cur_standby[0] != '\0' && strstr(item.path, cur_standby) != NULL);

    lv_obj_t *stat = lv_label_create(card);
    lv_label_set_text(stat, is_cur ? "当前状态: 已设为待机壁纸" : "当前状态: 未启用");
    if (f_ui) lv_obj_set_style_text_font(stat, f_ui, 0);
    lv_obj_set_style_text_color(stat, lv_color_black(), 0);

    // 操作按钮
    lv_obj_t *btn_set = lv_obj_create(card);
    lv_obj_remove_style_all(btn_set);
    lv_obj_set_size(btn_set, lv_pct(100), 50);
    lv_obj_set_style_bg_color(btn_set, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(btn_set, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn_set, 10, 0);
    lv_obj_add_flag(btn_set, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_set, on_set_standby_clicked, LV_EVENT_CLICKED, (void*)(intptr_t)s_preview_idx);

    lv_obj_t *lset = lv_label_create(btn_set);
    lv_label_set_text(lset, "设为待机壁纸");
    if (f22) lv_obj_set_style_text_font(lset, f22, 0);
    lv_obj_set_style_text_color(lset, lv_color_white(), 0);
    lv_obj_center(lset);

    lv_obj_t *btn_back = lv_obj_create(card);
    lv_obj_remove_style_all(btn_back);
    lv_obj_set_size(btn_back, lv_pct(100), 50);
    lv_obj_set_style_bg_color(btn_back, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn_back, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn_back, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn_back, 2, 0);
    lv_obj_set_style_radius(btn_back, 10, 0);
    lv_obj_add_flag(btn_back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn_back, on_back_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lback = lv_label_create(btn_back);
    lv_label_set_text(lback, "返回壁纸列表");
    if (f22) lv_obj_set_style_text_font(lback, f22, 0);
    lv_obj_set_style_text_color(lback, lv_color_black(), 0);
    lv_obj_center(lback);
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

    const lv_font_t *f22 = get_font(22, 2);
    const lv_font_t *f26 = get_font(26, 2);

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
    out->name = "壁纸设置";
    out->icon = "icon.png";
    out->create = wallpaper_create;
}
