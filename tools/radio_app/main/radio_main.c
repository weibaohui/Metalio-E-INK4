// tools/radio_app/main/radio_main.c
// 网络收音机 动态应用：.app Bundle 规范版
// 支持外部自定义电台（stations.json / stations.m3u / stations.txt）+ 系统内置 339 台 + 收藏夹
// 纯 C + LVGL，仅调用固件符号表导出接口。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cJSON.h>

#include "lvgl.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"
#include "services/radio/radio_player.h"
#include "services/radio/radio_catalog.h"

#define ROWS_PER_PAGE 10
#define MAX_FAVS 128
#define MAX_CUSTOM_STATIONS 256

typedef enum {
    SCOPE_CUSTOM = 0,
    SCOPE_BUILTIN = 1,
    SCOPE_FAV = 2,
} radio_scope_t;

static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_st_name_lbl = NULL;
static lv_obj_t *s_st_title_lbl = NULL;
static lv_obj_t *s_st_status_lbl = NULL;
static lv_obj_t *s_play_btn_lbl = NULL;
static lv_obj_t *s_fav_btn_lbl = NULL;
static lv_obj_t *s_scope_btn_lbl = NULL;
static lv_obj_t *s_page_lbl = NULL;
static lv_obj_t *s_list_box = NULL;
static lv_obj_t *s_footer_lbl = NULL;
static lv_timer_t *s_timer = NULL;

static radio_scope_t s_scope = SCOPE_BUILTIN;
static int s_page = 0;
static int s_volume = 70;
static char s_cur_playing_name[RADIO_NAME_MAX] = "";

// 收藏夹 (保存电台名称)
static char s_favs[MAX_FAVS][RADIO_NAME_MAX];
static int s_fav_count = 0;

// 自定义电台列表
static radio_station_t s_custom_stations[MAX_CUSTOM_STATIONS];
static int s_custom_count = 0;

static void *radio_create(void);

static const lv_font_t *get_font(int size, int weight) {
    if (size >= 30) {
        const lv_font_t *f = fontpack_lv_font_get(30, 2);
        if (f) return f;
    }
    return fontpack_lv_font_ui();
}

static void on_back_clicked(lv_event_t *e) {
    (void)e;
    dynapp_screen_go_home();
}

static void refresh_list(void);

static int radio_on_vk_key(const char* key) {
    if (key != NULL) {
        if (strcmp(key, "esc") == 0 || strcmp(key, "back") == 0 || strcmp(key, "vk_home") == 0) {
            dynapp_screen_go_home();
            return 1;
        }
        if (strcmp(key, "vk_prev") == 0) {
            if (s_page > 0) {
                s_page--;
                refresh_list();
            }
            return 1;
        }
        if (strcmp(key, "vk_next") == 0) {
            s_page++;
            refresh_list();
            return 1;
        }
    }
    return 0;
}

// ==================== 1. 收藏夹存储 ====================
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

// ==================== 2. 外部自定义电台读取 ====================
static void parse_json_stations(const char *buf) {
    cJSON *root = cJSON_Parse(buf);
    if (!root) return;

    if (cJSON_IsArray(root)) {
        cJSON *item = NULL;
        cJSON_ArrayForEach(item, root) {
            if (!item || s_custom_count >= MAX_CUSTOM_STATIONS) break;
            cJSON *j_name = cJSON_GetObjectItemCaseSensitive(item, "name");
            cJSON *j_url = cJSON_GetObjectItemCaseSensitive(item, "url");
            if (cJSON_IsString(j_name) && cJSON_IsString(j_url) &&
                j_name->valuestring && j_url->valuestring &&
                j_name->valuestring[0] != '\0' && j_url->valuestring[0] != '\0') {
                strncpy(s_custom_stations[s_custom_count].name, j_name->valuestring, RADIO_NAME_MAX - 1);
                s_custom_stations[s_custom_count].name[RADIO_NAME_MAX - 1] = '\0';
                strncpy(s_custom_stations[s_custom_count].url, j_url->valuestring, RADIO_URL_MAX - 1);
                s_custom_stations[s_custom_count].url[RADIO_URL_MAX - 1] = '\0';
                s_custom_count++;
            }
        }
    }
    cJSON_Delete(root);
}

static void parse_m3u_stations(FILE *fp) {
    char line[512];
    char pending_name[RADIO_NAME_MAX] = "";

    while (fgets(line, sizeof(line), fp) && s_custom_count < MAX_CUSTOM_STATIONS) {
        char *p = strchr(line, '\r');
        if (p) *p = '\0';
        p = strchr(line, '\n');
        if (p) *p = '\0';

        if (strncmp(line, "#EXTINF:", 8) == 0) {
            char *comma = strchr(line, ',');
            if (comma) {
                comma++;
                while (*comma == ' ') comma++;
                strncpy(pending_name, comma, RADIO_NAME_MAX - 1);
                pending_name[RADIO_NAME_MAX - 1] = '\0';
            }
        } else if (strncmp(line, "http://", 7) == 0 || strncmp(line, "https://", 8) == 0) {
            if (pending_name[0] == '\0') {
                snprintf(pending_name, sizeof(pending_name), "自定义电台 %d", s_custom_count + 1);
            }
            strncpy(s_custom_stations[s_custom_count].name, pending_name, RADIO_NAME_MAX - 1);
            s_custom_stations[s_custom_count].name[RADIO_NAME_MAX - 1] = '\0';
            strncpy(s_custom_stations[s_custom_count].url, line, RADIO_URL_MAX - 1);
            s_custom_stations[s_custom_count].url[RADIO_URL_MAX - 1] = '\0';
            s_custom_count++;
            pending_name[0] = '\0';
        }
    }
}

static void parse_txt_stations(FILE *fp) {
    char line[512];
    while (fgets(line, sizeof(line), fp) && s_custom_count < MAX_CUSTOM_STATIONS) {
        char *p = strchr(line, '\r');
        if (p) *p = '\0';
        p = strchr(line, '\n');
        if (p) *p = '\0';
        if (line[0] == '#' || line[0] == '\0') continue;

        char *sep = strchr(line, ',');
        if (!sep) sep = strchr(line, ';');
        if (!sep) sep = strchr(line, '\t');
        if (sep) {
            *sep = '\0';
            char *url = sep + 1;
            while (*url == ' ') url++;
            if (strncmp(url, "http", 4) == 0) {
                strncpy(s_custom_stations[s_custom_count].name, line, RADIO_NAME_MAX - 1);
                s_custom_stations[s_custom_count].name[RADIO_NAME_MAX - 1] = '\0';
                strncpy(s_custom_stations[s_custom_count].url, url, RADIO_URL_MAX - 1);
                s_custom_stations[s_custom_count].url[RADIO_URL_MAX - 1] = '\0';
                s_custom_count++;
            }
        }
    }
}

static void load_custom_stations(void) {
    s_custom_count = 0;

    // 1. 优先尝试 /sdcard/metalio/e-ink/radio/stations.json
    FILE *fp = fopen("/sdcard/metalio/e-ink/radio/stations.json", "rb");
    if (!fp) {
        // 2. 尝试应用私有数据目录 data/stations.json
        fp = dynapp_fopen_data("stations.json", "rb");
    }

    if (fp) {
        fseek(fp, 0, SEEK_END);
        long sz = ftell(fp);
        fseek(fp, 0, SEEK_SET);
        if (sz > 0 && sz < 128 * 1024) {
            char *buf = (char *)malloc(sz + 1);
            if (buf) {
                size_t nr = fread(buf, 1, sz, fp);
                buf[nr] = '\0';
                parse_json_stations(buf);
                free(buf);
            }
        }
        fclose(fp);
    }

    // 3. 尝试 /sdcard/metalio/e-ink/radio/stations.m3u
    if (s_custom_count == 0) {
        fp = fopen("/sdcard/metalio/e-ink/radio/stations.m3u", "r");
        if (!fp) fp = dynapp_fopen_data("stations.m3u", "r");
        if (fp) {
            parse_m3u_stations(fp);
            fclose(fp);
        }
    }

    // 4. 尝试 /sdcard/metalio/e-ink/radio/stations.txt
    if (s_custom_count == 0) {
        fp = fopen("/sdcard/metalio/e-ink/radio/stations.txt", "r");
        if (!fp) fp = dynapp_fopen_data("stations.txt", "r");
        if (fp) {
            parse_txt_stations(fp);
            fclose(fp);
        }
    }
}

// ==================== 3. 视图与播放控制 ====================
static int view_total_count(void) {
    if (s_scope == SCOPE_CUSTOM) return s_custom_count;
    if (s_scope == SCOPE_BUILTIN) return radio_catalog_count();
    return s_fav_count;
}

static int get_station_in_view(int view_idx, char *out_name, size_t name_sz, char *out_url, size_t url_sz, int *out_cat_idx) {
    if (s_scope == SCOPE_CUSTOM) {
        if (view_idx < 0 || view_idx >= s_custom_count) return 0;
        strncpy(out_name, s_custom_stations[view_idx].name, name_sz - 1);
        out_name[name_sz - 1] = '\0';
        strncpy(out_url, s_custom_stations[view_idx].url, url_sz - 1);
        out_url[url_sz - 1] = '\0';
        if (out_cat_idx) *out_cat_idx = -1;
        return 1;
    } else if (s_scope == SCOPE_BUILTIN) {
        if (view_idx < 0 || view_idx >= radio_catalog_count()) return 0;
        radio_station_t st;
        if (radio_catalog_get(view_idx, &st)) {
            strncpy(out_name, st.name, name_sz - 1);
            out_name[name_sz - 1] = '\0';
            strncpy(out_url, st.url, url_sz - 1);
            out_url[url_sz - 1] = '\0';
            if (out_cat_idx) *out_cat_idx = view_idx;
            return 1;
        }
        return 0;
    } else { // SCOPE_FAV
        if (view_idx < 0 || view_idx >= s_fav_count) return 0;
        const char *fav_name = s_favs[view_idx];
        strncpy(out_name, fav_name, name_sz - 1);
        out_name[name_sz - 1] = '\0';
        // 优先在自定义电台中查 URL
        for (int k = 0; k < s_custom_count; ++k) {
            if (strcmp(s_custom_stations[k].name, fav_name) == 0) {
                strncpy(out_url, s_custom_stations[k].url, url_sz - 1);
                out_url[url_sz - 1] = '\0';
                if (out_cat_idx) *out_cat_idx = -1;
                return 1;
            }
        }
        // 内置电台中查 URL
        for (int k = 0; k < radio_catalog_count(); ++k) {
            radio_station_t st;
            if (radio_catalog_get(k, &st) && strcmp(st.name, fav_name) == 0) {
                strncpy(out_url, st.url, url_sz - 1);
                out_url[url_sz - 1] = '\0';
                if (out_cat_idx) *out_cat_idx = k;
                return 1;
            }
        }
        out_url[0] = '\0';
        if (out_cat_idx) *out_cat_idx = -1;
        return 1;
    }
}

static void play_station(const char *name, const char *url, int cat_idx) {
    if (!name || !url) return;
    strncpy(s_cur_playing_name, name, sizeof(s_cur_playing_name) - 1);
    s_cur_playing_name[sizeof(s_cur_playing_name) - 1] = '\0';

    radio_play_indexed(name, url, cat_idx);
}

static void step_station(int delta) {
    int total = view_total_count();
    if (total <= 0) return;
    int cur = 0;
    for (int i = 0; i < total; ++i) {
        char name[RADIO_NAME_MAX];
        char url[RADIO_URL_MAX];
        int cat_idx = -1;
        if (get_station_in_view(i, name, sizeof(name), url, sizeof(url), &cat_idx)) {
            if (s_cur_playing_name[0] != '\0' && strcmp(s_cur_playing_name, name) == 0) {
                cur = i;
                break;
            }
        }
    }
    int next = (delta >= 0) ? ((cur + 1) % total) : ((cur + total - 1) % total);
    if (s_cur_playing_name[0] == '\0' && delta == 0) next = 0;
    char name[RADIO_NAME_MAX];
    char url[RADIO_URL_MAX];
    int cat_idx = -1;
    if (get_station_in_view(next, name, sizeof(name), url, sizeof(url), &cat_idx)) {
        play_station(name, url, cat_idx);
    }
}

static void update_scope_label(void) {
    if (!s_scope_btn_lbl) return;
    if (s_scope == SCOPE_CUSTOM) {
        lv_label_set_text(s_scope_btn_lbl, "外部电台");
    } else if (s_scope == SCOPE_BUILTIN) {
        lv_label_set_text(s_scope_btn_lbl, "全部电台");
    } else {
        lv_label_set_text(s_scope_btn_lbl, "收藏夹");
    }
}

static void on_scope_clicked(lv_event_t *e) {
    (void)e;
    if (s_custom_count > 0) {
        if (s_scope == SCOPE_BUILTIN) {
            s_scope = SCOPE_CUSTOM;
        } else if (s_scope == SCOPE_CUSTOM) {
            s_scope = SCOPE_FAV;
        } else {
            s_scope = SCOPE_BUILTIN;
        }
    } else {
        s_scope = (s_scope == SCOPE_BUILTIN) ? SCOPE_FAV : SCOPE_BUILTIN;
    }
    s_page = 0;
    update_scope_label();
    refresh_list();
    dynapp_toast_show(s_scope == SCOPE_FAV ? "已切换至收藏夹" :
                      (s_scope == SCOPE_CUSTOM ? "已切换至外部电台" : "已切换至全部电台"), 1200);
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
        s_cur_playing_name[0] = '\0';
    } else {
        if (s_cur_playing_name[0] != '\0') {
            char name[RADIO_NAME_MAX];
            char url[RADIO_URL_MAX];
            int cat_idx = -1;
            int total = view_total_count();
            for (int i = 0; i < total; ++i) {
                if (get_station_in_view(i, name, sizeof(name), url, sizeof(url), &cat_idx) &&
                    strcmp(name, s_cur_playing_name) == 0 && url[0] != '\0') {
                    play_station(name, url, cat_idx);
                    return;
                }
            }
        }
        step_station(1);
    }
}

static void on_prev_clicked(lv_event_t *e) {
    (void)e;
    step_station(-1);
}

static void on_next_clicked(lv_event_t *e) {
    (void)e;
    step_station(1);
}

static void on_rand_clicked(lv_event_t *e) {
    (void)e;
    int total = view_total_count();
    if (total <= 0) return;
    int pick = rand() % total;
    char name[RADIO_NAME_MAX];
    char url[RADIO_URL_MAX];
    int cat_idx = -1;
    if (get_station_in_view(pick, name, sizeof(name), url, sizeof(url), &cat_idx)) {
        play_station(name, url, cat_idx);
    }
}

static void update_status_ui(void);

static void on_fav_clicked(lv_event_t *e) {
    (void)e;
    radio_player_snap_t snap;
    radio_player_snapshot(&snap);
    const char *name = snap.station[0] ? snap.station : s_cur_playing_name;
    if (name[0] != '\0') {
        const int was_fav = is_fav(name);
        toggle_fav(name);
        dynapp_toast_show(was_fav ? "已取消收藏" : "已加入收藏", 1200);
        refresh_list();
        update_status_ui();
    }
}

static void on_vol_down(lv_event_t *e) {
    (void)e;
    if (s_volume > 0) {
        s_volume -= 10;
        if (s_volume < 0) s_volume = 0;
        radio_set_volume(s_volume);
        dynapp_radio_set_volume(s_volume);
        char buf[32];
        snprintf(buf, sizeof(buf), "音量 %d%%", s_volume);
        dynapp_toast_show(buf, 1000);
        update_status_ui();
    }
}

static void on_vol_up(lv_event_t *e) {
    (void)e;
    if (s_volume < 100) {
        s_volume += 10;
        if (s_volume > 100) s_volume = 100;
        radio_set_volume(s_volume);
        dynapp_radio_set_volume(s_volume);
        char buf[32];
        snprintf(buf, sizeof(buf), "音量 %d%%", s_volume);
        dynapp_toast_show(buf, 1000);
        update_status_ui();
    }
}

static void on_station_clicked(lv_event_t *e) {
    int view_idx = (int)(intptr_t)lv_event_get_user_data(e);
    char name[RADIO_NAME_MAX];
    char url[RADIO_URL_MAX];
    int cat_idx = -1;
    if (get_station_in_view(view_idx, name, sizeof(name), url, sizeof(url), &cat_idx)) {
        play_station(name, url, cat_idx);
    }
}

static void update_status_ui(void) {
    if (!s_scr || !lv_obj_is_valid(s_scr)) return;

    radio_player_snap_t snap;
    radio_player_snapshot(&snap);

    if (snap.station[0] != '\0') {
        lv_label_set_text(s_st_name_lbl, snap.station);
        if (snap.state == RADIO_PLAYING || snap.state == RADIO_CONNECTING) {
            strncpy(s_cur_playing_name, snap.station, sizeof(s_cur_playing_name) - 1);
            s_cur_playing_name[sizeof(s_cur_playing_name) - 1] = '\0';
        }
    } else {
        lv_label_set_text(s_st_name_lbl, "网络收音机");
    }

    if (s_st_title_lbl) {
        lv_label_set_text(s_st_title_lbl, snap.title[0] != '\0' ? snap.title : " ");
    }

    const char *state_str = "已停止";
    if (snap.state == RADIO_PLAYING) {
        state_str = "播放中";
    } else if (snap.state == RADIO_CONNECTING) {
        state_str = "连接中…";
    } else if (snap.state == RADIO_ERROR) {
        state_str = (snap.err[0] != '\0') ? snap.err : "出错";
    }

    char state_buf[128];
    snprintf(state_buf, sizeof(state_buf), "%s · 音量 %d%%", state_str, s_volume);
    lv_label_set_text(s_st_status_lbl, state_buf);

    if (s_play_btn_lbl) {
        lv_label_set_text(s_play_btn_lbl,
            (snap.state == RADIO_PLAYING || snap.state == RADIO_CONNECTING) ? "停止" : "播放");
    }

    if (s_fav_btn_lbl) {
        const char *chk = snap.station[0] ? snap.station : s_cur_playing_name;
        lv_label_set_text(s_fav_btn_lbl, is_fav(chk) ? "★ 已藏" : "☆ 收藏");
    }

    if (s_footer_lbl) {
        char foot[128];
        if (snap.state == RADIO_ERROR) {
            snprintf(foot, sizeof(foot), "播放失败: %s", snap.err[0] ? snap.err : "网络断开");
        } else if (snap.state == RADIO_PLAYING || snap.state == RADIO_CONNECTING) {
            snprintf(foot, sizeof(foot), "正在收听 %s", s_cur_playing_name);
        } else {
            snprintf(foot, sizeof(foot), "点「播放」续听上次电台,或从列表选台");
        }
        lv_label_set_text(s_footer_lbl, foot);
    }
}

static void timer_cb(lv_timer_t *tmr) {
    (void)tmr;
    update_status_ui();
}

static void refresh_list(void) {
    if (!s_scr || !lv_obj_is_valid(s_scr) || !s_list_box) return;
    lv_obj_clean(s_list_box);

    radio_player_snap_t snap;
    radio_player_snapshot(&snap);

    int total_items = view_total_count();
    int total_pages = (total_items + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE;
    if (total_pages <= 0) total_pages = 1;
    if (s_page >= total_pages) s_page = total_pages - 1;
    if (s_page < 0) s_page = 0;

    if (s_page_lbl) {
        char pbuf[32];
        snprintf(pbuf, sizeof(pbuf), "%d/%d", s_page + 1, total_pages);
        lv_label_set_text(s_page_lbl, pbuf);
    }

    if (total_items == 0) {
        lv_obj_t *tip = lv_label_create(s_list_box);
        if (s_scope == SCOPE_CUSTOM) {
            lv_label_set_text(tip, "暂无外部自定义电台\n可在 SD 卡 radio/stations.json 配置\n支持自定义名字与流地址");
        } else if (s_scope == SCOPE_FAV) {
            lv_label_set_text(tip, "暂无收藏电台\n点击播放器 ☆ 收藏 按钮添加");
        } else {
            lv_label_set_text(tip, "电台目录为空");
        }
        lv_obj_set_style_text_font(tip, fontpack_lv_font_ui(), 0);
        lv_obj_set_width(tip, lv_pct(100));
        lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    int start = s_page * ROWS_PER_PAGE;
    int end = start + ROWS_PER_PAGE;
    if (end > total_items) end = total_items;

    for (int i = start; i < end; ++i) {
        char name_buf[RADIO_NAME_MAX] = "";
        char url_buf[RADIO_URL_MAX] = "";
        int cat_idx = -1;
        if (!get_station_in_view(i, name_buf, sizeof(name_buf), url_buf, sizeof(url_buf), &cat_idx)) {
            continue;
        }

        bool is_playing = (snap.station[0] != '\0' && strcmp(snap.station, name_buf) == 0);
        bool fav = is_fav(name_buf);

        lv_obj_t *row = lv_obj_create(s_list_box);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 32);
        lv_obj_set_style_bg_color(row, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(row, lv_color_black(), 0);
        lv_obj_set_style_border_width(row, is_playing ? 2 : 1, 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_pad_left(row, 10, 0);
        lv_obj_set_style_pad_right(row, 10, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

        // 绑定下标参数（直接值传递，零堆分配与析构安全）
        lv_obj_add_event_cb(row, on_station_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        char item_txt[RADIO_NAME_MAX + 16];
        if (is_playing) {
            snprintf(item_txt, sizeof(item_txt), "● %s", name_buf);
        } else if (fav) {
            snprintf(item_txt, sizeof(item_txt), "★ %s", name_buf);
        } else {
            snprintf(item_txt, sizeof(item_txt), "%s", name_buf);
        }

        lv_obj_t *name_lbl = lv_label_create(row);
        lv_label_set_text(name_lbl, item_txt);
        lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name_lbl, lv_pct(100));
        lv_obj_set_style_text_font(name_lbl, fontpack_lv_font_ui(), 0);
        lv_obj_set_style_text_color(name_lbl, lv_color_black(), 0);
        lv_obj_clear_flag(name_lbl, LV_OBJ_FLAG_CLICKABLE);
    }
}

static void on_screen_deleted(lv_event_t *e) {
    (void)e;
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_scr == NULL) return;
    s_scr = NULL;
    s_st_name_lbl = NULL;
    s_st_title_lbl = NULL;
    s_st_status_lbl = NULL;
    s_play_btn_lbl = NULL;
    s_fav_btn_lbl = NULL;
    s_scope_btn_lbl = NULL;
    s_page_lbl = NULL;
    s_list_box = NULL;
    s_footer_lbl = NULL;

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
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, txt);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl, (w > 16) ? (w - 16) : w);
    lv_obj_set_style_text_font(lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    if (out_lbl) *out_lbl = lbl;
    return btn;
}

static void *radio_create(void) {
    radio_player_start();
    load_favs();
    load_custom_stations();

    // 默认展示内置 339 台目录（若用户 SD 卡有 stations.json 可随时按「切换」到外部电台）
    s_scope = SCOPE_BUILTIN;
    s_page = 0;

    s_volume = dynapp_radio_get_volume();
    if (s_volume <= 0) s_volume = 70;

    lv_obj_t *scr = lv_obj_create(NULL);
    s_scr = scr;
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 800);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(scr, on_screen_deleted, LV_EVENT_SCREEN_UNLOADED, NULL);
    lv_obj_add_event_cb(scr, on_screen_deleted, LV_EVENT_DELETE, NULL);

    dynapp_screen_status_bar(scr);
    dynapp_vk_attach(scr, "radio", radio_create, radio_on_vk_key);

    // 主内容区 (无返回顶栏，由底部 vk_home 触控键统一控制返回桌面)
    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, 456, 750);
    lv_obj_set_pos(body, 12, 42);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 8, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    // 1. 播放器状态卡片 (132px 高度，与原版完全一致)
    lv_obj_t *card = lv_obj_create(body);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, 132);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 6, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    s_st_name_lbl = lv_label_create(card);
    lv_label_set_text(s_st_name_lbl, "网络收音机");
    lv_obj_set_style_text_font(s_st_name_lbl, get_font(30, 2), 0);
    lv_obj_set_style_text_color(s_st_name_lbl, lv_color_black(), 0);
    lv_label_set_long_mode(s_st_name_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_st_name_lbl, lv_pct(100));

    s_st_title_lbl = lv_label_create(card);
    lv_label_set_text(s_st_title_lbl, " ");
    lv_obj_set_style_text_font(s_st_title_lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(s_st_title_lbl, lv_color_black(), 0);
    lv_label_set_long_mode(s_st_title_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_st_title_lbl, lv_pct(100));

    s_st_status_lbl = lv_label_create(card);
    lv_label_set_text(s_st_status_lbl, "已停止 · 音量 70%");
    lv_obj_set_style_text_font(s_st_status_lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(s_st_status_lbl, lv_color_black(), 0);
    lv_label_set_long_mode(s_st_status_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_st_status_lbl, lv_pct(100));

    // 2. 控制栏 1: [上一台] [播放] [下一台] (136px, 136px, 136px, 高度 44)
    lv_obj_t *ctl1 = lv_obj_create(body);
    lv_obj_remove_style_all(ctl1);
    lv_obj_set_size(ctl1, lv_pct(100), 44);
    lv_obj_set_flex_flow(ctl1, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctl1, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(ctl1, LV_OBJ_FLAG_SCROLLABLE);

    make_btn(ctl1, "上一台", 136, 44, on_prev_clicked, NULL, NULL);
    make_btn(ctl1, "播放", 136, 44, on_play_clicked, NULL, &s_play_btn_lbl);
    make_btn(ctl1, "下一台", 136, 44, on_next_clicked, NULL, NULL);

    // 3. 控制栏 2: [随机] [☆ 收藏] [音量-] [音量+] (100px, 100px, 100px, 100px, 高度 44)
    lv_obj_t *ctl2 = lv_obj_create(body);
    lv_obj_remove_style_all(ctl2);
    lv_obj_set_size(ctl2, lv_pct(100), 44);
    lv_obj_set_flex_flow(ctl2, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctl2, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(ctl2, LV_OBJ_FLAG_SCROLLABLE);

    make_btn(ctl2, "随机", 100, 44, on_rand_clicked, NULL, NULL);
    make_btn(ctl2, "☆ 收藏", 100, 44, on_fav_clicked, NULL, &s_fav_btn_lbl);
    make_btn(ctl2, "音量-", 100, 44, on_vol_down, NULL, NULL);
    make_btn(ctl2, "音量+", 100, 44, on_vol_up, NULL, NULL);

    // 4. 目录栏与翻页: [<] [1/1] [>] [切换范围] (90px, 90px, 90px, 110px, 高度 44)
    lv_obj_t *nav_bar = lv_obj_create(body);
    lv_obj_remove_style_all(nav_bar);
    lv_obj_set_size(nav_bar, lv_pct(100), 44);
    lv_obj_set_flex_flow(nav_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav_bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(nav_bar, LV_OBJ_FLAG_SCROLLABLE);

    make_btn(nav_bar, "<", 90, 44, on_page_prev, NULL, NULL);
    s_page_lbl = lv_label_create(nav_bar);
    lv_label_set_text(s_page_lbl, "1/1");
    lv_obj_set_width(s_page_lbl, 90);
    lv_obj_set_style_text_align(s_page_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_page_lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(s_page_lbl, lv_color_black(), 0);
    make_btn(nav_bar, ">", 90, 44, on_page_next, NULL, NULL);
    make_btn(nav_bar, "全部电台", 110, 44, on_scope_clicked, NULL, &s_scope_btn_lbl);
    update_scope_label();

    // 5. 电台列表容器 (10 行高度 356)
    s_list_box = lv_obj_create(body);
    lv_obj_remove_style_all(s_list_box);
    lv_obj_set_size(s_list_box, lv_pct(100), ROWS_PER_PAGE * 32 + (ROWS_PER_PAGE - 1) * 4);
    lv_obj_set_flex_flow(s_list_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list_box, 4, 0);
    lv_obj_clear_flag(s_list_box, LV_OBJ_FLAG_SCROLLABLE);

    // 6. 底部提示栏
    s_footer_lbl = lv_label_create(body);
    lv_label_set_text(s_footer_lbl, "点「播放」续听上次电台,或从列表选台");
    lv_obj_set_width(s_footer_lbl, lv_pct(100));
    lv_label_set_long_mode(s_footer_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_footer_lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(s_footer_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(s_footer_lbl, LV_OBJ_FLAG_CLICKABLE);

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
