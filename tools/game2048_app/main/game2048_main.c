// tools/game2048_app/main/game2048_main.c
// 2048 动态应用：.app Bundle 规范版
// 纯 C + LVGL，仅调用固件符号表内 API；点开 dlopen，退出 dlclose。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "esp_random.h"

#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

#define GRID_SIZE 4
#define BOARD_SIZE 440
#define CELL_SIZE 96
#define CELL_GAP 10
#define CELL_PAD 13
#define FULL_REFRESH_EVERY 15

typedef enum {
    DIR_UP = 0,
    DIR_DOWN = 1,
    DIR_LEFT = 2,
    DIR_RIGHT = 3
} dir_t;

// ---- 游戏核心状态 ----
static int s_board[GRID_SIZE][GRID_SIZE];
static int s_undo_board[GRID_SIZE][GRID_SIZE];
static int s_score = 0;
static int s_undo_score = 0;
static int s_best_score = 0;
static int s_has_undo = 0;
static int s_is_over = 0;
static int s_has_won = 0;
static int s_celebrated = 0;

static int s_move_count = 0;
static int s_last_full_refresh = 0;
static int s_alive = 0;

// ---- UI 元素 ----
static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_board_obj = NULL;
static lv_obj_t *s_cells[GRID_SIZE][GRID_SIZE];
static lv_obj_t *s_cell_lbls[GRID_SIZE][GRID_SIZE];
static int s_rendered[GRID_SIZE][GRID_SIZE];

static lv_obj_t *s_score_lbl = NULL;
static lv_obj_t *s_best_lbl = NULL;
static char s_score_txt[16] = "";
static char s_best_txt[16] = "";

static lv_point_t s_touch_start = {0, 0};
static int s_touch_active = 0;

static lv_obj_t *s_modal_mask = NULL;

static void *game2048_create(void);

// ---- 持久化最高分 (data/hiscore.txt) ----
static void load_best_score(void) {
    s_best_score = 0;
    FILE *fp = (FILE *)dynapp_fopen_data("hiscore.txt", "r");
    if (fp != NULL) {
        if (fscanf(fp, "%d", &s_best_score) != 1) {
            s_best_score = 0;
        }
        fclose(fp);
    }
}

static void save_best_score(void) {
    if (s_score > s_best_score) {
        s_best_score = s_score;
        FILE *fp = (FILE *)dynapp_fopen_data("hiscore.txt", "w");
        if (fp != NULL) {
            fprintf(fp, "%d\n", s_best_score);
            fclose(fp);
        }
    }
}

// ---- 核心逻辑 ----
static void spawn_random_tile(void) {
    struct { int r, c; } empty[GRID_SIZE * GRID_SIZE];
    int count = 0;

    for (int r = 0; r < GRID_SIZE; ++r) {
        for (int c = 0; c < GRID_SIZE; ++c) {
            if (s_board[r][c] == 0) {
                empty[count].r = r;
                empty[count].c = c;
                count++;
            }
        }
    }

    if (count == 0) return;

    int idx = (int)(esp_random() % count);
    int val = (esp_random() % 10 == 0) ? 4 : 2;
    s_board[empty[idx].r][empty[idx].c] = val;
}

static int can_move(void) {
    for (int r = 0; r < GRID_SIZE; ++r) {
        for (int c = 0; c < GRID_SIZE; ++c) {
            if (s_board[r][c] == 0) return 1;
            if (c + 1 < GRID_SIZE && s_board[r][c] == s_board[r][c + 1]) return 1;
            if (r + 1 < GRID_SIZE && s_board[r][c] == s_board[r + 1][c]) return 1;
        }
    }
    return 0;
}

static int process_line(int line[GRID_SIZE], int *score_add) {
    int original[GRID_SIZE];
    memcpy(original, line, sizeof(original));

    int compacted[GRID_SIZE] = {0};
    int w = 0;
    for (int i = 0; i < GRID_SIZE; ++i) {
        if (line[i] != 0) compacted[w++] = line[i];
    }

    int merged[GRID_SIZE] = {0};
    int mw = 0;
    for (int i = 0; i < w; ++i) {
        if (i + 1 < w && compacted[i] == compacted[i + 1]) {
            int val = compacted[i] * 2;
            merged[mw++] = val;
            *score_add += val;
            ++i;
        } else {
            merged[mw++] = compacted[i];
        }
    }

    memcpy(line, merged, sizeof(merged));
    return memcmp(original, line, sizeof(original)) != 0;
}

static int slide_and_merge(dir_t dir) {
    int changed = 0;
    int add_score = 0;

    switch (dir) {
        case DIR_LEFT:
            for (int r = 0; r < GRID_SIZE; ++r) {
                int line[GRID_SIZE];
                for (int c = 0; c < GRID_SIZE; ++c) line[c] = s_board[r][c];
                if (process_line(line, &add_score)) {
                    changed = 1;
                    for (int c = 0; c < GRID_SIZE; ++c) s_board[r][c] = line[c];
                }
            }
            break;

        case DIR_RIGHT:
            for (int r = 0; r < GRID_SIZE; ++r) {
                int line[GRID_SIZE];
                for (int c = 0; c < GRID_SIZE; ++c) line[c] = s_board[r][GRID_SIZE - 1 - c];
                if (process_line(line, &add_score)) {
                    changed = 1;
                    for (int c = 0; c < GRID_SIZE; ++c) s_board[r][GRID_SIZE - 1 - c] = line[c];
                }
            }
            break;

        case DIR_UP:
            for (int c = 0; c < GRID_SIZE; ++c) {
                int line[GRID_SIZE];
                for (int r = 0; r < GRID_SIZE; ++r) line[r] = s_board[r][c];
                if (process_line(line, &add_score)) {
                    changed = 1;
                    for (int r = 0; r < GRID_SIZE; ++r) s_board[r][c] = line[r];
                }
            }
            break;

        case DIR_DOWN:
            for (int c = 0; c < GRID_SIZE; ++c) {
                int line[GRID_SIZE];
                for (int r = 0; r < GRID_SIZE; ++r) line[r] = s_board[GRID_SIZE - 1 - r][c];
                if (process_line(line, &add_score)) {
                    changed = 1;
                    for (int r = 0; r < GRID_SIZE; ++r) s_board[GRID_SIZE - 1 - r][c] = line[r];
                }
            }
            break;
    }

    if (changed) {
        s_score += add_score;
        if (s_score > s_best_score) {
            save_best_score();
        }
    }
    return changed;
}

static void core_reset(void) {
    memset(s_board, 0, sizeof(s_board));
    memset(s_undo_board, 0, sizeof(s_undo_board));
    s_score = 0;
    s_undo_score = 0;
    s_has_undo = 0;
    s_is_over = 0;
    s_has_won = 0;
    s_celebrated = 0;

    spawn_random_tile();
    spawn_random_tile();
}

static int core_move(dir_t dir) {
    if (s_is_over) return 0;

    int prev_board[GRID_SIZE][GRID_SIZE];
    memcpy(prev_board, s_board, sizeof(s_board));
    int prev_score = s_score;

    if (!slide_and_merge(dir)) {
        return 0;
    }

    memcpy(s_undo_board, prev_board, sizeof(prev_board));
    s_undo_score = prev_score;
    s_has_undo = 1;

    spawn_random_tile();

    if (!s_has_won) {
        for (int r = 0; r < GRID_SIZE; ++r) {
            for (int c = 0; c < GRID_SIZE; ++c) {
                if (s_board[r][c] >= 2048) {
                    s_has_won = 1;
                    break;
                }
            }
        }
    }

    if (!can_move()) {
        s_is_over = 1;
    }

    return 1;
}

static int core_undo(void) {
    if (!s_has_undo) return 0;

    memcpy(s_board, s_undo_board, sizeof(s_board));
    s_score = s_undo_score;
    s_has_undo = 0;
    s_is_over = 0;
    return 1;
}

// ---- UI 渲染 ----
static void apply_tile_style(lv_obj_t *cell, lv_obj_t *lbl, int val) {
    if (cell == NULL || lbl == NULL) return;

    if (val == 0) {
        lv_obj_set_style_bg_color(cell, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(cell, lv_palette_lighten(LV_PALETTE_GREY, 2), 0);
        lv_obj_set_style_border_width(cell, 1, 0);
        lv_label_set_text(lbl, "");
        return;
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%d", val);
    lv_label_set_text(lbl, buf);

    int font_sz = 34;
    if (val >= 1024) font_sz = 24;
    else if (val >= 128) font_sz = 28;
    else if (val >= 16) font_sz = 32;

    const lv_font_t *f = fontpack_lv_font_get(font_sz, 2);
    if (f == NULL) f = fontpack_lv_font_ui();
    if (f != NULL) lv_obj_set_style_text_font(lbl, f, 0);

    if (val == 2) {
        lv_obj_set_style_bg_color(cell, lv_color_white(), 0);
        lv_obj_set_style_border_color(cell, lv_color_black(), 0);
        lv_obj_set_style_border_width(cell, 2, 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    } else if (val == 4) {
        lv_obj_set_style_bg_color(cell, lv_color_white(), 0);
        lv_obj_set_style_border_color(cell, lv_color_black(), 0);
        lv_obj_set_style_border_width(cell, 3, 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    } else if (val == 8) {
        lv_obj_set_style_bg_color(cell, lv_palette_lighten(LV_PALETTE_GREY, 4), 0);
        lv_obj_set_style_border_color(cell, lv_color_black(), 0);
        lv_obj_set_style_border_width(cell, 3, 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    } else if (val == 16) {
        lv_obj_set_style_bg_color(cell, lv_palette_lighten(LV_PALETTE_GREY, 3), 0);
        lv_obj_set_style_border_color(cell, lv_color_black(), 0);
        lv_obj_set_style_border_width(cell, 3, 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    } else if (val == 32) {
        lv_obj_set_style_bg_color(cell, lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
        lv_obj_set_style_border_color(cell, lv_color_black(), 0);
        lv_obj_set_style_border_width(cell, 3, 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    } else if (val == 64) {
        lv_obj_set_style_bg_color(cell, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
        lv_obj_set_style_border_color(cell, lv_color_black(), 0);
        lv_obj_set_style_border_width(cell, 3, 0);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    } else {
        lv_obj_set_style_bg_color(cell, lv_color_black(), 0);
        lv_obj_set_style_border_color(cell, lv_color_black(), 0);
        lv_obj_set_style_border_width(cell, val >= 1024 ? 4 : 2, 0);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    }
}

static void sync_best_label(void) {
    if (s_best_lbl == NULL) return;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", s_best_score);
    lv_label_set_text(s_best_lbl, buf);
}

static void redraw_board(void) {
    if (!s_alive) return;
    for (int r = 0; r < GRID_SIZE; ++r) {
        for (int c = 0; c < GRID_SIZE; ++c) {
            int v = s_board[r][c];
            if (s_rendered[r][c] == v) continue;
            s_rendered[r][c] = v;
            apply_tile_style(s_cells[r][c], s_cell_lbls[r][c], v);
        }
    }

    if (s_score_lbl != NULL) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", s_score);
        if (strcmp(buf, s_score_txt) != 0) {
            strncpy(s_score_txt, buf, sizeof(s_score_txt) - 1);
            lv_label_set_text(s_score_lbl, s_score_txt);
        }
    }

    if (s_score > s_best_score) {
        s_best_score = s_score;
        sync_best_label();
    }
}

static void maybe_full_refresh(void) {
    if (s_move_count - s_last_full_refresh >= FULL_REFRESH_EVERY) {
        dynapp_epd_full_refresh();
        s_last_full_refresh = s_move_count;
    }
}

static void close_modal(void) {
    if (s_modal_mask != NULL) {
        lv_obj_del(s_modal_mask);
        s_modal_mask = NULL;
    }
}

static void do_restart(void);

static void modal_restart_cb(lv_event_t *e) {
    close_modal();
    do_restart();
}

static void modal_dismiss_cb(lv_event_t *e) {
    close_modal();
}

static void show_game_over_modal(void) {
    if (!s_alive || s_modal_mask != NULL) return;
    save_best_score();

    lv_obj_t *mask = lv_obj_create(s_scr);
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, 480, 800);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_50, 0);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(mask, modal_restart_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = lv_obj_create(mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 360, 200);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 3, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_add_event_cb(card, modal_restart_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(card);
    char msg[128];
    snprintf(msg, sizeof(msg), "游戏结束\n\n得分: %d  |  最高: %d\n\n点按任意位置重新开始",
             s_score, s_best_score);
    lv_label_set_text(lbl, msg);
    const lv_font_t *f = fontpack_lv_font_get(20, 2);
    if (f == NULL) f = fontpack_lv_font_ui();
    if (f != NULL) lv_obj_set_style_text_font(lbl, f, 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl);

    s_modal_mask = mask;
}

static void show_victory_modal(void) {
    if (!s_alive || s_modal_mask != NULL) return;
    save_best_score();

    lv_obj_t *mask = lv_obj_create(s_scr);
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, 480, 800);
    lv_obj_set_pos(mask, 0, 0);
    lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(mask, LV_OPA_50, 0);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(mask, modal_dismiss_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = lv_obj_create(mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, 360, 200);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 3, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_add_event_cb(card, modal_dismiss_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(card);
    char msg[128];
    snprintf(msg, sizeof(msg), "恭喜达成 2048！\n\n当前得分: %d\n\n点按继续挑战更高分", s_score);
    lv_label_set_text(lbl, msg);
    const lv_font_t *f = fontpack_lv_font_get(20, 2);
    if (f == NULL) f = fontpack_lv_font_ui();
    if (f != NULL) lv_obj_set_style_text_font(lbl, f, 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl);

    s_modal_mask = mask;
}

static void handle_move(dir_t dir) {
    if (!s_alive || s_modal_mask != NULL) return;

    if (core_move(dir)) {
        s_move_count++;
        redraw_board();
        maybe_full_refresh();

        if (s_has_won && !s_celebrated) {
            s_celebrated = 1;
            show_victory_modal();
            return;
        }

        if (s_is_over) {
            show_game_over_modal();
        }
    }
}

static void do_restart(void) {
    close_modal();
    core_reset();
    s_move_count = 0;
    s_last_full_refresh = 0;
    memset(s_rendered, 0xFF, sizeof(s_rendered));
    redraw_board();
    sync_best_label();
}

static void on_btn_clicked(lv_event_t *e) {
    intptr_t kind = (intptr_t)lv_event_get_user_data(e);
    if (!s_alive) return;

    switch (kind) {
        case 0: // 撤销
            if (core_undo()) {
                redraw_board();
            }
            break;
        case 1: // 重开
            do_restart();
            break;
        case 2: // 退出
            save_best_score();
            dynapp_screen_go_home();
            break;
        default: break;
    }
}

static void on_board_touch(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) return;

    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(indev, &s_touch_start);
        s_touch_active = 1;
    } else if (code == LV_EVENT_RELEASED && s_touch_active) {
        s_touch_active = 0;
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        lv_coord_t dx = p.x - s_touch_start.x;
        lv_coord_t dy = p.y - s_touch_start.y;
        lv_coord_t min_swipe = 35;

        if (abs(dx) >= min_swipe || abs(dy) >= min_swipe) {
            if (abs(dx) > abs(dy)) {
                handle_move(dx > 0 ? DIR_RIGHT : DIR_LEFT);
            } else {
                handle_move(dy > 0 ? DIR_DOWN : DIR_UP);
            }
        }
    }
}

static void on_screen_gesture(lv_event_t *e) {
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) return;
    lv_indev_reset(indev, NULL);
    lv_indev_wait_release(indev);

    switch (lv_indev_get_gesture_dir(indev)) {
        case LV_DIR_LEFT:   handle_move(DIR_LEFT); break;
        case LV_DIR_RIGHT:  handle_move(DIR_RIGHT); break;
        case LV_DIR_TOP:    handle_move(DIR_UP); break;
        case LV_DIR_BOTTOM: handle_move(DIR_DOWN); break;
        default: break;
    }
}

// 盖板虚拟按键调度
static void async_move_cb(void *arg) {
    dir_t dir = (dir_t)(intptr_t)arg;
    handle_move(dir);
}

// vk_home 统一返回键：含结算弹窗在内的任意状态均直接安全退出应用回桌面
static int game2048_on_vk_key(const char *key) {
    if (key == NULL || !s_alive) return 0;
    if (strcmp(key, "vk_prev") == 0) {
        dynapp_lv_async(async_move_cb, (void *)(intptr_t)DIR_LEFT);
        return 1;
    }
    if (strcmp(key, "vk_next") == 0) {
        dynapp_lv_async(async_move_cb, (void *)(intptr_t)DIR_RIGHT);
        return 1;
    }
    if (strcmp(key, "vk_home") == 0 || strcmp(key, "home") == 0 || strcmp(key, "prev") == 0) {
        save_best_score();
        dynapp_screen_go_home();
        return 1;
    }
    return 0;
}

static void game2048_screen_delete_cb(lv_event_t *e) {
    s_alive = 0;
    save_best_score();
    s_modal_mask = NULL;
    s_scr = NULL;
    s_board_obj = NULL;
    s_score_lbl = NULL;
    s_best_lbl = NULL;
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, lv_coord_t w, lv_coord_t h,
                          lv_event_cb_t cb, void *user_data) {
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    const lv_font_t *f = fontpack_lv_font_ui();
    if (f != NULL) lv_obj_set_style_text_font(lbl, f, 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

static lv_obj_t *make_score_box(lv_obj_t *parent, const char *title, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h) {
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_border_color(box, lv_color_black(), 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_bg_color(box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *t_lbl = lv_label_create(box);
    lv_label_set_text(t_lbl, title);
    const lv_font_t *f18 = fontpack_lv_font_get(18, 2);
    if (f18 == NULL) f18 = fontpack_lv_font_ui();
    if (f18 != NULL) lv_obj_set_style_text_font(t_lbl, f18, 0);
    lv_obj_set_style_text_color(t_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_align(t_lbl, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t *v_lbl = lv_label_create(box);
    lv_label_set_text(v_lbl, "0");
    const lv_font_t *f24 = fontpack_lv_font_get(24, 2);
    if (f24 == NULL) f24 = fontpack_lv_font_ui();
    if (f24 != NULL) lv_obj_set_style_text_font(v_lbl, f24, 0);
    lv_obj_set_style_text_color(v_lbl, lv_color_black(), 0);
    lv_obj_align(v_lbl, LV_ALIGN_BOTTOM_MID, 0, -4);

    return v_lbl;
}

// 屏幕创建
static void *game2048_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 800);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_scr = scr;
    s_alive = 1;
    s_modal_mask = NULL;

    lv_obj_add_event_cb(scr, game2048_screen_delete_cb, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(scr, on_screen_gesture, LV_EVENT_GESTURE, NULL);

    // 1. 系统顶部状态栏
    dynapp_screen_status_bar(scr);

    // 2. 挂接虚拟按键
    dynapp_vk_attach(scr, "game2048", game2048_create, game2048_on_vk_key);

    // 3. 读取存档最高分
    load_best_score();
    core_reset();
    s_move_count = 0;
    s_last_full_refresh = 0;

    // 4. 标题与分数
    lv_obj_t *title_lbl = lv_label_create(scr);
    lv_label_set_text(title_lbl, "2048");
    const lv_font_t *f34 = fontpack_lv_font_get(34, 2);
    if (f34 == NULL) f34 = fontpack_lv_font_ui();
    if (f34 != NULL) lv_obj_set_style_text_font(title_lbl, f34, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_black(), 0);
    lv_obj_set_pos(title_lbl, 24, 46);

    lv_obj_t *sub_lbl = lv_label_create(scr);
    lv_label_set_text(sub_lbl, "滑屏合并，达成2048");
    const lv_font_t *f18 = fontpack_lv_font_get(18, 2);
    if (f18 == NULL) f18 = fontpack_lv_font_ui();
    if (f18 != NULL) lv_obj_set_style_text_font(sub_lbl, f18, 0);
    lv_obj_set_style_text_color(sub_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_pos(sub_lbl, 24, 82);

    s_score_lbl = make_score_box(scr, "得分", 260, 44, 94, 56);
    s_best_lbl = make_score_box(scr, "最高", 362, 44, 94, 56);

    // 5. 棋盘
    s_board_obj = lv_obj_create(scr);
    lv_obj_remove_style_all(s_board_obj);
    lv_obj_set_pos(s_board_obj, 20, 114);
    lv_obj_set_size(s_board_obj, BOARD_SIZE, BOARD_SIZE);
    lv_obj_set_style_bg_color(s_board_obj, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_board_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_board_obj, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_board_obj, 3, 0);
    lv_obj_set_style_radius(s_board_obj, 8, 0);
    lv_obj_clear_flag(s_board_obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_board_obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_board_obj, on_board_touch, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_board_obj, on_board_touch, LV_EVENT_RELEASED, NULL);

    memset(s_rendered, 0xFF, sizeof(s_rendered));
    for (int r = 0; r < GRID_SIZE; ++r) {
        for (int c = 0; c < GRID_SIZE; ++c) {
            lv_obj_t *cell = lv_obj_create(s_board_obj);
            lv_obj_remove_style_all(cell);
            lv_obj_set_size(cell, CELL_SIZE, CELL_SIZE);
            lv_coord_t px = (lv_coord_t)(CELL_PAD + c * (CELL_SIZE + CELL_GAP));
            lv_coord_t py = (lv_coord_t)(CELL_PAD + r * (CELL_SIZE + CELL_GAP));
            lv_obj_set_pos(cell, px, py);
            lv_obj_set_style_radius(cell, 6, 0);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

            lv_obj_t *lbl = lv_label_create(cell);
            lv_label_set_text(lbl, "");
            lv_obj_center(lbl);
            lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

            s_cells[r][c] = cell;
            s_cell_lbls[r][c] = lbl;
        }
    }

    // 6. 底部按键行
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 436, 56);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    make_btn(row, "撤销", 136, 56, on_btn_clicked, (void *)0);
    make_btn(row, "重开", 136, 56, on_btn_clicked, (void *)1);
    make_btn(row, "退出", 136, 56, on_btn_clicked, (void *)2);

    // 7. 初始刷新
    sync_best_label();
    redraw_board();

    return scr;
}

// 动态应用入口
void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "game2048";
    out->name = "2048";
    out->icon = "icon.png";
    out->create = game2048_create;
}
