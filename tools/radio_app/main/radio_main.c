// tools/radio_app/main/radio_main.c
// 收音机 动态应用：.app Bundle 规范版
// 纯 C + LVGL，仅调用固件符号表导出接口。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"
#include "services/radio/radio_player.h"
#include "services/radio/radio_catalog.h"

#define ROWS_PER_PAGE 8
#define MAX_FAVS 128

static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_st_name_lbl = NULL;
static lv_obj_t *s_st_status_lbl = NULL;
static lv_obj_t *s_play_btn_lbl = NULL;
static lv_obj_t *s_fav_btn_lbl = NULL;
static lv_obj_t *s_vol_lbl = NULL;
static lv_obj_t *s_scope_btn_lbl = NULL;
static lv_obj_t *s_page_lbl = NULL;
static lv_obj_t *s_list_box = NULL;
static lv_timer_t *s_timer = NULL;

static int s_cur_index = -1;
static int s_page = 0;
static int s_fav_only = 0;
static int s_volume = 70;

static char s_favs[MAX_FAVS][RADIO_NAME_MAX];
static int s_fav_count = 0;

static void *radio_create(void);

static const lv_font_t *get_font(int size, int weight) {
    const lv_font_t *f = fontpack_lv_font_get(size, weight);
    if (f == NULL) f = fontpack_lv_font_ui();
    return f;
}

static void on_back_clicked(lv_event_t *e) {
    (void)e;
    dynapp_screen_go_home();
}

static int radio_on_vk_key(const char* key) {
    if (key != NULL && (strcmp(key, "esc") == 0 || strcmp(key, "back") == 0)) {
        dynapp_screen_go_home();
        return 1;
    }
    return 0;
}

static void load_favs(void) {
    s_fav_count = 0;
    FILE *fp = dynapp_fopen_data("favs.txt", "r");
    if (!fp) return;

    char line[RADIO_NAME_MAX];
    while (fgets(line, sizeof(line), fp) && s_fav_count < MAX_FAVS) {
        char *p = strchr(line, '\r');
        if (p) *p = '\0';
        p = strchr(line, '\n');
        if (p) *p = '\0';
        if (line[0] != '\0') {
            strncpy(s_favs[s_fav_count++], line, RADIO_NAME_MAX - 1);
        }
    }
    fclose(fp);
}

static void save_favs(void) {
    FILE *fp = dynapp_fopen_data("favs.txt", "w");
    if (!fp) return;

    for (int i = 0; i < s_fav_count; ++i) {
        fprintf(fp, "%s\n", s_favs[i]);
    }
    fclose(fp);
}

static int is_fav(const char *name) {
    if (!name || name[0] == '\0') return 0;
    for (int i = 0; i < s_fav_count; ++i) {
        if (strcmp(s_favs[i], name) == 0) return 1;
    }
    return 0;
}

static void toggle_fav(const char *name) {
    if (!name || name[0] == '\0') return;
    for (int i = 0; i < s_fav_count; ++i) {
        if (strcmp(s_favs[i], name) == 0) {
            for (int j = i; j < s_fav_count - 1; ++j) {
                strncpy(s_favs[j], s_favs[j + 1], RADIO_NAME_MAX);
            }
            s_fav_count--;
            save_favs();
            return;
        }
    }
    if (s_fav_count < MAX_FAVS) {
        strncpy(s_favs[s_fav_count++], name, RADIO_NAME_MAX - 1);
        save_favs();
    }
}

static void play_station_at(int idx) {
    int total = radio_catalog_count();
    if (idx < 0 || idx >= total) return;

    radio_station_t st;
    if (radio_catalog_get(idx, &st)) {
        s_cur_index = idx;
        radio_play_indexed(st.name, st.url, idx);
        dynapp_toast_show("正在连接电台...", 1500);
    }
}

static void refresh_list(void);

static void on_scope_clicked(lv_event_t *e) {
    (void)e;
    s_fav_only = !s_fav_only;
    s_page = 0;
    lv_label_set_text(s_scope_btn_lbl, s_fav_only ? "仅收藏" : "全部");
    refresh_list();
}

static void on_page_prev(lv_event_t *e) {
    (void)e;
    if (s_page > 0) {
        s_page--;
        refresh_list();
    }
}

static void on_page_next(lv_event_t *e) {
    (void)e;
    s_page++;
    refresh_list();
}

static void on_play_clicked(lv_event_t *e) {
    (void)e;
    radio_player_snap_t snap;
    radio_player_snapshot(&snap);
    if (snap.state == RADIO_PLAYING || snap.state == RADIO_CONNECTING) {
        radio_stop();
    } else {
        if (s_cur_index >= 0) {
            play_station_at(s_cur_index);
        } else {
            play_station_at(0);
        }
    }
}

static void on_prev_clicked(lv_event_t *e) {
    (void)e;
    int total = radio_catalog_count();
    if (total <= 0) return;
    int next = (s_cur_index > 0) ? s_cur_index - 1 : total - 1;
    play_station_at(next);
}

static void on_next_clicked(lv_event_t *e) {
    (void)e;
    int total = radio_catalog_count();
    if (total <= 0) return;
    int next = (s_cur_index + 1 < total) ? s_cur_index + 1 : 0;
    play_station_at(next);
}

static void on_rand_clicked(lv_event_t *e) {
    (void)e;
    int total = radio_catalog_count();
    if (total <= 0) return;
    int next = rand() % total;
    play_station_at(next);
}

static void on_fav_clicked(lv_event_t *e) {
    (void)e;
    if (s_cur_index >= 0) {
        radio_station_t st;
        if (radio_catalog_get(s_cur_index, &st)) {
            toggle_fav(st.name);
            lv_label_set_text(s_fav_btn_lbl, is_fav(st.name) ? "★" : "☆");
            if (s_fav_only) refresh_list();
        }
    }
}

static void on_vol_down(lv_event_t *e) {
    (void)e;
    if (s_volume > 0) {
        s_volume -= 10;
        if (s_volume < 0) s_volume = 0;
        radio_set_volume(s_volume);
        char buf[32];
        snprintf(buf, sizeof(buf), "音量: %d%%", s_volume);
        lv_label_set_text(s_vol_lbl, buf);
    }
}

static void on_vol_up(lv_event_t *e) {
    (void)e;
    if (s_volume < 100) {
        s_volume += 10;
        if (s_volume > 100) s_volume = 100;
        radio_set_volume(s_volume);
        char buf[32];
        snprintf(buf, sizeof(buf), "音量: %d%%", s_volume);
        lv_label_set_text(s_vol_lbl, buf);
    }
}

static void on_station_clicked(lv_event_t *e) {
    uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    play_station_at((int)idx);
}

static void update_status_ui(void) {
    if (!s_scr || !lv_obj_is_valid(s_scr)) return;

    radio_player_snap_t snap;
    radio_player_snapshot(&snap);

    if (snap.station[0] != '\0') {
        lv_label_set_text(s_st_name_lbl, snap.station);
    } else {
        lv_label_set_text(s_st_name_lbl, "未选择电台");
    }

    if (snap.state == RADIO_PLAYING) {
        lv_label_set_text(s_play_btn_lbl, "停止");
        char buf[128];
        if (snap.title[0] != '\0') {
            snprintf(buf, sizeof(buf), "正在播放: %s", snap.title);
        } else {
            snprintf(buf, sizeof(buf), "正在播放 · 采样率: %u Hz", (unsigned)snap.sample_rate);
        }
        lv_label_set_text(s_st_status_lbl, buf);
    } else if (snap.state == RADIO_CONNECTING) {
        lv_label_set_text(s_play_btn_lbl, "停止");
        lv_label_set_text(s_st_status_lbl, "正在连接广播流...");
    } else if (snap.state == RADIO_ERROR) {
        lv_label_set_text(s_play_btn_lbl, "重试");
        char buf[128];
        snprintf(buf, sizeof(buf), "播放出错: %s", snap.err[0] ? snap.err : "网络断开");
        lv_label_set_text(s_st_status_lbl, buf);
    } else {
        lv_label_set_text(s_play_btn_lbl, "播放");
        lv_label_set_text(s_st_status_lbl, "已停止");
    }

    if (snap.station[0] != '\0') {
        lv_label_set_text(s_fav_btn_lbl, is_fav(snap.station) ? "★" : "☆");
    }
}

static void timer_cb(lv_timer_t *tmr) {
    (void)tmr;
    update_status_ui();
}

static void refresh_list(void) {
    if (!s_scr || !lv_obj_is_valid(s_scr)) return;
    lv_obj_clean(s_list_box);

    int total_catalog = radio_catalog_count();
    int visible_indices[350];
    int visible_count = 0;

    for (int i = 0; i < total_catalog && visible_count < 350; ++i) {
        if (!s_fav_only) {
            visible_indices[visible_count++] = i;
        } else {
            radio_station_t st;
            if (radio_catalog_get(i, &st) && is_fav(st.name)) {
                visible_indices[visible_count++] = i;
            }
        }
    }

    int total_pages = (visible_count + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE;
    if (total_pages <= 0) total_pages = 1;
    if (s_page >= total_pages) s_page = total_pages - 1;
    if (s_page < 0) s_page = 0;

    char pbuf[32];
    snprintf(pbuf, sizeof(pbuf), "%d / %d", s_page + 1, total_pages);
    lv_label_set_text(s_page_lbl, pbuf);

    if (visible_count == 0) {
        lv_obj_t *tip = lv_label_create(s_list_box);
        lv_label_set_text(tip, s_fav_only ? "暂无收藏电台\n点击播放器 ★ 按钮收藏电台" : "电台目录为空");
        const lv_font_t *f18 = get_font(18, 0);
        if (f18) lv_obj_set_style_text_font(tip, f18, 0);
        lv_obj_set_width(tip, lv_pct(100));
        lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    int start = s_page * ROWS_PER_PAGE;
    int end = start + ROWS_PER_PAGE;
    if (end > visible_count) end = visible_count;

    for (int i = start; i < end; ++i) {
        int cat_idx = visible_indices[i];
        radio_station_t st;
        if (!radio_catalog_get(cat_idx, &st)) continue;

        lv_obj_t *row = lv_obj_create(s_list_box);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 40);
        lv_obj_set_style_bg_color(row, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(row, lv_color_black(), 0);
        lv_obj_set_style_border_width(row, (cat_idx == s_cur_index) ? 2 : 1, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_pad_hor(row, 10, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, on_station_clicked, LV_EVENT_CLICKED, (void*)(uintptr_t)cat_idx);

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, st.name);
        const lv_font_t *f18 = get_font(18, (cat_idx == s_cur_index) ? 2 : 0);
        if (f18) lv_obj_set_style_text_font(name, f18, 0);
        lv_obj_clear_flag(name, LV_OBJ_FLAG_CLICKABLE);

        if (is_fav(st.name)) {
            lv_obj_t *fav_star = lv_label_create(row);
            lv_label_set_text(fav_star, "★");
            if (f18) lv_obj_set_style_text_font(fav_star, f18, 0);
            lv_obj_clear_flag(fav_star, LV_OBJ_FLAG_CLICKABLE);
        }
    }
}

static void on_screen_deleted(lv_event_t *e) {
    (void)e;
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    s_scr = NULL;
    s_st_name_lbl = NULL;
    s_st_status_lbl = NULL;
    s_play_btn_lbl = NULL;
    s_fav_btn_lbl = NULL;
    s_vol_lbl = NULL;
    s_scope_btn_lbl = NULL;
    s_page_lbl = NULL;
    s_list_box = NULL;

    radio_stop();
    radio_player_shutdown();
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *txt, lv_coord_t w, lv_coord_t h, lv_event_cb_t cb, void *user_data, lv_obj_t **out_lbl) {
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, txt);
    const lv_font_t *f18 = get_font(18, 2);
    if (f18) lv_obj_set_style_text_font(lbl, f18, 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    if (out_lbl) *out_lbl = lbl;
    return btn;
}

static void *radio_create(void) {
    radio_player_start();
    load_favs();

    lv_obj_t *scr = lv_obj_create(NULL);
    s_scr = scr;
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 800);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(scr, on_screen_deleted, LV_EVENT_DELETE, NULL);

    dynapp_vk_attach(scr, "radio", radio_create, radio_on_vk_key);

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
    lv_label_set_text(title_lbl, "收音机");
    const lv_font_t *f26 = get_font(26, 2);
    if (f26) lv_obj_set_style_text_font(title_lbl, f26, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_black(), 0);
    lv_obj_align(title_lbl, LV_ALIGN_CENTER, 0, 0);

    // 全部 / 仅收藏切换按钮
    make_btn(header, "全部", 76, 38, on_scope_clicked, NULL, &s_scope_btn_lbl);
    lv_obj_align(lv_obj_get_parent(s_scope_btn_lbl), LV_ALIGN_RIGHT_MID, -12, 0);

    // 主内容区
    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, 456, 696);
    lv_obj_set_pos(body, 12, 96);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 8, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    // 播放器状态卡片
    lv_obj_t *card = lv_obj_create(body);
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
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    s_st_name_lbl = lv_label_create(card);
    lv_label_set_text(s_st_name_lbl, "未选择电台");
    const lv_font_t *f24 = get_font(24, 2);
    if (f24) lv_obj_set_style_text_font(s_st_name_lbl, f24, 0);

    s_st_status_lbl = lv_label_create(card);
    lv_label_set_text(s_st_status_lbl, "已停止");
    const lv_font_t *f16 = get_font(16, 0);
    if (f16) lv_obj_set_style_text_font(s_st_status_lbl, f16, 0);
    lv_obj_set_width(s_st_status_lbl, lv_pct(100));
    lv_label_set_long_mode(s_st_status_lbl, LV_LABEL_LONG_WRAP);

    // 控制栏 1: [上一台] [播放] [下一台] [随机] [★]
    lv_obj_t *ctl1 = lv_obj_create(card);
    lv_obj_remove_style_all(ctl1);
    lv_obj_set_size(ctl1, lv_pct(100), 42);
    lv_obj_set_flex_flow(ctl1, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctl1, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(ctl1, LV_OBJ_FLAG_SCROLLABLE);

    make_btn(ctl1, "‹ 上一台", 90, 38, on_prev_clicked, NULL, NULL);
    make_btn(ctl1, "播放", 80, 38, on_play_clicked, NULL, &s_play_btn_lbl);
    make_btn(ctl1, "下一台 ›", 90, 38, on_next_clicked, NULL, NULL);
    make_btn(ctl1, "随机", 64, 38, on_rand_clicked, NULL, NULL);
    make_btn(ctl1, "☆", 52, 38, on_fav_clicked, NULL, &s_fav_btn_lbl);

    // 控制栏 2: 音量调节
    lv_obj_t *ctl2 = lv_obj_create(card);
    lv_obj_remove_style_all(ctl2);
    lv_obj_set_size(ctl2, lv_pct(100), 38);
    lv_obj_set_flex_flow(ctl2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctl2, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ctl2, 16, 0);
    lv_obj_clear_flag(ctl2, LV_OBJ_FLAG_SCROLLABLE);

    make_btn(ctl2, "音量 -", 76, 32, on_vol_down, NULL, NULL);
    s_vol_lbl = lv_label_create(ctl2);
    lv_label_set_text(s_vol_lbl, "音量: 70%");
    if (f16) lv_obj_set_style_text_font(s_vol_lbl, f16, 0);
    make_btn(ctl2, "音量 +", 76, 32, on_vol_up, NULL, NULL);

    // 目录栏与翻页
    lv_obj_t *nav_bar = lv_obj_create(body);
    lv_obj_remove_style_all(nav_bar);
    lv_obj_set_size(nav_bar, lv_pct(100), 36);
    lv_obj_set_flex_flow(nav_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav_bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(nav_bar, LV_OBJ_FLAG_SCROLLABLE);

    make_btn(nav_bar, "‹ 上页", 76, 32, on_page_prev, NULL, NULL);
    s_page_lbl = lv_label_create(nav_bar);
    lv_label_set_text(s_page_lbl, "1 / 1");
    if (f16) lv_obj_set_style_text_font(s_page_lbl, f16, 0);
    make_btn(nav_bar, "下页 ›", 76, 32, on_page_next, NULL, NULL);

    // 电台列表容器
    s_list_box = lv_obj_create(body);
    lv_obj_remove_style_all(s_list_box);
    lv_obj_set_size(s_list_box, lv_pct(100), 380);
    lv_obj_set_flex_flow(s_list_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list_box, 6, 0);
    lv_obj_clear_flag(s_list_box, LV_OBJ_FLAG_SCROLLABLE);

    refresh_list();
    update_status_ui();

    s_timer = lv_timer_create(timer_cb, 1000, NULL);

    return scr;
}

void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "radio";
    out->name = "收音机";
    out->icon = "icon.png";
    out->create = radio_create;
}
