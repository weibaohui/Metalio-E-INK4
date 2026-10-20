// tools/minesweeper_app/main/minesweeper_main.c
// 扫雷 动态应用：回合制、一步一刷，适配墨水屏低刷新率特性。
//
// 1bpp 单色屏的设计取舍：
//   - 数字没有颜色可用（1~8 在经典扫雷里靠红蓝绿区分），改成
//     「数字 + 下方点数」双编码：点阵像扑克牌点数，数出来就知道
//   - 未翻开的格子用 2px 边框、已翻开的去掉边框（墨水屏上"凹陷"效果）
//   - 旗子和雷都用几何图形画，不依赖字体里有没有对应符号
//   - 翻开一片空白区会一次改很多格子 → 只做一次局刷，不逐格刷
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "esp_random.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

#define N 9
#define CELL 50
#define BOARD (N * CELL)
#define MAX_DOTS 8

// ---- 难度：只改雷数，格子布局保持 9x9（和数独一致）----
typedef struct {
    const char *name;
    int mines;
} Difficulty;

static const Difficulty kDifficulties[] = {
    {"简单", 10},
    {"中等", 15},
    {"困难", 20},
};
#define DIFF_COUNT ((int)(sizeof(kDifficulties) / sizeof(kDifficulties[0])))

typedef struct {
    uint8_t mine;    // 1=雷
    uint8_t open;    // 1=已翻开
    uint8_t flag;    // 1=插旗
    uint8_t around;  // 周围雷数
} Cell;

static Cell s_cells[N][N];
static int s_diff_idx = 0;
static int s_flags = 0;
static int s_revealed = 0;   // 已翻开的非雷格数
static int s_dead = 0;       // 1=踩雷了
static int s_won = 0;
static int s_flag_mode = 0;  // 插旗模式：点格子插旗而不是翻开
static int s_first_click = 1;

// ---- 绘制对象 ----
static lv_obj_t *s_cell_obj[N][N];
static lv_obj_t *s_num_lbl[N][N];
static lv_obj_t *s_dots[N][N][MAX_DOTS];
static int s_dots_made[N][N];
static lv_obj_t *s_flag_obj[N][N];
static lv_obj_t *s_mine_obj[N][N];

static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_info_lbl = NULL;
static int s_move_count = 0;
static int s_last_full_refresh = 0;
#define FULL_REFRESH_EVERY 12

// ---- 字体 ----
static const lv_font_t *ui_font(void) {
    const lv_font_t *f = fontpack_lv_font_ui();
    return f != NULL ? f : fontpack_lv_font_get(25, 2);
}
static const lv_font_t *title_font(void) {
    const lv_font_t *f = fontpack_lv_font_get(30, 2);
    return f != NULL ? f : ui_font();
}

// ================= 棋盘逻辑 =================
static int mine_total(void) { return kDifficulties[s_diff_idx].mines; }

static void compute_around(void) {
    for (int r = 0; r < N; ++r) {
        for (int c = 0; c < N; ++c) {
            int n = 0;
            for (int dr = -1; dr <= 1; ++dr) {
                for (int dc = -1; dc <= 1; ++dc) {
                    if (dr == 0 && dc == 0) continue;
                    const int rr = r + dr, cc = c + dc;
                    if (rr < 0 || rr >= N || cc < 0 || cc >= N) continue;
                    if (s_cells[rr][cc].mine) n++;
                }
            }
            s_cells[r][c].around = (uint8_t)n;
        }
    }
}

static void place_mines(int skip_r, int skip_c) {
    memset(s_cells, 0, sizeof(s_cells));
    int placed = 0;
    while (placed < mine_total()) {
        const int idx = (int)(esp_random() % (uint32_t)(N * N));
        const int r = idx / N, c = idx % N;
        if (r == skip_r && c == skip_c) continue;  // 首点不踩雷
        if (s_cells[r][c].mine) continue;
        s_cells[r][c].mine = 1;
        placed++;
    }
    compute_around();
}

static void reset_board(int diff_idx) {
    s_diff_idx = diff_idx;
    place_mines(-1, -1);
    s_flags = 0;
    s_revealed = 0;
    s_dead = 0;
    s_won = 0;
    s_first_click = 1;
    s_move_count = 0;
    s_last_full_refresh = 0;
}

// 统计周围雷数（已插旗 + 实际雷）
static int flag_count_around(int r, int c) {
    int n = 0;
    for (int dr = -1; dr <= 1; ++dr) {
        for (int dc = -1; dc <= 1; ++dc) {
            if (dr == 0 && dc == 0) continue;
            const int rr = r + dr, cc = c + dc;
            if (rr < 0 || rr >= N || cc < 0 || cc >= N) continue;
            if (s_cells[rr][cc].flag) n++;
        }
    }
    return n;
}

static void render_all(void);
static void check_win(void);

static void reveal_flood(int r, int c) {
    if (r < 0 || r >= N || c < 0 || c >= N) return;
    if (s_cells[r][c].open || s_cells[r][c].flag) return;
    if (s_cells[r][c].mine) return;
    s_cells[r][c].open = 1;
    s_revealed++;
    if (s_cells[r][c].around == 0) {
        for (int dr = -1; dr <= 1; ++dr) {
            for (int dc = -1; dc <= 1; ++dc) {
                if (dr == 0 && dc == 0) continue;
                reveal_flood(r + dr, c + dc);
            }
        }
    }
}

static void reveal_at(int r, int c) {
    if (s_dead || s_won) return;
    if (r < 0 || r >= N || c < 0 || c >= N) return;
    if (s_cells[r][c].flag) return;

    // 首点：把雷挪走，保证第一次不会踩雷
    if (s_first_click) {
        s_first_click = 0;
        place_mines(r, c);
    }

    if (s_cells[r][c].mine) {
        s_dead = 1;
        s_cells[r][c].open = 1;
        // 翻开所有雷
        for (int rr = 0; rr < N; ++rr) {
            for (int cc = 0; cc < N; ++cc) {
                if (s_cells[rr][cc].mine) s_cells[rr][cc].open = 1;
            }
        }
        render_all();
        dynapp_modal_alert("踩雷了", "这局输了，点「新游戏」再来一次。", "确定");
        return;
    }

    reveal_flood(r, c);
    s_move_count++;
    render_all();
    check_win();
}

// 点已翻开的数字格：周围旗数匹配时把其余空格翻开
static void chord_at(int r, int c) {
    if (s_dead || s_won) return;
    if (!s_cells[r][c].open || s_cells[r][c].around == 0) return;
    if (flag_count_around(r, c) != s_cells[r][c].around) return;
    for (int dr = -1; dr <= 1; ++dr) {
        for (int dc = -1; dc <= 1; ++dc) {
            if (dr == 0 && dc == 0) continue;
            const int rr = r + dr, cc = c + dc;
            if (rr < 0 || rr >= N || cc < 0 || cc >= N) continue;
            if (s_cells[rr][cc].flag || s_cells[rr][cc].open) continue;
            if (s_cells[rr][cc].mine) {
                reveal_at(rr, cc);
                return;
            }
            reveal_flood(rr, cc);
        }
    }
    s_move_count++;
    render_all();
    check_win();
}

static void toggle_flag(int r, int c) {
    if (s_dead || s_won) return;
    if (r < 0 || r >= N || c < 0 || c >= N) return;
    if (s_cells[r][c].open) return;
    if (s_cells[r][c].flag) {
        s_cells[r][c].flag = 0;
        s_flags--;
    } else {
        s_cells[r][c].flag = 1;
        s_flags++;
    }
    s_move_count++;
    render_all();
}

// ================= 绘制 =================
static void make_dots(int r, int c, int k) {
    // 点阵：≤4 个一行，>4 分两行（像扑克牌点数）
    const int rows = (k > 4) ? 2 : 1;
    const int per_row = (k > 4) ? 4 : k;
    const int extra = (k > 4) ? (k - 4) : 0;
    // 第 0 行放 extra 个(没有则 0)，第 1 行放 per_row 个(单行时为 0)
    const int counts[2] = { (k > 4) ? extra : k, (k > 4) ? per_row : 0 };
    for (int ri = 0; ri < rows; ++ri) {
        const int cnt = counts[ri];
        if (cnt <= 0) continue;
        const int total_w = cnt * 6 - 2;  // 点 4px + 间隙 2px
        int x = 25 - total_w / 2;        // 相对格子中心
        const int y = 34 + ri * 8;       // 数字占上半，点阵排下半
        for (int i = 0; i < cnt; ++i) {
            const int idx = s_dots_made[r][c];
            if (idx >= MAX_DOTS) break;
            if (s_dots[r][c][idx] == NULL) {
                lv_obj_t *d = lv_obj_create(s_cell_obj[r][c]);
                lv_obj_remove_style_all(d);
                lv_obj_set_size(d, 4, 4);
                lv_obj_set_style_bg_color(d, lv_color_black(), 0);
                lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
                lv_obj_set_pos(d, x, y);
                lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
                s_dots[r][c][idx] = d;
                s_dots_made[r][c] = idx + 1;
            } else {
                lv_obj_set_pos(s_dots[r][c][idx], x, y);
                lv_obj_clear_flag(s_dots[r][c][idx], LV_OBJ_FLAG_HIDDEN);
            }
            x += 6;
        }
    }
    // 多余的点藏起来
    int used = 0;
    for (int ri = 0; ri < rows; ++ri) used += counts[ri];
    for (int i = used; i < s_dots_made[r][c]; ++i) {
        lv_obj_add_flag(s_dots[r][c][i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void render_cell(int r, int c) {
    lv_obj_t *cell = s_cell_obj[r][c];
    if (cell == NULL) return;
    const Cell *cc = &s_cells[r][c];

    // 未翻开的格子用 2px 边框（墨水屏上表示"还没揭"），翻开的去掉边框
    if (cc->open) {
        lv_obj_set_style_border_side(cell, LV_BORDER_SIDE_NONE, 0);
    } else {
        lv_obj_set_style_border_side(cell, LV_BORDER_SIDE_FULL, 0);
        lv_obj_set_style_border_width(cell, 2, 0);
        lv_obj_set_style_border_color(cell, lv_color_black(), 0);
    }

    // 数字
    if (s_num_lbl[r][c] != NULL) {
        if (cc->open && !cc->mine && cc->around > 0) {
            char buf[4] = {0};
            snprintf(buf, sizeof(buf), "%u", cc->around);
            lv_label_set_text(s_num_lbl[r][c], buf);
            lv_obj_clear_flag(s_num_lbl[r][c], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_label_set_text(s_num_lbl[r][c], "");
        }
    }

    // 点阵
    if (cc->open && !cc->mine && cc->around > 0) {
        make_dots(r, c, cc->around);
    } else {
        for (int i = 0; i < s_dots_made[r][c]; ++i) {
            lv_obj_add_flag(s_dots[r][c][i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    // 旗子：几何画的三角旗 + 旗杆（不依赖字体符号）
    if (s_flag_obj[r][c] != NULL) {
        if (cc->flag && !cc->open) {
            lv_obj_clear_flag(s_flag_obj[r][c], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_flag_obj[r][c], LV_OBJ_FLAG_HIDDEN);
        }
    }

    // 雷：中心方块 + 四向短线（爆炸星）
    if (s_mine_obj[r][c] != NULL) {
        if (cc->open && cc->mine) {
            lv_obj_clear_flag(s_mine_obj[r][c], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_mine_obj[r][c], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void render_all(void) {
    for (int r = 0; r < N; ++r) {
        for (int c = 0; c < N; ++c) render_cell(r, c);
    }
    if (s_info_lbl != NULL) {
        char buf[96];
        snprintf(buf, sizeof(buf), "%s · 雷 %d · 旗 %d",
                 kDifficulties[s_diff_idx].name, mine_total(), s_flags);
        lv_label_set_text(s_info_lbl, buf);
    }
}

static void check_win(void) {
    if (s_dead || s_won) return;
    const int total = N * N - mine_total();
    if (s_revealed < total) return;
    s_won = 1;
    render_all();
    dynapp_modal_alert("扫雷成功！", "所有雷都找出来了，厉害。", "确定");
}

// ================= 持久化 =================
static void save_game(void) {
    FILE *fp = (FILE *)dynapp_fopen_data("minesweeper.txt", "w");
    if (fp == NULL) return;
    fprintf(fp, "MINE1 %d %d %d %d\n", s_diff_idx, s_flags, s_revealed, s_dead);
    for (int r = 0; r < N; ++r) {
        for (int c = 0; c < N; ++c) {
            const Cell *cc = &s_cells[r][c];
            const int v = (cc->mine ? 1 : 0) | (cc->open ? 2 : 0) | (cc->flag ? 4 : 0);
            fputc('0' + v, fp);
        }
    }
    fputc('\n', fp);
    fclose(fp);
}

static int load_game(void) {
    FILE *fp = (FILE *)dynapp_fopen_data("minesweeper.txt", "r");
    if (fp == NULL) return 0;
    char magic[16] = {0};
    int diff = 0, flags = 0, revealed = 0, dead = 0;
    if (fscanf(fp, "%15s", magic) != 1 || strcmp(magic, "MINE1") != 0) { fclose(fp); return 0; }
    if (fscanf(fp, "%d %d %d %d", &diff, &flags, &revealed, &dead) != 4) { fclose(fp); return 0; }
    if (diff < 0 || diff >= DIFF_COUNT) { fclose(fp); return 0; }
    for (int r = 0; r < N; ++r) {
        for (int c = 0; c < N; ++c) {
            int v = 0;
            if (fscanf(fp, "%1d", &v) != 1) { fclose(fp); return 0; }
            s_cells[r][c].mine = (uint8_t)(v & 1);
            s_cells[r][c].open = (uint8_t)((v >> 1) & 1);
            s_cells[r][c].flag = (uint8_t)((v >> 2) & 1);
        }
    }
    fclose(fp);
    s_diff_idx = diff;
    s_flags = flags;
    s_revealed = revealed;
    s_dead = dead;
    s_won = 0;
    s_first_click = 0;
    s_move_count = 0;
    s_last_full_refresh = 0;
    compute_around();
    return 1;
}

// ================= 交互 =================
static void on_cell_clicked(lv_event_t *e) {
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const int r = idx / N, c = idx % N;
    if (s_flag_mode) {
        toggle_flag(r, c);
        return;
    }
    if (s_cells[r][c].open && !s_cells[r][c].mine) {
        chord_at(r, c);
    } else {
        reveal_at(r, c);
    }
    save_game();
    if (s_move_count - s_last_full_refresh >= FULL_REFRESH_EVERY) {
        dynapp_epd_full_refresh();
        s_last_full_refresh = s_move_count;
    }
}

static void on_cell_long_pressed(lv_event_t *e) {
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const int r = idx / N, c = idx % N;
    toggle_flag(r, c);
    save_game();
}

static void on_new_game_clicked(lv_event_t *e) {
    (void)e;
    reset_board(s_diff_idx);
    render_all();
    save_game();
    dynapp_toast_show("新游戏", 1200);
    dynapp_epd_full_refresh();
}

static void on_diff_clicked(lv_event_t *e) {
    (void)e;
    reset_board((s_diff_idx + 1) % DIFF_COUNT);
    render_all();
    save_game();
    dynapp_toast_show(kDifficulties[s_diff_idx].name, 1200);
    dynapp_epd_full_refresh();
}

static void on_flag_mode_clicked(lv_event_t *e) {
    (void)e;
    s_flag_mode = !s_flag_mode;
    dynapp_toast_show(s_flag_mode ? "插旗模式：点格子放旗" : "已退出插旗模式", 1500);
    dynapp_epd_full_refresh();
}

static int minesweeper_on_vk_key(const char* key) {
    if (key == NULL) return 0;
    if (strcmp(key, "vk_home") == 0 || strcmp(key, "esc") == 0 || strcmp(key, "back") == 0) {
        dynapp_screen_go_home();
        return 1;
    }
    return 0;
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
    memset(s_cell_obj, 0, sizeof(s_cell_obj));
    memset(s_num_lbl, 0, sizeof(s_num_lbl));
    memset(s_dots, 0, sizeof(s_dots));
    memset(s_dots_made, 0, sizeof(s_dots_made));
    memset(s_flag_obj, 0, sizeof(s_flag_obj));
    memset(s_mine_obj, 0, sizeof(s_mine_obj));
}

static void *minesweeper_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    s_scr = scr;
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 800);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_screen_deleted, LV_EVENT_DELETE, NULL);

    dynapp_screen_status_bar(scr);
    dynapp_vk_attach(scr, "minesweeper", minesweeper_create, minesweeper_on_vk_key);

    if (!load_game()) reset_board(0);

    // 1. 标题行
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "扫雷");
    const lv_font_t *f_title = title_font();
    if (f_title != NULL) lv_obj_set_style_text_font(title, f_title, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_set_pos(title, 16, 48);

    s_info_lbl = lv_label_create(scr);
    const lv_font_t *f_ui = ui_font();
    if (f_ui != NULL) lv_obj_set_style_text_font(s_info_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_info_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_align(s_info_lbl, LV_ALIGN_TOP_RIGHT, -16, 56);

    // 2. 棋盘
    lv_obj_t *board = lv_obj_create(scr);
    lv_obj_remove_style_all(board);
    lv_obj_set_pos(board, (480 - BOARD) / 2, 92);
    lv_obj_set_size(board, BOARD, BOARD);
    lv_obj_clear_flag(board, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    for (int r = 0; r < N; ++r) {
        for (int c = 0; c < N; ++c) {
            lv_obj_t *cell = lv_obj_create(board);
            lv_obj_remove_style_all(cell);
            lv_obj_set_pos(cell, c * CELL, r * CELL);
            lv_obj_set_size(cell, CELL, CELL);
            lv_obj_set_style_bg_color(cell, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(cell, on_cell_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)(r * N + c));
            lv_obj_add_event_cb(cell, on_cell_long_pressed, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)(r * N + c));
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
            s_cell_obj[r][c] = cell;

            // 数字标签（偏上）
            lv_obj_t *num = lv_label_create(cell);
            lv_label_set_text(num, "");
            if (f_ui != NULL) lv_obj_set_style_text_font(num, f_ui, 0);
            lv_obj_set_style_text_color(num, lv_color_black(), 0);
            lv_obj_align(num, LV_ALIGN_TOP_MID, 0, 1);
            lv_obj_clear_flag(num, LV_OBJ_FLAG_CLICKABLE);
            s_num_lbl[r][c] = num;

            // 旗子：旗杆 + 三角旗
            lv_obj_t *flag = lv_obj_create(cell);
            lv_obj_remove_style_all(flag);
            lv_obj_set_size(flag, 16, 20);
            lv_obj_set_style_bg_opa(flag, LV_OPA_TRANSP, 0);
            lv_obj_set_pos(flag, 17, 15);
            lv_obj_clear_flag(flag, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
            {
                lv_obj_t *pole = lv_obj_create(flag);
                lv_obj_remove_style_all(pole);
                lv_obj_set_pos(pole, 2, 0);
                lv_obj_set_size(pole, 2, 18);
                lv_obj_set_style_bg_color(pole, lv_color_black(), 0);
                lv_obj_set_style_bg_opa(pole, LV_OPA_COVER, 0);
                lv_obj_clear_flag(pole, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

                lv_obj_t *cloth = lv_obj_create(flag);
                lv_obj_remove_style_all(cloth);
                lv_obj_set_pos(cloth, 4, 1);
                lv_obj_set_size(cloth, 11, 8);
                lv_obj_set_style_bg_color(cloth, lv_color_black(), 0);
                lv_obj_set_style_bg_opa(cloth, LV_OPA_COVER, 0);
                lv_obj_clear_flag(cloth, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
            }
            lv_obj_add_flag(flag, LV_OBJ_FLAG_HIDDEN);
            s_flag_obj[r][c] = flag;

            // 雷：中心方块 + 四向短线
            lv_obj_t *mine = lv_obj_create(cell);
            lv_obj_remove_style_all(mine);
            lv_obj_set_size(mine, 22, 22);
            lv_obj_set_style_bg_opa(mine, LV_OPA_TRANSP, 0);
            lv_obj_set_pos(mine, 14, 14);
            lv_obj_clear_flag(mine, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
            {
                lv_obj_t *core = lv_obj_create(mine);
                lv_obj_remove_style_all(core);
                lv_obj_set_pos(core, 7, 7);
                lv_obj_set_size(core, 8, 8);
                lv_obj_set_style_bg_color(core, lv_color_black(), 0);
                lv_obj_set_style_bg_opa(core, LV_OPA_COVER, 0);
                lv_obj_clear_flag(core, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
                // 四向尖刺（横竖各两条）+ 四角小点 = 八角雷形。
                // 不用 transform_rotation：固件没调用该 API，会被 --gc-sections 裁掉
                // 而进不了 SDK 导出表，动态应用调用会加载失败
                const int arm_w = 4, arm_len = 12;
                const int ax = 11 - arm_w / 2, ay = 11 - arm_len / 2;
                const int arms[4][2] = {
                    {ax, ay},                      // 上
                    {ax, 11 + arm_len / 2},         // 下
                    {11 - arm_len / 2, ay},         // 左
                    {11 + arm_len / 2, ay},         // 右
                };
                for (int i = 0; i < 4; ++i) {
                    lv_obj_t *spike = lv_obj_create(mine);
                    lv_obj_remove_style_all(spike);
                    const bool vertical = (i < 2);
                    lv_obj_set_size(spike, vertical ? arm_w : arm_len, vertical ? arm_len : arm_w);
                    lv_obj_set_style_bg_color(spike, lv_color_black(), 0);
                    lv_obj_set_style_bg_opa(spike, LV_OPA_COVER, 0);
                    lv_obj_set_pos(spike, arms[i][0], arms[i][1]);
                    lv_obj_clear_flag(spike, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
                }
                for (int i = 0; i < 4; ++i) {
                    lv_obj_t *dot = lv_obj_create(mine);
                    lv_obj_remove_style_all(dot);
                    lv_obj_set_size(dot, 4, 4);
                    lv_obj_set_style_bg_color(dot, lv_color_black(), 0);
                    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
                    lv_obj_set_pos(dot, (i & 1) ? 2 : 16, (i & 2) ? 2 : 16);
                    lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
                }
            }
            lv_obj_add_flag(mine, LV_OBJ_FLAG_HIDDEN);
            s_mine_obj[r][c] = mine;
        }
    }

    // 宫格分隔线（和数独一致）
    for (int k = 0; k <= N; ++k) {
        const int thick = (k % 3 == 0);
        const lv_coord_t lw = thick ? 3 : 1;
        const lv_coord_t p = k * CELL;
        for (int axis = 0; axis < 2; ++axis) {
            lv_obj_t *line = lv_obj_create(board);
            lv_obj_remove_style_all(line);
            if (axis == 0) {
                lv_obj_set_pos(line, p - (thick ? 1 : 0), 0);
                lv_obj_set_size(line, lw, BOARD);
            } else {
                lv_obj_set_pos(line, 0, p - (thick ? 1 : 0));
                lv_obj_set_size(line, BOARD, lw);
            }
            lv_obj_set_style_bg_color(line, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
            lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        }
    }

    // 3. 操作提示
    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "点格子翻开 · 长按插旗");
    if (f_ui != NULL) lv_obj_set_style_text_font(hint, f_ui, 0);
    lv_obj_set_style_text_color(hint, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_pos(hint, 16, 552);

    // 4. 功能按钮
    const lv_coord_t bw = 144, bh = 56, bgap = 8;
    const lv_coord_t by = 596;
    lv_obj_t *b1 = make_btn(scr, "新游戏", bw, bh, on_new_game_clicked, NULL);
    lv_obj_set_pos(b1, 16, by);
    lv_obj_t *b2 = make_btn(scr, "难度", bw, bh, on_diff_clicked, NULL);
    lv_obj_set_pos(b2, 16 + bw + bgap, by);
    lv_obj_t *b3 = make_btn(scr, "插旗模式", bw, bh, on_flag_mode_clicked, NULL);
    lv_obj_set_pos(b3, 16 + 2 * (bw + bgap), by);

    render_all();
    return scr;
}

void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "minesweeper";
    out->name = "扫雷";
    out->icon = "icon.png";
    out->create = minesweeper_create;
}
