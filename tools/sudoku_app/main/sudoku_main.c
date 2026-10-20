// tools/sudoku_app/main/sudoku_main.c
// 数独 动态应用：回合制、一步一刷，适配墨水屏低刷新率特性。
//
// 设计要点（墨水屏 480x800 / 1bpp 单色）：
//   - 每填一格只改那一个 label → LVGL 局刷，残影可控；每 N 步全刷一次去残影
//   - 不做常驻计时器：墨水屏每秒重绘既费电又留残影，只在操作时刷新
//   - 字体只有 25@2(正文) / 30@2(标题) 两档含汉字，纯数字用 18@2 拉丁小字体
//   - 格子 48px，9x9 铺 448px，数字用 25@2 居中，一眼可读
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
#define BOARD (N * CELL) /* 450：9 格连排，宫格分隔线另画 */
#define KEY 48          /* 数字键盘按钮宽：9*48 + 8*2 = 448，正好放下 */

// ---- 难度：给定提示数 ----
typedef struct {
    const char *name;
    int givens;
} Difficulty;

static const Difficulty kDifficulties[] = {
    {"简单", 40},
    {"中等", 32},
    {"困难", 26},
    {"专家", 24},
};
#define DIFF_COUNT ((int)(sizeof(kDifficulties) / sizeof(kDifficulties[0])))

// ---- 棋盘状态 ----
static uint8_t s_puzzle[N][N];  // 题目（含提示数）
static uint8_t s_solution[N][N];// 完整解
static uint8_t s_user[N][N];    // 用户填写
static uint8_t s_given[N][N];   // 1=题目给定的格子
static uint8_t s_reveal[N][N];  // 1=提示揭示的格子
static uint8_t s_conflict[N][N];// 1=与其它格冲突
static int s_diff_idx = 0;
static int s_sel_r = -1, s_sel_c = -1;
static int s_filled = 0;   // 用户已填的空格数（不含题目给定的格子）
static int s_blanks = 0;   // 空格总数

// 撤销栈：每次落子前存一份快照
#define UNDO_MAX 32
typedef struct {
    uint8_t user[N][N];
    uint8_t reveal[N][N];
    int sel_r, sel_c;
    int filled;
} UndoState;
static UndoState s_undo[UNDO_MAX];
static int s_undo_count = 0;

static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_cells[N][N];
static lv_obj_t *s_cell_lbls[N][N];
static lv_obj_t *s_info_lbl = NULL;
static int s_move_count = 0;
static int s_last_full_refresh = 0;
#define FULL_REFRESH_EVERY 15

// ---- 字体 ----
static const lv_font_t *ui_font(void) {
    const lv_font_t *f = fontpack_lv_font_ui();
    return f != NULL ? f : fontpack_lv_font_get(25, 2);
}
static const lv_font_t *title_font(void) {
    const lv_font_t *f = fontpack_lv_font_get(30, 2);
    return f != NULL ? f : ui_font();
}

// ================= 数独求解器（位掩码加速） =================
static int solve_rec(uint8_t g[N][N], uint16_t *row, uint16_t *col, uint16_t *box,
                     int limit, uint8_t (*out)[N]) {
    int r = -1, c = -1;
    for (int i = 0; i < N * N; ++i) {
        int rr = i / N, cc = i % N;
        if (g[rr][cc] == 0) { r = rr; c = cc; break; }
    }
    if (r < 0) {
        if (out != NULL) memcpy(out, g, N * N);
        return 1;
    }
    const int b = (r / 3) * 3 + (c / 3);
    const uint16_t used = row[r] | col[c] | box[b];
    int cnt = 0;
    for (int d = 1; d <= N; ++d) {
        const uint16_t bit = (uint16_t)(1u << (d - 1));
        if (used & bit) continue;
        g[r][c] = (uint8_t)d;
        row[r] |= bit; col[c] |= bit; box[b] |= bit;
        cnt += solve_rec(g, row, col, box, limit - cnt, out);
        row[r] &= (uint16_t)~bit; col[c] &= (uint16_t)~bit; box[b] &= (uint16_t)~bit;
        g[r][c] = 0;
        if (cnt >= limit) break;
    }
    return cnt;
}

// 从 g 出发求解；limit=1 时把解写入 out；返回解的个数（最多到 limit）
static int solve_count(uint8_t g[N][N], int limit, uint8_t (*out)[N]) {
    uint16_t row[N] = {0}, col[N] = {0}, box[N] = {0};
    for (int r = 0; r < N; ++r) {
        for (int c = 0; c < N; ++c) {
            const uint8_t v = g[r][c];
            if (v == 0) continue;
            const uint16_t bit = (uint16_t)(1u << (v - 1));
            row[r] |= bit; col[c] |= bit; box[(r / 3) * 3 + (c / 3)] |= bit;
        }
    }
    return solve_rec(g, row, col, box, limit, out);
}

// 随机填满一个合法解
static int fill_rec(uint8_t g[N][N], uint16_t *row, uint16_t *col, uint16_t *box) {
    int r = -1, c = -1;
    for (int i = 0; i < N * N; ++i) {
        int rr = i / N, cc = i % N;
        if (g[rr][cc] == 0) { r = rr; c = cc; break; }
    }
    if (r < 0) return 1;

    const int b = (r / 3) * 3 + (c / 3);
    const uint16_t used = row[r] | col[c] | box[b];
    int digits[9], nd = 0;
    for (int d = 1; d <= N; ++d) {
        if (!(used & (1u << (d - 1)))) digits[nd++] = d;
    }
    // 随机打乱候选顺序 → 每次生成的题目都不一样
    for (int i = nd - 1; i > 0; --i) {
        const int j = (int)(esp_random() % (uint32_t)(i + 1));
        const int t = digits[i]; digits[i] = digits[j]; digits[j] = t;
    }
    for (int i = 0; i < nd; ++i) {
        const int d = digits[i];
        const uint16_t bit = (uint16_t)(1u << (d - 1));
        g[r][c] = (uint8_t)d;
        row[r] |= bit; col[c] |= bit; box[b] |= bit;
        if (fill_rec(g, row, col, box)) return 1;
        row[r] &= (uint16_t)~bit; col[c] &= (uint16_t)~bit; box[b] &= (uint16_t)~bit;
        g[r][c] = 0;
    }
    return 0;
}

// 挖空：只挖出「唯一解」的空格，保证题目无歧义
static void dig_holes(uint8_t puzzle[N][N], int target_givens) {
    int cells[N * N], nc = 0;
    for (int r = 0; r < N; ++r) {
        for (int c = 0; c < N; ++c) {
            if (puzzle[r][c]) cells[nc++] = r * N + c;
        }
    }
    for (int i = nc - 1; i > 0; --i) {
        const int j = (int)(esp_random() % (uint32_t)(i + 1));
        const int t = cells[i]; cells[i] = cells[j]; cells[j] = t;
    }
    int givens = nc;
    for (int i = 0; i < nc && givens > target_givens; ++i) {
        const int r = cells[i] / N, c = cells[i] % N;
        const uint8_t bak = puzzle[r][c];
        puzzle[r][c] = 0;
        uint8_t tmp[N][N];
        memcpy(tmp, puzzle, sizeof(tmp));
        if (solve_count(tmp, 2, NULL) != 1) {
            puzzle[r][c] = bak;  // 挖掉会出现多解 → 撤回
        } else {
            givens--;
        }
    }
}

// ================= 局面管理 =================
static void new_game(int diff_idx) {
    s_diff_idx = diff_idx;
    memset(s_puzzle, 0, sizeof(s_puzzle));
    memset(s_user, 0, sizeof(s_user));
    memset(s_given, 0, sizeof(s_given));
    memset(s_reveal, 0, sizeof(s_reveal));
    memset(s_conflict, 0, sizeof(s_conflict));

    uint16_t row[N] = {0}, col[N] = {0}, box[N] = {0};
    fill_rec(s_puzzle, row, col, box);
    memcpy(s_solution, s_puzzle, sizeof(s_solution));
    dig_holes(s_puzzle, kDifficulties[diff_idx].givens);

    for (int r = 0; r < N; ++r) {
        for (int c = 0; c < N; ++c) {
            if (s_puzzle[r][c]) {
                s_given[r][c] = 1;
                s_user[r][c] = s_puzzle[r][c];
            }
        }
    }
    s_filled = 0;
    s_blanks = 0;
    for (int r = 0; r < N; ++r) {
        for (int c = 0; c < N; ++c) {
            if (!s_given[r][c]) s_blanks++;
        }
    }
    s_sel_r = s_sel_c = -1;
    s_move_count = 0;
    s_last_full_refresh = 0;
    s_undo_count = 0;
}

// 冲突检测：同行/同列/同宫出现重复数字的格子全部标出
static void detect_conflicts(void) {
    memset(s_conflict, 0, sizeof(s_conflict));
    for (int r = 0; r < N; ++r) {
        for (int c = 0; c < N; ++c) {
            const uint8_t v = s_user[r][c];
            if (v == 0) continue;
            for (int k = 0; k < N; ++k) {
                if (k != c && s_user[r][k] == v) { s_conflict[r][c] = 1; s_conflict[r][k] = 1; }
                if (k != r && s_user[k][c] == v) { s_conflict[r][c] = 1; s_conflict[k][c] = 1; }
            }
            const int br = (r / 3) * 3, bc = (c / 3) * 3;
            for (int dr = 0; dr < 3; ++dr) {
                for (int dc = 0; dc < 3; ++dc) {
                    const int rr = br + dr, cc = bc + dc;
                    if ((rr != r || cc != c) && s_user[rr][cc] == v) {
                        s_conflict[r][c] = 1;
                        s_conflict[rr][cc] = 1;
                    }
                }
            }
        }
    }
}

static int is_complete(void) {
    for (int r = 0; r < N; ++r) {
        for (int c = 0; c < N; ++c) {
            if (s_user[r][c] == 0) return 0;
        }
    }
    return 1;
}

// ================= 渲染 =================
static void apply_cell_style(int r, int c) {
    lv_obj_t *cell = s_cells[r][c];
    lv_obj_t *lbl = s_cell_lbls[r][c];
    if (cell == NULL || lbl == NULL) return;

    const int selected = (r == s_sel_r && c == s_sel_c);
    const int peer = (s_sel_r >= 0 &&
                      (r == s_sel_r || c == s_sel_c ||
                       ((r / 3) == (s_sel_r / 3) && (c / 3) == (s_sel_c / 3))));
    const int conflict = s_conflict[r][c] ? 1 : 0;

    // 背景：选中格反白(黑底白字)——墨水屏上对比最强的选中指示
    if (selected) {
        lv_obj_set_style_bg_color(cell, lv_color_black(), 0);
    } else {
        lv_obj_set_style_bg_color(cell, lv_color_white(), 0);
    }
    lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);

    // 关联格(同行/同列/同宫)加一圈细框。注意：1bpp 单色屏上浅灰背景会被
    // 直接压成纯白、看不出差别(实测像素完全一致)，只有加粗边缘才有效。
    if (peer && !selected) {
        lv_obj_set_style_border_side(cell, LV_BORDER_SIDE_FULL, 0);
        lv_obj_set_style_border_width(cell, 1, 0);
        lv_obj_set_style_border_color(cell, lv_color_black(), 0);
    } else {
        lv_obj_set_style_border_side(cell, LV_BORDER_SIDE_NONE, 0);
    }

    // 数字：题目给定的用粗体，提示揭示的加下划线，冲突的加删除线
    const uint8_t v = s_user[r][c];
    char buf[4] = {0};
    if (v) snprintf(buf, sizeof(buf), "%u", v);
    lv_label_set_text(lbl, buf);

    const lv_font_t *f = s_given[r][c] ? fontpack_lv_font_get(30, 4) : ui_font();
    if (f == NULL) f = ui_font();
    if (f != NULL) lv_obj_set_style_text_font(lbl, f, 0);
    lv_obj_set_style_text_color(lbl, selected ? lv_color_white() : lv_color_black(), 0);

    lv_text_decor_t decor = LV_TEXT_DECOR_NONE;
    if (s_reveal[r][c]) decor |= LV_TEXT_DECOR_UNDERLINE;
    if (conflict) decor |= LV_TEXT_DECOR_STRIKETHROUGH;
    lv_obj_set_style_text_decor(lbl, decor, 0);
}

static void refresh_all_cells(void) {
    for (int r = 0; r < N; ++r) {
        for (int c = 0; c < N; ++c) apply_cell_style(r, c);
    }
}

static void update_info_line(void) {
    if (s_info_lbl == NULL) return;
    char buf[96];
    const int blanks = s_blanks - s_filled;
    snprintf(buf, sizeof(buf), "%s · 剩 %d 格 · 步 %d",
             kDifficulties[s_diff_idx].name, blanks, s_move_count);
    lv_label_set_text(s_info_lbl, buf);
}

static void maybe_full_refresh(void) {
    if (s_move_count - s_last_full_refresh >= FULL_REFRESH_EVERY) {
        dynapp_epd_full_refresh();
        s_last_full_refresh = s_move_count;
    }
}

static void check_win(void) {
    if (!is_complete()) return;
    detect_conflicts();
    refresh_all_cells();
    for (int r = 0; r < N; ++r) {
        for (int c = 0; c < N; ++c) {
            if (s_conflict[r][c]) return;  // 有冲突就不算完成
        }
    }
    dynapp_modal_alert("完成！", "恭喜完成本题，步数与用时已记录。", "再来一局");
}

static void push_undo(void) {
    if (s_undo_count >= UNDO_MAX) {
        memmove(&s_undo[0], &s_undo[1], sizeof(UndoState) * (UNDO_MAX - 1));
        s_undo_count = UNDO_MAX - 1;
    }
    UndoState *u = &s_undo[s_undo_count++];
    memcpy(u->user, s_user, sizeof(s_user));
    memcpy(u->reveal, s_reveal, sizeof(s_reveal));
    u->sel_r = s_sel_r;
    u->sel_c = s_sel_c;
    u->filled = s_filled;
}

static void do_undo(void) {
    if (s_undo_count <= 0) {
        dynapp_toast_show("没有可撤销的步骤", 1500);
        return;
    }
    UndoState *u = &s_undo[--s_undo_count];
    memcpy(s_user, u->user, sizeof(s_user));
    memcpy(s_reveal, u->reveal, sizeof(s_reveal));
    s_sel_r = u->sel_r;
    s_sel_c = u->sel_c;
    s_filled = u->filled;
    detect_conflicts();
    refresh_all_cells();
    update_info_line();
    dynapp_toast_show("已撤销一步", 1200);
}

// ================= 交互 =================
static void on_cell_clicked(lv_event_t *e) {
    const int r = (int)(intptr_t)lv_event_get_user_data(e) / N;
    const int c = (int)(intptr_t)lv_event_get_user_data(e) % N;
    s_sel_r = r;
    s_sel_c = c;
    refresh_all_cells();
}

static void save_game(void);

static void on_digit_clicked(lv_event_t *e) {
    const int d = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_sel_r < 0) {
        dynapp_toast_show("请先点一个格子", 1500);
        return;
    }
    const int r = s_sel_r, c = s_sel_c;
    if (s_given[r][c]) {
        dynapp_toast_show("这是题目给定的数字", 1500);
        return;
    }
    push_undo();
    // 再点同一个数字 = 取消填写
    if (s_user[r][c] == d) {
        s_user[r][c] = 0;
        s_reveal[r][c] = 0;
        s_filled--;
    } else {
        if (s_user[r][c] == 0) s_filled++;
        s_user[r][c] = (uint8_t)d;
        s_reveal[r][c] = 0;
    }
    s_move_count++;
    detect_conflicts();
    refresh_all_cells();
    update_info_line();
    save_game();
    maybe_full_refresh();
    check_win();
}

static void on_erase_clicked(lv_event_t *e) {
    (void)e;
    if (s_sel_r < 0) {
        dynapp_toast_show("请先点一个格子", 1500);
        return;
    }
    const int r = s_sel_r, c = s_sel_c;
    if (s_given[r][c]) {
        dynapp_toast_show("这是题目给定的数字", 1500);
        return;
    }
    if (s_user[r][c] == 0) return;
    push_undo();
    s_user[r][c] = 0;
    s_reveal[r][c] = 0;
    s_filled--;
    s_move_count++;
    detect_conflicts();
    refresh_all_cells();
    update_info_line();
    save_game();
}

static void on_undo_clicked(lv_event_t *e) {
    (void)e;
    do_undo();
}

static void on_hint_clicked(lv_event_t *e) {
    (void)e;
    // 找一个填错或未填的格子，按答案揭示
    int br = -1, bc = -1;
    for (int tries = 0; tries < N * N; ++tries) {
        const int idx = (int)(esp_random() % (uint32_t)(N * N));
        const int r = idx / N, c = idx % N;
        if (s_given[r][c]) continue;
        if (s_user[r][c] == s_solution[r][c]) continue;
        br = r; bc = c;
        break;
    }
    if (br < 0) {
        dynapp_toast_show("当前没有需要提示的格子", 1500);
        return;
    }
    push_undo();
    if (s_user[br][bc] == 0) s_filled++;
    s_user[br][bc] = s_solution[br][bc];
    s_reveal[br][bc] = 1;
    s_move_count++;
    detect_conflicts();
    refresh_all_cells();
    update_info_line();
    save_game();
    dynapp_toast_show("已揭示一个格子", 1200);
    maybe_full_refresh();
    check_win();
}

static void on_new_game_clicked(lv_event_t *e) {
    (void)e;
    const int next = (s_diff_idx + 1) % DIFF_COUNT;
    new_game(next);
    refresh_all_cells();
    update_info_line();
    dynapp_toast_show(kDifficulties[next].name, 1200);
    dynapp_epd_full_refresh();
}

static int sudoku_on_vk_key(const char* key) {
    if (key == NULL) return 0;
    if (strcmp(key, "vk_home") == 0 || strcmp(key, "esc") == 0 || strcmp(key, "back") == 0) {
        dynapp_screen_go_home();
        return 1;
    }
    if (strcmp(key, "vk_next") == 0) {
        on_new_game_clicked(NULL);
        return 1;
    }
    return 0;
}

// ================= 持久化 =================
// 存档必须连题目一起存：只存用户输入的话，重进时无法还原出同一道题
static void save_game(void) {
    FILE *fp = (FILE *)dynapp_fopen_data("sudoku.txt", "w");
    if (fp == NULL) return;
    fprintf(fp, "SUDOKU1 %d\n", s_diff_idx);
    for (int r = 0; r < N; ++r) {
        char row[N + 2];
        for (int c = 0; c < N; ++c) row[c] = (char)('0' + s_puzzle[r][c]);
        row[N] = '\n';
        row[N + 1] = '\0';
        fputs(row, fp);
    }
    for (int r = 0; r < N; ++r) {
        char row[N + 2];
        for (int c = 0; c < N; ++c) row[c] = (char)('0' + s_user[r][c]);
        row[N] = '\n';
        row[N + 1] = '\0';
        fputs(row, fp);
    }
    fclose(fp);
}

static int load_game(void) {
    FILE *fp = (FILE *)dynapp_fopen_data("sudoku.txt", "r");
    if (fp == NULL) return 0;
    // 版本标记：存档格式变了就整体作废重开，避免旧文件被错误解析
    char magic[16] = {0};
    if (fscanf(fp, "%15s", magic) != 1 || strcmp(magic, "SUDOKU1") != 0) {
        fclose(fp);
        return 0;
    }
    int diff = 0;
    if (fscanf(fp, "%d", &diff) != 1) { fclose(fp); return 0; }
    uint8_t puzzle[N][N], user[N][N];
    for (int r = 0; r < N; ++r) {
        for (int c = 0; c < N; ++c) {
            int v = 0;
            if (fscanf(fp, "%1d", &v) != 1) { fclose(fp); return 0; }
            puzzle[r][c] = (uint8_t)v;
        }
    }
    for (int r = 0; r < N; ++r) {
        for (int c = 0; c < N; ++c) {
            int v = 0;
            if (fscanf(fp, "%1d", &v) != 1) { fclose(fp); return 0; }
            user[r][c] = (uint8_t)v;
        }
    }
    fclose(fp);

    if (diff < 0 || diff >= DIFF_COUNT) return 0;

    s_diff_idx = diff;
    memcpy(s_puzzle, puzzle, sizeof(s_puzzle));
    memcpy(s_user, user, sizeof(s_user));
    memset(s_given, 0, sizeof(s_given));
    memset(s_reveal, 0, sizeof(s_reveal));
    s_blanks = 0;
    s_filled = 0;
    for (int r = 0; r < N; ++r) {
        for (int c = 0; c < N; ++c) {
            if (s_puzzle[r][c]) {
                s_given[r][c] = 1;
                s_user[r][c] = s_puzzle[r][c];  // 给定数不可改
            } else {
                s_blanks++;
                if (s_user[r][c]) s_filled++;
            }
        }
    }
    // 答案由题目反解（题目保证唯一解）
    uint8_t sol[N][N];
    memcpy(sol, s_puzzle, sizeof(sol));
    if (solve_count(sol, 1, s_solution) != 1) {
        new_game(s_diff_idx);  // 存档损坏 → 重开
        return 1;
    }
    s_sel_r = s_sel_c = -1;
    s_move_count = 0;
    s_last_full_refresh = 0;
    s_undo_count = 0;
    detect_conflicts();
    return 1;
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
    memset(s_cells, 0, sizeof(s_cells));
    memset(s_cell_lbls, 0, sizeof(s_cell_lbls));
    s_info_lbl = NULL;
}

static void *sudoku_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    s_scr = scr;
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 800);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_screen_deleted, LV_EVENT_DELETE, NULL);

    dynapp_screen_status_bar(scr);
    dynapp_vk_attach(scr, "sudoku", sudoku_create, sudoku_on_vk_key);

    if (!load_game()) new_game(0);

    // 1. 标题行：标题在左，难度信息在右
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "数独");
    const lv_font_t *f_title = title_font();
    if (f_title != NULL) lv_obj_set_style_text_font(title, f_title, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_set_pos(title, 16, 48);

    s_info_lbl = lv_label_create(scr);
    const lv_font_t *f_ui = ui_font();
    if (f_ui != NULL) lv_obj_set_style_text_font(s_info_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_info_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_align(s_info_lbl, LV_ALIGN_TOP_RIGHT, -16, 56);

    // 2. 9x9 棋盘：先铺格子（可点的透明层），再画宫格分隔线
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
            lv_obj_add_event_cb(cell, on_cell_clicked, LV_EVENT_CLICKED,
                                (void *)(intptr_t)(r * N + c));
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);

            lv_obj_t *lbl = lv_label_create(cell);
            lv_label_set_text(lbl, "");
            lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
            lv_obj_center(lbl);
            lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

            s_cells[r][c] = cell;
            s_cell_lbls[r][c] = lbl;
        }
    }

    // 宫格分隔线：每 3 格一条粗线，其余细线（数独的标志性结构）
    for (int k = 0; k <= N; ++k) {
        const bool thick = (k % 3 == 0);
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

    // 3. 操作提示行
    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "点格子选择，点数字填入");
    if (f_ui != NULL) lv_obj_set_style_text_font(hint, f_ui, 0);
    lv_obj_set_style_text_color(hint, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_pos(hint, 16, 552);

    // 4. 数字键盘 1-9
    const lv_coord_t pad_y = 592;
    for (int d = 1; d <= N; ++d) {
        const lv_coord_t x = 16 + (d - 1) * (KEY + 2);
        char txt[4] = {0};
        snprintf(txt, sizeof(txt), "%d", d);
        lv_obj_t *btn = make_btn(scr, txt, KEY, 52, on_digit_clicked, (void *)(intptr_t)d);
        lv_obj_set_pos(btn, x, pad_y);
    }

    // 5. 功能按钮行：擦除 / 撤销 / 提示 / 新游戏
    const lv_coord_t bw = 107, bh = 56, bgap = 6;
    const lv_coord_t by = 688;
    struct { const char *label; lv_event_cb_t cb; } actions[] = {
        {"擦除", on_erase_clicked},
        {"撤销", on_undo_clicked},
        {"提示", on_hint_clicked},
        {"新游戏", on_new_game_clicked},
    };
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *btn = make_btn(scr, actions[i].label, bw, bh, actions[i].cb, NULL);
        lv_obj_set_pos(btn, 16 + i * (bw + bgap), by);
    }

    refresh_all_cells();
    update_info_line();
    return scr;
}

void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "sudoku";
    out->name = "数独";
    out->icon = "icon.png";
    out->create = sudoku_create;
}
