// tools/knowledge_app/main/knowledge_main.c
// 知识卡片 动态应用：每日一卡 / 顺序翻阅 / 随机 / 书目浏览 / 收藏 / 关联跳转。
// 数据来自 Mac 端 tools/knowledge_pack/pack_cards.py 预编译的 cards.db
// （单文件随机访问，设备端零解析；按卡 fseek 局部读，PSRAM 占用仅单卡）。
// 纯 C + LVGL，仅调用固件符号表导出接口。
//
// 注意：本应用不能有任何"非零初始化的静态变量"——那会生成 .data 节，
// 触发 elf_loader .got 重定位的野指针写路径（加载必崩，已双重加固：
// 加载器侧 where==0 跳过 + 应用侧全部静态零初始化）。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

#define APP_TAG "KnowApp"
#define PAD 10
#define BTN_H 44
#define MAX_TITLE 128
#define MAX_CHAPTER 192

// ---- cards.db 结构（pack_cards.py v1 输出，little-endian）----
#define DB_HDR_SIZE 64
#define CARD_REC 24
#define BOOK_REC 12

typedef struct {
    uint32_t card_count, book_count;
    uint32_t card_table_off, book_table_off, strings_off, links_off, body_off;
} DbHeader;

typedef struct {
    char title[MAX_TITLE];
    char chapter[MAX_CHAPTER];
    char *body;        // malloc；节标记：\x01 普通节头，\x02 引用节头（原文/要点）
    uint32_t body_len;
    uint32_t *links;   // malloc；关联目标卡号
    uint16_t link_count;
    uint16_t book_id;
} Card;

static FILE *s_db = NULL;
static DbHeader s_hdr;
static int s_db_ok = 0;

// ---- 视图与状态（全部零初始化：不生成 .data 节）----
#define VIEW_CARD 0
#define VIEW_BOOKS 1
#define VIEW_LIST 2
static int s_view = VIEW_CARD;
static int s_list_kind = 0;      // 0=某书卡片 1=收藏夹
static uint16_t s_list_book = 0;
static int s_cur = 0;            // 当前卡号
static int s_page = 0, s_pages = 0;
static uint32_t *s_fav = NULL;
static int s_fav_n = 0, s_fav_cap = 0;

// ---- UI ----
static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_body_box = NULL;   // 当前视图根容器
static lv_obj_t *s_clip = NULL;       // 卡片视口（固定高）
static lv_obj_t *s_content = NULL;    // 卡片内容（自适应高）
static lv_obj_t *s_pager_lbl = NULL;
static lv_obj_t *s_fav_btn = NULL;

// ---- 前向声明 ----
static void load_card_and_show(int idx, int page);
static void enter_card_view(void);
static void build_card_view(void);
static void build_books_view(void);
static void build_list_view(void);
static void page_next(void);
static void page_prev(void);
static void next_card(void);
static void prev_card(void);
static void on_next_card_cb(lv_event_t *e);
static void on_prev_card_cb(lv_event_t *e);
static void on_page_next_cb(lv_event_t *e);
static void on_page_prev_cb(lv_event_t *e);
static void on_daily_cb(lv_event_t *e);
static void on_random_cb(lv_event_t *e);
static void on_fav_cb(lv_event_t *e);
static void on_books_cb(lv_event_t *e);
static void on_back_card_cb(lv_event_t *e);
static void on_back_cb(lv_event_t *e);
static void on_book_clicked(lv_event_t *e);
static void on_card_clicked(lv_event_t *e);
static void on_link_clicked(lv_event_t *e);
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
            s_fav = realloc(s_fav, (size_t)s_fav_cap * sizeof(uint32_t));
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
    char line[256];
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
    fseek(s_db, (long)off, SEEK_SET);
    if (fread(buf, 1, len, s_db) != len) memset(buf, 0, len);
}

static void db_read_str(uint32_t off, char *buf, size_t cap) {
    if (cap == 0) return;
    buf[0] = '\0';
    db_read(off, buf, cap - 1);
    buf[cap - 1] = '\0';
}

static int db_open(void) {
    s_db = dynapp_fopen_data("cards.db", "rb");
    if (s_db == NULL) return 0;
    uint8_t hdr[DB_HDR_SIZE];
    db_read(0, hdr, DB_HDR_SIZE);
    if (memcmp(hdr, "KNOW", 4) != 0) return 0;
    memcpy(&s_hdr.card_count, hdr + 8, 4);
    memcpy(&s_hdr.book_count, hdr + 12, 4);
    memcpy(&s_hdr.card_table_off, hdr + 16, 4);
    memcpy(&s_hdr.book_table_off, hdr + 20, 4);
    memcpy(&s_hdr.strings_off, hdr + 24, 4);
    memcpy(&s_hdr.links_off, hdr + 28, 4);
    memcpy(&s_hdr.body_off, hdr + 32, 4);
    s_db_ok = (s_hdr.card_count > 0);
    return s_db_ok;
}

static void card_free(Card *c) {
    free(c->body); c->body = NULL;
    free(c->links); c->links = NULL;
    c->link_count = 0;
}

static void card_load(int idx, Card *c) {
    memset(c, 0, sizeof(*c));
    if (!s_db_ok || idx < 0 || (uint32_t)idx >= s_hdr.card_count) return;
    uint8_t rec[CARD_REC];
    db_read(s_hdr.card_table_off + CARD_REC * idx, rec, CARD_REC);
    uint32_t title_off, chapter_off, body_off, body_len, links_off;
    uint16_t link_count, book_id;
    memcpy(&title_off, rec, 4);
    memcpy(&chapter_off, rec + 4, 4);
    memcpy(&body_off, rec + 8, 4);
    memcpy(&body_len, rec + 12, 4);
    memcpy(&links_off, rec + 16, 4);
    memcpy(&link_count, rec + 20, 2);
    memcpy(&book_id, rec + 22, 2);
    db_read_str(s_hdr.strings_off + title_off, c->title, sizeof(c->title));
    db_read_str(s_hdr.strings_off + chapter_off, c->chapter, sizeof(c->chapter));
    c->body_len = body_len;
    c->body = malloc((size_t)body_len + 1);
    if (c->body) {
        db_read(s_hdr.body_off + body_off, c->body, body_len);
        c->body[body_len] = '\0';
    }
    c->link_count = link_count;
    c->links = NULL;
    if (link_count) {
        c->links = malloc((size_t)link_count * 4);
        if (c->links) db_read(s_hdr.links_off + links_off, c->links, (size_t)link_count * 4);
    }
    c->book_id = book_id;
}

static void book_name(uint16_t book_id, char *buf, size_t cap) {
    if (!s_db_ok || book_id >= s_hdr.book_count) { snprintf(buf, cap, "?"); return; }
    uint8_t rec[BOOK_REC];
    db_read(s_hdr.book_table_off + BOOK_REC * book_id, rec, BOOK_REC);
    uint32_t name_off;
    memcpy(&name_off, rec, 4);
    db_read_str(s_hdr.strings_off + name_off, buf, cap);
}

static void daily_idx(int *out) {
    time_t now = time(NULL);
    struct tm local = {};
    localtime_r(&now, &local);
    uint32_t seed = (uint32_t)((local.tm_year + 1900) * 10000 +
                               (local.tm_mon + 1) * 100 + local.tm_mday);
    seed = seed * 2654435761u + 12345u;
    *out = (int)(seed % (s_hdr.card_count ? s_hdr.card_count : 1));
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
    lv_obj_set_style_text_font(lbl, get_font(17, 0), 0);
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

// ================= 卡片视图 =================
static Card s_card;               // 当前卡
static int s_card_valid = 0;

// 正文分节渲染：\x01 普通节（节头【名】+正文），\x02 引用节（左黑竖线块）
static void render_card_content(Card *c) {
    lv_obj_clean(s_content);
    if (!c->body || c->body_len == 0) {
        make_label(s_content, "（无内容）", 16, 0);
        return;
    }
    char *text = malloc((size_t)c->body_len + 2);
    if (!text) return;
    char sec_title[64];
    uint32_t i = 0;
    while (i < c->body_len) {
        char marker = c->body[i];
        if (marker != '\x01' && marker != '\x02') { ++i; continue; }
        const int in_quote = (marker == '\x02');
        ++i;
        size_t t = 0;
        while (i < c->body_len && c->body[i] != '\n') {
            if (t < sizeof(sec_title) - 1) sec_title[t++] = c->body[i];
            ++i;
        }
        if (i < c->body_len) ++i;
        sec_title[t] = '\0';

        size_t used = 0;
        while (i < c->body_len && c->body[i] != '\x01' && c->body[i] != '\x02') {
            char ch = c->body[i];
            if (ch != '\r' && used < (size_t)c->body_len) text[used++] = ch;
            ++i;
        }
        while (used > 0 && text[used - 1] == '\n') --used;
        text[used] = '\0';

        if (in_quote) {
            lv_obj_t *q = lv_obj_create(s_content);
            lv_obj_remove_style_all(q);
            lv_obj_set_width(q, lv_pct(100));
            lv_obj_set_height(q, LV_SIZE_CONTENT);
            lv_obj_set_style_border_color(q, lv_color_black(), 0);
            lv_obj_set_style_border_width(q, 3, 0);
            lv_obj_set_style_border_side(q, LV_BORDER_SIDE_LEFT, 0);
            lv_obj_set_style_pad_left(q, 10, 0);
            lv_obj_set_style_pad_ver(q, 4, 0);
            lv_obj_set_style_bg_color(q, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(q, LV_OPA_COVER, 0);
            lv_obj_clear_flag(q, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
            make_label(q, text, 16, 0);
        } else {
            if (sec_title[0]) {
                char hd[80];
                snprintf(hd, sizeof(hd), "【%s】", sec_title);
                make_label(s_content, hd, 17, 2);
            }
            if (used) make_label(s_content, text, 16, 0);
        }
    }
    free(text);

    // 关联跳转行
    if (c->link_count > 0) {
        make_label(s_content, "—— 关联 ——", 14, 0);
        for (uint16_t k = 0; k < c->link_count; ++k) {
            char t_title[MAX_TITLE];
            uint8_t rec[CARD_REC];
            db_read(s_hdr.card_table_off + CARD_REC * c->links[k], rec, CARD_REC);
            uint32_t toff;
            memcpy(&toff, rec, 4);
            db_read_str(s_hdr.strings_off + toff, t_title, sizeof(t_title));
            char label[MAX_TITLE + 8];
            snprintf(label, sizeof(label), "→ %s", t_title);
            make_btn(s_content, label, lv_pct(100), 38,
                     on_link_clicked, (void *)(uintptr_t)c->links[k]);
        }
    }
}

static void show_page(int page) {
    if (page < 0) page = 0;
    if (page > s_pages - 1) page = s_pages - 1;
    s_page = page;
    const lv_coord_t vh = lv_obj_get_height(s_clip);
    if (vh > 0) lv_obj_scroll_to_y(s_clip, s_page * vh, LV_ANIM_OFF);   // 正值=向下翻页
    if (s_pager_lbl != NULL && s_pages > 1) {
        char buf[32];
        snprintf(buf, sizeof(buf), "第 %d / %d 页", s_page + 1, s_pages);
        lv_label_set_text(s_pager_lbl, buf);
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

static void build_card_view(void) {
    lv_obj_clean(s_body_box);
    card_load(s_cur, &s_card);
    s_card_valid = 1;

    lv_obj_t *box = lv_obj_create(s_body_box);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 6, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);

    make_label(box, s_card.title, 20, 2);
    make_label(box, s_card.chapter, 14, 0);

    // 视口：固定高（占满剩余空间），内容超高则分页
    s_clip = lv_obj_create(s_body_box);
    lv_obj_remove_style_all(s_clip);
    lv_obj_set_size(s_clip, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(s_clip, 1);
    lv_obj_set_style_pad_all(s_clip, 4, 0);
    lv_obj_set_style_radius(s_clip, 0, 0);
    lv_obj_add_flag(s_clip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_clip, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_clip, LV_SCROLLBAR_MODE_OFF);

    s_content = lv_obj_create(s_clip);
    lv_obj_remove_style_all(s_content);
    lv_obj_set_width(s_content, lv_pct(100));
    lv_obj_set_height(s_content, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_content, 6, 0);
    lv_obj_clear_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
    render_card_content(&s_card);

    // 页指示 + 翻页按钮
    lv_obj_t *pager = lv_obj_create(s_body_box);
    lv_obj_remove_style_all(pager);
    lv_obj_set_size(pager, lv_pct(100), 34);
    s_pager_lbl = make_label(pager, "", 15, 0);
    lv_obj_center(s_pager_lbl);
    lv_obj_t *pp = make_btn(pager, "◀上页", 90, 30, on_page_prev_cb, NULL);
    lv_obj_align(pp, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_t *pn = make_btn(pager, "下页▶", 90, 30, on_page_next_cb, NULL);
    lv_obj_align(pn, LV_ALIGN_RIGHT_MID, 0, 0);

    // 底部按钮排
    lv_obj_t *row = lv_obj_create(s_body_box);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), BTN_H);
    const lv_coord_t bw = (lv_coord_t)((LV_HOR_RES - 2 * PAD - 5 * 6) / 6);
    s_fav_btn = make_btn(row, "收藏", bw, BTN_H, on_fav_cb, NULL);
    make_btn(row, "上张", bw, BTN_H, on_prev_card_cb, NULL);
    make_btn(row, "每日", bw, BTN_H, on_daily_cb, NULL);
    make_btn(row, "随机", bw, BTN_H, on_random_cb, NULL);
    make_btn(row, "书目", bw, BTN_H, on_books_cb, NULL);
    make_btn(row, "下张", bw, BTN_H, on_next_card_cb, NULL);
    const int n = (int)lv_obj_get_child_count(row);
    for (int i = 0; i < n && i < 6; ++i) {
        lv_obj_align(lv_obj_get_child(row, i), LV_ALIGN_LEFT_MID,
                     (lv_coord_t)(i * (bw + 6)), 0);
    }

    refresh_fav_btn();
    s_view = VIEW_CARD;
}

static void enter_card_view(void) {
    lv_obj_clean(s_body_box);
    build_card_view();
    lv_obj_update_layout(s_body_box);
    const lv_coord_t ch = lv_obj_get_height(s_content);
    const lv_coord_t vh = lv_obj_get_height(s_clip);
    s_pages = (vh > 0) ? (int)((ch + vh - 1) / vh) : 1;
    if (s_pages < 1) s_pages = 1;
    if (s_page > s_pages - 1) s_page = s_pages - 1;
    show_page(s_page);
}

static void load_card_and_show(int idx, int page) {
    if (!s_db_ok || idx < 0 || (uint32_t)idx >= s_hdr.card_count) return;
    s_cur = idx;
    s_page = page;
    enter_card_view();
    state_save();
}

static void next_card(void) {
    if (!s_db_ok) return;
    load_card_and_show((s_cur + 1) % (int)s_hdr.card_count, 0);
}
static void prev_card(void) {
    if (!s_db_ok) return;
    load_card_and_show((s_cur - 1 + (int)s_hdr.card_count) % (int)s_hdr.card_count, 0);
}
static void page_next(void) {
    if (s_page < s_pages - 1) show_page(s_page + 1);
    else next_card();
}
static void page_prev(void) {
    if (s_page > 0) show_page(s_page - 1);
    else {
        const int prev = (s_cur - 1 + (int)s_hdr.card_count) % (int)s_hdr.card_count;
        load_card_and_show(prev, 9999);   // 尾页
    }
}

// ================= 列表视图 =================
static void build_books_view(void) {
    lv_obj_clean(s_body_box);
    s_view = VIEW_BOOKS;
    lv_obj_t *top = lv_obj_create(s_body_box);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, lv_pct(100), 44);
    make_label(top, "书目", 18, 2);
    make_btn(top, "◀ 返回卡片", 130, BTN_H, on_back_card_cb, NULL);

    lv_obj_t *list = lv_obj_create(s_body_box);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    for (uint32_t b = 0; b < s_hdr.book_count; ++b) {
        char name[128];
        book_name((uint16_t)b, name, sizeof(name));
        uint8_t rec[BOOK_REC];
        db_read(s_hdr.book_table_off + BOOK_REC * b, rec, BOOK_REC);
        uint32_t first, count;
        memcpy(&first, rec + 4, 4);
        memcpy(&count, rec + 8, 4);
        char label[160];
        snprintf(label, sizeof(label), "%s (%u)", name, (unsigned)count);
        make_btn(list, label, lv_pct(100), 46, on_book_clicked, (void *)(uintptr_t)b);
    }
}

static void add_list_buttons(lv_obj_t *list, const int *ids, int n) {
    for (int k = 0; k < n; ++k) {
        Card c;
        card_load(ids[k], &c);
        make_btn(list, c.title[0] ? c.title : "?", lv_pct(100), 44, on_card_clicked,
                 (void *)(uintptr_t)ids[k]);
        card_free(&c);
    }
}

static void build_list_view(void) {
    lv_obj_clean(s_body_box);
    s_view = VIEW_LIST;
    lv_obj_t *top = lv_obj_create(s_body_box);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, lv_pct(100), 44);
    if (s_list_kind == 1) {
        make_label(top, "收藏夹", 18, 2);
    } else {
        char bn[128];
        book_name(s_list_book, bn, sizeof(bn));
        make_label(top, bn, 18, 2);
    }
    make_btn(top, "◀ 返回", 110, BTN_H, on_back_cb, NULL);

    lv_obj_t *list = lv_obj_create(s_body_box);
    lv_obj_remove_style_all(list);
    lv_obj_set_size(list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    if (s_list_kind == 1) {
        add_list_buttons(list, (const int *)s_fav, s_fav_n);
        if (s_fav_n == 0) make_label(list, "暂无收藏", 16, 0);
    } else {
        uint8_t rec[BOOK_REC];
        db_read(s_hdr.book_table_off + BOOK_REC * s_list_book, rec, BOOK_REC);
        uint32_t first, count;
        memcpy(&first, rec + 4, 4);
        memcpy(&count, rec + 8, 4);
        int *ids = malloc(sizeof(int) * (count ? count : 1));
        if (ids != NULL) {
            for (uint32_t k = 0; k < count; ++k) ids[k] = (int)(first + k);
            add_list_buttons(list, ids, (int)count);
            free(ids);
        }
        if (count == 0) make_label(list, "（空）", 16, 0);
    }
}

// ================= 事件 =================
static void on_next_card_cb(lv_event_t *e) { (void)e; next_card(); }
static void on_prev_card_cb(lv_event_t *e) { (void)e; prev_card(); }
static void on_page_next_cb(lv_event_t *e) { (void)e; page_next(); }
static void on_page_prev_cb(lv_event_t *e) { (void)e; page_prev(); }
static void on_daily_cb(lv_event_t *e) {
    (void)e;
    int idx = 0;
    daily_idx(&idx);
    load_card_and_show(idx, 0);
}
static void on_random_cb(lv_event_t *e) {
    (void)e;
    if (!s_db_ok) return;
    int idx = s_cur;
    while (idx == s_cur && s_hdr.card_count > 1) {
        idx = rand() % (int)s_hdr.card_count;
    }
    load_card_and_show(idx, 0);
}
static void on_fav_cb(lv_event_t *e) {
    (void)e;
    fav_toggle((uint32_t)s_cur);
    refresh_fav_btn();
}
static void on_books_cb(lv_event_t *e) { (void)e; build_books_view(); }
static void on_back_card_cb(lv_event_t *e) { (void)e; enter_card_view(); }
static void on_back_cb(lv_event_t *e) {
    (void)e;
    if (s_list_kind == 1) enter_card_view();
    else build_books_view();
}
static void on_book_clicked(lv_event_t *e) {
    s_list_book = (uint16_t)(uintptr_t)lv_event_get_user_data(e);
    s_list_kind = 0;
    build_list_view();
}
static void on_card_clicked(lv_event_t *e) {
    const int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    load_card_and_show(idx, 0);
}
static void on_link_clicked(lv_event_t *e) {
    const int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    load_card_and_show(idx, 0);
}

// ================= 侧键 =================
static int kb_on_vk(const char *key) {
    if (key == NULL) return 0;
    if (strcmp(key, "vk_home") == 0 || strcmp(key, "home") == 0) {
        dynapp_screen_go_home();
        return 1;
    }
    if (strcmp(key, "vk_next") == 0 || strcmp(key, "next") == 0) {
        if (s_view == VIEW_CARD) page_next();
        return 1;
    }
    if (strcmp(key, "vk_prev") == 0 || strcmp(key, "prev") == 0) {
        if (s_view == VIEW_CARD) page_prev();
        return 1;
    }
    return 1;   // 其余按键吞掉，避免误触返回
}

static void *knowledge_create(void);

static void on_screen_deleted(lv_event_t *e) {
    (void)e;
    s_scr = NULL;
    s_body_box = NULL;
    s_clip = NULL;
    s_content = NULL;
    s_pager_lbl = NULL;
    s_fav_btn = NULL;
    card_free(&s_card);
    s_card_valid = 0;
    // 应用退出：释放句柄与收藏缓存（收藏已落 state.json，重进自动加载）
    if (s_db != NULL) { fclose(s_db); s_db = NULL; }
    s_db_ok = 0;
    free(s_fav); s_fav = NULL; s_fav_n = s_fav_cap = 0;
}

static void *knowledge_create(void) {
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

    dynapp_vk_attach(s_scr, "knowledge", knowledge_create, kb_on_vk);

    if (!db_open()) {
        make_label(s_body_box, "未找到知识库数据(cards.db)，请重新安装应用", 16, 0);
        return s_scr;
    }

    state_load();
    if (s_cur < 0 || (uint32_t)s_cur >= s_hdr.card_count) s_cur = 0;
    s_page = 0;
    enter_card_view();
    return s_scr;
}

// 固件入口点：仅填 desc（开机扫描与点开时都会调用，必须无副作用）
void metalio_app_entry(metalio_app_desc_t *out) {
    memset(out, 0, sizeof(*out));
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "knowledge";
    out->name = "知识卡片";
    out->icon = "icon.png";
    out->create = knowledge_create;
}
