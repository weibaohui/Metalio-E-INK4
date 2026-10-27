// tools/weread_app/main/weread_main.c
// 微信读书动态应用（Metalio E-Ink4 480x800 竖屏墨水屏定制）
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"
#include "cJSON.h"

#define MAX_BOOKS_PER_PAGE 8
#define MAX_PAGES_PER_CHAPTER 256
#define SCREEN_W 480
#define SCREEN_H 800

typedef enum {
    VIEW_LOADING = 0,
    VIEW_QR_LOGIN,
    VIEW_SHELF,
    VIEW_READER,
    VIEW_TOC
} app_view_t;

typedef struct {
    char id[48];
    char title[128];
    char author[64];
    int progress;
    char chapter_uid[48];
    int chapter_offset;
    uint32_t update_time;
} book_item_t;

typedef struct {
    char uid[48];
    char title[128];
    int index;
    int word_count;
} toc_item_t;

// 全局应用状态
static lv_obj_t *s_scr = NULL;
static app_view_t s_curr_view = VIEW_LOADING;

// 字体辅助
static const lv_font_t *get_safe_font(int size, int weight) {
    const lv_font_t *f = fontpack_lv_font_get(size, weight);
    if (f == NULL) f = fontpack_lv_font_ui();
    return f;
}

// 书架数据
static book_item_t *s_shelf_books = NULL;
static int s_shelf_count = 0;
static int s_shelf_page = 0;

// 当前阅读书籍与章节
static char s_cur_book_id[48] = {0};
static char s_cur_book_title[128] = {0};
static toc_item_t *s_toc_items = NULL;
static int s_toc_count = 0;
static int s_cur_chapter_idx = 0;
static char s_cur_chapter_uid[48] = {0};
static char s_cur_chapter_title[128] = {0};

// 正文排版与翻页
static char *s_chapter_text = NULL;
static size_t s_chapter_text_len = 0;
static int s_page_offsets[MAX_PAGES_PER_CHAPTER];
static int s_page_count = 0;
static int s_cur_page = 0;
static int s_pages_turned_since_refresh = 0;
static time_t s_read_start_time = 0;
static int s_font_size = 24; // 20, 24, 28

// UI 元素
static lv_obj_t *s_view_obj = NULL;
static lv_obj_t *s_qr_code = NULL;
static lv_obj_t *s_qr_tip_lbl = NULL;
static lv_obj_t *s_reader_lbl = NULL;
static lv_obj_t *s_reader_footer_lbl = NULL;
static lv_obj_t *s_menu_layer = NULL;

// 前向声明
static void switch_view(app_view_t view);
static void refresh_shelf(void);
static void load_and_read_chapter(int chapter_idx);
static void paginate_chapter(void);
static void render_current_page(void);
static void sync_read_progress(void);
static void on_toc_received(const char *json_toc, int status, void *user_data);

// ==================== 视图切换与清理 ====================
static void clean_view_obj(void) {
    if (s_view_obj != NULL) {
        lv_obj_delete(s_view_obj);
        s_view_obj = NULL;
    }
    s_qr_code = NULL;
    s_qr_tip_lbl = NULL;
    s_reader_lbl = NULL;
    s_reader_footer_lbl = NULL;
    s_menu_layer = NULL;
}

// ==================== 扫码登录页 (480x800) ====================
static void on_qr_url_ready(const char *qr_url, void *user_data) {
    (void)user_data;
    if (s_curr_view != VIEW_QR_LOGIN || s_qr_code == NULL) return;
    if (qr_url && strlen(qr_url) > 0) {
        lv_qrcode_update(s_qr_code, qr_url, strlen(qr_url));
        if (s_qr_tip_lbl) lv_label_set_text(s_qr_tip_lbl, "请使用微信扫描上方二维码登录");
    }
}

static void on_login_finished(int success, const char *err_msg, void *user_data) {
    (void)user_data;
    if (success) {
        dynapp_toast_show("微信读书登录成功！", 1500);
        switch_view(VIEW_SHELF);
        refresh_shelf();
    } else {
        if (s_qr_tip_lbl) {
            char buf[128];
            snprintf(buf, sizeof(buf), "登录未完成: %s", err_msg ? err_msg : "请重试");
            lv_label_set_text(s_qr_tip_lbl, buf);
        }
    }
}

static void on_retry_login_btn(lv_event_t *e) {
    (void)e;
    if (s_qr_tip_lbl) lv_label_set_text(s_qr_tip_lbl, "正在获取登录二维码…");
    dynapp_weread_start_qr_login(on_qr_url_ready, on_login_finished, NULL);
}

static void build_view_qr(void) {
    s_view_obj = lv_obj_create(s_scr);
    lv_obj_set_size(s_view_obj, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(s_view_obj, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_view_obj, 0, 0);
    lv_obj_set_style_pad_all(s_view_obj, 0, 0);

    const lv_font_t *f36 = get_safe_font(36, 1);
    const lv_font_t *f22 = get_safe_font(22, 0);
    const lv_font_t *f20 = get_safe_font(20, 0);

    // 顶部标题
    lv_obj_t *title = lv_label_create(s_view_obj);
    lv_label_set_text(title, "微信读书");
    if (f36) lv_obj_set_style_text_font(title, f36, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 50);

    lv_obj_t *sub = lv_label_create(s_view_obj);
    lv_label_set_text(sub, "墨水屏专属版 · 书架同步");
    if (f20) lv_obj_set_style_text_font(sub, f20, 0);
    lv_obj_set_style_text_color(sub, lv_color_black(), 0);
    lv_obj_align(sub, LV_ALIGN_TOP_MID, 0, 105);

    // 二维码居中 (240x240)
    s_qr_code = lv_qrcode_create(s_view_obj);
    lv_qrcode_set_size(s_qr_code, 240);
    lv_qrcode_set_dark_color(s_qr_code, lv_color_black());
    lv_qrcode_set_light_color(s_qr_code, lv_color_white());
    lv_obj_align(s_qr_code, LV_ALIGN_TOP_MID, 0, 160);
    lv_obj_set_style_border_color(s_qr_code, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_qr_code, 3, 0);
    lv_obj_set_style_pad_all(s_qr_code, 8, 0);

    // 下方提示
    s_qr_tip_lbl = lv_label_create(s_view_obj);
    lv_obj_set_width(s_qr_tip_lbl, 400);
    lv_label_set_long_mode(s_qr_tip_lbl, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_qr_tip_lbl, "正在获取登录二维码…");
    if (f22) lv_obj_set_style_text_font(s_qr_tip_lbl, f22, 0);
    lv_obj_set_style_text_color(s_qr_tip_lbl, lv_color_black(), 0);
    lv_obj_set_style_text_align(s_qr_tip_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_qr_tip_lbl, LV_ALIGN_TOP_MID, 0, 440);

    // 刷新按钮
    lv_obj_t *retry_btn = lv_btn_create(s_view_obj);
    lv_obj_set_size(retry_btn, 160, 48);
    lv_obj_align(retry_btn, LV_ALIGN_TOP_MID, 0, 520);
    lv_obj_set_style_bg_color(retry_btn, lv_color_white(), 0);
    lv_obj_set_style_border_color(retry_btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(retry_btn, 2, 0);
    lv_obj_set_style_radius(retry_btn, 8, 0);
    lv_obj_add_event_cb(retry_btn, on_retry_login_btn, LV_EVENT_CLICKED, NULL);

    lv_obj_t *retry_lbl = lv_label_create(retry_btn);
    lv_label_set_text(retry_lbl, "刷新二维码");
    lv_obj_center(retry_lbl);
    if (f20) lv_obj_set_style_text_font(retry_lbl, f20, 0);
    lv_obj_set_style_text_color(retry_lbl, lv_color_black(), 0);

    // 启动登录轮询任务
    dynapp_weread_start_qr_login(on_qr_url_ready, on_login_finished, NULL);
}

// ==================== 书架列表页 (480x800) ====================
static void on_book_card_clicked(lv_event_t *e) {
    uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    if (idx < (uintptr_t)s_shelf_count) {
        book_item_t *b = &s_shelf_books[idx];
        if (strcmp(s_cur_book_id, b->id) != 0) {
            if (s_toc_items != NULL) {
                free(s_toc_items);
                s_toc_items = NULL;
                s_toc_count = 0;
            }
            if (s_chapter_text != NULL) {
                free(s_chapter_text);
                s_chapter_text = NULL;
                s_chapter_text_len = 0;
            }
            s_cur_chapter_idx = 0;
        }

        strncpy(s_cur_book_id, b->id, sizeof(s_cur_book_id) - 1);
        strncpy(s_cur_book_title, b->title, sizeof(s_cur_book_title) - 1);
        strncpy(s_cur_chapter_uid, b->chapter_uid, sizeof(s_cur_chapter_uid) - 1);

        dynapp_toast_show("正在加载正文…", 1500);
        switch_view(VIEW_READER);
    }
}

static void on_shelf_prev_btn(lv_event_t *e) {
    (void)e;
    if (s_shelf_page > 0) {
        s_shelf_page--;
        switch_view(VIEW_SHELF);
    }
}

static void on_shelf_next_btn(lv_event_t *e) {
    (void)e;
    int max_page = (s_shelf_count + MAX_BOOKS_PER_PAGE - 1) / MAX_BOOKS_PER_PAGE;
    if (s_shelf_page + 1 < max_page) {
        s_shelf_page++;
        switch_view(VIEW_SHELF);
    }
}

static void on_shelf_refresh_btn(lv_event_t *e) {
    (void)e;
    dynapp_toast_show("正在刷新书架…", 1000);
    refresh_shelf();
}

static void on_shelf_logout_btn(lv_event_t *e) {
    (void)e;
    dynapp_weread_logout();
    dynapp_toast_show("已退出登录", 1000);
    switch_view(VIEW_QR_LOGIN);
}

static void build_view_shelf(void) {
    s_view_obj = lv_obj_create(s_scr);
    lv_obj_set_size(s_view_obj, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(s_view_obj, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_view_obj, 0, 0);
    lv_obj_set_style_pad_all(s_view_obj, 0, 0);

    const lv_font_t *f22 = get_safe_font(22, 1);
    const lv_font_t *f20 = get_safe_font(20, 0);
    const lv_font_t *f18 = get_safe_font(18, 0);

    // 顶部顶栏 (高度 54)
    lv_obj_t *topbar = lv_obj_create(s_view_obj);
    lv_obj_set_pos(topbar, 0, 0);
    lv_obj_set_size(topbar, SCREEN_W, 54);
    lv_obj_set_style_bg_color(topbar, lv_color_white(), 0);
    lv_obj_set_style_border_side(topbar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(topbar, 2, 0);
    lv_obj_set_style_border_color(topbar, lv_color_black(), 0);

    lv_obj_t *title_lbl = lv_label_create(topbar);
    lv_label_set_text(title_lbl, "微信读书");
    if (f22) lv_obj_set_style_text_font(title_lbl, f22, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_black(), 0);
    lv_obj_align(title_lbl, LV_ALIGN_LEFT_MID, 12, 0);

    lv_obj_t *ref_btn = lv_btn_create(topbar);
    lv_obj_set_size(ref_btn, 72, 36);
    lv_obj_align(ref_btn, LV_ALIGN_RIGHT_MID, -86, 0);
    lv_obj_set_style_bg_color(ref_btn, lv_color_white(), 0);
    lv_obj_set_style_border_color(ref_btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(ref_btn, 2, 0);
    lv_obj_set_style_radius(ref_btn, 6, 0);
    lv_obj_add_event_cb(ref_btn, on_shelf_refresh_btn, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ref_lbl = lv_label_create(ref_btn);
    lv_label_set_text(ref_lbl, "刷新");
    lv_obj_center(ref_lbl);
    if (f18) lv_obj_set_style_text_font(ref_lbl, f18, 0);
    lv_obj_set_style_text_color(ref_lbl, lv_color_black(), 0);

    lv_obj_t *exit_btn = lv_btn_create(topbar);
    lv_obj_set_size(exit_btn, 72, 36);
    lv_obj_align(exit_btn, LV_ALIGN_RIGHT_MID, -8, 0);
    lv_obj_set_style_bg_color(exit_btn, lv_color_white(), 0);
    lv_obj_set_style_border_color(exit_btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(exit_btn, 2, 0);
    lv_obj_set_style_radius(exit_btn, 6, 0);
    lv_obj_add_event_cb(exit_btn, on_shelf_logout_btn, LV_EVENT_CLICKED, NULL);

    lv_obj_t *exit_lbl = lv_label_create(exit_btn);
    lv_label_set_text(exit_lbl, "退出");
    lv_obj_center(exit_lbl);
    if (f18) lv_obj_set_style_text_font(exit_lbl, f18, 0);
    lv_obj_set_style_text_color(exit_lbl, lv_color_black(), 0);

    // 书架空提示
    if (s_shelf_count == 0) {
        lv_obj_t *empty_lbl = lv_label_create(s_view_obj);
        lv_label_set_text(empty_lbl, "书架为空或拉取中\n请点击右上角刷新");
        if (f22) lv_obj_set_style_text_font(empty_lbl, f22, 0);
        lv_obj_set_style_text_align(empty_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(empty_lbl);
        return;
    }

    // 2 列 × 4 行网格卡片 (480 宽)
    int start_idx = s_shelf_page * MAX_BOOKS_PER_PAGE;
    int end_idx = start_idx + MAX_BOOKS_PER_PAGE;
    if (end_idx > s_shelf_count) end_idx = s_shelf_count;

    const int card_w = 212;
    const int card_h = 145;
    const int gap_x = 16;
    const int gap_y = 12;
    const int offset_x = 20;
    const int offset_y = 66;

    for (int i = start_idx; i < end_idx; ++i) {
        int pos = i - start_idx;
        int row = pos / 2;
        int col = pos % 2;

        int x = offset_x + col * (card_w + gap_x);
        int y = offset_y + row * (card_h + gap_y);

        lv_obj_t *card = lv_btn_create(s_view_obj);
        lv_obj_set_pos(card, x, y);
        lv_obj_set_size(card, card_w, card_h);
        lv_obj_set_style_bg_color(card, lv_color_white(), 0);
        lv_obj_set_style_border_color(card, lv_color_black(), 0);
        lv_obj_set_style_border_width(card, 2, 0);
        lv_obj_set_style_radius(card, 8, 0);
        lv_obj_set_style_pad_all(card, 10, 0);
        lv_obj_add_event_cb(card, on_book_card_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        // 书名
        lv_obj_t *btitle = lv_label_create(card);
        lv_obj_set_pos(btitle, 0, 0);
        lv_obj_set_width(btitle, card_w - 20);
        lv_label_set_long_mode(btitle, LV_LABEL_LONG_DOT);
        lv_label_set_text(btitle, s_shelf_books[i].title);
        if (f20) lv_obj_set_style_text_font(btitle, f20, 0);
        lv_obj_set_style_text_color(btitle, lv_color_black(), 0);

        // 作者
        lv_obj_t *bauthor = lv_label_create(card);
        lv_obj_set_pos(bauthor, 0, 52);
        lv_obj_set_width(bauthor, card_w - 20);
        lv_label_set_long_mode(bauthor, LV_LABEL_LONG_DOT);
        lv_label_set_text(bauthor, s_shelf_books[i].author);
        if (f18) lv_obj_set_style_text_font(bauthor, f18, 0);
        lv_obj_set_style_text_color(bauthor, lv_color_black(), 0);

        // 阅读进度
        lv_obj_t *bprog = lv_label_create(card);
        lv_obj_set_pos(bprog, 0, 95);
        char pbuf[32];
        if (s_shelf_books[i].progress > 0) {
            snprintf(pbuf, sizeof(pbuf), "已读 %d%%", s_shelf_books[i].progress);
        } else {
            snprintf(pbuf, sizeof(pbuf), "未读");
        }
        lv_label_set_text(bprog, pbuf);
        if (f18) lv_obj_set_style_text_font(bprog, f18, 0);
        lv_obj_set_style_text_color(bprog, lv_color_black(), 0);
    }

    // 底部翻页栏
    int max_page = (s_shelf_count + MAX_BOOKS_PER_PAGE - 1) / MAX_BOOKS_PER_PAGE;
    if (max_page > 1) {
        lv_obj_t *prev_btn = lv_btn_create(s_view_obj);
        lv_obj_set_pos(prev_btn, 60, 730);
        lv_obj_set_size(prev_btn, 90, 42);
        lv_obj_set_style_bg_color(prev_btn, lv_color_white(), 0);
        lv_obj_set_style_border_color(prev_btn, lv_color_black(), 0);
        lv_obj_set_style_border_width(prev_btn, 2, 0);
        lv_obj_set_style_radius(prev_btn, 6, 0);
        lv_obj_add_event_cb(prev_btn, on_shelf_prev_btn, LV_EVENT_CLICKED, NULL);

        lv_obj_t *prev_lbl = lv_label_create(prev_btn);
        lv_label_set_text(prev_lbl, "上一页");
        lv_obj_center(prev_lbl);
        if (f18) lv_obj_set_style_text_font(prev_lbl, f18, 0);
        lv_obj_set_style_text_color(prev_lbl, lv_color_black(), 0);

        lv_obj_t *page_lbl = lv_label_create(s_view_obj);
        char pstr[32];
        snprintf(pstr, sizeof(pstr), "%d / %d", s_shelf_page + 1, max_page);
        lv_label_set_text(page_lbl, pstr);
        lv_obj_align(page_lbl, LV_ALIGN_BOTTOM_MID, 0, -36);
        if (f18) lv_obj_set_style_text_font(page_lbl, f18, 0);
        lv_obj_set_style_text_color(page_lbl, lv_color_black(), 0);

        lv_obj_t *next_btn = lv_btn_create(s_view_obj);
        lv_obj_set_pos(next_btn, 330, 730);
        lv_obj_set_size(next_btn, 90, 42);
        lv_obj_set_style_bg_color(next_btn, lv_color_white(), 0);
        lv_obj_set_style_border_color(next_btn, lv_color_black(), 0);
        lv_obj_set_style_border_width(next_btn, 2, 0);
        lv_obj_set_style_radius(next_btn, 6, 0);
        lv_obj_add_event_cb(next_btn, on_shelf_next_btn, LV_EVENT_CLICKED, NULL);

        lv_obj_t *next_lbl = lv_label_create(next_btn);
        lv_label_set_text(next_lbl, "下一页");
        lv_obj_center(next_lbl);
        if (f18) lv_obj_set_style_text_font(next_lbl, f18, 0);
        lv_obj_set_style_text_color(next_lbl, lv_color_black(), 0);
    }
}

// 书架拉取回调
static void on_shelf_data_received(const char *json_shelf, int status, void *user_data) {
    (void)user_data;
    if (status != 200 || json_shelf == NULL) {
        dynapp_toast_show("书架更新失败，请检查网络", 1500);
        return;
    }

    cJSON *root = cJSON_Parse(json_shelf);
    if (!root || !cJSON_IsArray(root)) {
        if (root) cJSON_Delete(root);
        return;
    }

    if (s_shelf_books) {
        free(s_shelf_books);
        s_shelf_books = NULL;
    }

    s_shelf_count = cJSON_GetArraySize(root);
    if (s_shelf_count > 0) {
        s_shelf_books = (book_item_t *)calloc(s_shelf_count, sizeof(book_item_t));
        for (int i = 0; i < s_shelf_count; ++i) {
            cJSON *item = cJSON_GetArrayItem(root, i);
            cJSON *c_id = cJSON_GetObjectItem(item, "id");
            cJSON *c_title = cJSON_GetObjectItem(item, "title");
            cJSON *c_author = cJSON_GetObjectItem(item, "author");
            cJSON *c_prog = cJSON_GetObjectItem(item, "progress");
            cJSON *c_cuid = cJSON_GetObjectItem(item, "chapterUid");

            if (c_id && c_id->valuestring) strncpy(s_shelf_books[i].id, c_id->valuestring, sizeof(s_shelf_books[i].id) - 1);
            if (c_title && c_title->valuestring) strncpy(s_shelf_books[i].title, c_title->valuestring, sizeof(s_shelf_books[i].title) - 1);
            if (c_author && c_author->valuestring) strncpy(s_shelf_books[i].author, c_author->valuestring, sizeof(s_shelf_books[i].author) - 1);
            if (c_prog) s_shelf_books[i].progress = c_prog->valueint;
            if (c_cuid && c_cuid->valuestring) strncpy(s_shelf_books[i].chapter_uid, c_cuid->valuestring, sizeof(s_shelf_books[i].chapter_uid) - 1);
        }
    }
    cJSON_Delete(root);

    if (s_curr_view == VIEW_SHELF) {
        build_view_shelf();
    }
}

static void refresh_shelf(void) {
    dynapp_weread_fetch_shelf_async(on_shelf_data_received, NULL);
}

// ==================== 480x800 正文排版与阅读 ====================

static inline int get_utf8_char_len(unsigned char c) {
    if ((c & 0x80) == 0) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

static inline int is_utf8_cjk_or_fullwidth(const char *p, int clen) {
    if (clen == 1) return 0;
    return 1;
}

// 动态行流式分页算法：按照 480x800 屏幕逻辑像素与字号计算每页真实排版行数
static void paginate_chapter(void) {
    s_page_count = 0;
    s_cur_page = 0;
    if (s_chapter_text == NULL || s_chapter_text_len == 0) return;

    // 480x800 竖屏排版参数 (正文可用宽度 428px, 顶部 y=22, 页脚 y=766):
    // 字号 20px: 行高约 28px, 每页最多 20 行, 底部与页脚保留充分留白
    // 字号 24px: 行高约 34-36px, 每页最多 17 行, 底部与页脚保留约 70-90px 间隔, 彻底根除覆盖
    // 字号 28px: 行高约 40px, 每页最多 15 行, 底部与页脚保留充分留白
    int max_lines = 17;
    int cols_per_line = 34;
    if (s_font_size == 20) {
        max_lines = 20;
        cols_per_line = 42;
    } else if (s_font_size == 28) {
        max_lines = 15;
        cols_per_line = 30;
    }

    int offset = 0;
    s_page_offsets[0] = 0;
    s_page_count = 1;

    while (offset < (int)s_chapter_text_len && s_page_count < MAX_PAGES_PER_CHAPTER) {
        int p = offset;
        int curr_line = 0;
        int curr_col = 0;
        int last_good_break = -1;

        while (p < (int)s_chapter_text_len && curr_line < max_lines) {
            unsigned char c = (unsigned char)s_chapter_text[p];
            int clen = get_utf8_char_len(c);
            if (p + clen > (int)s_chapter_text_len) clen = (int)s_chapter_text_len - p;

            if (c == '\r') {
                p += clen;
                continue;
            }

            if (c == '\n') {
                curr_line++;
                curr_col = 0;
                p += clen;
                last_good_break = p;
                continue;
            }

            int w = is_utf8_cjk_or_fullwidth(s_chapter_text + p, clen) ? 2 : 1;
            if (curr_col + w > cols_per_line) {
                curr_line++;
                curr_col = 0;
                if (curr_line >= max_lines) {
                    break;
                }
            }

            curr_col += w;
            p += clen;

            // 遇到中文标点（句号、感叹号、问号、分号）记录为最佳分页候选点
            if (clen == 3) {
                unsigned char b0 = (unsigned char)s_chapter_text[p - 3];
                unsigned char b1 = (unsigned char)s_chapter_text[p - 2];
                unsigned char b2 = (unsigned char)s_chapter_text[p - 1];
                if (b0 == 0xE3 && b1 == 0x80 && (b2 == 0x82 || b2 == 0x9B)) { // 。
                    last_good_break = p;
                } else if (b0 == 0xEF && b1 == 0xBC && (b2 == 0x81 || b2 == 0x9F || b2 == 0x9B)) { // ！ ？ ；
                    last_good_break = p;
                }
            } else if (clen == 1 && (c == '.' || c == '!' || c == '?' || c == ';')) {
                last_good_break = p;
            }
        }

        if (p >= (int)s_chapter_text_len) {
            break;
        }

        int break_point = p;
        // 如果在行满时，近处 60 字节内有句末标点或段落换行，优先在句末断开，避免词句被腰斩
        if (last_good_break > offset && (p - last_good_break) < 60) {
            break_point = last_good_break;
        }

        // 确保断在 UTF-8 字符边界
        while (break_point < (int)s_chapter_text_len &&
               ((unsigned char)s_chapter_text[break_point] & 0xC0) == 0x80) {
            break_point++;
        }

        // 跳过页面开头的空换行
        while (break_point < (int)s_chapter_text_len &&
               (s_chapter_text[break_point] == '\n' || s_chapter_text[break_point] == '\r')) {
            break_point++;
        }

        if (break_point <= offset) {
            break_point = offset + 1;
        }

        offset = break_point;
        s_page_offsets[s_page_count++] = offset;
    }
}

static void render_current_page(void) {
    if (s_reader_lbl == NULL || s_chapter_text == NULL) return;

    int start = s_page_offsets[s_cur_page];
    int end = (s_cur_page + 1 < s_page_count) ? s_page_offsets[s_cur_page + 1] : (int)s_chapter_text_len;
    int len = end - start;
    if (len < 0) len = 0;

    static char s_page_buf[4096];
    if (len >= (int)sizeof(s_page_buf)) len = sizeof(s_page_buf) - 1;
    memcpy(s_page_buf, s_chapter_text + start, len);
    s_page_buf[len] = '\0';

    lv_label_set_text(s_reader_lbl, s_page_buf);

    if (s_reader_footer_lbl) {
        char fbuf[128];
        snprintf(fbuf, sizeof(fbuf), "%s  ·  %d / %d",
                 s_cur_chapter_title[0] ? s_cur_chapter_title : "阅读",
                 s_cur_page + 1, s_page_count);
        lv_label_set_text(s_reader_footer_lbl, fbuf);
    }

    // 翻页满 8 页全刷清除墨水屏残影
    s_pages_turned_since_refresh++;
    if (s_pages_turned_since_refresh >= 8) {
        s_pages_turned_since_refresh = 0;
        dynapp_epd_full_refresh();
    }
}

static void toggle_reader_menu(void);

// 触控手势处理：左退右进中菜单 (480 宽)
static void on_reader_touch_area(lv_event_t *e) {
    lv_point_t p;
    lv_indev_get_point(lv_indev_active(), &p);

    // 如果目录拉取失败，点击屏幕任意位置重新拉取
    if (s_toc_items == NULL || s_toc_count == 0) {
        if (s_reader_lbl) lv_label_set_text(s_reader_lbl, "正在重新拉取目录…");
        dynapp_weread_fetch_chapters_async(s_cur_book_id, on_toc_received, NULL);
        return;
    }

    if (p.x < 168) {
        // 上一页
        if (s_cur_page > 0) {
            s_cur_page--;
            render_current_page();
        } else if (s_cur_chapter_idx > 0) {
            sync_read_progress();
            dynapp_toast_show("上一章", 800);
            load_and_read_chapter(s_cur_chapter_idx - 1);
        }
    } else if (p.x > 312) {
        // 下一页
        if (s_cur_page + 1 < s_page_count) {
            s_cur_page++;
            render_current_page();
        } else if (s_cur_chapter_idx + 1 < s_toc_count) {
            sync_read_progress();
            dynapp_toast_show("下一章", 800);
            load_and_read_chapter(s_cur_chapter_idx + 1);
        }
    } else {
        toggle_reader_menu();
    }
}

// 悬浮菜单事件
static void on_menu_back_btn(lv_event_t *e) {
    (void)e;
    sync_read_progress();
    switch_view(VIEW_SHELF);
}

static void on_menu_toc_btn(lv_event_t *e) {
    (void)e;
    switch_view(VIEW_TOC);
}

static void on_menu_font_minus(lv_event_t *e) {
    (void)e;
    if (s_font_size > 20) {
        s_font_size -= 4;
        const lv_font_t *f = get_safe_font(s_font_size, 0);
        if (f && s_reader_lbl) lv_obj_set_style_text_font(s_reader_lbl, f, 0);
        paginate_chapter();
        render_current_page();
    }
}

static void on_menu_font_plus(lv_event_t *e) {
    (void)e;
    if (s_font_size < 28) {
        s_font_size += 4;
        const lv_font_t *f = get_safe_font(s_font_size, 0);
        if (f && s_reader_lbl) lv_obj_set_style_text_font(s_reader_lbl, f, 0);
        paginate_chapter();
        render_current_page();
    }
}

static void toggle_reader_menu(void) {
    if (s_menu_layer == NULL) {
        const lv_font_t *f20 = get_safe_font(20, 0);

        // 创建全屏悬浮层
        s_menu_layer = lv_obj_create(s_view_obj);
        lv_obj_set_size(s_menu_layer, SCREEN_W, SCREEN_H);
        lv_obj_set_pos(s_menu_layer, 0, 0);
        lv_obj_set_style_bg_opa(s_menu_layer, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(s_menu_layer, 0, 0);
        lv_obj_set_style_pad_all(s_menu_layer, 0, 0);

        // 顶部控制条 (返回书架 + 书名)
        lv_obj_t *top_bar = lv_obj_create(s_menu_layer);
        lv_obj_set_pos(top_bar, 0, 0);
        lv_obj_set_size(top_bar, SCREEN_W, 56);
        lv_obj_set_style_bg_color(top_bar, lv_color_white(), 0);
        lv_obj_set_style_border_side(top_bar, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(top_bar, 2, 0);
        lv_obj_set_style_border_color(top_bar, lv_color_black(), 0);

        lv_obj_t *back_btn = lv_btn_create(top_bar);
        lv_obj_set_size(back_btn, 84, 40);
        lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, 8, 0);
        lv_obj_set_style_bg_color(back_btn, lv_color_white(), 0);
        lv_obj_set_style_border_color(back_btn, lv_color_black(), 0);
        lv_obj_set_style_border_width(back_btn, 2, 0);
        lv_obj_set_style_radius(back_btn, 6, 0);
        lv_obj_add_event_cb(back_btn, on_menu_back_btn, LV_EVENT_CLICKED, NULL);

        lv_obj_t *back_lbl = lv_label_create(back_btn);
        lv_label_set_text(back_lbl, "〈 书架");
        lv_obj_center(back_lbl);
        if (f20) lv_obj_set_style_text_font(back_lbl, f20, 0);
        lv_obj_set_style_text_color(back_lbl, lv_color_black(), 0);

        lv_obj_t *btitle = lv_label_create(top_bar);
        lv_label_set_long_mode(btitle, LV_LABEL_LONG_DOT);
        lv_obj_set_width(btitle, 260);
        lv_label_set_text(btitle, s_cur_book_title);
        lv_obj_align(btitle, LV_ALIGN_CENTER, 30, 0);
        if (f20) lv_obj_set_style_text_font(btitle, f20, 0);
        lv_obj_set_style_text_color(btitle, lv_color_black(), 0);

        // 底部控制条 (目录 + 字号)
        lv_obj_t *bot_bar = lv_obj_create(s_menu_layer);
        lv_obj_set_pos(bot_bar, 0, SCREEN_H - 64);
        lv_obj_set_size(bot_bar, SCREEN_W, 64);
        lv_obj_set_style_bg_color(bot_bar, lv_color_white(), 0);
        lv_obj_set_style_border_side(bot_bar, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(bot_bar, 2, 0);
        lv_obj_set_style_border_color(bot_bar, lv_color_black(), 0);

        lv_obj_t *toc_btn = lv_btn_create(bot_bar);
        lv_obj_set_size(toc_btn, 84, 42);
        lv_obj_align(toc_btn, LV_ALIGN_LEFT_MID, 16, 0);
        lv_obj_set_style_bg_color(toc_btn, lv_color_white(), 0);
        lv_obj_set_style_border_color(toc_btn, lv_color_black(), 0);
        lv_obj_set_style_border_width(toc_btn, 2, 0);
        lv_obj_set_style_radius(toc_btn, 6, 0);
        lv_obj_add_event_cb(toc_btn, on_menu_toc_btn, LV_EVENT_CLICKED, NULL);

        lv_obj_t *toc_lbl = lv_label_create(toc_btn);
        lv_label_set_text(toc_lbl, "目录");
        lv_obj_center(toc_lbl);
        if (f20) lv_obj_set_style_text_font(toc_lbl, f20, 0);
        lv_obj_set_style_text_color(toc_lbl, lv_color_black(), 0);

        // 字号调节
        lv_obj_t *fminus_btn = lv_btn_create(bot_bar);
        lv_obj_set_size(fminus_btn, 68, 42);
        lv_obj_align(fminus_btn, LV_ALIGN_RIGHT_MID, -92, 0);
        lv_obj_set_style_bg_color(fminus_btn, lv_color_white(), 0);
        lv_obj_set_style_border_color(fminus_btn, lv_color_black(), 0);
        lv_obj_set_style_border_width(fminus_btn, 2, 0);
        lv_obj_set_style_radius(fminus_btn, 6, 0);
        lv_obj_add_event_cb(fminus_btn, on_menu_font_minus, LV_EVENT_CLICKED, NULL);

        lv_obj_t *fminus_lbl = lv_label_create(fminus_btn);
        lv_label_set_text(fminus_lbl, "A -");
        lv_obj_center(fminus_lbl);
        if (f20) lv_obj_set_style_text_font(fminus_lbl, f20, 0);
        lv_obj_set_style_text_color(fminus_lbl, lv_color_black(), 0);

        lv_obj_t *fplus_btn = lv_btn_create(bot_bar);
        lv_obj_set_size(fplus_btn, 68, 42);
        lv_obj_align(fplus_btn, LV_ALIGN_RIGHT_MID, -16, 0);
        lv_obj_set_style_bg_color(fplus_btn, lv_color_white(), 0);
        lv_obj_set_style_border_color(fplus_btn, lv_color_black(), 0);
        lv_obj_set_style_border_width(fplus_btn, 2, 0);
        lv_obj_set_style_radius(fplus_btn, 6, 0);
        lv_obj_add_event_cb(fplus_btn, on_menu_font_plus, LV_EVENT_CLICKED, NULL);

        lv_obj_t *fplus_lbl = lv_label_create(fplus_btn);
        lv_label_set_text(fplus_lbl, "A +");
        lv_obj_center(fplus_lbl);
        if (f20) lv_obj_set_style_text_font(fplus_lbl, f20, 0);
        lv_obj_set_style_text_color(fplus_lbl, lv_color_black(), 0);
    } else {
        lv_obj_delete(s_menu_layer);
        s_menu_layer = NULL;
    }
}

// 正文文本下载完成回调
static void on_text_received(const char *text, int len, int status, void *user_data) {
    (void)user_data;
    if (status != 200 || text == NULL || len <= 0) {
        if (s_reader_lbl) lv_label_set_text(s_reader_lbl, "正文下载/解密失败，请重试");
        return;
    }

    if (s_chapter_text) {
        free(s_chapter_text);
        s_chapter_text = NULL;
    }

    // 清理连续空行，段落之间紧凑排版，避免空行浪费屏幕空间
    s_chapter_text = (char *)malloc(len + 1);
    if (s_chapter_text) {
        int dst = 0;
        int consecutive_nl = 0;
        for (int i = 0; i < len; ++i) {
            char c = text[i];
            if (c == '\r') continue;
            if (c == '\n') {
                consecutive_nl++;
                if (consecutive_nl <= 1) {
                    s_chapter_text[dst++] = '\n';
                }
            } else {
                consecutive_nl = 0;
                s_chapter_text[dst++] = c;
            }
        }
        s_chapter_text[dst] = '\0';
        s_chapter_text_len = dst;
    }

    paginate_chapter();
    render_current_page();
    s_read_start_time = time(NULL);
}

// 目录拉取完成回调
static void on_toc_received(const char *json_toc, int status, void *user_data) {
    (void)user_data;
    if (status != 200 || json_toc == NULL) {
        if (s_reader_lbl) lv_label_set_text(s_reader_lbl, "获取目录失败，点击屏幕重试");
        return;
    }

    cJSON *root = cJSON_Parse(json_toc);
    if (!root || !cJSON_IsArray(root)) {
        if (root) cJSON_Delete(root);
        if (s_reader_lbl) lv_label_set_text(s_reader_lbl, "目录解析失败，点击重试");
        return;
    }

    if (s_toc_items) {
        free(s_toc_items);
        s_toc_items = NULL;
    }

    s_toc_count = cJSON_GetArraySize(root);
    if (s_toc_count > 0) {
        s_toc_items = (toc_item_t *)calloc(s_toc_count, sizeof(toc_item_t));
        int target_idx = 0;
        for (int i = 0; i < s_toc_count; ++i) {
            cJSON *item = cJSON_GetArrayItem(root, i);
            cJSON *c_uid = cJSON_GetObjectItem(item, "uid");
            cJSON *c_title = cJSON_GetObjectItem(item, "title");
            cJSON *c_idx = cJSON_GetObjectItem(item, "index");
            cJSON *c_words = cJSON_GetObjectItem(item, "wordCount");

            if (c_uid && c_uid->valuestring) strncpy(s_toc_items[i].uid, c_uid->valuestring, sizeof(s_toc_items[i].uid) - 1);
            if (c_title && c_title->valuestring) strncpy(s_toc_items[i].title, c_title->valuestring, sizeof(s_toc_items[i].title) - 1);
            if (c_idx) s_toc_items[i].index = c_idx->valueint;
            if (c_words) s_toc_items[i].word_count = c_words->valueint;

            if (s_cur_chapter_uid[0] && strcmp(s_cur_chapter_uid, s_toc_items[i].uid) == 0) {
                target_idx = i;
            }
        }

        cJSON_Delete(root);
        load_and_read_chapter(target_idx);
    } else {
        cJSON_Delete(root);
        if (s_reader_lbl) lv_label_set_text(s_reader_lbl, "本书无可用章节，点击屏幕重试");
    }
}

static void load_and_read_chapter(int chapter_idx) {
    if (chapter_idx < 0 || chapter_idx >= s_toc_count) return;
    s_cur_chapter_idx = chapter_idx;
    strncpy(s_cur_chapter_uid, s_toc_items[chapter_idx].uid, sizeof(s_cur_chapter_uid) - 1);
    strncpy(s_cur_chapter_title, s_toc_items[chapter_idx].title, sizeof(s_cur_chapter_title) - 1);

    if (s_reader_lbl) lv_label_set_text(s_reader_lbl, "正在解密章节正文…");
    dynapp_weread_fetch_chapter_text_async(s_cur_book_id, s_cur_chapter_uid, on_text_received, NULL);
}

static void sync_read_progress(void) {
    if (s_cur_book_id[0] == '\0' || s_cur_chapter_uid[0] == '\0') return;
    time_t now = time(NULL);
    int read_sec = (int)(now - s_read_start_time);
    if (read_sec < 5) read_sec = 5;
    if (read_sec > 3600) read_sec = 3600;

    int offset = s_page_offsets[s_cur_page];
    int percent = (s_toc_count > 0) ? ((s_cur_chapter_idx + 1) * 100 / s_toc_count) : 0;

    dynapp_weread_sync_progress(s_cur_book_id, s_cur_chapter_uid, s_cur_chapter_idx, offset, percent, read_sec);
    s_read_start_time = now;
}

static void build_view_reader(void) {
    s_view_obj = lv_obj_create(s_scr);
    lv_obj_set_size(s_view_obj, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(s_view_obj, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_view_obj, 0, 0);
    lv_obj_set_style_pad_all(s_view_obj, 0, 0);
    lv_obj_clear_flag(s_view_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_view_obj, on_reader_touch_area, LV_EVENT_CLICKED, NULL);

    // 正文文本区域 (480x800: 左右边距 26, 顶部 y=22, 高度 710)
    s_reader_lbl = lv_label_create(s_view_obj);
    lv_obj_set_pos(s_reader_lbl, 26, 22);
    lv_obj_set_size(s_reader_lbl, 428, 710);
    lv_label_set_long_mode(s_reader_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_clear_flag(s_reader_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(s_reader_lbl, LV_OBJ_FLAG_SCROLLABLE);
    lv_label_set_text(s_reader_lbl, "正在拉取目录…");
    const lv_font_t *f_content = get_safe_font(s_font_size, 0);
    if (f_content) lv_obj_set_style_text_font(s_reader_lbl, f_content, 0);
    lv_obj_set_style_text_color(s_reader_lbl, lv_color_black(), 0);

    static lv_style_t s_reader_style;
    static int s_style_init = 0;
    if (!s_style_init) {
        lv_style_init(&s_reader_style);
        lv_style_set_text_line_space(&s_reader_style, 8);
        s_style_init = 1;
    }
    lv_obj_add_style(s_reader_lbl, &s_reader_style, 0);

    // 页脚信息 (底部安全区: y=766, 高度 26, 纯白背景遮挡并置顶防穿透)
    s_reader_footer_lbl = lv_label_create(s_view_obj);
    lv_obj_set_pos(s_reader_footer_lbl, 20, 766);
    lv_obj_set_size(s_reader_footer_lbl, 440, 26);
    lv_obj_set_style_bg_color(s_reader_footer_lbl, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_reader_footer_lbl, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_reader_footer_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_label_set_text(s_reader_footer_lbl, "");
    const lv_font_t *f16 = get_safe_font(16, 0);
    if (f16) lv_obj_set_style_text_font(s_reader_footer_lbl, f16, 0);
    lv_obj_set_style_text_color(s_reader_footer_lbl, lv_color_black(), 0);
    lv_obj_set_style_text_align(s_reader_footer_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_move_foreground(s_reader_footer_lbl);

    // 只有当尚未拉取过全书目录，或者切换了书籍时，才拉取全书目录
    if (s_toc_items == NULL || s_toc_count == 0) {
        lv_label_set_text(s_reader_lbl, "正在拉取目录…");
        dynapp_weread_fetch_chapters_async(s_cur_book_id, on_toc_received, NULL);
    } else {
        if (s_chapter_text == NULL) {
            load_and_read_chapter(s_cur_chapter_idx);
        } else {
            render_current_page();
        }
    }
}

// ==================== 章节目录视图 (TOC) ====================
static void on_toc_back_btn(lv_event_t *e) {
    (void)e;
    switch_view(VIEW_READER);
}

static void on_toc_item_clicked(lv_event_t *e) {
    uintptr_t idx = (uintptr_t)lv_event_get_user_data(e);
    s_cur_chapter_idx = (int)idx;
    if (s_chapter_text) {
        free(s_chapter_text);
        s_chapter_text = NULL;
        s_chapter_text_len = 0;
    }
    switch_view(VIEW_READER);
}

static void build_view_toc(void) {
    s_view_obj = lv_obj_create(s_scr);
    lv_obj_set_size(s_view_obj, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(s_view_obj, lv_color_white(), 0);
    lv_obj_set_style_border_width(s_view_obj, 0, 0);
    lv_obj_set_style_pad_all(s_view_obj, 0, 0);

    const lv_font_t *f22 = get_safe_font(22, 1);
    const lv_font_t *f20 = get_safe_font(20, 0);

    // 顶栏 (高度 54)
    lv_obj_t *topbar = lv_obj_create(s_view_obj);
    lv_obj_set_pos(topbar, 0, 0);
    lv_obj_set_size(topbar, SCREEN_W, 54);
    lv_obj_set_style_bg_color(topbar, lv_color_white(), 0);
    lv_obj_set_style_border_side(topbar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(topbar, 2, 0);
    lv_obj_set_style_border_color(topbar, lv_color_black(), 0);

    lv_obj_t *back_btn = lv_btn_create(topbar);
    lv_obj_set_size(back_btn, 80, 38);
    lv_obj_align(back_btn, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_set_style_bg_color(back_btn, lv_color_white(), 0);
    lv_obj_set_style_border_color(back_btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(back_btn, 2, 0);
    lv_obj_set_style_radius(back_btn, 6, 0);
    lv_obj_add_event_cb(back_btn, on_toc_back_btn, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, "返回");
    lv_obj_center(back_lbl);
    if (f20) lv_obj_set_style_text_font(back_lbl, f20, 0);
    lv_obj_set_style_text_color(back_lbl, lv_color_black(), 0);

    lv_obj_t *title = lv_label_create(topbar);
    lv_label_set_text(title, "目录");
    lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);
    if (f22) lv_obj_set_style_text_font(title, f22, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);

    // 目录列表
    lv_obj_t *list = lv_obj_create(s_view_obj);
    lv_obj_set_pos(list, 0, 54);
    lv_obj_set_size(list, SCREEN_W, SCREEN_H - 54);
    lv_obj_set_style_bg_color(list, lv_color_white(), 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 10, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);

    for (int i = 0; i < s_toc_count; ++i) {
        lv_obj_t *btn = lv_btn_create(list);
        lv_obj_set_size(btn, SCREEN_W - 30, 48);
        lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
        lv_obj_set_style_border_side(btn, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, lv_color_black(), 0);
        lv_obj_set_style_radius(btn, 0, 0);
        lv_obj_set_style_pad_all(btn, 8, 0);
        lv_obj_add_event_cb(btn, on_toc_item_clicked, LV_EVENT_CLICKED, (void *)(uintptr_t)i);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, SCREEN_W - 60);

        char tbuf[160];
        if (i == s_cur_chapter_idx) {
            snprintf(tbuf, sizeof(tbuf), "✓  %s", s_toc_items[i].title);
        } else {
            snprintf(tbuf, sizeof(tbuf), "    %s", s_toc_items[i].title);
        }
        lv_label_set_text(lbl, tbuf);
        if (f20) lv_obj_set_style_text_font(lbl, f20, 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
    }
}

// ==================== 统一视图切换 ====================
static void switch_view(app_view_t view) {
    clean_view_obj();
    s_curr_view = view;

    switch (view) {
        case VIEW_QR_LOGIN:
            build_view_qr();
            break;
        case VIEW_SHELF:
            build_view_shelf();
            break;
        case VIEW_READER:
            build_view_reader();
            break;
        case VIEW_TOC:
            build_view_toc();
            break;
        default:
            break;
    }
}

// 按键拦截与导航
static int weread_on_vk_key(const char *key) {
    if (key == NULL) return 0;

    if (strcmp(key, "esc") == 0 || strcmp(key, "back") == 0 ||
        strcmp(key, "vk_home") == 0 || strcmp(key, "home") == 0) {
        if (s_curr_view == VIEW_READER) {
            sync_read_progress();
            switch_view(VIEW_SHELF);
            return 1;
        } else if (s_curr_view == VIEW_TOC) {
            switch_view(VIEW_READER);
            return 1;
        } else {
            dynapp_screen_go_home();
            return 1;
        }
    }

    if (strcmp(key, "vk_prev") == 0) {
        if (s_curr_view == VIEW_READER) {
            if (s_cur_page > 0) {
                s_cur_page--;
                render_current_page();
            } else if (s_cur_chapter_idx > 0) {
                sync_read_progress();
                load_and_read_chapter(s_cur_chapter_idx - 1);
            }
            return 1;
        } else if (s_curr_view == VIEW_SHELF && s_shelf_page > 0) {
            s_shelf_page--;
            switch_view(VIEW_SHELF);
            return 1;
        }
    }

    if (strcmp(key, "vk_next") == 0) {
        if (s_curr_view == VIEW_READER) {
            if (s_cur_page + 1 < s_page_count) {
                s_cur_page++;
                render_current_page();
            } else if (s_cur_chapter_idx + 1 < s_toc_count) {
                sync_read_progress();
                load_and_read_chapter(s_cur_chapter_idx + 1);
            }
            return 1;
        } else if (s_curr_view == VIEW_SHELF) {
            int max_page = (s_shelf_count + MAX_BOOKS_PER_PAGE - 1) / MAX_BOOKS_PER_PAGE;
            if (s_shelf_page + 1 < max_page) {
                s_shelf_page++;
                switch_view(VIEW_SHELF);
            }
            return 1;
        }
    }

    return 0;
}

// 根屏幕创建
static void *weread_create(void) {
    s_scr = lv_obj_create(NULL);
    lv_obj_set_size(s_scr, SCREEN_W, SCREEN_H);
    lv_obj_set_style_bg_color(s_scr, lv_color_white(), 0);

    dynapp_vk_attach(s_scr, "weread", weread_create, weread_on_vk_key);

    if (dynapp_weread_is_logged_in()) {
        switch_view(VIEW_SHELF);
        refresh_shelf();
    } else {
        switch_view(VIEW_QR_LOGIN);
    }

    return s_scr;
}

// 动态应用入口导出
void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "weread";
    out->name = "微信读书";
    out->icon = "icon.png";
    out->create = weread_create;
}
