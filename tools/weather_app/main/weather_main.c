// tools/weather_app/main/weather_main.c
// 天气 动态应用：墨水屏当"第二屏"的经典形态——一天刷几次，静默待着。
//
// 设计要点（都是墨水屏逼出来的）：
//   1. 大温度用七段数码管（纯几何，1bpp 下最清晰）；图标也是几何拼的，不用位图。
//      灭掉的段一律不画——留一圈空心框/细鬼影只会把字形搞脏。
//   2. 不跳秒：只在数据真的变了的时候重画。定时刷新 30 分钟一次，值没变就不刷屏。
//   3. 断网不白屏：读数缓存在 app 自己的 data/ 目录，下次打开先显示上次读数，
//      联网成功后自动覆盖。墨水屏常断电常待机，缓存是刚需。
//   4. 能熄屏只出声：点「朗读」用固件的 TTS 播报天气（电子墨水的独占能力）。
//   5. 城市库 330+ 个（全国地级市 + 港澳台），面板按拼音首字母过滤 + 分页，
//      不做成滚动的——墨水屏拖动会留满残影，翻页一次只刷一屏反而干净。
//
// 数据源：Open-Meteo（免注册、免 key、JSON、体积小）
//   https://api.open-meteo.com/v1/forecast?latitude=..&longitude=..&current=..&daily=..
// JSON 解析用固件导出的 cJSON。
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"
#include "cJSON.h"

#include "../../_shared/sevenseg.h"
#include "city_db.h" // 自动生成的城市库（纯字符数组）

// ================= 配置 =================
#define CITY_NAME_MAX 24
#define MAX_CUSTOM 16     // cities.txt 里的自定义城市上限
#define NDAYS 3
#define REFRESH_MS (30 * 60 * 1000) // 30 分钟自动刷新一次
#define DATA_FILE "weather.txt"
#define CITY_FILE "cities.txt"
#define API_FILE "api.txt"
#define DEFAULT_API "https://api.open-meteo.com/v1/forecast"

// 城市面板
#define ROWS_PER_PAGE 11
#define ROW_H 35
#define LETTER_COLS 13

typedef struct {
    char name[CITY_NAME_MAX];
    int lat_e4;
    int lon_e4;
    char initial; // 拼音首字母
} City;

typedef struct {
    int temp;   // 当前温度（整数，℃）
    int feels;  // 体感温度
    int hum;    // 相对湿度 %
    int wind;   // 风速 km/h
    int code;   // WMO 天气代码
    int tmax[NDAYS];
    int tmin[NDAYS];
    int dcode[NDAYS];
    int pop[NDAYS]; // 降水概率 %
    char obs[24];  // 观测时刻 "MM-DD HH:MM"（城市本地时）
    long updated;  // 本地抓取时刻 epoch
    int valid;
} Weather;

// ================= 全局状态 =================
static City s_custom[MAX_CUSTOM];
static int s_custom_n = 0;
static int s_db_n = 0;    // kCityDb 条目数
static int s_city_idx = 0; // 当前城市（统一索引空间：0..db-1 是库，db.. 是自定义）
static Weather s_w;
static int s_loading = 0;
static int s_have_data = 0;

static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_city_lbl = NULL;
static lv_obj_t *s_time_lbl = NULL;
static SevenSeg s_seg;
static lv_obj_t *s_icon_box = NULL;
static lv_obj_t *s_desc_lbl = NULL;
static lv_obj_t *s_feels_lbl = NULL;
static lv_obj_t *s_hum_lbl = NULL;
static lv_obj_t *s_wind_lbl = NULL;
static lv_obj_t *s_pop_lbl = NULL;
static lv_obj_t *s_day_lbl[NDAYS] = {NULL};
static lv_obj_t *s_fc_lbl[NDAYS] = {NULL};
static lv_obj_t *s_fct_lbl[NDAYS] = {NULL};
static lv_obj_t *s_fcp_lbl[NDAYS] = {NULL};
static lv_obj_t *s_status_lbl = NULL;
static lv_obj_t *s_src_lbl = NULL;
static lv_obj_t *s_hint_lbl = NULL;

// 城市面板
static lv_obj_t *s_picker = NULL;
static lv_obj_t *s_picker_list = NULL;
static lv_obj_t *s_picker_status = NULL;
static lv_obj_t *s_letter_lbl[26] = {NULL};
static lv_obj_t *s_letter_hl = NULL;
static lv_obj_t *s_btn_prev = NULL;
static lv_obj_t *s_btn_next = NULL;
static char s_filter = 0; // 0 = 全部，否则 'A'..'Z'
static int s_page = 0;
static int s_total_rows = 0;
static int s_row_total = 0;
static int s_row_on_page = 0;
static int s_match_cities = 0;

typedef struct {
    int kind; // 0 = 组标题, 1 = 城市
    int idx;  // 城市在统一索引空间里的下标
    char initial;
} Row;
static Row s_rows[ROWS_PER_PAGE];

static lv_timer_t *s_auto_timer = NULL;
static lv_timer_t *s_retry_timer = NULL;
static lv_timer_t *s_first_timer = NULL;

static void picker_build_page(void);
static void schedule_retry(void);
static void build_picker(void);
static void fetch_weather(void);

// 内存不够时 2 分钟后再试一次（别让用户干等半小时）
static void on_retry_tick(lv_timer_t *tmr) {
    (void)tmr;
    s_retry_timer = NULL;
    fetch_weather();
}

static void schedule_retry(void) {
    if (s_retry_timer != NULL) return;
    s_retry_timer = lv_timer_create(on_retry_tick, 120000, NULL);
}

// 首次取数延后 8 秒：设备刚开机那会儿网络栈还在建连/碎片化，
// 这时候硬发 HTTPS 最容易把网络打死（实测过整机软死）。
static void on_first_fetch(lv_timer_t *tmr) {
    (void)tmr;
    s_first_timer = NULL;
    fetch_weather();
}

// ================= UTF-8 边界 =================
// 汉字 3 字节，按字节硬截会把一个字劈成两半 → 非法 UTF-8 → 显示豆腐块。
static size_t utf8_char_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

static void utf8_clip(char *s, size_t max) {
    size_t len = strlen(s);
    if (len <= max) return;
    size_t end = max;
    size_t start = end;
    while (start > 0 && ((unsigned char)s[start] & 0xC0) == 0x80) start--;
    size_t need = utf8_char_len((unsigned char)s[start]);
    if (start + need > end) end = start;
    s[end] = '\0';
}

// ================= 字体 =================
static const lv_font_t *ui_font(void) {
    const lv_font_t *f = fontpack_lv_font_ui();
    return f != NULL ? f : fontpack_lv_font_get(25, 2);
}
static const lv_font_t *title_font(void) {
    const lv_font_t *f = fontpack_lv_font_get(30, 2);
    return f != NULL ? f : ui_font();
}
static const lv_font_t *latin_font(void) {
    const lv_font_t *f = fontpack_lv_font_latin(18);
    return f != NULL ? f : ui_font();
}

// ================= 小工具 =================
static lv_obj_t *mk_rect(lv_obj_t *parent, int x, int y, int w, int h, int radius, int filled) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_color(o, lv_color_black(), 0);
    lv_obj_set_style_border_color(o, lv_color_black(), 0);
    if (filled) {
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(o, 0, 0);
    } else {
        lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(o, 2, 0);
    }
    lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *mk_label(lv_obj_t *parent, const char *text, const lv_font_t *f, int x, int y,
                          lv_color_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    if (f != NULL) lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_pos(l, x, y);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
    return l;
}

static lv_obj_t *mk_button(lv_obj_t *parent, const char *text, int x, int y, int w, int h,
                           lv_event_cb_t cb, void *ud) {
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
    if (cb != NULL) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    const lv_font_t *f = ui_font();
    if (f != NULL) lv_obj_set_style_text_font(lbl, f, 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

// ================= WMO 天气代码 =================
static const char *wmo_text(int code) {
    switch (code) {
    case 0: return "晴";
    case 1: return "晴间多云";
    case 2: return "多云";
    case 3: return "阴";
    case 45: return "雾";
    case 48: return "雾凇";
    case 51: return "毛毛雨";
    case 53: return "毛毛雨";
    case 55: return "密集毛毛雨";
    case 56: return "冻毛毛雨";
    case 57: return "冻雨";
    case 61: return "小雨";
    case 63: return "中雨";
    case 65: return "大雨";
    case 66: return "冻雨";
    case 67: return "强冻雨";
    case 71: return "小雪";
    case 73: return "中雪";
    case 75: return "大雪";
    case 77: return "米雪";
    case 80: return "阵雨";
    case 81: return "强阵雨";
    case 82: return "暴雨";
    case 85: return "阵雪";
    case 86: return "强阵雪";
    case 95: return "雷阵雨";
    case 96: return "雷雨伴冰雹";
    case 99: return "强雷雨伴冰雹";
    default: return "未知";
    }
}

// 预报列里的短名（最多 4 个汉字）
static const char *wmo_short(int code) {
    if (code == 0 || code == 1) return "晴";
    if (code == 2) return "多云";
    if (code == 3) return "阴";
    if (code == 45 || code == 48) return "雾";
    if (code == 51 || code == 53 || code == 55) return "毛毛雨";
    if (code == 56 || code == 57 || code == 66 || code == 67) return "冻雨";
    if (code == 61) return "小雨";
    if (code == 63) return "中雨";
    if (code == 65) return "大雨";
    if (code == 80 || code == 81) return "阵雨";
    if (code == 82) return "暴雨";
    if (code == 71) return "小雪";
    if (code == 73) return "中雪";
    if (code == 75) return "大雪";
    if (code == 77) return "米雪";
    if (code == 85 || code == 86) return "阵雪";
    if (code >= 95) return "雷阵雨";
    return "—";
}

// 图标类别：0 晴 1 云 2 雾 3 雨 4 雪 5 雷
static int icon_category(int code) {
    if (code == 0 || code == 1) return 0;
    if (code == 45 || code == 48) return 2;
    if (code == 51 || code == 53 || code == 55 || code == 56 || code == 57 || code == 61 || code == 63 ||
        code == 65 || code == 66 || code == 67 || code == 80 || code == 81 || code == 82)
        return 3;
    if (code == 71 || code == 73 || code == 75 || code == 77 || code == 85 || code == 86) return 4;
    if (code >= 95) return 5;
    return 1;
}

// 把 度×10000 格式回 "39.9042"
static void fmt_deg(char *out, size_t n, int e4) {
    const int a = e4 / 10000;
    const int b = e4 % 10000;
    if (b < 0) {
        snprintf(out, n, "%d.%04d", a, -b);
    } else {
        snprintf(out, n, "%d.%04d", a, b);
    }
}

// 四舍五入取整。
// 固件导出表里有 __floatsidf / __subdf3 / __gedf2（int→double、double 减法、
// double 比较），唯独没有 double→int 的 __fixdfsi。所以不能直接 (int)v，
// 而是借 cJSON 已经算好的截断值 valueint，再用小数部分决定进位。
static int json_round(const cJSON *it) {
    const double v = it->valuedouble;
    const int base = it->valueint;        // cJSON 内部 (int)valuedouble，截断向零
    const double frac = v - (double)base; // 小数部分
    if (base >= 0) return (frac >= 0.5) ? (base + 1) : base;
    return (frac <= -0.5) ? (base - 1) : base;
}

// ================= 天气图标（纯几何） =================
static void draw_cloud(lv_obj_t *p, int dy) {
    mk_rect(p, 8, dy + 16, 20, 20, 10, 1);
    mk_rect(p, 24, dy + 8, 26, 28, 13, 1);
    mk_rect(p, 44, dy + 16, 16, 20, 10, 1);
    mk_rect(p, 12, dy + 32, 42, 12, 6, 1);
}

static void draw_icon(int code) {
    if (s_icon_box == NULL) return;
    lv_obj_clean(s_icon_box);
    const int cat = icon_category(code);
    switch (cat) {
    case 0: // 晴：实心圆 + 外圈
        mk_rect(s_icon_box, 18, 18, 28, 28, 14, 1);
        mk_rect(s_icon_box, 6, 6, 52, 52, 26, 0);
        break;
    case 1: // 云
        draw_cloud(s_icon_box, 12);
        break;
    case 2: // 雾：三条横线
        mk_rect(s_icon_box, 8, 14, 48, 6, 3, 1);
        mk_rect(s_icon_box, 14, 28, 36, 6, 3, 1);
        mk_rect(s_icon_box, 8, 42, 48, 6, 3, 1);
        break;
    case 3: // 雨：云 + 三条雨线
        draw_cloud(s_icon_box, 2);
        mk_rect(s_icon_box, 14, 46, 5, 14, 2, 1);
        mk_rect(s_icon_box, 29, 46, 5, 14, 2, 1);
        mk_rect(s_icon_box, 44, 46, 5, 14, 2, 1);
        break;
    case 4: // 雪：云 + 三个点
        draw_cloud(s_icon_box, 2);
        mk_rect(s_icon_box, 13, 48, 7, 7, 3, 1);
        mk_rect(s_icon_box, 28, 52, 7, 7, 3, 1);
        mk_rect(s_icon_box, 43, 48, 7, 7, 3, 1);
        break;
    case 5: // 雷：云 + 折线
        draw_cloud(s_icon_box, 2);
        mk_rect(s_icon_box, 30, 44, 7, 10, 1, 1);
        mk_rect(s_icon_box, 23, 52, 9, 7, 1, 1);
        mk_rect(s_icon_box, 28, 57, 7, 9, 1, 1);
        break;
    default:
        break;
    }
}

// 在 [p, end) 里找字符 c（固件导出表里没有 memchr）
static const char *find_chr(const char *p, const char *end, char c) {
    while (p < end) {
        if (*p == c) return p;
        p++;
    }
    return NULL;
}

// ================= 城市库 =================
// 统一索引空间：0 .. db_n-1 是内置库，db_n .. 是 cities.txt 里的自定义。
// 不用指针数组存名字——动态应用里全局 const char* 指针表会触发 LoadProhibited。
static int db_count(void) {
    if (s_db_n > 0) return s_db_n;
    int n = 0;
    for (const char *p = kCityDb; *p != '\0';) {
        const char *eol = strchr(p, '\n');
        if (eol == NULL) break;
        if (find_chr(p, eol, '|') != NULL) n++;
        p = eol + 1;
    }
    s_db_n = n;
    return n;
}

static int db_get(int idx, City *out) {
    const char *p = kCityDb;
    int i = 0;
    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        if (eol == NULL) eol = p + strlen(p);
        const char *f1 = find_chr(p, eol, '|');
        if (f1 != NULL) {
            const char *f2 = find_chr(f1 + 1, eol, '|');
            if (f2 != NULL) {
                const char *f3 = find_chr(f2 + 1, eol, '|');
                if (f3 != NULL) {
                    if (i == idx) {
                        // 行格式：名称|首字母|纬度|经度
                        //   f1 = 第 1 个 '|'（名称后）
                        //   f2 = 第 2 个 '|'（首字母后）→ [f2+1, f3) 是纬度
                        //   f3 = 第 3 个 '|'（纬度后）   → [f3+1, eol) 是经度
                        size_t nl = (size_t)(f1 - p);
                        if (nl >= CITY_NAME_MAX) nl = CITY_NAME_MAX - 1;
                        memcpy(out->name, p, nl);
                        out->name[nl] = '\0';
                        out->initial = (f1 + 1 < f2) ? f1[1] : '#';
                        char nb[16];
                        size_t l1 = (size_t)(f3 - (f2 + 1));
                        if (l1 >= sizeof(nb)) l1 = sizeof(nb) - 1;
                        memcpy(nb, f2 + 1, l1);
                        nb[l1] = '\0';
                        out->lat_e4 = atoi(nb);
                        size_t l2 = (size_t)(eol - (f3 + 1));
                        if (l2 >= sizeof(nb)) l2 = sizeof(nb) - 1;
                        memcpy(nb, f3 + 1, l2);
                        nb[l2] = '\0';
                        out->lon_e4 = atoi(nb);
                        return 1;
                    }
                    i++;
                }
            }
        }
        p = (*eol == '\n') ? eol + 1 : eol;
    }
    return 0;
}

static int city_total(void) { return db_count() + s_custom_n; }

static int city_get(int idx, City *out) {
    if (idx < 0) return 0;
    if (idx < db_count()) return db_get(idx, out);
    const int ci = idx - db_count();
    if (ci < 0 || ci >= s_custom_n) return 0;
    *out = s_custom[ci];
    return 1;
}

// 按名字找城市（单趟扫描，别写成「每个城市再扫一遍」的 O(n²)——
// 353 个城市时那会白吃掉 1 秒 LVGL 线程）
static int city_find(const char *name) {
    const char *p = kCityDb;
    int i = 0;
    while (*p != '\0') {
        const char *eol = strchr(p, '\n');
        if (eol == NULL) eol = p + strlen(p);
        const char *f1 = find_chr(p, eol, '|');
        if (f1 != NULL) {
            const size_t nl = (size_t)(f1 - p);
            char buf[CITY_NAME_MAX];
            const size_t cl = (nl >= CITY_NAME_MAX) ? CITY_NAME_MAX - 1 : nl;
            memcpy(buf, p, cl);
            buf[cl] = '\0';
            if (strcmp(buf, name) == 0) return i;
            i++;
        }
        p = (*eol == '\n') ? eol + 1 : eol;
    }
    for (int k = 0; k < s_custom_n; ++k) {
        if (strcmp(s_custom[k].name, name) == 0) return db_count() + k;
    }
    return -1;
}

// 解析十进制度字符串 → 度×10000 的整数（不碰浮点）
static int parse_deg(const char *s, int *out) {
    int sign = 1;
    if (*s == '-') {
        sign = -1;
        s++;
    } else if (*s == '+') {
        s++;
    }
    if (*s < '0' || *s > '9') return 0;
    int deg = 0;
    while (*s >= '0' && *s <= '9') {
        deg = deg * 10 + (*s - '0');
        s++;
    }
    int frac = 0, ndig = 0;
    if (*s == '.') {
        s++;
        while (*s >= '0' && *s <= '9') {
            if (ndig < 4) {
                frac = frac * 10 + (*s - '0');
                ndig++;
            }
            s++;
        }
    }
    while (ndig < 4) {
        frac *= 10;
        ndig++;
    }
    *out = sign * (deg * 10000 + frac);
    return 1;
}

// 从 app data 目录读自定义城市：每行 "名称,纬度,经度"
static void load_custom_cities(void) {
    s_custom_n = 0;
    FILE *fp = (FILE *)dynapp_fopen_data(CITY_FILE, "r");
    if (fp == NULL) return;
    char line[128];
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '\0' || *p == '\n' || *p == '#') continue;
        char *comma = strchr(p, ',');
        if (comma == NULL) continue;
        *comma = '\0';
        char *comma2 = strchr(comma + 1, ',');
        if (comma2 == NULL) continue;
        *comma2 = '\0';
        size_t nl = strlen(p);
        while (nl > 0 && (p[nl - 1] == '\n' || p[nl - 1] == '\r' || p[nl - 1] == ' ')) p[--nl] = '\0';
        nl = strlen(comma2);
        while (nl > 0 && (comma2[nl - 1] == '\n' || comma2[nl - 1] == '\r' || comma2[nl - 1] == ' '))
            comma2[--nl] = '\0';
        if (p[0] == '\0') continue;
        int lat = 0, lon = 0;
        if (!parse_deg(comma + 1, &lat)) continue;
        if (!parse_deg(comma2 + 1, &lon)) continue;
        if (lat < -900000 || lat > 900000 || lon < -1800000 || lon > 1800000) continue;
        if (city_find(p) >= 0) continue;
        if (s_custom_n >= MAX_CUSTOM) break;
        City *c = &s_custom[s_custom_n++];
        snprintf(c->name, CITY_NAME_MAX, "%s", p);
        utf8_clip(c->name, CITY_NAME_MAX - 1);
        c->lat_e4 = lat;
        c->lon_e4 = lon;
        c->initial = 'Z'; // 自定义城市排在库后面，单独一个组
    }
    fclose(fp);
}

// API 基址可覆盖：data/api.txt 里写一行完整基址（默认 Open-Meteo）。
static void load_api_base(char *out, size_t n) {
    snprintf(out, n, "%s", DEFAULT_API);
    FILE *fp = (FILE *)dynapp_fopen_data(API_FILE, "r");
    if (fp == NULL) return;
    char line[192];
    if (fgets(line, sizeof(line), fp) != NULL) {
        size_t nl = strlen(line);
        while (nl > 0 && (line[nl - 1] == '\n' || line[nl - 1] == '\r' || line[nl - 1] == ' '))
            line[--nl] = '\0';
        if (nl > 0 && strncmp(line, "http", 4) == 0) {
            snprintf(out, n, "%s", line);
        }
    }
    fclose(fp);
}

// ================= 持久化 =================
static void save_state(void) {
    FILE *fp = (FILE *)dynapp_fopen_data(DATA_FILE, "w");
    if (fp == NULL) return;
    City c;
    if (city_get(s_city_idx, &c)) {
        fprintf(fp, "WTH2\nname=%s\nlat=%d\nlon=%d\n", c.name, c.lat_e4, c.lon_e4);
    }
    if (s_w.valid) {
        fprintf(fp, "temp=%d\nfeels=%d\nhum=%d\nwind=%d\ncode=%d\nobs=%s\nupdated=%ld\n", s_w.temp,
                s_w.feels, s_w.hum, s_w.wind, s_w.code, s_w.obs, s_w.updated);
        for (int i = 0; i < NDAYS; ++i)
            fprintf(fp, "d%d=%d,%d,%d,%d\n", i, s_w.dcode[i], s_w.tmax[i], s_w.tmin[i], s_w.pop[i]);
    }
    fclose(fp);
}

static void load_state(void) {
    FILE *fp = (FILE *)dynapp_fopen_data(DATA_FILE, "r");
    if (fp == NULL) return;
    char line[160];
    char name[CITY_NAME_MAX] = {0};
    int lat = 0, lon = 0;
    int have_city = 0;
    int tmp[NDAYS][4];
    memset(tmp, 0, sizeof(tmp));
    int have_days = 0;
    while (fgets(line, sizeof(line), fp) != NULL) {
        size_t nl = strlen(line);
        while (nl > 0 && (line[nl - 1] == '\n' || line[nl - 1] == '\r')) line[--nl] = '\0';
        if (strncmp(line, "name=", 5) == 0) {
            snprintf(name, sizeof(name), "%s", line + 5);
            utf8_clip(name, sizeof(name) - 1);
            have_city = 1;
        } else if (strncmp(line, "lat=", 4) == 0) {
            lat = atoi(line + 4);
        } else if (strncmp(line, "lon=", 4) == 0) {
            lon = atoi(line + 4);
        } else if (strncmp(line, "temp=", 5) == 0) {
            s_w.temp = atoi(line + 5);
            s_w.valid = 1;
        } else if (strncmp(line, "feels=", 6) == 0) {
            s_w.feels = atoi(line + 6);
        } else if (strncmp(line, "hum=", 4) == 0) {
            s_w.hum = atoi(line + 4);
        } else if (strncmp(line, "wind=", 5) == 0) {
            s_w.wind = atoi(line + 5);
        } else if (strncmp(line, "code=", 5) == 0) {
            s_w.code = atoi(line + 5);
        } else if (strncmp(line, "obs=", 4) == 0) {
            snprintf(s_w.obs, sizeof(s_w.obs), "%s", line + 4);
        } else if (strncmp(line, "updated=", 8) == 0) {
            s_w.updated = (long)strtol(line + 8, NULL, 10);
        } else if (line[0] == 'd' && line[1] >= '0' && line[1] <= '2' && line[2] == '=') {
            const int i = line[1] - '0';
            int a = 0, b = 0, c2 = 0, d = 0;
            if (sscanf(line + 3, "%d,%d,%d,%d", &a, &b, &c2, &d) == 4) {
                tmp[i][0] = a;
                tmp[i][1] = b;
                tmp[i][2] = c2;
                tmp[i][3] = d;
                have_days = 1;
            }
        }
    }
    fclose(fp);
    if (have_city && name[0] != '\0') {
        int idx = city_find(name);
        if (idx < 0 && s_custom_n < MAX_CUSTOM) {
            // 上次选的城市不在库里（自定义或老版本存的）：补进自定义区
            City *c = &s_custom[s_custom_n++];
            snprintf(c->name, CITY_NAME_MAX, "%s", name);
            utf8_clip(c->name, CITY_NAME_MAX - 1);
            c->lat_e4 = lat;
            c->lon_e4 = lon;
            c->initial = 'Z';
            idx = db_count() + s_custom_n - 1;
        }
        if (idx >= 0) s_city_idx = idx;
    }
    if (have_days) {
        for (int i = 0; i < NDAYS; ++i) {
            s_w.dcode[i] = tmp[i][0];
            s_w.tmax[i] = tmp[i][1];
            s_w.tmin[i] = tmp[i][2];
            s_w.pop[i] = tmp[i][3];
        }
    }
    if (s_w.valid) s_have_data = 1;
}

// ================= 界面 =================
static void render_main(void) {
    if (s_scr == NULL) return;
    City c;
    const int have_city = city_get(s_city_idx, &c);

    if (s_city_lbl != NULL) {
        char b[CITY_NAME_MAX + 4];
        snprintf(b, sizeof(b), "%s", have_city ? c.name : "—");
        utf8_clip(b, sizeof(b) - 1);
        lv_label_set_text(s_city_lbl, b);
    }

    if (!s_have_data) {
        if (s_desc_lbl != NULL) lv_label_set_text(s_desc_lbl, s_loading ? "获取中…" : "暂无数据");
        if (s_status_lbl != NULL)
            lv_label_set_text(s_status_lbl, s_loading ? "正在联网获取天气" : "点「刷新」获取天气");
        if (s_hint_lbl != NULL) lv_label_set_text(s_hint_lbl, "");
        sevenseg_clear(&s_seg);
        return;
    }

    // 大温度：隐藏前导零（8 度不该显示成 "  8"）
    const int v = s_w.temp;
    uint8_t blank = 0;
    if (v >= 0) {
        if (v < 10) blank |= 0x03;
        else if (v < 100) blank |= 0x01;
    }
    sevenseg_set_mask(&s_seg, v, 1, blank);

    if (s_desc_lbl != NULL) lv_label_set_text(s_desc_lbl, wmo_text(s_w.code));
    draw_icon(s_w.code);

    char b[64];
    snprintf(b, sizeof(b), "体感 %d℃", s_w.feels);
    utf8_clip(b, sizeof(b) - 1);
    if (s_feels_lbl != NULL) lv_label_set_text(s_feels_lbl, b);

    snprintf(b, sizeof(b), "湿度 %d%%", s_w.hum);
    utf8_clip(b, sizeof(b) - 1);
    if (s_hum_lbl != NULL) lv_label_set_text(s_hum_lbl, b);

    snprintf(b, sizeof(b), "风速 %dkm/h", s_w.wind);
    utf8_clip(b, sizeof(b) - 1);
    if (s_wind_lbl != NULL) lv_label_set_text(s_wind_lbl, b);

    snprintf(b, sizeof(b), "降水 %d%%", s_w.pop[0]);
    utf8_clip(b, sizeof(b) - 1);
    if (s_pop_lbl != NULL) lv_label_set_text(s_pop_lbl, b);

    static const char *kDayName[NDAYS] = {"今天", "明天", "后天"};
    for (int i = 0; i < NDAYS; ++i) {
        if (s_day_lbl[i] != NULL) lv_label_set_text(s_day_lbl[i], kDayName[i]);
        if (s_fc_lbl[i] != NULL) lv_label_set_text(s_fc_lbl[i], wmo_short(s_w.dcode[i]));
        if (s_fct_lbl[i] != NULL) {
            snprintf(b, sizeof(b), "%d/%d℃", s_w.tmax[i], s_w.tmin[i]);
            utf8_clip(b, sizeof(b) - 1);
            lv_label_set_text(s_fct_lbl[i], b);
        }
        if (s_fcp_lbl[i] != NULL) {
            snprintf(b, sizeof(b), "雨 %d%%", s_w.pop[i]);
            utf8_clip(b, sizeof(b) - 1);
            lv_label_set_text(s_fcp_lbl[i], b);
        }
    }

    if (s_time_lbl != NULL) {
        // 右上角只放城市本地钟点（完整日期在状态行），避免长文本顶出屏幕
        // obs 形如 "10-22 07:00"（11 字节），钟点从第 7 个字符开始
        if (strlen(s_w.obs) >= 11) {
            char t[8];
            snprintf(t, sizeof(t), "%s", s_w.obs + 6);
            lv_label_set_text(s_time_lbl, t);
        } else {
            char t[16];
            const time_t now = time(NULL);
            struct tm tmv;
            localtime_r(&now, &tmv);
            snprintf(t, sizeof(t), "%02d:%02d", tmv.tm_hour, tmv.tm_min);
            lv_label_set_text(s_time_lbl, t);
        }
    }

    if (s_status_lbl != NULL) {
        char st[64];
        snprintf(st, sizeof(st), "更新于 %s", s_w.obs[0] != '\0' ? s_w.obs : "—");
        utf8_clip(st, sizeof(st) - 1);
        lv_label_set_text(s_status_lbl, st);
    }
    if (s_hint_lbl != NULL) lv_label_set_text(s_hint_lbl, "");
    if (s_src_lbl != NULL) {
        // 数据源标签跟着实际用的 API 基址走（api.txt 可覆盖）
        char base[192];
        load_api_base(base, sizeof(base));
        const char *p = strstr(base, "://");
        const char *host = (p != NULL) ? (p + 3) : base;
        size_t hl = 0;
        while (host[hl] != '\0' && host[hl] != '/' && hl < sizeof(base)) hl++;
        char src[128];
        snprintf(src, sizeof(src), "%.*s", (int)hl, host);
        utf8_clip(src, sizeof(src) - 1);
        lv_label_set_text(s_src_lbl, src);
    }
}

// ================= 联网 =================
static void on_http_resp(const char *data, int len, int status, void *user_data);

// https 握手要 14KB 连续内部 RAM，内部内存碎片化时硬连会把网络栈饿崩
// （实测整机软死：WiFi 掉线、HTTP 端口全无响应，只有串口还活着）。
// 所以取数前先看最大连续块，不够就等一会儿再说——宁可数据晚到，也别把网络打死。
static int ram_ok_for_tls(void) {
    dynapp_sysinfo_t info;
    if (dynapp_sysinfo_get(&info) != 0) return 1; // 拿不到就按可以处理
    return info.ram_largest >= 16 * 1024;
}

static void fetch_weather(void) {
    City c;
    if (!city_get(s_city_idx, &c)) return;
    if (s_loading) return;
    if (!ram_ok_for_tls()) {
        if (s_status_lbl != NULL) lv_label_set_text(s_status_lbl, "内存紧张 · 稍后更新");
        schedule_retry();
        return;
    }
    if (dynapp_is_network_ready() == 0) {
        if (s_status_lbl != NULL)
            lv_label_set_text(s_status_lbl, s_have_data ? "离线 · 显示上次读数" : "无网络连接");
        if (s_hint_lbl != NULL) lv_label_set_text(s_hint_lbl, "联网后再点「刷新」");
        dynapp_toast_show("无网络连接", 1400);
        return;
    }
    char lat[24], lon[24];
    fmt_deg(lat, sizeof(lat), c.lat_e4);
    fmt_deg(lon, sizeof(lon), c.lon_e4);
    char base[192];
    load_api_base(base, sizeof(base));
    char url[512];
    snprintf(url, sizeof(url),
             "%s?latitude=%s&longitude=%s"
             "&current=temperature_2m,relative_humidity_2m,apparent_temperature,weather_code,"
             "wind_speed_10m"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
             "&timezone=auto&forecast_days=3",
             base, lat, lon);
    s_loading = 1;
    if (s_status_lbl != NULL) lv_label_set_text(s_status_lbl, "正在更新…");
    if (!dynapp_http_get_async(url, on_http_resp, NULL)) {
        s_loading = 0;
        if (s_status_lbl != NULL)
            lv_label_set_text(s_status_lbl, s_have_data ? "离线 · 显示上次读数" : "请求发起失败");
        dynapp_toast_show("请求发起失败", 1400);
    }
}

// HTTP 回调（LVGL 线程）
static void on_http_resp(const char *data, int len, int status, void *user_data) {
    (void)user_data;
    s_loading = 0;
    if (status != 200 || data == NULL || len <= 0) {
        if (s_status_lbl != NULL)
            lv_label_set_text(s_status_lbl, s_have_data ? "离线 · 显示上次读数" : "获取失败");
        if (s_hint_lbl != NULL) lv_label_set_text(s_hint_lbl, "网络不通，稍后自动重试");
        dynapp_toast_show("获取失败", 1400);
        return;
    }
    cJSON *root = cJSON_Parse(data);
    if (root == NULL) {
        if (s_status_lbl != NULL) lv_label_set_text(s_status_lbl, "数据解析失败");
        dynapp_toast_show("数据解析失败", 1400);
        return;
    }
    cJSON *cur = cJSON_GetObjectItem(root, "current");
    if (cur == NULL) {
        cJSON_Delete(root);
        if (s_status_lbl != NULL) lv_label_set_text(s_status_lbl, "返回数据不完整");
        return;
    }
    Weather w;
    memset(&w, 0, sizeof(w));
    cJSON *it = NULL;

// 只用 cJSON 的 valueint / json_round()：本工程不直接做 double→int，
// 固件导出表里没有 __fixdfsi，那样写会在链接期失败。
#define NUM_GET(obj, key, dst)                  \
    do {                                        \
        it = cJSON_GetObjectItem((obj), (key)); \
        if (it != NULL && cJSON_IsNumber(it)) { \
            (dst) = json_round(it);             \
        }                                       \
    } while (0)

    NUM_GET(cur, "temperature_2m", w.temp);
    NUM_GET(cur, "apparent_temperature", w.feels);
    NUM_GET(cur, "relative_humidity_2m", w.hum);
    NUM_GET(cur, "wind_speed_10m", w.wind);
    NUM_GET(cur, "weather_code", w.code);
#undef NUM_GET

    // 观测时刻（城市本地时）"2026-10-22T06:45" → "10-22 06:45"
    it = cJSON_GetObjectItem(cur, "time");
    if (it != NULL && cJSON_IsString(it)) {
        const char *ts = it->valuestring;
        const char *tpos = strchr(ts, 'T');
        if (tpos != NULL && strlen(ts) >= 16) {
            // 日期是 YYYY-MM-DD，取 MM-DD 从第 5 个字符开始
            snprintf(w.obs, sizeof(w.obs), "%.5s %.5s", ts + 5, tpos + 1);
        } else if (ts != NULL) {
            snprintf(w.obs, sizeof(w.obs), "%s", ts);
        }
        utf8_clip(w.obs, sizeof(w.obs) - 1);
    }

    cJSON *daily = cJSON_GetObjectItem(root, "daily");
    if (daily != NULL) {
        cJSON *a_code = cJSON_GetObjectItem(daily, "weather_code");
        cJSON *a_max = cJSON_GetObjectItem(daily, "temperature_2m_max");
        cJSON *a_min = cJSON_GetObjectItem(daily, "temperature_2m_min");
        cJSON *a_pop = cJSON_GetObjectItem(daily, "precipitation_probability_max");
        const int n_code = (a_code != NULL) ? cJSON_GetArraySize(a_code) : 0;
        for (int i = 0; i < NDAYS && i < n_code; ++i) {
            cJSON *e = cJSON_GetArrayItem(a_code, i);
            if (e != NULL && cJSON_IsNumber(e)) w.dcode[i] = e->valueint;
            if (a_max != NULL) {
                e = cJSON_GetArrayItem(a_max, i);
                if (e != NULL && cJSON_IsNumber(e)) w.tmax[i] = json_round(e);
            }
            if (a_min != NULL) {
                e = cJSON_GetArrayItem(a_min, i);
                if (e != NULL && cJSON_IsNumber(e)) w.tmin[i] = json_round(e);
            }
            if (a_pop != NULL) {
                e = cJSON_GetArrayItem(a_pop, i);
                if (e != NULL && cJSON_IsNumber(e)) w.pop[i] = e->valueint;
            }
        }
    }
    cJSON_Delete(root);

    w.updated = (long)time(NULL);
    w.valid = 1;
    // 值没变就别刷屏（墨水屏每刷一次都在老化）
    const int same = s_have_data && s_w.temp == w.temp && s_w.feels == w.feels && s_w.hum == w.hum &&
                     s_w.wind == w.wind && s_w.code == w.code && strcmp(s_w.obs, w.obs) == 0;
    s_w = w;
    s_have_data = 1;
    save_state();
    if (!same) render_main();
    if (s_status_lbl != NULL) {
        char st[64];
        snprintf(st, sizeof(st), "更新于 %s", s_w.obs[0] != '\0' ? s_w.obs : "—");
        utf8_clip(st, sizeof(st) - 1);
        lv_label_set_text(s_status_lbl, st);
    }
    if (s_hint_lbl != NULL) lv_label_set_text(s_hint_lbl, "");
}

// ================= 城市面板 =================
static void on_city_row_clicked(lv_event_t *e) {
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= city_total()) return;
    s_city_idx = idx;
    if (s_picker != NULL) lv_obj_add_flag(s_picker, LV_OBJ_FLAG_HIDDEN);
    s_have_data = 0;
    memset(&s_w, 0, sizeof(s_w));
    render_main();
    save_state();
    fetch_weather();
}

static void picker_update_letters(void) {
    // 高亮用「一根黑条 + 该字母改白字」实现——不用 26 个按钮/标签，
    // 省下几十个 LVGL 对象（墨水屏设备的内部 RAM 很紧，TLS 握手要 14KB 连续块）。
    for (int i = 0; i < 26; ++i) {
        if (s_letter_lbl[i] == NULL) continue;
        const char ch = (char)('A' + i);
        const int on = (s_filter == ch);
        lv_obj_set_style_text_color(s_letter_lbl[i], on ? lv_color_white() : lv_color_black(), 0);
    }
    if (s_letter_hl == NULL) return;
    if (s_filter == 0) {
        lv_obj_add_flag(s_letter_hl, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const int li = s_filter - 'A';
    const int row = li / LETTER_COLS;
    const int col = li % LETTER_COLS;
    lv_obj_set_pos(s_letter_hl, 16 + col * 34, 540 + row * 36);
    lv_obj_clear_flag(s_letter_hl, LV_OBJ_FLAG_HIDDEN);
}

static void picker_build_page(void) {
    if (s_picker_list == NULL) return;
    lv_obj_clean(s_picker_list);

    s_row_total = 0;
    s_row_on_page = 0;
    s_match_cities = 0;
    char cur = 0;
    const int total = city_total();
    for (int i = 0; i < total; ++i) {
        City c;
        if (!city_get(i, &c)) continue;
        if (s_filter != 0 && c.initial != s_filter) continue;
        if (s_filter == 0 && c.initial != cur) {
            cur = c.initial;
            // 组标题占一行
            if (s_row_total >= s_page * ROWS_PER_PAGE && s_row_on_page < ROWS_PER_PAGE) {
                s_rows[s_row_on_page].kind = 0;
                s_rows[s_row_on_page].idx = -1;
                s_rows[s_row_on_page].initial = cur;
                s_row_on_page++;
            }
            s_row_total++;
        }
        if (s_row_total >= s_page * ROWS_PER_PAGE && s_row_on_page < ROWS_PER_PAGE) {
            s_rows[s_row_on_page].kind = 1;
            s_rows[s_row_on_page].idx = i;
            s_rows[s_row_on_page].initial = c.initial;
            s_row_on_page++;
        }
        s_row_total++;
        s_match_cities++;
    }
    s_total_rows = s_row_total;

    int pages = (s_total_rows + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE;
    if (pages <= 0) pages = 1;
    if (s_page >= pages) s_page = pages - 1;
    if (s_page < 0) s_page = 0;

    const lv_font_t *f_ui = ui_font();
    const lv_font_t *f_title = title_font();
    const lv_color_t grey = lv_palette_darken(LV_PALETTE_GREY, 2);

    for (int r = 0; r < s_row_on_page; ++r) {
        const int y = r * ROW_H;
        if (s_rows[r].kind == 0) {
            char t[4];
            snprintf(t, sizeof(t), "%c", s_rows[r].initial);
            mk_label(s_picker_list, t, f_title, 20, y + 2, lv_color_black());
            mk_rect(s_picker_list, 50, y + 16, 398, 1, 0, 1);
        } else {
            City c;
            if (!city_get(s_rows[r].idx, &c)) continue;
            lv_obj_t *l = mk_label(s_picker_list, c.name, f_ui, 24, y + 4, lv_color_black());
            lv_obj_add_flag(l, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(l, on_city_row_clicked, LV_EVENT_CLICKED,
                                (void *)(intptr_t)s_rows[r].idx);
        }
    }
    (void)grey;

    // 状态行：当前过滤 + 命中数量 + 页码
    if (s_picker_status != NULL) {
        char st[80];
        if (s_filter != 0) {
            snprintf(st, sizeof(st), "%c 组 · %d 个城市 · %d/%d 页", s_filter, s_match_cities,
                     s_page + 1, pages);
        } else {
            snprintf(st, sizeof(st), "全部 %d 个城市 · %d/%d 页", s_match_cities, s_page + 1, pages);
        }
        utf8_clip(st, sizeof(st) - 1);
        lv_label_set_text(s_picker_status, st);
    }
    if (s_btn_prev != NULL) {
        lv_obj_set_style_opa(s_btn_prev, s_page > 0 ? LV_OPA_COVER : LV_OPA_30, 0);
    }
    if (s_btn_next != NULL) {
        lv_obj_set_style_opa(s_btn_next, s_page + 1 < pages ? LV_OPA_COVER : LV_OPA_30, 0);
    }
    picker_update_letters();
}

static void on_letter_clicked(lv_event_t *e) {
    (void)e;
    // 字母条是「一块透明点击层 + 26 个文字标签」，这里按触点坐标反推字母
    lv_indev_t *indev = lv_indev_active();
    lv_point_t pt;
    if (indev == NULL) return;
    lv_indev_get_point(indev, &pt);
    const int rel_y = pt.y - 540;
    const int row = (rel_y < 36) ? 0 : 1;
    const int col = (pt.x - 16) / 34;
    if (col < 0 || col >= LETTER_COLS) return;
    const int li = row * LETTER_COLS + col;
    if (li < 0 || li > 25) return;
    const char letter = (char)('A' + li);
    s_filter = (s_filter == letter) ? 0 : letter; // 再点一次取消过滤
    s_page = 0;
    picker_build_page();
}

static void on_show_all_clicked(lv_event_t *e) {
    (void)e;
    s_filter = 0;
    s_page = 0;
    picker_build_page();
}

static void on_prev_page(lv_event_t *e) {
    (void)e;
    if (s_page <= 0) return;
    s_page--;
    picker_build_page();
}

static void on_next_page(lv_event_t *e) {
    (void)e;
    const int pages = (s_total_rows + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE;
    if (s_page + 1 >= pages) return;
    s_page++;
    picker_build_page();
}

static void on_picker_reload(lv_event_t *e) {
    (void)e;
    load_custom_cities();
    s_page = 0;
    picker_build_page();
    dynapp_toast_show("城市列表已重载", 1200);
}

static void on_picker_cancel(lv_event_t *e) {
    (void)e;
    if (s_picker != NULL) lv_obj_add_flag(s_picker, LV_OBJ_FLAG_HIDDEN);
}

static void on_city_btn(lv_event_t *e) {
    (void)e;
    if (s_picker == NULL) {
        // 面板延后建：主界面 + 联网取数时先别占那几十个 LVGL 对象的内存
        // （https 握手要 14KB 连续内部 RAM，省下来的都是余量）
        build_picker();
    }
    if (s_picker == NULL) return;
    s_page = 0;
    picker_build_page();
    lv_obj_clear_flag(s_picker, LV_OBJ_FLAG_HIDDEN);
}

static void switch_city(int delta) {
    const int total = city_total();
    if (total <= 0) return;
    s_city_idx = (s_city_idx + delta + total) % total;
    s_have_data = 0; // 换城市先清空，避免显示上一个城市的读数
    memset(&s_w, 0, sizeof(s_w));
    render_main();
    save_state();
    fetch_weather();
}

static void on_refresh(lv_event_t *e) {
    (void)e;
    fetch_weather();
}

static void on_tts_done(int ok, const char *err, void *user_data) {
    (void)user_data;
    if (ok) {
        dynapp_toast_show("播报完成", 1200);
    } else {
        char b[96];
        snprintf(b, sizeof(b), "朗读失败：%s", (err && err[0]) ? err : "未知");
        utf8_clip(b, sizeof(b) - 1);
        dynapp_toast_show(b, 1800);
    }
}

static void on_speak(lv_event_t *e) {
    (void)e;
    if (dynapp_tts_available() == 0) {
        dynapp_toast_show("没配置 MiniMax API Key", 1600);
        return;
    }
    if (!s_have_data) {
        dynapp_toast_show("还没有天气数据", 1400);
        return;
    }
    City c;
    const char *name = city_get(s_city_idx, &c) ? c.name : "";
    char text[320];
    snprintf(text, sizeof(text),
             "%s，%s。当前温度 %d 度，体感 %d 度，湿度 %d%%，风速 %d 公里每小时。"
             "今天最高 %d 度，最低 %d 度，降水概率 %d%%。",
             name, wmo_text(s_w.code), s_w.temp, s_w.feels, s_w.hum, s_w.wind, s_w.tmax[0], s_w.tmin[0],
             s_w.pop[0]);
    utf8_clip(text, sizeof(text) - 1);
    if (dynapp_tts_speak_ex(text, on_tts_done, NULL)) {
        dynapp_toast_show("正在合成语音…", 1400);
    } else {
        dynapp_toast_show("排队失败", 1200);
    }
}

static void build_picker(void) {
    s_picker = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_picker);
    lv_obj_set_pos(s_picker, 0, 0);
    lv_obj_set_size(s_picker, 480, 800);
    lv_obj_set_style_bg_color(s_picker, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_picker, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_picker, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_picker, 2, 0);
    lv_obj_clear_flag(s_picker, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_picker, LV_OBJ_FLAG_HIDDEN);

    mk_label(s_picker, "选择城市", title_font(), 16, 40, lv_color_black());
    mk_button(s_picker, "全部", 392, 36, 72, 42, on_show_all_clicked, NULL);
    s_picker_status = mk_label(s_picker, "", ui_font(), 16, 92,
                               lv_palette_darken(LV_PALETTE_GREY, 2));

    s_picker_list = lv_obj_create(s_picker);
    lv_obj_remove_style_all(s_picker_list);
    lv_obj_set_pos(s_picker_list, 16, 126);
    lv_obj_set_size(s_picker_list, 448, ROWS_PER_PAGE * ROW_H);
    lv_obj_clear_flag(s_picker_list, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    mk_rect(s_picker, 16, 528, 448, 2, 0, 1);

    // 拼音首字母过滤条：26 个字母标签（定距排布）+ 一根高亮条 + 一个透明点击层
    s_letter_hl = mk_rect(s_picker, 0, 0, 34, 36, 4, 1);
    lv_obj_add_flag(s_letter_hl, LV_OBJ_FLAG_HIDDEN);
    for (int i = 0; i < 26; ++i) {
        const int row = i / LETTER_COLS;
        const int col = i % LETTER_COLS;
        const int x = 16 + col * 34;
        const int y = 540 + row * 36;
        char t[2] = {(char)('A' + i), '\0'};
        lv_obj_t *l = mk_label(s_picker, t, latin_font(), x, y + 8, lv_color_black());
        lv_obj_set_width(l, 34);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        s_letter_lbl[i] = l;
    }
    {
        lv_obj_t *hit = lv_obj_create(s_picker);
        lv_obj_remove_style_all(hit);
        lv_obj_set_pos(hit, 10, 536);
        lv_obj_set_size(hit, 460, 76);
        lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(hit, on_letter_clicked, LV_EVENT_CLICKED, NULL);
    }

    s_btn_prev = mk_button(s_picker, "上页", 16, 622, 104, 52, on_prev_page, NULL);
    s_btn_next = mk_button(s_picker, "下页", 128, 622, 104, 52, on_next_page, NULL);
    mk_button(s_picker, "重载", 240, 622, 104, 52, on_picker_reload, NULL);
    mk_button(s_picker, "取消", 352, 622, 112, 52, on_picker_cancel, NULL);
}

// ================= 生命周期 =================
// 30 分钟一次的自动刷新：值没变就不重画界面。
static void on_auto_tick(lv_timer_t *tmr) {
    (void)tmr;
    fetch_weather();
}

static void on_screen_deleted(lv_event_t *e) {
    (void)e;
    if (s_auto_timer != NULL) {
        lv_timer_delete(s_auto_timer);
        s_auto_timer = NULL;
    }
    if (s_retry_timer != NULL) {
        lv_timer_delete(s_retry_timer);
        s_retry_timer = NULL;
    }
    if (s_first_timer != NULL) {
        lv_timer_delete(s_first_timer);
        s_first_timer = NULL;
    }
    save_state();
    s_scr = NULL;
    s_city_lbl = NULL;
    s_time_lbl = NULL;
    s_icon_box = NULL;
    s_desc_lbl = NULL;
    s_feels_lbl = NULL;
    s_hum_lbl = NULL;
    s_wind_lbl = NULL;
    s_pop_lbl = NULL;
    s_status_lbl = NULL;
    s_src_lbl = NULL;
    s_hint_lbl = NULL;
    s_picker = NULL;
    s_picker_list = NULL;
    s_picker_status = NULL;
    s_btn_prev = NULL;
    s_btn_next = NULL;
    for (int i = 0; i < 26; ++i) s_letter_lbl[i] = NULL;
    s_letter_hl = NULL;
    for (int i = 0; i < NDAYS; ++i) {
        s_day_lbl[i] = NULL;
        s_fc_lbl[i] = NULL;
        s_fct_lbl[i] = NULL;
        s_fcp_lbl[i] = NULL;
    }
    memset(&s_seg, 0, sizeof(s_seg));
}

static int weather_on_vk_key(const char *key) {
    if (key == NULL) return 0;
    if (strcmp(key, "vk_home") == 0 || strcmp(key, "esc") == 0 || strcmp(key, "back") == 0) {
        dynapp_screen_go_home();
        return 1;
    }
    if (strcmp(key, "vk_prev") == 0) {
        switch_city(-1);
        return 1;
    }
    if (strcmp(key, "vk_next") == 0) {
        switch_city(1);
        return 1;
    }
    return 0;
}

static void *weather_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    s_scr = scr;
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 800);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_screen_deleted, LV_EVENT_DELETE, NULL);

    dynapp_screen_status_bar(scr);
    dynapp_vk_attach(scr, "weather", weather_create, weather_on_vk_key);

    const lv_font_t *f_ui = ui_font();
    const lv_font_t *f_title = title_font();
    const lv_font_t *f_latin = latin_font();
    const lv_color_t grey = lv_palette_darken(LV_PALETTE_GREY, 2);

    // 1. 城市名 + 观测时刻
    s_city_lbl = mk_label(scr, "—", f_title, 16, 44, lv_color_black());
    s_time_lbl = mk_label(scr, "", f_ui, 380, 52, grey);

    // 2. 七段数码管大温度（3 位）
    {
        const int dw = 90, dh = 140, stroke = 15, x0 = 16, y0 = 96;
        sevenseg_init(&s_seg, scr, 3, x0, y0, dw, dh, stroke);
        sevenseg_set_minus_pos(&s_seg, x0, y0 + dh / 2 - stroke / 2);
    }
    mk_label(scr, "℃", f_title, 322, 174, lv_color_black());

    // 3. 天气图标
    s_icon_box = lv_obj_create(scr);
    lv_obj_remove_style_all(s_icon_box);
    lv_obj_set_pos(s_icon_box, 392, 92);
    lv_obj_set_size(s_icon_box, 64, 64);
    lv_obj_clear_flag(s_icon_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    // 4. 天气描述 + 四项指标
    s_desc_lbl = mk_label(scr, "—", f_title, 16, 252, lv_color_black());
    s_feels_lbl = mk_label(scr, "", f_ui, 16, 302, lv_color_black());
    s_hum_lbl = mk_label(scr, "", f_ui, 250, 302, lv_color_black());
    s_wind_lbl = mk_label(scr, "", f_ui, 16, 340, lv_color_black());
    s_pop_lbl = mk_label(scr, "", f_ui, 250, 340, lv_color_black());

    // 5. 分隔线
    mk_rect(scr, 16, 386, 448, 2, 0, 1);

    // 6. 三天预报
    for (int i = 0; i < NDAYS; ++i) {
        const int x = 16 + i * 149;
        s_day_lbl[i] = mk_label(scr, "", f_ui, x, 404, lv_color_black());
        s_fc_lbl[i] = mk_label(scr, "", f_ui, x, 440, lv_color_black());
        s_fct_lbl[i] = mk_label(scr, "", f_ui, x, 476, lv_color_black());
        s_fcp_lbl[i] = mk_label(scr, "", f_ui, x, 512, grey);
        if (i > 0) mk_rect(scr, x - 10, 404, 1, 120, 0, 1);
    }

    // 7. 分隔线 + 状态行
    mk_rect(scr, 16, 556, 448, 2, 0, 1);
    s_status_lbl = mk_label(scr, "", f_ui, 16, 574, lv_color_black());
    s_src_lbl = mk_label(scr, "Open-Meteo", f_latin, 16, 610, grey);
    s_hint_lbl = mk_label(scr, "", f_ui, 16, 644, grey);

    // 8. 按钮行
    mk_button(scr, "城市", 16, 700, 144, 60, on_city_btn, NULL);
    mk_button(scr, "刷新", 168, 700, 144, 60, on_refresh, NULL);
    mk_button(scr, "朗读", 320, 700, 144, 60, on_speak, NULL);

    // 9. 城市选择面板：第一次点「城市」时才建（见 on_city_btn）

    // 10. 数据
    db_count();
    load_custom_cities();
    load_state();
    render_main();

    // 11. 自动刷新 + 首次联网（延后 8 秒，避开刚开机那会儿的网络碎片化）
    s_auto_timer = lv_timer_create(on_auto_tick, REFRESH_MS, NULL);
    s_first_timer = lv_timer_create(on_first_fetch, 8000, NULL);
    return scr;
}

void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "weather";
    out->name = "天气";
    out->icon = "icon.png";
    out->create = weather_create;
}
