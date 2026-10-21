// tools/gomoku_app/main/gomoku_main.c
// 五子棋 动态应用：回合制、一步一刷，适配墨水屏低刷新率特性。
//
// 设计要点（墨水屏 480x800 / 1bpp 单色）：
//   - 落一子只改那一个棋子对象 → LVGL 局刷，残影可控；每 N 步全刷一次去残影
//   - 棋盘 15x15，格线 30px，整盘 420px 居中；棋子用圆形几何体（黑实心/白描边）
//   - 不做常驻计时器：墨水屏每秒重绘既费电又留残影，只在操作时刷新
//   - 字体只有 25@2(正文) / 30@2(标题) 两档含汉字
//   - AI 用形状打分启发式（活四/冲四/活三/眠三…），3 档难度靠候选集与攻防权重区分
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

#define N 15
#define CELL 30
#define GRID (N * CELL) /* 450：15 格 + 16 条边线，左右各留 15px */
#define BOARD_X 15
#define BOARD_Y 92

#define EMPTY 0
#define BLACK 1
#define WHITE 2

#define MODE_PVE 0 /* 人机（人执黑先行） */
#define MODE_PVP 1 /* 双人同机 */
#define MODE_COUNT 2

#define WIN_NONE 0
#define WIN_BLACK 1
#define WIN_WHITE 2
#define WIN_DRAW 3

#define MAX_MOVES (N * N)

// ---- 棋局状态 ----
static uint8_t s_board[N][N];
static int s_mode = MODE_PVE;
static int s_level = 1; /* 0 简单 / 1 普通 / 2 困难 */
static int s_next = BLACK;
static int s_winner = WIN_NONE;
static int s_nmoves = 0;
static int s_human = BLACK; /* 人机模式下人执哪色 */
static int s_ai_pending = 0;

static int s_hist_r[MAX_MOVES];
static int s_hist_c[MAX_MOVES];

// ---- 绘制对象 ----
static lv_obj_t *s_cell_obj[N][N];
static lv_obj_t *s_last_mark = NULL; /* 最后一手的小圆点标记 */
static lv_obj_t *s_info_lbl = NULL;
static lv_obj_t *s_status_lbl = NULL;

// 每格上次渲染的状态：0=空 1=黑 2=白。用 0xFF 作"未渲染"哨兵——
// 0 是合法取值(空格)，若用 0 初始化，首帧会被指纹短路跳过。
static uint8_t s_rendered[N][N];

static lv_obj_t *s_scr = NULL;
static int s_move_count = 0;
static int s_last_full_refresh = 0;
#define FULL_REFRESH_EVERY 12

static const char *kModeNames[MODE_COUNT] = {"人机", "双人"};
static const char *kLevelNames[3] = {"简单", "普通", "困难"};

// ---- 字体 ----
static const lv_font_t *ui_font(void) {
    const lv_font_t *f = fontpack_lv_font_ui();
    return f != NULL ? f : fontpack_lv_font_get(25, 2);
}
static const lv_font_t *title_font(void) {
    const lv_font_t *f = fontpack_lv_font_get(30, 2);
    return f != NULL ? f : ui_font();
}

// ================= 棋局逻辑 =================
static void reset_board(int mode, int level) {
    memset(s_board, EMPTY, sizeof(s_board));
    memset(s_rendered, 0xFF, sizeof(s_rendered));
    s_mode = mode;
    s_level = level;
    s_next = BLACK;
    s_winner = WIN_NONE;
    s_nmoves = 0;
    s_human = BLACK;
    s_ai_pending = 0;
    s_move_count = 0;
    s_last_full_refresh = 0;
}

static int in_board(int r, int c) { return r >= 0 && r < N && c >= 0 && c < N; }

// 落子后以 (r,c) 为中心判断是否连五（>=5 连子即胜）
static int has_five(int r, int c, int color) {
    static const int dr[4] = {0, 1, 1, 1};
    static const int dc[4] = {1, 0, 1, -1};
    for (int k = 0; k < 4; ++k) {
        int cnt = 1;
        for (int s = -1; s <= 1; s += 2) {
            int rr = r + dr[k] * s;
            int cc = c + dc[k] * s;
            while (in_board(rr, cc) && s_board[rr][cc] == color) {
                cnt++;
                rr += dr[k] * s;
                cc += dc[k] * s;
            }
        }
        if (cnt >= 5) return 1;
    }
    return 0;
}

static int board_full(void) {
    for (int r = 0; r < N; ++r)
        for (int c = 0; c < N; ++c)
            if (s_board[r][c] == EMPTY) return 0;
    return 1;
}

// ================= 持久化 =================
static void save_game(void) {
    FILE *fp = (FILE *)dynapp_fopen_data("gomoku.txt", "w");
    if (fp == NULL) return;
    fprintf(fp, "GOMOKU1 %d %d %d %d %d\n", s_mode, s_level, s_next, s_winner, s_nmoves);
    for (int r = 0; r < N; ++r) {
        for (int c = 0; c < N; ++c) fputc('0' + s_board[r][c], fp);
    }
    fputc('\n', fp);
    fclose(fp);
}

static int load_game(void) {
    FILE *fp = (FILE *)dynapp_fopen_data("gomoku.txt", "r");
    if (fp == NULL) return 0;
    char magic[16] = {0};
    int mode = 0, level = 0, next = 0, winner = 0, nmoves = 0;
    if (fscanf(fp, "%15s", magic) != 1 || strcmp(magic, "GOMOKU1") != 0) {
        fclose(fp);
        return 0;
    }
    if (fscanf(fp, "%d %d %d %d %d", &mode, &level, &next, &winner, &nmoves) != 5) {
        fclose(fp);
        return 0;
    }
    if (mode < 0 || mode >= MODE_COUNT || level < 0 || level > 2) {
        fclose(fp);
        return 0;
    }
    if (next != BLACK && next != WHITE) {
        fclose(fp);
        return 0;
    }
    if (winner < WIN_NONE || winner > WIN_DRAW) {
        fclose(fp);
        return 0;
    }
    for (int r = 0; r < N; ++r) {
        for (int c = 0; c < N; ++c) {
            int v = 0;
            if (fscanf(fp, "%1d", &v) != 1) {
                fclose(fp);
                return 0;
            }
            if (v < EMPTY || v > WHITE) {
                fclose(fp);
                return 0;
            }
            s_board[r][c] = (uint8_t)v;
        }
    }
    fclose(fp);
    s_mode = mode;
    s_level = level;
    s_next = next;
    s_winner = winner;
    s_nmoves = nmoves;
    s_human = BLACK;
    s_ai_pending = 0;
    s_move_count = 0;
    s_last_full_refresh = 0;
    memset(s_rendered, 0xFF, sizeof(s_rendered));
    return 1;
}

// ================= 绘制 =================
static void render_cell(int r, int c) {
    lv_obj_t *o = s_cell_obj[r][c];
    if (o == NULL) return;
    const uint8_t v = s_board[r][c];
    if (s_rendered[r][c] == v) return; // 没变 → 不动 LVGL，避免无谓脏区
    s_rendered[r][c] = v;

    if (v == EMPTY) {
        // 空位不能用 HIDDEN：LVGL 命中测试会跳过隐藏对象，格子就点不到了。
        // 改成透明底 + 无边框：照样什么都不画，但仍是可点的落子靶子。
        lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(o, 0, 0);
        return;
    }
    lv_obj_set_style_bg_color(o, v == BLACK ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    // 白子描黑边，否则在白底上完全看不见
    lv_obj_set_style_border_width(o, v == BLACK ? 0 : 2, 0);
    lv_obj_set_style_border_color(o, lv_color_black(), 0);
    lv_obj_set_style_radius(o, LV_RADIUS_CIRCLE, 0);
}

static void update_last_mark(int r, int c) {
    if (s_last_mark == NULL) return;
    if (r < 0) {
        lv_obj_add_flag(s_last_mark, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const uint8_t v = s_board[r][c];
    lv_obj_clear_flag(s_last_mark, LV_OBJ_FLAG_HIDDEN);
    // 黑白子各用对比色点，否则标记看不见
    lv_obj_set_style_bg_color(s_last_mark, v == BLACK ? lv_color_white() : lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_last_mark, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_last_mark, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_pos(s_last_mark, CELL * c + CELL / 2 - 3, CELL * r + CELL / 2 - 3);
}

static void update_info_line(void) {
    if (s_info_lbl == NULL) return;
    int nb = 0, nw = 0;
    for (int r = 0; r < N; ++r)
        for (int c = 0; c < N; ++c) {
            if (s_board[r][c] == BLACK) nb++;
            else if (s_board[r][c] == WHITE) nw++;
        }
    char buf[64];
    snprintf(buf, sizeof(buf), "黑 %d · 白 %d", nb, nw);
    lv_label_set_text(s_info_lbl, buf);
}

static void update_status_line(void) {
    if (s_status_lbl == NULL) return;
    char buf[96];
    if (s_winner == WIN_BLACK) {
        snprintf(buf, sizeof(buf), "%s · 黑棋胜", kModeNames[s_mode]);
    } else if (s_winner == WIN_WHITE) {
        snprintf(buf, sizeof(buf), "%s · 白棋胜", kModeNames[s_mode]);
    } else if (s_winner == WIN_DRAW) {
        snprintf(buf, sizeof(buf), "%s · 平局", kModeNames[s_mode]);
    } else if (s_mode == MODE_PVE) {
        snprintf(buf, sizeof(buf), "%s·%s · %s", kModeNames[s_mode], kLevelNames[s_level],
                 s_next == s_human ? "该你落子" : "电脑思考中");
    } else {
        snprintf(buf, sizeof(buf), "%s · 轮到%s", kModeNames[s_mode], s_next == BLACK ? "黑" : "白");
    }
    lv_label_set_text(s_status_lbl, buf);
}

static void render_all(void) {
    for (int r = 0; r < N; ++r)
        for (int c = 0; c < N; ++c) render_cell(r, c);
    update_info_line();
    update_status_line();
}

// ================= AI：形状打分启发式 =================
// 沿某方向数：落子后该方向的连续同色子数(含落点)与两端开放数
static void line_info(int r, int c, int dr, int dc, int color, int *cnt, int *opens) {
    int n = 0, open = 0;
    int rr = r + dr, cc = c + dc;
    while (in_board(rr, cc) && s_board[rr][cc] == color) {
        n++;
        rr += dr;
        cc += dc;
    }
    if (in_board(rr, cc) && s_board[rr][cc] == EMPTY) open++;
    rr = r - dr;
    cc = c - dc;
    while (in_board(rr, cc) && s_board[rr][cc] == color) {
        n++;
        rr -= dr;
        cc -= dc;
    }
    if (in_board(rr, cc) && s_board[rr][cc] == EMPTY) open++;
    *cnt = n;
    *opens = open;
}

// cnt=落子后连续子数, opens=两端开放格数
static long shape_score(int cnt, int opens) {
    switch (cnt) {
        case 5: return 1000000L;                              /* 连五 */
        case 4: return opens == 2 ? 100000L : (opens == 1 ? 10000L : 0L); /* 活四 / 冲四 */
        case 3: return opens == 2 ? 8000L : (opens == 1 ? 800L : 0L);     /* 活三 / 眠三 */
        case 2: return opens == 2 ? 400L : (opens == 1 ? 80L : 0L);       /* 活二 / 眠二 */
        case 1: return opens == 2 ? 30L : 10L;                            /* 活一 */
        default: return 0L;
    }
}

// 在 (r,c) 落 color 后的进攻分与防守分（防守分=对手在同点的威胁）
static void point_scores(int r, int c, int color, long *attack, long *defend) {
    static const int dr[4] = {0, 1, 1, 1};
    static const int dc[4] = {1, 0, 1, -1};
    long atk = 0, def = 0;
    for (int k = 0; k < 4; ++k) {
        int cnt, opens;
        line_info(r, c, dr[k], dc[k], color, &cnt, &opens);
        atk += shape_score(cnt + 1, opens);
        line_info(r, c, dr[k], dc[k], color == BLACK ? WHITE : BLACK, &cnt, &opens);
        def += shape_score(cnt + 1, opens);
    }
    *attack = atk;
    *defend = def;
}

// 难度：候选集 + 攻防权重。简单档只看已有棋子附近的空位且几乎不防守，
// 普通人机全盘扫描，困难档进攻权重更高。
static void ai_pick(int color, int *out_r, int *out_c) {
    const int simple = (s_level == 0);
    long best = -1;
    int br = -1, bc = -1;
    const long w_def = simple ? 10L : (s_level == 1 ? 80L : 100L);
    const long w_atk = simple ? 100L : (s_level == 1 ? 100L : 120L);

    for (int r = 0; r < N; ++r) {
        for (int c = 0; c < N; ++c) {
            if (s_board[r][c] != EMPTY) continue;
            if (simple) {
                // 只看已有棋子相邻的空位（开局时退化为中心一点）
                int near = 0;
                for (int dr = -1; dr <= 1 && !near; ++dr)
                    for (int dc = -1; dc <= 1 && !near; ++dc)
                        if (in_board(r + dr, c + dc) && s_board[r + dr][c + dc] != EMPTY) near = 1;
                if (!near && s_nmoves > 0) continue;
            }
            long atk, def;
            point_scores(r, c, color, &atk, &def);
            const long score = atk * w_atk + def * w_def;
            if (score > best) {
                best = score;
                br = r;
                bc = c;
            }
        }
    }
    if (br < 0) {
        // 满盘兜底
        for (int r = 0; r < N && br < 0; ++r)
            for (int c = 0; c < N; ++c)
                if (s_board[r][c] == EMPTY) {
                    br = r;
                    bc = c;
                    break;
                }
    }
    *out_r = br;
    *out_c = bc;
}

// ================= 交互 =================
static void schedule_idle_maintenance(void);

static void after_move(int r, int c, int color) {
    s_hist_r[s_nmoves] = r;
    s_hist_c[s_nmoves] = c;
    s_nmoves++;
    s_move_count++;
    render_cell(r, c);
    update_last_mark(r, c);
    update_info_line();

    if (has_five(r, c, color)) {
        s_winner = (color == BLACK) ? WIN_BLACK : WIN_WHITE;
        s_next = color;
        update_status_line();
        save_game();
        char msg[64];
        snprintf(msg, sizeof(msg), "%s棋五子连珠。", color == BLACK ? "黑" : "白");
        dynapp_modal_alert(color == BLACK ? "黑棋胜！" : "白棋胜！", msg, "确定");
        return;
    }
    if (board_full()) {
        s_winner = WIN_DRAW;
        update_status_line();
        save_game();
        dynapp_modal_alert("平局", "棋盘下满了。", "确定");
        return;
    }
    s_next = (color == BLACK) ? WHITE : BLACK;
    update_status_line();
}

static void place_stone(int r, int c, int color) {
    if (!in_board(r, c)) return;
    if (s_board[r][c] != EMPTY) return;
    if (s_winner != WIN_NONE) return;
    s_board[r][c] = (uint8_t)color;
    after_move(r, c, color);
}

static void ai_move_cb(lv_timer_t *tmr) {
    lv_timer_delete(tmr);
    s_ai_pending = 0;
    if (s_winner != WIN_NONE) return;
    const int color = s_next;
    int r = -1, c = -1;
    ai_pick(color, &r, &c);
    if (r < 0) return;
    place_stone(r, c, color);
    schedule_idle_maintenance();
}

static void schedule_ai_move(void) {
    if (s_ai_pending) return;
    s_ai_pending = 1;
    // 稍等一拍再落子：先让人看到自己的棋子上了屏（墨水屏刷一帧要 0.3~1s）
    lv_timer_create(ai_move_cb, 600, NULL);
}

// ================= 空闲维护：落盘 与 全刷 分开 =================
//   落盘(3.5s)：操作只改内存，停手 3.5s 后写一次 SD。
//   全刷(6s)  ：全刷 480x800 = 38 万像素，墨水屏要 2~3 秒，期间 LVGL/触摸都被
//                阻塞，注入的"按下"会被 LVGL 当成手指移动 → CLICKED 丢失。
//                所以全刷必须等到"真空闲"（6 秒无操作）才做。
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

static void on_cell_clicked(lv_event_t *e) {
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const int r = idx / N, c = idx % N;
    if (s_winner != WIN_NONE) return;
    if (s_board[r][c] != EMPTY) return;
    if (s_mode == MODE_PVE) {
        if (s_next != s_human) return; // 电脑回合不接人工输入
        if (s_ai_pending) return;
        place_stone(r, c, s_human);
        schedule_ai_move();
    } else {
        place_stone(r, c, s_next);
    }
    schedule_idle_maintenance();
}

static void on_new_game_clicked(lv_event_t *e) {
    (void)e;
    reset_board(s_mode, s_level);
    render_all();
    update_last_mark(-1, -1);
    save_game();
    dynapp_toast_show("新局", 1200);
    dynapp_epd_full_refresh();
}

static void on_mode_clicked(lv_event_t *e) {
    (void)e;
    const int mode = (s_mode + 1) % MODE_COUNT;
    reset_board(mode, s_level);
    render_all();
    update_last_mark(-1, -1);
    save_game();
    dynapp_toast_show(mode == MODE_PVE ? "人机对战" : "双人同机", 1400);
    dynapp_epd_full_refresh();
}

static void on_level_clicked(lv_event_t *e) {
    (void)e;
    if (s_mode != MODE_PVE) {
        dynapp_toast_show("双人模式没有难度", 1400);
        return;
    }
    s_level = (s_level + 1) % 3;
    update_status_line();
    save_game();
    dynapp_toast_show(kLevelNames[s_level], 1200);
    dynapp_epd_full_refresh();
}

static void on_undo_clicked(lv_event_t *e) {
    (void)e;
    if (s_nmoves <= 0) {
        dynapp_toast_show("还没有棋可悔", 1200);
        return;
    }
    // 人机模式悔棋要连退两步（电脑那步 + 自己那步）
    int steps = (s_mode == MODE_PVE) ? 2 : 1;
    if (steps > s_nmoves) steps = s_nmoves;
    for (int i = 0; i < steps; ++i) {
        const int idx = s_nmoves - 1;
        s_board[s_hist_r[idx]][s_hist_c[idx]] = EMPTY;
        s_nmoves--;
        render_cell(s_hist_r[idx], s_hist_c[idx]);
    }
    s_winner = WIN_NONE;
    // 轮到谁：由历史棋子的颜色推：下一手是最后落下那子的对手
    if (s_nmoves > 0) {
        const int last = s_board[s_hist_r[s_nmoves - 1]][s_hist_c[s_nmoves - 1]];
        s_next = (last == BLACK) ? WHITE : BLACK;
    } else {
        s_next = BLACK;
    }
    if (s_nmoves > 0) {
        update_last_mark(s_hist_r[s_nmoves - 1], s_hist_c[s_nmoves - 1]);
    } else {
        update_last_mark(-1, -1);
    }
    s_ai_pending = 0;
    update_info_line();
    update_status_line();
    save_game();
    dynapp_toast_show("已悔棋", 1200);
    dynapp_epd_full_refresh();
}

static int gomoku_on_vk_key(const char *key) {
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
    s_status_lbl = NULL;
    memset(s_cell_obj, 0, sizeof(s_cell_obj));
    s_last_mark = NULL;
}

static void *gomoku_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    s_scr = scr;
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 800);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_screen_deleted, LV_EVENT_DELETE, NULL);

    dynapp_screen_status_bar(scr);
    dynapp_vk_attach(scr, "gomoku", gomoku_create, gomoku_on_vk_key);

    if (!load_game()) reset_board(MODE_PVE, 1);

    // 1. 标题行
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "五子棋");
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
    lv_obj_set_pos(board, BOARD_X, BOARD_Y);
    lv_obj_set_size(board, GRID, GRID);
    lv_obj_clear_flag(board, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // 格线：外框 2px，内部 1px
    for (int k = 0; k <= N; ++k) {
        const bool edge = (k == 0 || k == N);
        const lv_coord_t lw = edge ? 2 : 1;
        const lv_coord_t p = k * CELL;
        for (int axis = 0; axis < 2; ++axis) {
            lv_obj_t *line = lv_obj_create(board);
            lv_obj_remove_style_all(line);
            if (axis == 0) {
                lv_obj_set_pos(line, p - (edge ? 1 : 0), 0);
                lv_obj_set_size(line, lw, GRID);
            } else {
                lv_obj_set_pos(line, 0, p - (edge ? 1 : 0));
                lv_obj_set_size(line, GRID, lw);
            }
            lv_obj_set_style_bg_color(line, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
            lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        }
    }

    // 星位（天元 + 四星）
    static const int kStars[5][2] = {{3, 3}, {3, 11}, {7, 7}, {11, 3}, {11, 11}};
    for (int i = 0; i < 5; ++i) {
        lv_obj_t *dot = lv_obj_create(board);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 4, 4);
        lv_obj_set_style_bg_color(dot, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_pos(dot, kStars[i][1] * CELL - 2, kStars[i][0] * CELL - 2);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    // 棋子位（先建空对象，落子时再显示）
    for (int r = 0; r < N; ++r) {
        for (int c = 0; c < N; ++c) {
            lv_obj_t *stone = lv_obj_create(board);
            lv_obj_remove_style_all(stone);
            lv_obj_set_size(stone, 26, 26);
            lv_obj_set_style_radius(stone, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(stone, LV_OPA_COVER, 0);
            lv_obj_set_pos(stone, c * CELL + 2, r * CELL + 2);
            lv_obj_add_flag(stone, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(stone, on_cell_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)(r * N + c));
            lv_obj_clear_flag(stone, LV_OBJ_FLAG_SCROLLABLE);
            // 空位不隐藏：隐藏对象会被 LVGL 命中测试跳过，格子就点不到了
            lv_obj_set_style_bg_opa(stone, LV_OPA_TRANSP, 0);
            s_cell_obj[r][c] = stone;
        }
    }

    // 最后一手标记（小圆点，颜色与棋子相反）
    s_last_mark = lv_obj_create(board);
    lv_obj_remove_style_all(s_last_mark);
    lv_obj_set_size(s_last_mark, 6, 6);
    lv_obj_set_style_bg_opa(s_last_mark, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_last_mark, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(s_last_mark, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_last_mark, LV_OBJ_FLAG_HIDDEN);

    // 3. 状态行（label 的 y 即文字顶端：25@2 字体行高与 glyph 顶部对齐，
    //    两行文字要占 546..599，按钮得放到 620 才不压字）
    s_status_lbl = lv_label_create(scr);
    if (f_ui != NULL) lv_obj_set_style_text_font(s_status_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_status_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_pos(s_status_lbl, 16, BOARD_Y + GRID + 4);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "点空位落子 · 连五即胜");
    if (f_ui != NULL) lv_obj_set_style_text_font(hint, f_ui, 0);
    lv_obj_set_style_text_color(hint, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_pos(hint, 16, BOARD_Y + GRID + 34);

    // 4. 功能按钮
    const lv_coord_t bw = 107, bh = 56, bgap = 6;
    const lv_coord_t by = 620;
    lv_obj_t *b1 = make_btn(scr, "新局", bw, bh, on_new_game_clicked, NULL);
    lv_obj_set_pos(b1, 16, by);
    lv_obj_t *b2 = make_btn(scr, "悔棋", bw, bh, on_undo_clicked, NULL);
    lv_obj_set_pos(b2, 16 + bw + bgap, by);
    lv_obj_t *b3 = make_btn(scr, "模式", bw, bh, on_mode_clicked, NULL);
    lv_obj_set_pos(b3, 16 + 2 * (bw + bgap), by);
    lv_obj_t *b4 = make_btn(scr, "难度", bw, bh, on_level_clicked, NULL);
    lv_obj_set_pos(b4, 16 + 3 * (bw + bgap), by);

    render_all();
    if (s_nmoves > 0) {
        update_last_mark(s_hist_r[s_nmoves - 1], s_hist_c[s_nmoves - 1]);
    }
    return scr;
}

void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "gomoku";
    out->name = "五子棋";
    out->icon = "icon.png";
    out->create = gomoku_create;
}
