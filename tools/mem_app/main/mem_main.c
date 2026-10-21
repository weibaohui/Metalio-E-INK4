// tools/mem_app/main/mem_main.c
// 记忆翻牌 动态应用：回合制、一步一刷，适配墨水屏低刷新率特性。
//
// 设计要点（墨水屏 480x800 / 1bpp 单色）：
//   - 一次只翻一张牌（翻牌=整张卡面重画），不做连续动画：墨水屏刷新慢，
//     两张牌同时翻反而看不清
//   - 牌面符号用字体包里现成的 8 个字形：♠♥♦♣ A J Q K（30@2 都有）
//   - 暗牌画成黑牌背（1bpp 下最省事也最像"背面"）；配对成功的牌加双线框标记
//   - 配错的两张 1.2 秒后自动翻回（LVGL 定时器，不占常驻任务）
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "esp_random.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

#define MAX_PAIRS 8
#define MAX_CARDS (MAX_PAIRS * 2)
#define NCOL 4
#define MEM_CW 90  // 牌宽
#define MEM_CH 120 // 牌高
#define MEM_GX 10  // 横向间距
#define MEM_GY 10  // 纵向间距

// 8 种牌面符号
static const char *kSym[8] = {"♠", "♥", "♦", "♣", "A", "J", "Q", "K"};

#define MODE_EASY 0 // 4x3 = 6 对
#define MODE_HARD 1 // 4x4 = 8 对

static int s_mode = MODE_HARD;
static int s_ncards = 16;
static int s_ncol = NCOL;
static int s_nrow = 4;

static uint8_t s_card[MAX_CARDS];  // 每张牌的符号 id
static uint8_t s_state[MAX_CARDS]; // 0=暗 1=翻开待配 2=已配对
static int s_flip[2];              // 当前翻开的两个位置
static int s_nflip = 0;
static int s_matched = 0;
static int s_moves = 0;            // 翻牌次数
static int s_won = 0;

static lv_obj_t *s_card_obj[MAX_CARDS];
static lv_obj_t *s_card_lbl[MAX_CARDS];
static lv_obj_t *s_info_lbl = NULL;
static lv_obj_t *s_status_lbl = NULL;

static lv_obj_t *s_scr = NULL;
static int s_move_count = 0;
static int s_last_full_refresh = 0;
#define FULL_REFRESH_EVERY 10

// ---- 字体 ----
static const lv_font_t *ui_font(void) {
    const lv_font_t *f = fontpack_lv_font_ui();
    return f != NULL ? f : fontpack_lv_font_get(25, 2);
}
static const lv_font_t *title_font(void) {
    const lv_font_t *f = fontpack_lv_font_get(30, 2);
    return f != NULL ? f : ui_font();
}
static const lv_font_t *sym_font(void) {
    const lv_font_t *f = fontpack_lv_font_get(30, 2); // ♠♥♦♣ A J Q K 都在这一档
    return f != NULL ? f : ui_font();
}

// ================= 牌局 =================
static void shuffle(int *a, int n) {
    for (int i = n - 1; i > 0; --i) {
        const int j = (int)(esp_random() % (uint32_t)(i + 1));
        const int t = a[i];
        a[i] = a[j];
        a[j] = t;
    }
}

static void new_game(int mode) {
    s_mode = mode;
    s_ncards = (mode == MODE_EASY) ? 12 : 16;
    s_nrow = s_ncards / NCOL;
    const int pairs = s_ncards / 2;
    int deck[MAX_CARDS];
    for (int i = 0; i < pairs; ++i) {
        deck[i * 2] = (uint8_t)i;
        deck[i * 2 + 1] = (uint8_t)i;
    }
    shuffle(deck, s_ncards);
    for (int i = 0; i < s_ncards; ++i) {
        s_card[i] = (uint8_t)deck[i];
        s_state[i] = 0;
    }
    s_nflip = 0;
    s_matched = 0;
    s_moves = 0;
    s_won = 0;
    s_move_count = 0;
    s_last_full_refresh = 0;
    s_flip[0] = s_flip[1] = -1;
}

static void render_card(int i) {
    lv_obj_t *o = s_card_obj[i];
    lv_obj_t *l = s_card_lbl[i];
    if (o == NULL || l == NULL) return;
    if (s_state[i] == 0) {
        // 暗牌：黑牌背
        lv_obj_set_style_bg_color(o, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(o, lv_color_white(), 0);
        lv_obj_set_style_border_width(o, 2, 0);
        lv_obj_add_flag(l, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    // 翻开：白底黑字；已配对的加双线框（再画一圈内框）
    lv_obj_set_style_bg_color(o, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, lv_color_black(), 0);
    lv_obj_set_style_border_width(o, s_state[i] == 2 ? 3 : 2, 0);
    lv_label_set_text(l, kSym[s_card[i]]);
    lv_obj_set_style_text_color(l, lv_color_black(), 0);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_HIDDEN);
}

// 牌阵布局：按当前牌数摆位置，多余的牌隐藏
static void layout_cards(void) {
    const int grid_w = NCOL * MEM_CW + (NCOL - 1) * MEM_GX;
    const int x0 = (480 - grid_w) / 2;
    const int y0 = 96;
    for (int i = 0; i < MAX_CARDS; ++i) {
        if (s_card_obj[i] == NULL) continue;
        if (i < s_ncards) {
            const int c = i % NCOL;
            const int r = i / NCOL;
            lv_obj_set_pos(s_card_obj[i], x0 + c * (MEM_CW + MEM_GX), y0 + r * (MEM_CH + MEM_GY));
            lv_obj_clear_flag(s_card_obj[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_card_obj[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void render_all(void) {
    for (int i = 0; i < s_ncards; ++i) render_card(i);
}

static void update_info_line(void) {
    if (s_info_lbl == NULL) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "配对 %d/%d · 翻 %d", s_matched, s_ncards / 2, s_moves);
    lv_label_set_text(s_info_lbl, buf);
}

static void update_status_line(void) {
    if (s_status_lbl == NULL) return;
    char buf[96];
    if (s_won) {
        snprintf(buf, sizeof(buf), "全部配对，%d 次翻牌通关", s_moves);
    } else if (s_nflip == 1) {
        snprintf(buf, sizeof(buf), "已翻一张，再翻一张试试");
    } else {
        snprintf(buf, sizeof(buf), "点牌翻开 · 翻出两张相同的即配对");
    }
    lv_label_set_text(s_status_lbl, buf);
}

// ================= 持久化 =================
static void save_game(void) {
    FILE *fp = (FILE *)dynapp_fopen_data("mem.txt", "w");
    if (fp == NULL) return;
    fprintf(fp, "MEM1 %d %d %d %d %d\n", s_mode, s_ncards, s_matched, s_moves, s_won);
    for (int i = 0; i < s_ncards; ++i) fprintf(fp, "%d%d", s_card[i], s_state[i]);
    fputc('\n', fp);
    fclose(fp);
}

static int load_game(void) {
    FILE *fp = (FILE *)dynapp_fopen_data("mem.txt", "r");
    if (fp == NULL) return 0;
    char magic[16] = {0};
    int mode = 0, ncards = 0, matched = 0, moves = 0, won = 0;
    if (fscanf(fp, "%15s", magic) != 1 || strcmp(magic, "MEM1") != 0) {
        fclose(fp);
        return 0;
    }
    if (fscanf(fp, "%d %d %d %d %d", &mode, &ncards, &matched, &moves, &won) != 5) {
        fclose(fp);
        return 0;
    }
    if ((mode != MODE_EASY && mode != MODE_HARD) || (ncards != 12 && ncards != 16)) {
        fclose(fp);
        return 0;
    }
    s_mode = mode;
    s_ncards = ncards;
    s_nrow = ncards / NCOL;
    for (int i = 0; i < ncards; ++i) {
        int c = 0, st = 0;
        if (fscanf(fp, "%1d%1d", &c, &st) != 2 || c < 0 || c > 7 || st > 2) {
            fclose(fp);
            return 0;
        }
        s_card[i] = (uint8_t)c;
        s_state[i] = (uint8_t)st;
    }
    fclose(fp);
    s_matched = matched;
    s_moves = moves;
    s_won = won;
    s_nflip = 0;
    s_flip[0] = s_flip[1] = -1;
    s_move_count = 0;
    s_last_full_refresh = 0;
    return 1;
}

// ================= 交互 =================
static void schedule_idle_maintenance(void);

static void flip_back_cb(lv_timer_t *tmr) {
    lv_timer_delete(tmr);
    // 配错的两张翻回背面
    for (int k = 0; k < 2; ++k) {
        const int i = s_flip[k];
        if (i >= 0 && s_state[i] == 1) {
            s_state[i] = 0;
            render_card(i);
        }
    }
    s_nflip = 0;
    s_flip[0] = s_flip[1] = -1;
    update_status_line();
    schedule_idle_maintenance();
}

static void on_card_clicked(lv_event_t *e) {
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_ncards) return;
    if (s_won) return;
    if (s_state[i] != 0) return; // 已翻开或已配对，不响应
    if (s_nflip >= 2) return;   // 已翻两张，等它们判定完

    s_state[i] = 1;
    render_card(i);
    s_moves++;
    s_move_count++;
    s_flip[s_nflip++] = i;
    update_info_line();

    if (s_nflip == 1) {
        update_status_line();
        schedule_idle_maintenance();
        return;
    }
    // 翻了两张：比对
    const int a = s_flip[0], b = s_flip[1];
    if (s_card[a] == s_card[b]) {
        s_state[a] = 2;
        s_state[b] = 2;
        render_card(a);
        render_card(b);
        s_matched++;
        s_nflip = 0;
        s_flip[0] = s_flip[1] = -1;
        update_info_line();
        if (s_matched >= s_ncards / 2) {
            s_won = 1;
            update_status_line();
            save_game();
            dynapp_modal_alert("配对成功！", "全部牌都配上了。", "确定");
            return;
        }
        update_status_line();
    } else {
        // 配错：1.2 秒后自动翻回
        update_status_line();
        lv_timer_create(flip_back_cb, 1200, NULL);
    }
    schedule_idle_maintenance();
}

static void on_new_game_clicked(lv_event_t *e) {
    (void)e;
    new_game(s_mode);
    layout_cards();
    render_all();
    update_info_line();
    update_status_line();
    save_game();
    dynapp_toast_show("新局", 1200);
    dynapp_epd_full_refresh();
}

static void on_mode_clicked(lv_event_t *e) {
    (void)e;
    new_game(s_mode == MODE_EASY ? MODE_HARD : MODE_EASY);
    layout_cards();
    render_all();
    update_info_line();
    update_status_line();
    save_game();
    dynapp_toast_show(s_mode == MODE_EASY ? "简单 6 对" : "困难 8 对", 1400);
    dynapp_epd_full_refresh();
}

static void on_hint_clicked(lv_event_t *e) {
    (void)e;
    if (s_won) {
        dynapp_toast_show("已经通关了", 1200);
        return;
    }
    // 找一对还没配对的（同符号），高亮其中一张
    for (int sym = 0; sym < 8; ++sym) {
        int first = -1;
        for (int i = 0; i < s_ncards; ++i) {
            if (s_card[i] != sym) continue;
            if (s_state[i] == 2) continue;
            if (first < 0) {
                first = i;
            } else {
                // 把这两张翻开提示一下
                s_state[first] = 1;
                s_state[i] = 1;
                render_card(first);
                render_card(i);
                char buf[48];
                snprintf(buf, sizeof(buf), "%s 在这两张", kSym[sym]);
                dynapp_toast_show(buf, 1600);
                s_nflip = 0;
                s_flip[0] = s_flip[1] = -1;
                update_info_line();
                schedule_idle_maintenance(); // 提示翻开的牌也要落盘，否则退出重进就没了
                dynapp_epd_full_refresh();
                return;
            }
        }
    }
    dynapp_toast_show("没有可提示的了", 1200);
}

static int mem_on_vk_key(const char *key) {
    if (key == NULL) return 0;
    if (strcmp(key, "vk_home") == 0 || strcmp(key, "esc") == 0 || strcmp(key, "back") == 0) {
        dynapp_screen_go_home();
        return 1;
    }
    return 0;
}

// ================= 空闲维护 =================
static lv_timer_t *s_save_timer = NULL;
static lv_timer_t *s_refresh_timer = NULL;
static int s_save_dirty = 0;

static void idle_save_cb(lv_timer_t *tmr) {
    if (s_save_dirty) {
        s_save_dirty = 0;
        save_game();
    }
    lv_timer_delete(tmr);
    s_save_timer = NULL;
}

static void idle_full_refresh_cb(lv_timer_t *tmr) {
    if (s_move_count - s_last_full_refresh >= FULL_REFRESH_EVERY) {
        s_last_full_refresh = s_move_count;
        dynapp_epd_full_refresh();
    }
    lv_timer_delete(tmr);
    s_refresh_timer = NULL;
}

static void schedule_idle_maintenance(void) {
    s_save_dirty = 1;
    if (s_save_timer != NULL) lv_timer_delete(s_save_timer);
    s_save_timer = lv_timer_create(idle_save_cb, 3500, NULL);
    if (s_refresh_timer != NULL) lv_timer_delete(s_refresh_timer);
    s_refresh_timer = lv_timer_create(idle_full_refresh_cb, 6000, NULL);
}

// ================= 界面 =================
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
    if (cb != NULL) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    const lv_font_t *f = ui_font();
    if (f != NULL) lv_obj_set_style_text_font(lbl, f, 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

static void on_screen_deleted(lv_event_t *e) {
    (void)e;
    save_game();
    s_scr = NULL;
    s_info_lbl = NULL;
    s_status_lbl = NULL;
    memset(s_card_obj, 0, sizeof(s_card_obj));
    memset(s_card_lbl, 0, sizeof(s_card_lbl));
}

static void *mem_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    s_scr = scr;
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 800);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_screen_deleted, LV_EVENT_DELETE, NULL);

    dynapp_screen_status_bar(scr);
    dynapp_vk_attach(scr, "mem", mem_create, mem_on_vk_key);

    if (!load_game()) new_game(MODE_HARD);

    // 1. 标题
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "记忆翻牌");
    const lv_font_t *f_title = title_font();
    if (f_title != NULL) lv_obj_set_style_text_font(title, f_title, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_set_pos(title, 16, 48);

    s_info_lbl = lv_label_create(scr);
    const lv_font_t *f_ui = ui_font();
    if (f_ui != NULL) lv_obj_set_style_text_font(s_info_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_info_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_align(s_info_lbl, LV_ALIGN_TOP_RIGHT, -16, 56);

    // 2. 牌阵：4 列，按牌数算行数
    for (int i = 0; i < MAX_CARDS; ++i) {
        lv_obj_t *card = lv_obj_create(scr);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, MEM_CW, MEM_CH);
        lv_obj_set_style_radius(card, 8, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(card, 2, 0);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, on_card_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        s_card_obj[i] = card;

        lv_obj_t *l = lv_label_create(card);
        lv_label_set_text(l, "");
        const lv_font_t *fs = sym_font();
        if (fs != NULL) lv_obj_set_style_text_font(l, fs, 0);
        lv_obj_set_style_text_color(l, lv_color_black(), 0);
        lv_obj_center(l);
        lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
        s_card_lbl[i] = l;
    }

    // 3. 状态行
    s_status_lbl = lv_label_create(scr);
    if (f_ui != NULL) lv_obj_set_style_text_font(s_status_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_status_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_pos(s_status_lbl, 16, 660);

    // 4. 按钮
    const lv_coord_t bw = 144, bh = 56, bgap = 8;
    const lv_coord_t by = 700;
    lv_obj_t *b1 = make_btn(scr, "新局", bw, bh, on_new_game_clicked, NULL);
    lv_obj_set_pos(b1, 16, by);
    lv_obj_t *b2 = make_btn(scr, "提示", bw, bh, on_hint_clicked, NULL);
    lv_obj_set_pos(b2, 16 + bw + bgap, by);
    lv_obj_t *b3 = make_btn(scr, "难度", bw, bh, on_mode_clicked, NULL);
    lv_obj_set_pos(b3, 16 + 2 * (bw + bgap), by);

    layout_cards();

    render_all();
    update_info_line();
    update_status_line();
    return scr;
}

void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "mem";
    out->name = "记忆翻牌";
    out->icon = "icon.png";
    out->create = mem_create;
}
