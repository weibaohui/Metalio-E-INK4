// tools/poetry_app/main/poems_main.c
// 唐诗宋词 动态应用：每日一诗 / 随机一首 / 分类浏览(唐诗三百首·宋词三百首·
// 千家诗·诗经) / 课本讲解 / 词牌故事 / 收藏。
// 数据来自 Mac 端 tools/poetry_pack/pack_poems.py 预编译的 poems.db
// （诗文与赏析按条 deflate，经固件 dynapp_zlib_inflate 解压，单条驻留）。
//
// 诗词正文统一用 fontpack 30@2 渲染（补字只烧在 30@2 档），
// 切勿对语料文本使用其他字号——生僻字字形只在 30@2 里有。
//
// 注意：不能有任何"非零初始化的静态变量"（.data 节会触发 elf_loader
// GOT 重定位野指针写路径，加载必崩）。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

#define APP_TAG "PoemApp"
#define PAD 10
#define BTN_H 46
#define BODY_FONT 30     // 语料文本专用字号(补字档)
#define UI_FONT 18       // 界面控件文字(常用字, 25@2 回退)

// ---- poems.db 结构（pack_poems.py 输出，little-endian）----
#define DB_HDR_SIZE 40
#define COLL_REC 32
#define POEM_REC 16
#define APPR_REC 16
#define POEM_DB_MAGIC "PMDB"
#define NO_APPR 0xFF

typedef struct {
    uint32_t coll_count, poem_count, appr_count;
    uint64_t blob_off;
} DbHeader;

typedef struct {
    char title[96];
    char author[96];
    char meta[48];     // 朝代 · 体裁
    char *body;        // malloc, '\n' 分句
    uint32_t body_len;
    uint8_t appr;      // 赏析 id 或 NO_APPR
    uint16_t coll;
} Poem;

typedef struct {
    char title[128];
    char *text;
    uint32_t text_len;
    uint16_t poem;     // 挂接诗号 / 0xFFFF
    uint8_t cat;       // 0 课内讲解 1 独立讲解 2 词牌故事
} Appr;

static FILE *s_db = NULL;
static DbHeader s_hdr;
static int s_db_ok = 0;

// ---- 视图状态（全部零初始化）----
#define VIEW_HOME 0
#define VIEW_BROWSE 1    // 分类列表
#define VIEW_LIST 2      // 诗列表 / 赏析列表
#define VIEW_POEM 3
#define VIEW_APPR 4
static int s_view = VIEW_HOME;
static int s_list_kind = 0;      // 0=诗集 1=收藏 2=课本讲解 3=词牌故事
static uint16_t s_list_coll = 0;
static int s_cur = 0;            // 当前诗号
static int s_appr_cur = -1;      // 当前赏析号(直接从赏析列表进入时)
static int s_page = 0, s_pages = 0;
static uint32_t s_seed = 0;      // 随机序列
static uint32_t *s_fav = NULL;
static int s_fav_n = 0, s_fav_cap = 0;

// ---- UI ----
static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_body_box = NULL;
static lv_obj_t *s_clip = NULL;
static lv_obj_t *s_content = NULL;
static lv_obj_t *s_pager_lbl = NULL;
static lv_obj_t *s_fav_btn = NULL;

// ---- 前向声明 ----
static void load_poem_and_show(int idx, int page);
static void enter_poem_view(void);
static void build_home_view(void);
static void build_browse_view(void);
static void build_list_view(void);
static void enter_appr_view(int appr_id);
static void show_page(int page);
static void page_next(void);
static void page_prev(void);
static void on_daily_cb(lv_event_t *e);
static void on_random_cb(lv_event_t *e);
static void on_fav_cb(lv_event_t *e);
static void on_browse_cb(lv_event_t *e);
static void on_appr_cb(lv_event_t *e);
static void on_back_home_cb(lv_event_t *e);
static void on_back_browse_cb(lv_event_t *e);
static void on_coll_clicked(lv_event_t *e);
static void on_poem_clicked(lv_event_t *e);
static void on_appr_clicked(lv_event_t *e);
static void on_favlist_cb(lv_event_t *e);
static void on_apprlist_cb(lv_event_t *e);
static void state_save(void);

static const lv_font_t *get_font(int size, int weight) {
    const lv_font_t *f = fontpack_lv_font_get(size, weight);
    if (f == NULL) f = fontpack_lv_font_ui();
    return f;
}

// ================= 收藏/状态 =================
static int is_fav(uint32_t idx) {
    for (int i = 0; i < s_fav_n; ++i) if (s_fav[i] == idx) return 1;
    return 0;
}

static void fav_toggle(uint32_t idx) {
    int at = -1;
    for (int i = 0; i < s_fav_n; ++i) if (s_fav[i] == idx) { at = i; break; }
    if (at >= 0) {
        memmove(s_fav + at, s_fav + at + 1, (size_t)(s_fav_n - at - 1) * sizeof(uint32_t));
        s_fav_n--;
    } else {
        if (s_fav_n == s_fav_cap) {
            s_fav_cap = s_fav_cap ? s_fav_cap * 2 : 16;
            uint32_t *nf = realloc(s_fav, (size_t)s_fav_cap * sizeof(uint32_t));
            if (nf == NULL) return;
            s_fav = nf;
        }
        s_fav[s_fav_n++] = idx;
    }
    state_save();
}

static void state_save(void) {
    FILE *f = dynapp_fopen_data("state.txt", "wb");
    if (f == NULL) return;
    fprintf(f, "last=%d\n", s_cur);
    fprintf(f, "fav=");
    for (int i = 0; i < s_fav_n; ++i) fprintf(f, "%s%u", i ? "," : "", s_fav[i]);
    fprintf(f, "\n");
    fclose(f);
}

static void state_load(void) {
    FILE *f = dynapp_fopen_data("state.txt", "rb");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "last=", 5) == 0) {
            const int v = atoi(line + 5);
            if (v >= 0) s_cur = v;
        } else if (strncmp(line, "fav=", 4) == 0) {
            char *p = line + 4;
            char *end = NULL;
            for (long v = strtol(p, &end, 10); end != p; v = strtol(p, &end, 10)) {
                if (v >= 0) fav_toggle((uint32_t)v);
                p = (*end == ',') ? end + 1 : end;
            }
        }
    }
    fclose(f);
}

// ================= DB 读取 =================
static void db_read(uint32_t off, void *buf, size_t len) {
    if (fseek(s_db, (long)off, SEEK_SET) != 0 ||
        fread(buf, 1, len, s_db) != len) memset(buf, 0, len);
}

static void db_read_str(uint32_t off, char *buf, size_t cap) {
    if (cap == 0) return;
    buf[0] = '\0';
    db_read(off, buf, cap - 1);
    buf[cap - 1] = '\0';
}

static int db_open(void) {
    s_db = dynapp_fopen_data("poems.db", "rb");
    if (s_db == NULL) { printf("[poems] db open FAIL\n"); return 0; }
    uint8_t hdr[DB_HDR_SIZE];
    db_read(0, hdr, DB_HDR_SIZE);
    if (memcmp(hdr, POEM_DB_MAGIC, 4) != 0) return 0;
    memcpy(&s_hdr.coll_count, hdr + 8, 4);
    memcpy(&s_hdr.poem_count, hdr + 12, 4);
    memcpy(&s_hdr.appr_count, hdr + 16, 4);
    memcpy(&s_hdr.blob_off, hdr + 20, 8);
    s_db_ok = (s_hdr.poem_count > 0);
    return s_db_ok;
}

static int coll_poem_start(uint16_t coll, uint16_t *start, uint16_t *count) {
    if (!s_db_ok || coll >= s_hdr.coll_count) return 0;
    uint8_t rec[COLL_REC];
    db_read(DB_HDR_SIZE + COLL_REC * coll, rec, COLL_REC);
    memcpy(start, rec + 28, 2);
    memcpy(count, rec + 30, 2);
    return 1;
}

static void poem_free(Poem *p) {
    lv_free(p->body);
    p->body = NULL;
    p->body_len = 0;
}

static void poem_load(int idx, Poem *p) {
    memset(p, 0, sizeof(*p));
    p->appr = NO_APPR;
    if (!s_db_ok || idx < 0 || (uint32_t)idx >= s_hdr.poem_count) return;
    uint8_t rec[POEM_REC];
    db_read(DB_HDR_SIZE + COLL_REC * s_hdr.coll_count + POEM_REC * idx, rec, POEM_REC);
    uint32_t off = 0, zlen = 0, rawlen = 0;
    memcpy(&off, rec, 4);
    memcpy(&zlen, rec + 4, 2);
    memcpy(&rawlen, rec + 6, 2);
    memcpy(&p->coll, rec + 8, 1);
    memcpy(&p->appr, rec + 9, 1);

    char *comp = (char *)lv_malloc((size_t)zlen + 1);
    if (comp == NULL) { printf("[poems] comp malloc fail zlen=%u\n", (unsigned)zlen); return; }
    db_read((uint32_t)s_hdr.blob_off + off, comp, zlen);
    char *plain = (char *)lv_malloc((size_t)rawlen + 1);
    if (plain == NULL) { lv_free(comp); printf("[poems] plain malloc fail raw=%u\n", (unsigned)rawlen); return; }
    const int n = dynapp_zlib_inflate(comp, (int)zlen, plain, rawlen);
    lv_free(comp);
    if (n < 0) { lv_free(plain); return; }
    plain[n] = '\0';

    // title\0author\0dynasty\0kind\0body
    char *fields[4];
    int nf = 0;
    fields[0] = plain;
    for (char *q = plain; nf < 4; ++q) {
        if (*q == '\0') {
            ++nf;
            if (nf < 4) fields[nf] = q + 1;
            else break;
        }
    }
    snprintf(p->title, sizeof(p->title), "%s", fields[0]);
    snprintf(p->author, sizeof(p->author), "%s", nf >= 1 ? fields[1] : "");
    const char *dyn = nf >= 2 ? fields[2] : "";
    const char *kind = nf >= 3 ? fields[3] : "";
    snprintf(p->meta, sizeof(p->meta), "%s%s%s", dyn,
             (dyn[0] && kind[0]) ? " · " : "", kind);
    const char *body = plain + strlen(fields[0]) + 1 + strlen(fields[1]) + 1 +
                       strlen(fields[2]) + 1 + strlen(fields[3]) + 1;
    p->body_len = rawlen - (uint32_t)(body - plain);
    if (p->body_len > 0 && p->body_len < rawlen + 4) {
        p->body = (char *)lv_malloc((size_t)p->body_len + 1);
        if (p->body != NULL) {
            memcpy(p->body, body, p->body_len);
            p->body[p->body_len] = '\0';
        }
    }
    lv_free(plain);
}

static void appr_load(int idx, Appr *a) {
    memset(a, 0, sizeof(*a));
    a->poem = 0xFFFF;
    if (!s_db_ok || idx < 0 || (uint32_t)idx >= s_hdr.appr_count) return;
    uint8_t rec[APPR_REC];
    db_read(DB_HDR_SIZE + COLL_REC * s_hdr.coll_count + POEM_REC * s_hdr.poem_count +
            APPR_REC * idx, rec, APPR_REC);
    uint32_t off = 0, zlen = 0, rawlen = 0;
    memcpy(&off, rec, 4);
    memcpy(&zlen, rec + 4, 2);
    memcpy(&rawlen, rec + 6, 2);
    memcpy(&a->poem, rec + 8, 2);
    memcpy(&a->cat, rec + 10, 1);

    char *comp = (char *)lv_malloc((size_t)zlen + 1);
    if (comp == NULL) { printf("[poems] comp malloc fail zlen=%u\n", (unsigned)zlen); return; }
    db_read((uint32_t)s_hdr.blob_off + off, comp, zlen);
    char *plain = (char *)lv_malloc((size_t)rawlen + 1);
    if (plain == NULL) { lv_free(comp); printf("[poems] plain malloc fail raw=%u\n", (unsigned)rawlen); return; }
    const int n = dynapp_zlib_inflate(comp, (int)zlen, plain, rawlen);
    lv_free(comp);
    if (n < 0) { lv_free(plain); return; }
    plain[n] = '\0';

    char *sep = plain;                 // title\0text
    while (*sep != '\0' && sep < plain + n) ++sep;
    snprintf(a->title, sizeof(a->title), "%s", plain);
    const uint32_t tlen = (uint32_t)(sep - plain) + 1;
    a->text_len = (rawlen > tlen) ? (rawlen - tlen) : 0;
    if (a->text_len > 0) {
        a->text = (char *)lv_malloc((size_t)a->text_len + 1);
        if (a->text != NULL) {
            memcpy(a->text, sep + 1, a->text_len);
            a->text[a->text_len] = '\0';
        }
    }
    lv_free(plain);
}

static void appr_free(Appr *a) {
    lv_free(a->text);
    a->text = NULL;
    a->text_len = 0;
}

static uint32_t next_rand(uint32_t mod) {
    if (s_seed == 0) s_seed = (uint32_t)time(NULL) | 1u;
    s_seed = s_seed * 1664525u + 1013904223u;
    return mod ? s_seed % mod : 0;
}

// ================= UI 工具 =================
static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, lv_coord_t w, lv_coord_t h,
                          lv_event_cb_t cb, void *user_data) {
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
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, get_font(UI_FONT, 0), 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, int size, int weight) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, get_font(size, weight), 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_width(lbl, lv_pct(100));
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return lbl;
}

// 语料文本标签：统一 30@2，居中
static lv_obj_t *make_poem_label(lv_obj_t *parent, const char *text, int center) {
    lv_obj_t *lbl = make_label(parent, text, BODY_FONT, 2);
    if (center) lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    return lbl;
}

// ================= 诗视图 =================
static Poem s_poem;

static void render_poem_body(Poem *p) {
    lv_obj_clean(s_content);
    if (p->body == NULL || p->body_len == 0) {
        make_poem_label(s_content, "（无内容）", 1);
        return;
    }
    const char *q = p->body;
    while (*q) {
        char line[512];
        size_t t = 0;
        while (*q && *q != '\n' && t < sizeof(line) - 3) line[t++] = *q++;
        while (t > 0 && (line[t - 1] == '\r' || line[t - 1] == ' ')) --t;
        line[t] = '\0';
        if (*q == '\n') ++q;
        if (t == 0) { make_poem_label(s_content, " ", 1); continue; }
        make_poem_label(s_content, line, 1);
    }
}

static void show_page(int page) {
    if (page < 0) page = 0;
    if (page > s_pages - 1) page = s_pages - 1;
    s_page = page;
    const lv_coord_t vh = lv_obj_get_height(s_clip);
    if (vh > 0) lv_obj_scroll_to_y(s_clip, s_page * vh, LV_ANIM_OFF);
    if (s_pager_lbl != NULL && s_pages > 1) {
        char buf[32];
        snprintf(buf, sizeof(buf), "第 %d / %d 页", s_page + 1, s_pages);
        lv_label_set_text(s_pager_lbl, buf);
    } else if (s_pager_lbl != NULL) {
        lv_label_set_text(s_pager_lbl, " ");
    }
}

static void refresh_fav_btn(void) {
    if (s_fav_btn == NULL) return;
    const int fav = is_fav((uint32_t)s_cur);
    lv_obj_t *lbl = lv_obj_get_child(s_fav_btn, 0);
    if (lbl != NULL) {
        lv_label_set_text(lbl, fav ? "已藏" : "收藏");
        if (fav) {
            lv_obj_set_style_bg_color(s_fav_btn, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(s_fav_btn, LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        } else {
            lv_obj_set_style_bg_opa(s_fav_btn, LV_OPA_TRANSP, 0);
            lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        }
    }
}

static void build_poem_view(void) {
    lv_obj_clean(s_body_box);
    lv_obj_set_style_pad_top(s_body_box, PAD, 0);
    poem_load(s_cur, &s_poem);

    lv_obj_t *box = lv_obj_create(s_body_box);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 4, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *ttl = make_poem_label(box, s_poem.title, 1);
    lv_obj_set_style_pad_hor(ttl, 4, 0);
    char who[192];
    snprintf(who, sizeof(who), "%s%s%s", s_poem.author,
             s_poem.meta[0] ? "  " : "", s_poem.meta);
    make_poem_label(box, who, 1);

    // 分隔线
    lv_obj_t *hr = lv_obj_create(s_body_box);
    lv_obj_remove_style_all(hr);
    lv_obj_set_size(hr, lv_pct(100), 2);
    lv_obj_set_style_bg_color(hr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(hr, LV_OPA_40, 0);

    s_clip = lv_obj_create(s_body_box);
    lv_obj_remove_style_all(s_clip);
    lv_obj_set_size(s_clip, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(s_clip, 1);
    lv_obj_set_style_pad_all(s_clip, 6, 0);
    lv_obj_add_flag(s_clip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_clip, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_clip, LV_SCROLLBAR_MODE_OFF);

    s_content = lv_obj_create(s_clip);
    lv_obj_remove_style_all(s_content);
    lv_obj_set_width(s_content, lv_pct(100));
    lv_obj_set_height(s_content, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_content, 10, 0);
    lv_obj_clear_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
    render_poem_body(&s_poem);

    // 页指示 + 提示
    lv_obj_t *pager = lv_obj_create(s_body_box);
    lv_obj_remove_style_all(pager);
    lv_obj_set_size(pager, lv_pct(100), 26);
    char hint[64];
    snprintf(hint, sizeof(hint), "%s", s_poem.appr != NO_APPR ? "有赏析" : "");
    s_pager_lbl = make_label(pager, hint, UI_FONT, 0);
    lv_obj_center(s_pager_lbl);

    // 底部按钮排
    lv_obj_t *row = lv_obj_create(s_body_box);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), BTN_H);
    const lv_coord_t bw = (lv_coord_t)((LV_HOR_RES - 2 * PAD - 3 * 6) / 4);
    s_fav_btn = make_btn(row, "收藏", bw, BTN_H, on_fav_cb, NULL);
    make_btn(row, "赏析", bw, BTN_H, on_appr_cb, NULL);
    make_btn(row, "随机", bw, BTN_H, on_random_cb, NULL);
    make_btn(row, "书目", bw, BTN_H, on_browse_cb, NULL);
    const int n = (int)lv_obj_get_child_count(row);
    for (int i = 0; i < n && i < 4; ++i) {
        lv_obj_align(lv_obj_get_child(row, i), LV_ALIGN_LEFT_MID,
                     (lv_coord_t)(i * (bw + 6)), 0);
    }

    refresh_fav_btn();
    s_view = VIEW_POEM;
}

static void enter_poem_view(void) {
    lv_obj_clean(s_body_box);
    build_poem_view();
    lv_obj_update_layout(s_body_box);
    const lv_coord_t ch = lv_obj_get_height(s_content);
    const lv_coord_t vh = lv_obj_get_height(s_clip);
    s_pages = (vh > 0) ? (int)((ch + vh - 1) / vh) : 1;
    if (s_pages < 1) s_pages = 1;
    if (s_page > s_pages - 1) s_page = s_pages - 1;
    show_page(s_page);
}

static void load_poem_and_show(int idx, int page) {
    if (!s_db_ok || idx < 0 || (uint32_t)idx >= s_hdr.poem_count) return;
    s_cur = idx;
    s_appr_cur = -1;
    s_page = page;
    enter_poem_view();
    state_save();
}

static void next_poem(void) {
    if (!s_db_ok) return;
    load_poem_and_show((s_cur + 1) % (int)s_hdr.poem_count, 0);
}
static void prev_poem(void) {
    if (!s_db_ok) return;
    load_poem_and_show((s_cur - 1 + (int)s_hdr.poem_count) % (int)s_hdr.poem_count, 0);
}
static void page_next(void) {
    if (s_page < s_pages - 1) show_page(s_page + 1);
    else if (s_view == VIEW_POEM) next_poem();
}
static void page_prev(void) {
    if (s_page > 0) show_page(s_page - 1);
    else if (s_view == VIEW_POEM) prev_poem();
}

// ================= 赏析视图 =================
static Appr s_appr;

static void build_appr_view(int appr_id) {
    lv_obj_clean(s_body_box);
    lv_obj_set_style_pad_top(s_body_box, PAD, 0);
    appr_load(appr_id, &s_appr);

    lv_obj_t *box = lv_obj_create(s_body_box);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 4, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *ttl = make_poem_label(box, s_appr.title, 1);

    lv_obj_t *hr = lv_obj_create(s_body_box);
    lv_obj_remove_style_all(hr);
    lv_obj_set_size(hr, lv_pct(100), 2);
    lv_obj_set_style_bg_color(hr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(hr, LV_OPA_40, 0);

    s_clip = lv_obj_create(s_body_box);
    lv_obj_remove_style_all(s_clip);
    lv_obj_set_size(s_clip, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(s_clip, 1);
    lv_obj_set_style_pad_all(s_clip, 6, 0);
    lv_obj_add_flag(s_clip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_clip, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_clip, LV_SCROLLBAR_MODE_OFF);

    s_content = lv_obj_create(s_clip);
    lv_obj_remove_style_all(s_content);
    lv_obj_set_width(s_content, lv_pct(100));
    lv_obj_set_height(s_content, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_content, 8, 0);
    lv_obj_clear_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);

    if (s_appr.text == NULL || s_appr.text_len == 0) {
        make_poem_label(s_content, "（无内容）", 0);
    } else {
        // 按 \n 分段渲染; 单段过长则按句拆行
        const char *q = s_appr.text;
        while (*q) {
            char para[2048];
            size_t t = 0;
            while (*q && *q != '\n' && t < sizeof(para) - 3) para[t++] = *q++;
            para[t] = '\0';
            if (*q == '\n') ++q;
            if (t == 0) continue;
            make_poem_label(s_content, para, 0);
        }
    }

    lv_obj_t *pager = lv_obj_create(s_body_box);
    lv_obj_remove_style_all(pager);
    lv_obj_set_size(pager, lv_pct(100), 26);
    s_pager_lbl = make_label(pager, "", UI_FONT, 0);
    lv_obj_center(s_pager_lbl);

    lv_obj_t *row = lv_obj_create(s_body_box);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), BTN_H);
    const lv_coord_t bw = (lv_coord_t)((LV_HOR_RES - 2 * PAD - 2 * 6) / 3);
    if (s_appr.poem != 0xFFFF) {
        make_btn(row, "◀ 原诗", bw, BTN_H, on_poem_clicked,
                 (void *)(uintptr_t)s_appr.poem);
    } else {
        make_btn(row, "◀ 返回", bw, BTN_H, on_back_browse_cb, NULL);
    }
    make_btn(row, "随机", bw, BTN_H, on_random_cb, NULL);
    s_view = VIEW_APPR;
}

static void enter_appr_view(int appr_id) {
    s_appr_cur = appr_id;
    lv_obj_clean(s_body_box);
    build_appr_view(appr_id);
    lv_obj_update_layout(s_body_box);
    const lv_coord_t ch = lv_obj_get_height(s_content);
    const lv_coord_t vh = lv_obj_get_height(s_clip);
    s_pages = (vh > 0) ? (int)((ch + vh - 1) / vh) : 1;
    if (s_pages < 1) s_pages = 1;
    s_page = 0;
    show_page(0);
}

// ================= 首页 / 浏览 / 列表 =================
static void build_home_view(void) {
    lv_obj_clean(s_body_box);
    s_view = VIEW_HOME;
    lv_obj_set_style_pad_top(s_body_box, 90, 0);

    lv_obj_t *ttl = make_label(s_body_box, "唐诗宋词", 40, 2);
    lv_obj_set_style_text_align(ttl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_t *sub = make_label(s_body_box, "每日一诗 · 侧键翻页", UI_FONT, 0);
    lv_obj_set_style_text_align(sub, LV_TEXT_ALIGN_CENTER, 0);

    struct { const char *txt; lv_event_cb_t cb; void *ud; } items[4] = {
        {"每日一诗", on_daily_cb, NULL},
        {"随机一首", on_random_cb, NULL},
        {"书目浏览", on_browse_cb, NULL},
        {"我的收藏", on_favlist_cb, NULL},
    };
    const lv_coord_t bw = (lv_coord_t)((LV_HOR_RES - 2 * PAD - 18) / 2);
    for (int r = 0; r < 2; ++r) {
        lv_obj_t *row = lv_obj_create(s_body_box);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 64);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, 18, 0);
        for (int c = 0; c < 2; ++c) {
            make_btn(row, items[r * 2 + c].txt, bw, 64,
                     items[r * 2 + c].cb, items[r * 2 + c].ud);
        }
    }
}

static void build_browse_view(void) {
    lv_obj_clean(s_body_box);
    lv_obj_set_style_pad_top(s_body_box, PAD, 0);
    s_view = VIEW_BROWSE;
    lv_obj_t *top = lv_obj_create(s_body_box);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, lv_pct(100), 44);
    make_label(top, "书目浏览", 20, 2);
    make_btn(top, "首页", 90, BTN_H, on_back_home_cb, NULL);
    lv_obj_align(lv_obj_get_child(top, 1), LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *list = lv_obj_create(s_body_box);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    for (uint16_t c = 0; c < s_hdr.coll_count; ++c) {
        uint8_t rec[COLL_REC];
        db_read(DB_HDR_SIZE + COLL_REC * c, rec, COLL_REC);
        char label[96];
        uint16_t st, cnt;
        memcpy(&st, rec + 28, 2);
        memcpy(&cnt, rec + 30, 2);
        snprintf(label, sizeof(label), "%.*s (%u 首)", 28, (const char *)rec, (unsigned)cnt);
        make_btn(list, label, lv_pct(100), 48, on_coll_clicked, (void *)(uintptr_t)c);
    }
    uint32_t lk = 0, nk = 0;
    for (uint32_t a = 0; a < s_hdr.appr_count; ++a) {
        uint8_t rec[APPR_REC];
        db_read(DB_HDR_SIZE + COLL_REC * s_hdr.coll_count + POEM_REC * s_hdr.poem_count +
                APPR_REC * a, rec, APPR_REC);
        uint8_t cat = rec[10];
        if (cat == 2) ++lk; else ++nk;
    }
    char l1[64], l2[64];
    snprintf(l1, sizeof(l1), "课本讲解与诗话 (%u)", (unsigned)nk);
    snprintf(l2, sizeof(l2), "词牌故事 (%u)", (unsigned)lk);
    make_btn(list, l1, lv_pct(100), 48, on_apprlist_cb, (void *)2);
    make_btn(list, l2, lv_pct(100), 48, on_apprlist_cb, (void *)3);
}

static void add_poem_buttons(lv_obj_t *list, uint16_t start, uint16_t count) {
    for (uint16_t k = 0; k < count; ++k) {
        Poem p;
        poem_load((int)(start + k), &p);
        char label[224];
        snprintf(label, sizeof(label), "%s  %s", p.title, p.author);
        make_btn(list, label, lv_pct(100), 44, on_poem_clicked,
                 (void *)(uintptr_t)(start + k));
        poem_free(&p);
    }
}

static void add_appr_buttons(lv_obj_t *list, uint8_t cat) {
    for (uint32_t a = 0; a < s_hdr.appr_count; ++a) {
        uint8_t rec[APPR_REC];
        db_read(DB_HDR_SIZE + COLL_REC * s_hdr.coll_count + POEM_REC * s_hdr.poem_count +
                APPR_REC * a, rec, APPR_REC);
        if (rec[10] != cat) continue;
        Appr ap;
        appr_load((int)a, &ap);
        make_btn(list, ap.title[0] ? ap.title : "?", lv_pct(100), 44,
                 on_appr_clicked, (void *)(uintptr_t)a);
        appr_free(&ap);
    }
}

static void build_list_view(void) {
    lv_obj_clean(s_body_box);
    lv_obj_set_style_pad_top(s_body_box, PAD, 0);
    s_view = VIEW_LIST;
    lv_obj_t *top = lv_obj_create(s_body_box);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, lv_pct(100), 44);
    if (s_list_kind == 1) make_label(top, "我的收藏", 20, 2);
    else if (s_list_kind == 2) make_label(top, "课本讲解与诗话", 20, 2);
    else if (s_list_kind == 3) make_label(top, "词牌故事", 20, 2);
    else {
        uint8_t rec[COLL_REC];
        db_read(DB_HDR_SIZE + COLL_REC * s_list_coll, rec, COLL_REC);
        char label[64];
        snprintf(label, sizeof(label), "%.*s", 28, (const char *)rec);
        make_label(top, label, 20, 2);
    }
    make_btn(top, "返回", 90, BTN_H, on_back_browse_cb, NULL);
    lv_obj_align(lv_obj_get_child(top, 1), LV_ALIGN_RIGHT_MID, 0, 0);

    lv_obj_t *list = lv_obj_create(s_body_box);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    if (s_list_kind == 1) {
        for (int k = 0; k < s_fav_n; ++k) {
            Poem p;
            poem_load((int)s_fav[k], &p);
            char label[224];
            snprintf(label, sizeof(label), "%s  %s", p.title, p.author);
            make_btn(list, label, lv_pct(100), 44, on_poem_clicked,
                     (void *)(uintptr_t)s_fav[k]);
            poem_free(&p);
        }
        if (s_fav_n == 0) make_label(list, "暂无收藏", UI_FONT, 0);
    } else if (s_list_kind == 2) {
        add_appr_buttons(list, 0);
        add_appr_buttons(list, 1);
    } else if (s_list_kind == 3) {
        add_appr_buttons(list, 2);
    } else {
        uint16_t st, cnt;
        if (coll_poem_start(s_list_coll, &st, &cnt)) {
            add_poem_buttons(list, st, cnt);
        }
    }
}

// ================= 事件 =================
static void on_daily_cb(lv_event_t *e) {
    (void)e;
    if (!s_db_ok) return;
    time_t now = time(NULL);
    struct tm local;
    localtime_r(&now, &local);
    uint32_t seed = (uint32_t)((local.tm_year + 1900) * 10000 +
                               (local.tm_mon + 1) * 100 + local.tm_mday);
    seed = seed * 2654435761u + 12345u;
    load_poem_and_show((int)(seed % s_hdr.poem_count), 0);
}
static void on_random_cb(lv_event_t *e) {
    (void)e;
    if (!s_db_ok) return;
    int idx = (int)next_rand(s_hdr.poem_count);
    if (idx == s_cur && s_hdr.poem_count > 1) idx = (idx + 1) % (int)s_hdr.poem_count;
    load_poem_and_show(idx, 0);
}
static void on_fav_cb(lv_event_t *e) {
    (void)e;
    fav_toggle((uint32_t)s_cur);
    refresh_fav_btn();
}
static void on_favlist_cb(lv_event_t *e) {
    (void)e;
    s_list_kind = 1;
    build_list_view();
}
static void on_apprlist_cb(lv_event_t *e) {
    s_list_kind = (int)(uintptr_t)lv_event_get_user_data(e);
    build_list_view();
}
static void on_browse_cb(lv_event_t *e) { (void)e; build_browse_view(); }
static void on_back_home_cb(lv_event_t *e) { (void)e; build_home_view(); }
static void on_back_browse_cb(lv_event_t *e) { (void)e; build_browse_view(); }
static void on_coll_clicked(lv_event_t *e) {
    s_list_coll = (uint16_t)(uintptr_t)lv_event_get_user_data(e);
    s_list_kind = 0;
    build_list_view();
}
static void on_poem_clicked(lv_event_t *e) {
    load_poem_and_show((int)(uintptr_t)lv_event_get_user_data(e), 0);
}
static void on_appr_clicked(lv_event_t *e) {
    enter_appr_view((int)(uintptr_t)lv_event_get_user_data(e));
}
static void on_appr_cb(lv_event_t *e) {
    (void)e;
    if (s_poem.appr != NO_APPR) enter_appr_view(s_poem.appr);
}

// ================= 侧键 =================
static int kb_on_vk(const char *key) {
    if (key == NULL) return 0;
    if (strcmp(key, "vk_home") == 0 || strcmp(key, "home") == 0) {
        dynapp_screen_go_home();
        return 1;
    }
    if (strcmp(key, "vk_next") == 0 || strcmp(key, "next") == 0) {
        if (s_view == VIEW_POEM || s_view == VIEW_APPR) page_next();
        return 1;
    }
    if (strcmp(key, "vk_prev") == 0 || strcmp(key, "prev") == 0) {
        if (s_view == VIEW_POEM || s_view == VIEW_APPR) page_prev();
        return 1;
    }
    return 1;
}

static void *poems_create(void);

static void on_screen_deleted(lv_event_t *e) {
    (void)e;
    s_scr = NULL;
    s_body_box = NULL;
    s_clip = NULL;
    s_content = NULL;
    s_pager_lbl = NULL;
    s_fav_btn = NULL;
    poem_free(&s_poem);
    appr_free(&s_appr);
    if (s_db != NULL) { fclose(s_db); s_db = NULL; }
    s_db_ok = 0;
    free(s_fav);
    s_fav = NULL;
    s_fav_n = s_fav_cap = 0;
}

static void *poems_create(void) {
    s_scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(s_scr);
    lv_obj_set_style_bg_color(s_scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(s_scr, on_screen_deleted, LV_EVENT_DELETE, NULL);

    s_body_box = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_body_box);
    lv_obj_set_size(s_body_box, 480, 800);
    lv_obj_set_pos(s_body_box, 0, 0);
    lv_obj_set_flex_flow(s_body_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(s_body_box, PAD, 0);
    lv_obj_set_style_pad_row(s_body_box, 6, 0);
    lv_obj_clear_flag(s_body_box, LV_OBJ_FLAG_SCROLLABLE);

    dynapp_vk_attach(s_scr, "poems", poems_create, kb_on_vk);

    if (!db_open()) {
        make_label(s_body_box, "未找到诗词数据(poems.db)，请重新安装应用", UI_FONT, 0);
        return s_scr;
    }

    state_load();
    if (s_cur < 0 || (uint32_t)s_cur >= s_hdr.poem_count) s_cur = 0;
    build_home_view();
    return s_scr;
}

void metalio_app_entry(metalio_app_desc_t *out) {
    memset(out, 0, sizeof(*out));
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "poems";
    out->name = "唐诗宋词";
    out->icon = "icon.png";
    out->create = poems_create;
}
