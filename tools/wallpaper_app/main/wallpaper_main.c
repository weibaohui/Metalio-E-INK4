// tools/wallpaper_app/main/wallpaper_main.c
// 壁纸 动态应用：.app Bundle 规范版
// 纯 C + LVGL，仅调用固件符号表导出接口。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_cur_lbl = NULL;
static lv_obj_t *s_list_box = NULL;
static lv_obj_t *s_modal = NULL;

static void *wallpaper_create(void);

static const lv_font_t *get_font(int size, int weight) {
    const lv_font_t *f = fontpack_lv_font_get(size, weight);
    if (f == NULL) f = fontpack_lv_font_ui();
    return f;
}

static void on_back_clicked(lv_event_t *e) {
    (void)e;
    dynapp_screen_go_home();
}

static int wallpaper_on_vk_key(const char* key) {
    if (key != NULL && (strcmp(key, "esc") == 0 || strcmp(key, "back") == 0)) {
        if (s_modal != NULL) {
            lv_obj_del(s_modal);
            s_modal = NULL;
            return 1;
        }
        dynapp_screen_go_home();
        return 1;
    }
    return 0;
}

static void refresh_list(void);

static void close_modal(void) {
    if (s_modal) {
        lv_obj_del(s_modal);
        s_modal = NULL;
    }
}

static void on_cancel_modal_clicked(lv_event_t *e) {
    (void)e;
    close_modal();
}

static void on_clear_standby_clicked(lv_event_t *e) {
    (void)e;
    dynapp_wallpaper_clear_standby();
    dynapp_toast_show("已恢复系统默认待机画面", 2000);
    refresh_list();
}

static void on_confirm_set_clicked(lv_event_t *e) {
    const char *path = (const char *)lv_event_get_user_data(e);
    if (path) {
        int ret = dynapp_wallpaper_set_standby(path);
        if (ret) {
            dynapp_toast_show("已设为待机壁纸", 2000);
        } else {
            dynapp_toast_show("设置待机壁纸失败", 2000);
        }
    }
    close_modal();
    refresh_list();
}

static void open_action_modal(const dynapp_wallpaper_info_t *item) {
    if (s_modal) close_modal();

    s_modal = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_modal);
    lv_obj_set_size(s_modal, 480, 800);
    lv_obj_set_style_bg_color(s_modal, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_50, 0);
    lv_obj_clear_flag(s_modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *box = lv_obj_create(s_modal);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, 400, 220);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, lv_color_black(), 0);
    lv_obj_set_style_border_width(box, 3, 0);
    lv_obj_set_style_radius(box, 16, 0);
    lv_obj_set_style_pad_all(box, 16, 0);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 12, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(box);
    lv_label_set_text(title, item->name);
    const lv_font_t *f20 = get_font(20, 2);
    if (f20) lv_obj_set_style_text_font(title, f20, 0);
    lv_obj_set_width(title, lv_pct(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);

    lv_obj_t *desc = lv_label_create(box);
    char buf[128];
    snprintf(buf, sizeof(buf), "大小: %u 字节\n是否将此图片设为设备待机壁纸？", (unsigned)item->size_bytes);
    lv_label_set_text(desc, buf);
    const lv_font_t *f16 = get_font(16, 0);
    if (f16) lv_obj_set_style_text_font(desc, f16, 0);

    lv_obj_t *btn_row = lv_obj_create(box);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, lv_pct(100), 50);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);

    // 确认设置按钮
    lv_obj_t *b_set = lv_obj_create(btn_row);
    lv_obj_remove_style_all(b_set);
    lv_obj_set_size(b_set, 170, 44);
    lv_obj_set_style_bg_color(b_set, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(b_set, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(b_set, 8, 0);
    lv_obj_add_flag(b_set, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b_set, on_confirm_set_clicked, LV_EVENT_CLICKED, (void*)item->path);
    lv_obj_t *lbl_set = lv_label_create(b_set);
    lv_label_set_text(lbl_set, "设为待机壁纸");
    const lv_font_t *f18 = get_font(18, 2);
    if (f18) lv_obj_set_style_text_font(lbl_set, f18, 0);
    lv_obj_set_style_text_color(lbl_set, lv_color_white(), 0);
    lv_obj_center(lbl_set);

    // 取消按钮
    lv_obj_t *b_can = lv_obj_create(btn_row);
    lv_obj_remove_style_all(b_can);
    lv_obj_set_size(b_can, 170, 44);
    lv_obj_set_style_bg_color(b_can, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(b_can, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(b_can, lv_color_black(), 0);
    lv_obj_set_style_border_width(b_can, 2, 0);
    lv_obj_set_style_radius(b_can, 8, 0);
    lv_obj_add_flag(b_can, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(b_can, on_cancel_modal_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *lbl_can = lv_label_create(b_can);
    lv_label_set_text(lbl_can, "取消");
    if (f18) lv_obj_set_style_text_font(lbl_can, f18, 0);
    lv_obj_set_style_text_color(lbl_can, lv_color_black(), 0);
    lv_obj_center(lbl_can);
}

static void on_item_clicked(lv_event_t *e) {
    dynapp_wallpaper_info_t *item = (dynapp_wallpaper_info_t *)lv_event_get_user_data(e);
    if (item) {
        open_action_modal(item);
    }
}

static void on_card_deleted(lv_event_t *e) {
    void *d = lv_event_get_user_data(e);
    if (d) free(d);
}

static void refresh_list(void) {
    if (!s_scr || !lv_obj_is_valid(s_scr)) return;

    char active_name[64] = {0};
    int has_active = dynapp_wallpaper_get_standby(active_name, sizeof(active_name));
    if (has_active && active_name[0] != '\0') {
        char buf[128];
        snprintf(buf, sizeof(buf), "当前待机壁纸: %s", active_name);
        lv_label_set_text(s_cur_lbl, buf);
    } else {
        lv_label_set_text(s_cur_lbl, "当前待机壁纸: 未设置 (使用默认待机画面)");
    }

    lv_obj_clean(s_list_box);

    int count = dynapp_wallpaper_list_count();
    if (count == 0) {
        lv_obj_t *tip = lv_label_create(s_list_box);
        lv_label_set_text(tip, "未在 SD 卡找到壁纸图片\n请将图片放入 /sdcard/metalio/e-ink/wallpaper/ 目录");
        const lv_font_t *f18 = get_font(18, 0);
        if (f18) lv_obj_set_style_text_font(tip, f18, 0);
        lv_obj_set_width(tip, lv_pct(100));
        lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    for (int i = 0; i < count; ++i) {
        dynapp_wallpaper_info_t *item = malloc(sizeof(dynapp_wallpaper_info_t));
        if (!item) continue;
        memset(item, 0, sizeof(*item));
        if (!dynapp_wallpaper_get_item(i, item)) {
            free(item);
            continue;
        }

        lv_obj_t *card = lv_obj_create(s_list_box);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, lv_pct(100), 68);
        lv_obj_set_style_bg_color(card, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(card, lv_color_black(), 0);
        lv_obj_set_style_border_width(card, 2, 0);
        lv_obj_set_style_radius(card, 10, 0);
        lv_obj_set_style_pad_all(card, 8, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(card, 4, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, on_item_clicked, LV_EVENT_CLICKED, item);
        lv_obj_add_event_cb(card, on_card_deleted, LV_EVENT_DELETE, item);

        lv_obj_t *name_lbl = lv_label_create(card);
        lv_label_set_text(name_lbl, item->name);
        const lv_font_t *f18 = get_font(18, 2);
        if (f18) lv_obj_set_style_text_font(name_lbl, f18, 0);
        lv_obj_clear_flag(name_lbl, LV_OBJ_FLAG_CLICKABLE);

        char meta_buf[64];
        snprintf(meta_buf, sizeof(meta_buf), "文件大小: %u 字节", (unsigned)item->size_bytes);
        lv_obj_t *meta_lbl = lv_label_create(card);
        lv_label_set_text(meta_lbl, meta_buf);
        const lv_font_t *f14 = get_font(14, 0);
        if (f14) lv_obj_set_style_text_font(meta_lbl, f14, 0);
        lv_obj_clear_flag(meta_lbl, LV_OBJ_FLAG_CLICKABLE);
    }
}

static void on_screen_deleted(lv_event_t *e) {
    (void)e;
    s_scr = NULL;
    s_cur_lbl = NULL;
    s_list_box = NULL;
    s_modal = NULL;
}

static void *wallpaper_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    s_scr = scr;
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 800);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(scr, on_screen_deleted, LV_EVENT_DELETE, NULL);

    dynapp_vk_attach(scr, "wallpaper", wallpaper_create, wallpaper_on_vk_key);

    // 顶部 Header
    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, 480, 56);
    lv_obj_set_pos(header, 0, 36);

    lv_obj_t *back_btn = lv_obj_create(header);
    lv_obj_remove_style_all(back_btn);
    lv_obj_set_size(back_btn, 64, 48);
    lv_obj_set_pos(back_btn, 12, 4);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back_btn, on_back_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, "‹ 返回");
    const lv_font_t *f22 = get_font(22, 2);
    if (f22) lv_obj_set_style_text_font(back_lbl, f22, 0);
    lv_obj_set_style_text_color(back_lbl, lv_color_black(), 0);
    lv_obj_center(back_lbl);

    lv_obj_t *title_lbl = lv_label_create(header);
    lv_label_set_text(title_lbl, "壁纸");
    const lv_font_t *f26 = get_font(26, 2);
    if (f26) lv_obj_set_style_text_font(title_lbl, f26, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_black(), 0);
    lv_obj_align(title_lbl, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *clr_btn = lv_obj_create(header);
    lv_obj_remove_style_all(clr_btn);
    lv_obj_set_size(clr_btn, 88, 38);
    lv_obj_align(clr_btn, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_set_style_bg_color(clr_btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(clr_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(clr_btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(clr_btn, 2, 0);
    lv_obj_set_style_radius(clr_btn, 8, 0);
    lv_obj_add_flag(clr_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(clr_btn, on_clear_standby_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *clr_lbl = lv_label_create(clr_btn);
    lv_label_set_text(clr_lbl, "清除待机");
    const lv_font_t *f18 = get_font(18, 0);
    if (f18) lv_obj_set_style_text_font(clr_lbl, f18, 0);
    lv_obj_set_style_text_color(clr_lbl, lv_color_black(), 0);
    lv_obj_center(clr_lbl);

    // 主内容区
    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, 456, 696);
    lv_obj_set_pos(body, 12, 96);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 10, 0);

    // 当前待机壁纸状态卡片
    lv_obj_t *card = lv_obj_create(body);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    s_cur_lbl = lv_label_create(card);
    lv_label_set_text(s_cur_lbl, "当前待机壁纸: 查询中...");
    const lv_font_t *f16 = get_font(16, 0);
    if (f16) lv_obj_set_style_text_font(s_cur_lbl, f16, 0);

    // 标题条
    lv_obj_t *sec_bar = lv_obj_create(body);
    lv_obj_remove_style_all(sec_bar);
    lv_obj_set_size(sec_bar, lv_pct(100), 32);
    lv_obj_set_style_bg_color(sec_bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(sec_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(sec_bar, 8, 0);
    lv_obj_set_style_radius(sec_bar, 4, 0);
    lv_obj_clear_flag(sec_bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *sec_lbl = lv_label_create(sec_bar);
    lv_label_set_text(sec_lbl, "壁纸库 (点击设为待机壁纸)");
    if (f16) lv_obj_set_style_text_font(sec_lbl, f16, 0);
    lv_obj_set_style_text_color(sec_lbl, lv_color_white(), 0);
    lv_obj_align(sec_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    // 列表容器
    s_list_box = lv_obj_create(body);
    lv_obj_remove_style_all(s_list_box);
    lv_obj_set_size(s_list_box, lv_pct(100), 570);
    lv_obj_set_flex_flow(s_list_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list_box, 8, 0);

    refresh_list();
    return scr;
}

void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "wallpaper";
    out->name = "壁纸";
    out->icon = "icon.png";
    out->create = wallpaper_create;
}
