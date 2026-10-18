// tools/glm_app/main/glm_main.c
// GLM 用量 动态应用：.app Bundle 规范版
// 纯 C + LVGL，仅调用固件符号表导出接口。
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
static lv_obj_t *s_title_lbl = NULL;
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

static void on_back_clicked(lv_event_t *e) {
    (void)e;
    dynapp_screen_go_home();
}

static int glm_on_vk_key(const char* key) {
    if (key != NULL && (strcmp(key, "esc") == 0 || strcmp(key, "back") == 0)) {
        dynapp_screen_go_home();
        return 1;
    }
    return 0;
}

static void on_refresh_clicked(lv_event_t *e) {
    (void)e;
    dynapp_glm_request_refresh();
}

static void on_context_clicked(lv_event_t *e) {
    uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    dynapp_glm_select_context((int)idx);
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
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return card;
}

static void load_local_config(void) {
    FILE *fp = dynapp_fopen_data("config.json", "r");
    if (!fp) return;

    char buf[512] = {0};
    size_t len = fread(buf, 1, sizeof(buf) - 1, fp);
    fclose(fp);
    if (len == 0) return;

    char api_key[128] = {0};
    char jwt_token[256] = {0};

    char *k = strstr(buf, "\"api_key\"");
    if (k) {
        char *colon = strchr(k, ':');
        if (colon) {
            char *q1 = strchr(colon, '\"');
            if (q1) {
                char *q2 = strchr(q1 + 1, '\"');
                if (q2) {
                    size_t klen = q2 - (q1 + 1);
                    if (klen < sizeof(api_key)) {
                        memcpy(api_key, q1 + 1, klen);
                    }
                }
            }
        }
    }

    char *j = strstr(buf, "\"jwt_token\"");
    if (j) {
        char *colon = strchr(j, ':');
        if (colon) {
            char *q1 = strchr(colon, '\"');
            if (q1) {
                char *q2 = strchr(q1 + 1, '\"');
                if (q2) {
                    size_t jlen = q2 - (q1 + 1);
                    if (jlen < sizeof(jwt_token)) {
                        memcpy(jwt_token, q1 + 1, jlen);
                    }
                }
            }
        }
    }

    if (api_key[0] != '\0') {
        dynapp_glm_configure(api_key, jwt_token);
    }
}

static void update_ui(void) {
    if (!s_scr || !lv_obj_is_valid(s_scr)) return;

    dynapp_glm_snapshot_t snap;
    memset(&snap, 0, sizeof(snap));
    if (!dynapp_glm_get_snapshot(&snap)) return;

    // 动态多上下文标签
    if (snap.context_count > 1 && snap.context_count != s_rendered_context_count) {
        lv_obj_clean(s_context_row);
        for (int i = 0; i < snap.context_count; ++i) {
            char cname[64] = {0};
            dynapp_glm_get_context_name(i, cname, sizeof(cname));
            if (cname[0] == '\0') snprintf(cname, sizeof(cname), "项目 %d", i + 1);

            lv_obj_t *btn = lv_obj_create(s_context_row);
            lv_obj_remove_style_all(btn);
            lv_obj_set_size(btn, 136, 40);
            lv_obj_set_style_radius(btn, 20, 0);
            lv_obj_set_style_border_color(btn, lv_color_black(), 0);
            lv_obj_set_style_border_width(btn, 2, 0);
            lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(btn, on_context_clicked, LV_EVENT_CLICKED, (void*)(uintptr_t)i);

            int is_sel = (i == snap.active_context);
            lv_obj_set_style_bg_color(btn, is_sel ? lv_color_black() : lv_color_white(), 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);

            lv_obj_t *lbl = lv_label_create(btn);
            lv_label_set_text(lbl, cname);
            const lv_font_t *f16 = get_font(16, 0);
            if (f16) lv_obj_set_style_text_font(lbl, f16, 0);
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

    // 标题卡
    if (snap.context_name[0] != '\0') {
        lv_label_set_text(s_title_lbl, snap.context_name);
    } else {
        lv_label_set_text(s_title_lbl, "智谱 GLM API");
    }

    char plan_str[64] = "套餐 · 正常";
    if (snap.level[0] != '\0') {
        snprintf(plan_str, sizeof(plan_str), "%s", snap.level);
    }
    lv_label_set_text(s_plan_lbl, plan_str);

    // 5 小时额度卡
    char f_pct_str[64];
    if (snap.tokens_5h_pct >= 0) {
        snprintf(f_pct_str, sizeof(f_pct_str), "5 小时窗口已用: %d%%", snap.tokens_5h_pct);
        lv_bar_set_value(s_five_bar, snap.tokens_5h_pct, LV_ANIM_OFF);
    } else {
        snprintf(f_pct_str, sizeof(f_pct_str), "5 小时窗口已用: --");
        lv_bar_set_value(s_five_bar, 0, LV_ANIM_OFF);
    }
    lv_label_set_text(s_five_pct_lbl, f_pct_str);

    char f_det_str[64];
    if (snap.tokens_5h_remaining >= 0) {
        snprintf(f_det_str, sizeof(f_det_str), "剩余 Tokens: %d", snap.tokens_5h_remaining);
    } else {
        snprintf(f_det_str, sizeof(f_det_str), "剩余 Tokens: --");
    }
    lv_label_set_text(s_five_detail_lbl, f_det_str);

    char f_rst_str[64];
    if (snap.five_hour_resets_left >= 0) {
        snprintf(f_rst_str, sizeof(f_rst_str), "5 小时重置次数: %d", snap.five_hour_resets_left);
    } else {
        snprintf(f_rst_str, sizeof(f_rst_str), "5 小时重置次数: --");
    }
    lv_label_set_text(s_five_reset_lbl, f_rst_str);

    // 每周额度卡
    char w_pct_str[64];
    if (snap.tokens_week_pct >= 0) {
        snprintf(w_pct_str, sizeof(w_pct_str), "本周额度已用: %d%%", snap.tokens_week_pct);
        lv_bar_set_value(s_week_bar, snap.tokens_week_pct, LV_ANIM_OFF);
    } else {
        snprintf(w_pct_str, sizeof(w_pct_str), "本周额度已用: --");
        lv_bar_set_value(s_week_bar, 0, LV_ANIM_OFF);
    }
    lv_label_set_text(s_week_pct_lbl, w_pct_str);

    char w_det_str[64];
    if (snap.tokens_week_remaining >= 0) {
        snprintf(w_det_str, sizeof(w_det_str), "剩余 Tokens: %d", snap.tokens_week_remaining);
    } else {
        snprintf(w_det_str, sizeof(w_det_str), "剩余 Tokens: --");
    }
    lv_label_set_text(s_week_detail_lbl, w_det_str);

    char w_rst_str[64];
    if (snap.week_resets_left >= 0) {
        snprintf(w_rst_str, sizeof(w_rst_str), "周重置次数: %d", snap.week_resets_left);
    } else {
        snprintf(w_rst_str, sizeof(w_rst_str), "周重置次数: --");
    }
    lv_label_set_text(s_week_reset_lbl, w_rst_str);

    // 底部状态提示
    if (!snap.configured) {
        lv_label_set_text(s_footer_lbl, "请通过 MCP 配置 API Key 或在 data/config.json 设置");
    } else if (snap.discovering) {
        lv_label_set_text(s_footer_lbl, "正在发现机构/项目...");
    } else if (snap.busy) {
        lv_label_set_text(s_footer_lbl, "正在查询最新用量...");
    } else if (snap.last_ok) {
        lv_label_set_text(s_footer_lbl, "已更新最新额度数据");
    } else if (snap.error[0] != '\0') {
        char err[128];
        snprintf(err, sizeof(err), "查询失败：%s", snap.error);
        lv_label_set_text(s_footer_lbl, err);
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
    s_rendered_context_count = -1;
    dynapp_glm_stop();
}

static void *glm_create(void) {
    dynapp_glm_start();
    load_local_config();
    dynapp_glm_request_refresh();

    lv_obj_t *scr = lv_obj_create(NULL);
    s_scr = scr;
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 800);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(scr, on_screen_deleted, LV_EVENT_DELETE, NULL);

    dynapp_vk_attach(scr, "glm", glm_create, glm_on_vk_key);

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

    lv_obj_t *hdr_lbl = lv_label_create(header);
    lv_label_set_text(hdr_lbl, "GLM 用量");
    const lv_font_t *f26 = get_font(26, 2);
    if (f26) lv_obj_set_style_text_font(hdr_lbl, f26, 0);
    lv_obj_set_style_text_color(hdr_lbl, lv_color_black(), 0);
    lv_obj_align(hdr_lbl, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *refresh_btn = lv_obj_create(header);
    lv_obj_remove_style_all(refresh_btn);
    lv_obj_set_size(refresh_btn, 72, 38);
    lv_obj_align(refresh_btn, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_set_style_bg_color(refresh_btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(refresh_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(refresh_btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(refresh_btn, 2, 0);
    lv_obj_set_style_radius(refresh_btn, 8, 0);
    lv_obj_add_flag(refresh_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(refresh_btn, on_refresh_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ref_lbl = lv_label_create(refresh_btn);
    lv_label_set_text(ref_lbl, "刷新");
    const lv_font_t *f18 = get_font(18, 0);
    if (f18) lv_obj_set_style_text_font(ref_lbl, f18, 0);
    lv_obj_set_style_text_color(ref_lbl, lv_color_black(), 0);
    lv_obj_center(ref_lbl);

    // 主内容区
    s_body = lv_obj_create(scr);
    lv_obj_remove_style_all(s_body);
    lv_obj_set_size(s_body, 456, 696);
    lv_obj_set_pos(s_body, 12, 96);
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_body, 12, 0);
    lv_obj_clear_flag(s_body, LV_OBJ_FLAG_SCROLLABLE);

    // 上下文选择条（如果有多个组织/项目）
    s_context_row = lv_obj_create(s_body);
    lv_obj_remove_style_all(s_context_row);
    lv_obj_set_size(s_context_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_context_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s_context_row, 8, 0);
    lv_obj_clear_flag(s_context_row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // 卡片 1: 标题与套餐信息
    lv_obj_t *card_top = make_card(s_body);
    s_title_lbl = lv_label_create(card_top);
    lv_label_set_text(s_title_lbl, "智谱 GLM API");
    const lv_font_t *f24 = get_font(24, 2);
    if (f24) lv_obj_set_style_text_font(s_title_lbl, f24, 0);

    s_plan_lbl = lv_label_create(card_top);
    lv_label_set_text(s_plan_lbl, "套餐 · 查询中...");
    if (f18) lv_obj_set_style_text_font(s_plan_lbl, f18, 0);

    // 卡片 2: 5 小时额度
    lv_obj_t *card_5h = make_card(s_body);
    s_five_pct_lbl = lv_label_create(card_5h);
    lv_label_set_text(s_five_pct_lbl, "5 小时窗口已用: --");
    const lv_font_t *f20 = get_font(20, 2);
    if (f20) lv_obj_set_style_text_font(s_five_pct_lbl, f20, 0);

    s_five_bar = lv_bar_create(card_5h);
    lv_obj_set_size(s_five_bar, lv_pct(100), 16);
    lv_bar_set_range(s_five_bar, 0, 100);
    lv_bar_set_value(s_five_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_five_bar, lv_color_white(), 0);
    lv_obj_set_style_border_color(s_five_bar, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_five_bar, 1, 0);
    lv_obj_set_style_bg_color(s_five_bar, lv_color_black(), LV_PART_INDICATOR);

    s_five_detail_lbl = lv_label_create(card_5h);
    lv_label_set_text(s_five_detail_lbl, "剩余 Tokens: --");
    if (f18) lv_obj_set_style_text_font(s_five_detail_lbl, f18, 0);

    s_five_reset_lbl = lv_label_create(card_5h);
    lv_label_set_text(s_five_reset_lbl, "5 小时重置次数: --");
    if (f18) lv_obj_set_style_text_font(s_five_reset_lbl, f18, 0);

    // 卡片 3: 每周额度
    lv_obj_t *card_wk = make_card(s_body);
    s_week_pct_lbl = lv_label_create(card_wk);
    lv_label_set_text(s_week_pct_lbl, "本周额度已用: --");
    if (f20) lv_obj_set_style_text_font(s_week_pct_lbl, f20, 0);

    s_week_bar = lv_bar_create(card_wk);
    lv_obj_set_size(s_week_bar, lv_pct(100), 16);
    lv_bar_set_range(s_week_bar, 0, 100);
    lv_bar_set_value(s_week_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_week_bar, lv_color_white(), 0);
    lv_obj_set_style_border_color(s_week_bar, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_week_bar, 1, 0);
    lv_obj_set_style_bg_color(s_week_bar, lv_color_black(), LV_PART_INDICATOR);

    s_week_detail_lbl = lv_label_create(card_wk);
    lv_label_set_text(s_week_detail_lbl, "剩余 Tokens: --");
    if (f18) lv_obj_set_style_text_font(s_week_detail_lbl, f18, 0);

    s_week_reset_lbl = lv_label_create(card_wk);
    lv_label_set_text(s_week_reset_lbl, "周重置次数: --");
    if (f18) lv_obj_set_style_text_font(s_week_reset_lbl, f18, 0);

    // 底部状态提示
    s_footer_lbl = lv_label_create(s_body);
    lv_obj_set_width(s_footer_lbl, lv_pct(100));
    lv_label_set_long_mode(s_footer_lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_footer_lbl, "正在连接...");
    const lv_font_t *f16 = get_font(16, 0);
    if (f16) lv_obj_set_style_text_font(s_footer_lbl, f16, 0);
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
