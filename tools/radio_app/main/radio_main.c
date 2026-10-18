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

#define ROWS_PER_PAGE 8
#define MAX_FAVS 128
#define MAX_CUSTOM_STATIONS 256

typedef enum {
    SCOPE_CUSTOM = 0,
    SCOPE_BUILTIN = 1,
    SCOPE_FAV = 2,
} radio_scope_t;

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

static radio_scope_t s_scope = SCOPE_CUSTOM;
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

    // 3. 尝试 .m3u
    if (s_custom_count == 0) {
        fp = fopen("/sdcard/metalio/e-ink/radio/stations.m3u", "r");
        if (!fp) fp = dynapp_fopen_data("stations.m3u", "r");
        if (fp) {
            parse_m3u_stations(fp);
            fclose(fp);
        }
    }

    // 4. 尝试 .txt
    if (s_custom_count == 0) {
        fp = fopen("/sdcard/metalio/e-ink/radio/stations.txt", "r");
        if (!fp) fp = dynapp_fopen_data("stations.txt", "r");
        if (fp) {
            parse_txt_stations(fp);
            fclose(fp);
        }
    }

    // 5. 若均不存在，自动生成示范模板到应用私有 data/stations.json
    if (s_custom_count == 0) {
        FILE *wfp = dynapp_fopen_data("stations.json", "w");
        if (wfp) {
            const char *tpl = "[\n"
                "  {\"name\": \"中国之声\", \"url\": \"https://lhttp.qtfm.cn/live/5021381/64k.mp3\"},\n"
                "  {\"name\": \"北京音乐广播\", \"url\": \"https://lhttp.qtfm.cn/live/332/64k.mp3\"},\n"
                "  {\"name\": \"上海交通广播\", \"url\": \"http://lhttp.qingting.fm/live/266/64k.mp3\"}\n"
                "]\n";
            fputs(tpl, wfp);
            fclose(wfp);
            parse_json_stations(tpl);
        }
    }
}

// ==================== 3. 播放控制 ====================
static void play_station(const char *name, const char *url, int cat_idx) {
    if (!name || !url) return;
    strncpy(s_cur_playing_name, name, sizeof(s_cur_playing_name) - 1);
    s_cur_playing_name[sizeof(s_cur_playing_name) - 1] = '\0';

    radio_play_indexed(name, url, cat_idx);
    dynapp_toast_show("正在连接电台...", 1500);
}

static void refresh_list(void);

static void update_scope_label(void) {
    if (!s_scope_btn_lbl) return;
    if (s_scope == SCOPE_CUSTOM) {
        char buf[32];
        snprintf(buf, sizeof(buf), "自定义 (%d)", s_custom_count);
        lv_label_set_text(s_scope_btn_lbl, buf);
    } else if (s_scope == SCOPE_BUILTIN) {
        lv_label_set_text(s_scope_btn_lbl, "内置 (339)");
    } else {
        lv_label_set_text(s_scope_btn_lbl, "仅收藏");
    }
}

static void on_scope_clicked(lv_event_t *e) {
    (void)e;
    if (s_custom_count > 0) {
        if (s_scope == SCOPE_CUSTOM) {
            s_scope = SCOPE_BUILTIN;
        } else if (s_scope == SCOPE_BUILTIN) {
            s_scope = SCOPE_FAV;
        } else {
            s_scope = SCOPE_CUSTOM;
        }
    } else {
        s_scope = (s_scope == SCOPE_BUILTIN) ? SCOPE_FAV : SCOPE_BUILTIN;
    }
    s_page = 0;
    update_scope_label();
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
        return;
    }

    if (s_scope == SCOPE_CUSTOM && s_custom_count > 0) {
        play_station(s_custom_stations[0].name, s_custom_stations[0].url, -1);
    } else {
        radio_station_t st;
        if (radio_catalog_get(0, &st)) {
            play_station(st.name, st.url, 0);
        }
    }
}

static void on_prev_clicked(lv_event_t *e) {
    (void)e;
    if (s_scope == SCOPE_CUSTOM && s_custom_count > 0) {
        int cur_idx = -1;
        for (int i = 0; i < s_custom_count; ++i) {
            if (strcmp(s_custom_stations[i].name, s_cur_playing_name) == 0) {
                cur_idx = i;
                break;
            }
        }
        int next = (cur_idx > 0) ? cur_idx - 1 : s_custom_count - 1;
        play_station(s_custom_stations[next].name, s_custom_stations[next].url, -1);
    } else {
        int total = radio_catalog_count();
        if (total <= 0) return;
        radio_player_snap_t snap;
        radio_player_snapshot(&snap);
        int cur = snap.catalog_index;
        int next = (cur > 0) ? cur - 1 : total - 1;
        radio_station_t st;
        if (radio_catalog_get(next, &st)) {
            play_station(st.name, st.url, next);
        }
    }
}

static void on_next_clicked(lv_event_t *e) {
    (void)e;
    if (s_scope == SCOPE_CUSTOM && s_custom_count > 0) {
        int cur_idx = -1;
        for (int i = 0; i < s_custom_count; ++i) {
            if (strcmp(s_custom_stations[i].name, s_cur_playing_name) == 0) {
                cur_idx = i;
                break;
            }
        }
        int next = (cur_idx + 1 < s_custom_count) ? cur_idx + 1 : 0;
        play_station(s_custom_stations[next].name, s_custom_stations[next].url, -1);
    } else {
        int total = radio_catalog_count();
        if (total <= 0) return;
        radio_player_snap_t snap;
        radio_player_snapshot(&snap);
        int cur = snap.catalog_index;
        int next = (cur + 1 < total) ? cur + 1 : 0;
        radio_station_t st;
        if (radio_catalog_get(next, &st)) {
            play_station(st.name, st.url, next);
        }
    }
}

static void on_rand_clicked(lv_event_t *e) {
    (void)e;
    if (s_scope == SCOPE_CUSTOM && s_custom_count > 0) {
        int next = rand() % s_custom_count;
        play_station(s_custom_stations[next].name, s_custom_stations[next].url, -1);
    } else {
        int total = radio_catalog_count();
        if (total <= 0) return;
        int next = rand() % total;
        radio_station_t st;
        if (radio_catalog_get(next, &st)) {
            play_station(st.name, st.url, next);
        }
    }
}

static void on_fav_clicked(lv_event_t *e) {
    (void)e;
    radio_player_snap_t snap;
    radio_player_snapshot(&snap);
    if (snap.station[0] != '\0') {
        toggle_fav(snap.station);
        lv_label_set_text(s_fav_btn_lbl, is_fav(snap.station) ? "★" : "☆");
        if (s_scope == SCOPE_FAV) refresh_list();
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

// 电台行点击回调结构体
typedef struct {
    char name[RADIO_NAME_MAX];
    char url[RADIO_URL_MAX];
    int cat_idx;
} station_click_info_t;

static void on_station_clicked(lv_event_t *e) {
    station_click_info_t *info = (station_click_info_t *)lv_event_get_user_data(e);
    if (info) {
        play_station(info->name, info->url, info->cat_idx);
    }
}

static void update_status_ui(void) {
    if (!s_scr || !lv_obj_is_valid(s_scr)) return;

    radio_player_snap_t snap;
    radio_player_snapshot(&snap);

    if (snap.station[0] != '\0') {
        lv_label_set_text(s_st_name_lbl, snap.station);
        strncpy(s_cur_playing_name, snap.station, sizeof(s_cur_playing_name) - 1);
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

    radio_player_snap_t snap;
    radio_player_snapshot(&snap);

    int total_items = 0;
    if (s_scope == SCOPE_CUSTOM) {
        total_items = s_custom_count;
    } else if (s_scope == SCOPE_BUILTIN) {
        total_items = radio_catalog_count();
    } else {
        total_items = s_fav_count;
    }

    int total_pages = (total_items + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE;
    if (total_pages <= 0) total_pages = 1;
    if (s_page >= total_pages) s_page = total_pages - 1;
    if (s_page < 0) s_page = 0;

    char pbuf[32];
    snprintf(pbuf, sizeof(pbuf), "%d / %d", s_page + 1, total_pages);
    lv_label_set_text(s_page_lbl, pbuf);

    if (total_items == 0) {
        lv_obj_t *tip = lv_label_create(s_list_box);
        if (s_scope == SCOPE_CUSTOM) {
            lv_label_set_text(tip, "暂无外部自定义电台\n可在 SD 卡 radio/stations.json 配置\n支持自定义名字与流地址");
        } else if (s_scope == SCOPE_FAV) {
            lv_label_set_text(tip, "暂无收藏电台\n点击播放器 ★ 按钮收藏电台");
        } else {
            lv_label_set_text(tip, "电台目录为空");
        }
        const lv_font_t *f18 = get_font(18, 0);
        if (f18) lv_obj_set_style_text_font(tip, f18, 0);
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

        if (s_scope == SCOPE_CUSTOM) {
            strncpy(name_buf, s_custom_stations[i].name, sizeof(name_buf) - 1);
            strncpy(url_buf, s_custom_stations[i].url, sizeof(url_buf) - 1);
            cat_idx = -1;
        } else if (s_scope == SCOPE_BUILTIN) {
            radio_station_t st;
            if (radio_catalog_get(i, &st)) {
                strncpy(name_buf, st.name, sizeof(name_buf) - 1);
                strncpy(url_buf, st.url, sizeof(url_buf) - 1);
                cat_idx = i;
            }
        } else { // SCOPE_FAV
            strncpy(name_buf, s_favs[i], sizeof(name_buf) - 1);
            // 优先在自定义电台中查匹配 URL
            bool found = false;
            for (int k = 0; k < s_custom_count; ++k) {
                if (strcmp(s_custom_stations[k].name, name_buf) == 0) {
                    strncpy(url_buf, s_custom_stations[k].url, sizeof(url_buf) - 1);
                    cat_idx = -1;
                    found = true;
                    break;
                }
            }
            if (!found) {
                // 在内置电台中查匹配 URL
                for (int k = 0; k < radio_catalog_count(); ++k) {
                    radio_station_t st;
                    if (radio_catalog_get(k, &st) && strcmp(st.name, name_buf) == 0) {
                        strncpy(url_buf, st.url, sizeof(url_buf) - 1);
                        cat_idx = k;
                        found = true;
                        break;
                    }
                }
            }
        }

        if (name_buf[0] == '\0') continue;

        bool is_playing = (snap.station[0] != '\0' && strcmp(snap.station, name_buf) == 0);

        lv_obj_t *row = lv_obj_create(s_list_box);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 40);
        lv_obj_set_style_bg_color(row, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(row, lv_color_black(), 0);
        lv_obj_set_style_border_width(row, is_playing ? 2 : 1, 0);
        lv_obj_set_style_radius(row, 6, 0);
        lv_obj_set_style_pad_hor(row, 10, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);

        // 分配点击参数，跟随行控件析构自动释放
        station_click_info_t *click_info = (station_click_info_t *)malloc(sizeof(station_click_info_t));
        if (click_info) {
            strncpy(click_info->name, name_buf, sizeof(click_info->name) - 1);
            strncpy(click_info->url, url_buf, sizeof(click_info->url) - 1);
            click_info->cat_idx = cat_idx;
            lv_obj_add_event_cb(row, on_station_clicked, LV_EVENT_CLICKED, click_info);
            lv_obj_add_event_cb(row, (lv_event_cb_t)free, LV_EVENT_DELETE, click_info);
        }

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, name_buf);
        const lv_font_t *f18 = get_font(18, is_playing ? 2 : 0);
        if (f18) lv_obj_set_style_text_font(name, f18, 0);
        lv_obj_clear_flag(name, LV_OBJ_FLAG_CLICKABLE);

        if (is_fav(name_buf)) {
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
    load_custom_stations();

    // 默认展示模式：若有自定义电台则优先展示自定义，否则展示内置
    s_scope = (s_custom_count > 0) ? SCOPE_CUSTOM : SCOPE_BUILTIN;
    s_page = 0;

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
    lv_label_set_text(title_lbl, "网络收音机");
    const lv_font_t *f26 = get_font(26, 2);
    if (f26) lv_obj_set_style_text_font(title_lbl, f26, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_black(), 0);
    lv_obj_align(title_lbl, LV_ALIGN_CENTER, 0, 0);

    // 目录范围切换按钮 (自定义 / 内置 / 仅收藏)
    make_btn(header, "切换", 100, 38, on_scope_clicked, NULL, &s_scope_btn_lbl);
    lv_obj_align(lv_obj_get_parent(s_scope_btn_lbl), LV_ALIGN_RIGHT_MID, -12, 0);
    update_scope_label();

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
