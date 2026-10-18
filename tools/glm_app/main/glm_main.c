// tools/glm_app/main/glm_main.c
// GLM 用量 动态应用：严格对齐 2739acf glm_usage_screen 完整界面与逻辑
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_body = NULL;
static lv_obj_t *s_context_row = NULL;
static lv_obj_t *s_plan_lbl = NULL;
static lv_obj_t *s_five_pct_lbl = NULL;
static lv_obj_t *s_five_bar = NULL;
static lv_obj_t *s_five_detail_lbl = NULL;
static lv_obj_t *s_week_pct_lbl = NULL;
static lv_obj_t *s_week_bar = NULL;
static lv_obj_t *s_week_detail_lbl = NULL;
static lv_obj_t *s_five_reset_lbl = NULL;
static lv_obj_t *s_week_reset_lbl = NULL;
static lv_obj_t *s_footer_lbl = NULL;
static lv_timer_t *s_timer = NULL;

static int s_rendered_context_count = -1;

static void *glm_create(void);

static const lv_font_t *get_font(int size, int weight) {
    const lv_font_t *f = fontpack_lv_font_get(size, weight);
    if (f == NULL) f = fontpack_lv_font_ui();
    return f;
}

static void on_context_clicked(lv_event_t *e) {
    uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    dynapp_glm_select_context((int)idx);
}

static int glm_on_vk_key(const char* key) {
    if (key != NULL) {
        if (strcmp(key, "esc") == 0 || strcmp(key, "back") == 0 || strcmp(key, "vk_home") == 0 || strcmp(key, "home") == 0) {
            dynapp_screen_go_home();
            return 1;
        }
        dynapp_glm_snapshot_t snap;
        if (dynapp_glm_get_snapshot(&snap) && snap.context_count > 1) {
            if (strcmp(key, "vk_prev") == 0) {
                int prev = (snap.active_context + snap.context_count - 1) % snap.context_count;
                dynapp_glm_select_context(prev);
                return 1;
            }
            if (strcmp(key, "vk_next") == 0) {
                int next = (snap.active_context + 1) % snap.context_count;
                dynapp_glm_select_context(next);
                return 1;
            }
        }
    }
    return 0;
}

static lv_obj_t *make_card(lv_obj_t *parent) {
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return card;
}

static void update_ui(void) {
    if (!s_scr || !lv_obj_is_valid(s_scr)) return;

    dynapp_glm_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    if (!dynapp_glm_get_snapshot(&snap)) return;

    const lv_font_t *f_ui = fontpack_lv_font_ui();

    // 动态多上下文标签 (宽度 210，胶囊圆角，对齐原版)
    if (snap.context_count > 1 && snap.context_count != s_rendered_context_count) {
        lv_obj_clean(s_context_row);
        for (int i = 0; i < snap.context_count; ++i) {
            char cname[64] = {0};
            dynapp_glm_get_context_name(i, cname, sizeof(cname));
            if (cname[0] == '\0') snprintf(cname, sizeof(cname), "项目 %d", i + 1);

            lv_obj_t *btn = lv_obj_create(s_context_row);
            lv_obj_remove_style_all(btn);
            lv_obj_set_size(btn, 210, 44);
            lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_color(btn, lv_color_black(), 0);
            lv_obj_set_style_border_width(btn, 2, 0);
            lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(btn, on_context_clicked, LV_EVENT_CLICKED, (void*)(uintptr_t)i);

            int is_sel = (i == snap.active_context);
            lv_obj_set_style_bg_color(btn, is_sel ? lv_color_black() : lv_color_white(), 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);

            lv_obj_t *lbl = lv_label_create(btn);
            lv_label_set_text(lbl, cname);
            lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
            lv_obj_set_width(lbl, 190);
            if (f_ui) lv_obj_set_style_text_font(lbl, f_ui, 0);
            lv_obj_set_style_text_color(lbl, is_sel ? lv_color_white() : lv_color_black(), 0);
            lv_obj_center(lbl);
            lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        }
        s_rendered_context_count = snap.context_count;
    } else if (snap.context_count > 1) {
        uint32_t cnt = lv_obj_get_child_cnt(s_context_row);
        for (uint32_t i = 0; i < cnt; ++i) {
            lv_obj_t *btn = lv_obj_get_child(s_context_row, i);
            int is_sel = ((int)i == snap.active_context);
            lv_obj_set_style_bg_color(btn, is_sel ? lv_color_black() : lv_color_white(), 0);
            lv_obj_t *lbl = lv_obj_get_child(btn, 0);
            if (lbl) {
                lv_obj_set_style_text_color(lbl, is_sel ? lv_color_white() : lv_color_black(), 0);
            }
        }
    }

    // 套餐标题
    char plan_str[64] = "套餐 · 正常";
    if (snap.level[0] != '\0') {
        snprintf(plan_str, sizeof(plan_str), "%s", snap.level);
    }
    lv_label_set_text(s_plan_lbl, plan_str);

    // 5 小时额度卡
    char f_pct_str[64];
    if (snap.tokens_5h_pct >= 0) {
        snprintf(f_pct_str, sizeof(f_pct_str), "5 小时窗口  %d%%", snap.tokens_5h_pct);
        lv_bar_set_value(s_five_bar, snap.tokens_5h_pct, LV_ANIM_OFF);
    } else {
        snprintf(f_pct_str, sizeof(f_pct_str), "5 小时窗口  --");
        lv_bar_set_value(s_five_bar, 0, LV_ANIM_OFF);
    }
    lv_label_set_text(s_five_pct_lbl, f_pct_str);

    char f_det_str[64];
    if (snap.tokens_5h_remaining >= 0) {
        snprintf(f_det_str, sizeof(f_det_str), "重置 · 剩余 Tokens: %d", snap.tokens_5h_remaining);
    } else {
        snprintf(f_det_str, sizeof(f_det_str), "重置 --");
    }
    lv_label_set_text(s_five_detail_lbl, f_det_str);

    // 每周额度卡
    char w_pct_str[64];
    if (snap.tokens_week_pct >= 0) {
        snprintf(w_pct_str, sizeof(w_pct_str), "本周额度  %d%%", snap.tokens_week_pct);
        lv_bar_set_value(s_week_bar, snap.tokens_week_pct, LV_ANIM_OFF);
    } else {
        snprintf(w_pct_str, sizeof(w_pct_str), "本周额度  --");
        lv_bar_set_value(s_week_bar, 0, LV_ANIM_OFF);
    }
    lv_label_set_text(s_week_pct_lbl, w_pct_str);

    char w_det_str[64];
    if (snap.tokens_week_remaining >= 0) {
        snprintf(w_det_str, sizeof(w_det_str), "重置 · 剩余 Tokens: %d", snap.tokens_week_remaining);
    } else {
        snprintf(w_det_str, sizeof(w_det_str), "重置 --");
    }
    lv_label_set_text(s_week_detail_lbl, w_det_str);

    // 重置次数卡
    char f_rst_str[64];
    if (snap.five_hour_resets_left >= 0) {
        snprintf(f_rst_str, sizeof(f_rst_str), "5小时重置次数  %d次", snap.five_hour_resets_left);
    } else {
        snprintf(f_rst_str, sizeof(f_rst_str), "5小时重置次数 --");
    }
    lv_label_set_text(s_five_reset_lbl, f_rst_str);

    char w_rst_str[64];
    if (snap.week_resets_left >= 0) {
        snprintf(w_rst_str, sizeof(w_rst_str), "周重置次数  %d次", snap.week_resets_left);
    } else {
        snprintf(w_rst_str, sizeof(w_rst_str), "周重置次数 --");
    }
    lv_label_set_text(s_week_reset_lbl, w_rst_str);

    // 底部状态提示
    if (!snap.configured) {
        lv_label_set_text(s_footer_lbl, "请通过 MCP 配置 API Key");
    } else if (snap.discovering) {
        lv_label_set_text(s_footer_lbl, "正在发现机构/项目...");
    } else if (snap.busy) {
        lv_label_set_text(s_footer_lbl, "正在查询...");
    } else if (snap.last_ok) {
        if (snap.context_name[0] != '\0') {
            char ubuf[96];
            snprintf(ubuf, sizeof(ubuf), "%s · 已就绪", snap.context_name);
            lv_label_set_text(s_footer_lbl, ubuf);
        } else {
            lv_label_set_text(s_footer_lbl, "已就绪");
        }
    } else if (snap.error[0] != '\0') {
        char ebuf[128];
        snprintf(ebuf, sizeof(ebuf), "查询失败：%s", snap.error);
        lv_label_set_text(s_footer_lbl, ebuf);
    } else {
        lv_label_set_text(s_footer_lbl, "就绪");
    }
}

static void timer_cb(lv_timer_t *tmr) {
    (void)tmr;
    update_ui();
}

static void on_screen_deleted(lv_event_t *e) {
    (void)e;
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    s_scr = NULL;
    s_body = NULL;
    s_context_row = NULL;
    s_plan_lbl = NULL;
    s_five_pct_lbl = NULL;
    s_five_bar = NULL;
    s_five_detail_lbl = NULL;
    s_week_pct_lbl = NULL;
    s_week_bar = NULL;
    s_week_detail_lbl = NULL;
    s_five_reset_lbl = NULL;
    s_week_reset_lbl = NULL;
    s_footer_lbl = NULL;
    s_rendered_context_count = -1;
    dynapp_glm_stop();
}

static void *glm_create(void) {
    dynapp_glm_start();
    dynapp_glm_request_refresh();

    lv_obj_t *scr = lv_obj_create(NULL);
    s_scr = scr;
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 800);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(scr, on_screen_deleted, LV_EVENT_DELETE, NULL);

    dynapp_screen_status_bar(scr);
    dynapp_vk_attach(scr, "glm", glm_create, glm_on_vk_key);

    const lv_font_t *f_ui = fontpack_lv_font_ui();
    const lv_font_t *f25 = get_font(25, 2);
    const lv_font_t *f30 = get_font(30, 2);

    // 内容滚动容器 (无返回顶栏，由底部 vk_home 键统一控制返回)
    s_body = lv_obj_create(scr);
    lv_obj_remove_style_all(s_body);
    lv_obj_set_size(s_body, 452, 750);
    lv_obj_set_pos(s_body, 14, 42);
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_body, 10, 0);

    // 多上下文水平滑动 Row
    s_context_row = lv_obj_create(s_body);
    lv_obj_remove_style_all(s_context_row);
    lv_obj_set_size(s_context_row, lv_pct(100), 52);
    lv_obj_set_flex_flow(s_context_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_context_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_context_row, 12, 0);

    s_plan_lbl = lv_label_create(s_body);
    lv_label_set_text(s_plan_lbl, "尚未更新");
    if (f25) lv_obj_set_style_text_font(s_plan_lbl, f25, 0);
    lv_obj_set_style_text_color(s_plan_lbl, lv_color_black(), 0);

    // 卡片 1: 5 小时窗口
    lv_obj_t *card_5h = make_card(s_body);
    s_five_pct_lbl = lv_label_create(card_5h);
    lv_label_set_text(s_five_pct_lbl, "5 小时窗口  --");
    if (f30) lv_obj_set_style_text_font(s_five_pct_lbl, f30, 0);
    lv_obj_set_style_text_color(s_five_pct_lbl, lv_color_black(), 0);

    s_five_bar = lv_bar_create(card_5h);
    lv_obj_set_size(s_five_bar, lv_pct(100), 18);
    lv_bar_set_range(s_five_bar, 0, 100);
    lv_bar_set_value(s_five_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_five_bar, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_five_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_five_bar, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_five_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s_five_bar, 9, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_five_bar, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_five_bar, 9, LV_PART_INDICATOR);

    s_five_detail_lbl = lv_label_create(card_5h);
    lv_label_set_text(s_five_detail_lbl, "重置 --");
    if (f25) lv_obj_set_style_text_font(s_five_detail_lbl, f25, 0);
    lv_obj_set_style_text_color(s_five_detail_lbl, lv_color_black(), 0);

    // 卡片 2: 每周额度
    lv_obj_t *card_wk = make_card(s_body);
    s_week_pct_lbl = lv_label_create(card_wk);
    lv_label_set_text(s_week_pct_lbl, "本周额度  --");
    if (f30) lv_obj_set_style_text_font(s_week_pct_lbl, f30, 0);
    lv_obj_set_style_text_color(s_week_pct_lbl, lv_color_black(), 0);

    s_week_bar = lv_bar_create(card_wk);
    lv_obj_set_size(s_week_bar, lv_pct(100), 18);
    lv_bar_set_range(s_week_bar, 0, 100);
    lv_bar_set_value(s_week_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_week_bar, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_week_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_week_bar, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_week_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s_week_bar, 9, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_week_bar, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_week_bar, 9, LV_PART_INDICATOR);

    s_week_detail_lbl = lv_label_create(card_wk);
    lv_label_set_text(s_week_detail_lbl, "重置 --");
    if (f25) lv_obj_set_style_text_font(s_week_detail_lbl, f25, 0);
    lv_obj_set_style_text_color(s_week_detail_lbl, lv_color_black(), 0);

    // 卡片 3: 重置次数
    lv_obj_t *card_rst = make_card(s_body);
    s_five_reset_lbl = lv_label_create(card_rst);
    lv_label_set_text(s_five_reset_lbl, "5小时重置次数 --");
    if (f25) lv_obj_set_style_text_font(s_five_reset_lbl, f25, 0);
    lv_obj_set_style_text_color(s_five_reset_lbl, lv_color_black(), 0);

    s_week_reset_lbl = lv_label_create(card_rst);
    lv_label_set_text(s_week_reset_lbl, "周重置次数 --");
    if (f25) lv_obj_set_style_text_font(s_week_reset_lbl, f25, 0);
    lv_obj_set_style_text_color(s_week_reset_lbl, lv_color_black(), 0);

    // 底部状态提示
    s_footer_lbl = lv_label_create(s_body);
    lv_obj_set_width(s_footer_lbl, lv_pct(100));
    lv_label_set_long_mode(s_footer_lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_footer_lbl, "正在初始化...");
    if (f_ui) lv_obj_set_style_text_font(s_footer_lbl, f_ui, 0);
    lv_obj_set_style_text_align(s_footer_lbl, LV_TEXT_ALIGN_CENTER, 0);

    update_ui();

    s_timer = lv_timer_create(timer_cb, 1000, NULL);
    return scr;
}

void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "glm";
    out->name = "GLM 用量";
    out->icon = "icon.png";
    out->create = glm_create;
}
