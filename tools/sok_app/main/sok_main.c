// tools/sok_app/main/sok_main.c
// 推箱子 动态应用：经典 Sokoban，一步一刷，适配墨水屏低刷新率特性。
//
// 设计要点（墨水屏 480x800 / 1bpp 单色）：
//   - 一步一刷：走一步只改"人 + 那个箱子 + 露出的一张暗牌"三四个对象，
//     脏区很小；每 N 步全刷一次去残影（沿用数独/扫雷那套）。
//   - 关卡是**逆向生成**的：从"箱子都在目标点"的终局出发，反向拉箱子若干步，
//     得到的局面**保证有解**（构造出来的），不用手写关卡表。
//     关卡号决定箱子数与打乱深度，种子存在存档里，可复现。
//   - 1bpp 没有红绿：目标点=中心实心点，空箱子=空心方框，
//     箱子在目标上=空心方框+中心实心点，人=实心圆，墙=实心方块。
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "esp_random.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

#define GW 10
#define GH 10
#define CELL 44
#define BOARD_W (GW * CELL)
#define BOARD_H (GH * CELL)
#define BOARD_X ((480 - BOARD_W) / 2)
#define BOARD_Y 104

#define DIR_UP 0
#define DIR_DOWN 1
#define DIR_LEFT 2
#define DIR_RIGHT 3

#define MAX_UNDO 256

// ---- 棋局状态 ----
static uint8_t s_wall[GH][GW]; // 1=墙
static uint8_t s_goal[GH][GW]; // 1=目标点
static uint8_t s_box[GH][GW];  // 1=箱子
static uint8_t s_drawn[GH][GW]; // 上次画出来的格子样式，0xFF=未画
static int s_px, s_py;         // 人的位置
static int s_level = 1;
static uint32_t s_seed = 20261021u;
static int s_moves = 0;
static int s_best = 0;   // 本关最少步数（0=还没通关过）
static int s_solved = 0; // 本关是否已解开
static uint8_t s_undo[MAX_UNDO];
static int s_undo_n = 0;

// ---- 绘制对象 ----
static lv_obj_t *s_cell_obj[GH][GW];
static lv_obj_t *s_goal_obj[GH][GW];
static lv_obj_t *s_player = NULL;
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

// ================= 随机数（可复现） =================
static uint32_t s_rng_state = 1;

static void rng_seed(uint32_t seed) { s_rng_state = seed ? seed : 1; }

static uint32_t rng_next(void) {
    uint32_t x = s_rng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng_state = x;
    return x;
}

static int rng_below(int n) { return n > 0 ? (int)(rng_next() % (uint32_t)n) : 0; }

// ================= 基础 =================
static const int kDirDx[4] = {0, 0, -1, 1};
static const int kDirDy[4] = {-1, 1, 0, 0};

static int in_grid(int x, int y) { return x >= 0 && x < GW && y >= 0 && y < GH; }

// 从 (x,y) 出发人能走到的格子
static void flood(int x, int y, uint8_t *used) {
    static int qx[GW * GH], qy[GW * GH];
    int head = 0, tail = 0;
    memset(used, 0, GW * GH);
    if (!in_grid(x, y) || s_wall[y][x]) return;
    used[y * GW + x] = 1;
    qx[tail] = x;
    qy[tail] = y;
    tail++;
    while (head < tail) {
        const int cx = qx[head], cy = qy[head];
        head++;
        for (int d = 0; d < 4; ++d) {
            const int nx = cx + kDirDx[d], ny = cy + kDirDy[d];
            if (!in_grid(nx, ny) || s_wall[ny][nx] || used[ny * GW + nx]) continue;
            used[ny * GW + nx] = 1;
            qx[tail] = nx;
            qy[tail] = ny;
            tail++;
        }
    }
}

// ================= 关卡生成（逆向拉箱子，保证有解） =================
static void gen_level(int level, uint32_t seed) {
    rng_seed(seed);
    memset(s_wall, 0, sizeof(s_wall));
    memset(s_goal, 0, sizeof(s_goal));
    memset(s_box, 0, sizeof(s_box));

    // 1) 外墙
    for (int x = 0; x < GW; ++x) {
        s_wall[0][x] = 1;
        s_wall[GH - 1][x] = 1;
    }
    for (int y = 0; y < GH; ++y) {
        s_wall[y][0] = 1;
        s_wall[y][GW - 1] = 1;
    }
    // 2) 内部随机墙
    const int inner = (GW - 2) * (GH - 2);
    int walls = 5 + level * 2;
    if (walls > inner / 4) walls = inner / 4;
    for (int i = 0; i < walls; ++i) {
        const int x = 1 + rng_below(GW - 2);
        const int y = 1 + rng_below(GH - 2);
        s_wall[y][x] = 1;
    }

    // 3) 找最大的连通空地
    static uint8_t visited[GW * GH];
    static uint8_t region[GW * GH];
    static uint8_t best_region[GW * GH];
    memset(visited, 0, sizeof(visited));
    memset(best_region, 0, sizeof(best_region));
    int best_cnt = 0;
    for (int y = 1; y < GH - 1; ++y) {
        for (int x = 1; x < GW - 1; ++x) {
            if (s_wall[y][x] || visited[y * GW + x]) continue;
            flood(x, y, region);
            int cnt = 0;
            for (int i = 0; i < GW * GH; ++i)
                if (region[i]) {
                    visited[i] = 1;
                    cnt++;
                }
            if (cnt > best_cnt) {
                best_cnt = cnt;
                memcpy(best_region, region, sizeof(best_region));
            }
        }
    }
    if (best_cnt < 16) { // 空地太小就退化成纯空场
        memset(s_wall, 0, sizeof(s_wall));
        for (int x = 0; x < GW; ++x) {
            s_wall[0][x] = 1;
            s_wall[GH - 1][x] = 1;
        }
        for (int y = 0; y < GH; ++y) {
            s_wall[y][0] = 1;
            s_wall[y][GW - 1] = 1;
        }
        memset(best_region, 0, sizeof(best_region));
        for (int y = 1; y < GH - 1; ++y)
            for (int x = 1; x < GW - 1; ++x) best_region[y * GW + x] = 1;
        best_cnt = inner;
    }

    // 4) 目标点（箱子先放目标上 = 终局）
    int nbox = 3 + level / 2;
    if (nbox > 6) nbox = 6;
    if (nbox > best_cnt / 5) nbox = best_cnt / 5;
    if (nbox < 1) nbox = 1;

    int cx[GW * GH], cy[GW * GH];
    int nc = 0;
    for (int i = 0; i < GW * GH; ++i) {
        if (best_region[i]) {
            cx[nc] = i % GW;
            cy[nc] = i / GW;
            nc++;
        }
    }
    for (int i = 0; i < nbox && nc > 0; ++i) {
        const int k = rng_below(nc);
        s_goal[cy[k]][cx[k]] = 1;
        s_box[cy[k]][cx[k]] = 1;
        cx[k] = cx[nc - 1];
        cy[k] = cy[nc - 1];
        nc--;
    }

    // 5) 人的位置
    for (int tries = 0; tries < 200; ++tries) {
        s_px = 1 + rng_below(GW - 2);
        s_py = 1 + rng_below(GH - 2);
        if (!s_wall[s_py][s_px] && !s_box[s_py][s_px]) break;
    }

    // 6) 逆向拉箱子：把箱子从目标点拉走 → 得到的局面保证可解
    static uint8_t reach[GW * GH];
    const int pulls = 20 + level * 12;
    for (int i = 0; i < pulls; ++i) {
        int bx = -1, by = -1;
        for (int tries = 0; tries < 30; ++tries) {
            const int x = 1 + rng_below(GW - 2);
            const int y = 1 + rng_below(GH - 2);
            if (s_box[y][x]) {
                bx = x;
                by = y;
                break;
            }
        }
        if (bx < 0) continue;
        const int d = rng_below(4);
        const int sx = bx - kDirDx[d]; // 人要站的位置（箱子反方向）
        const int sy = by - kDirDy[d];
        const int nx = bx + kDirDx[d]; // 箱子被拉到的地方
        const int ny = by + kDirDy[d];
        if (!in_grid(sx, sy) || !in_grid(nx, ny)) continue;
        if (s_wall[sy][sx] || s_box[sy][sx]) continue;
        if (s_wall[ny][nx] || s_box[ny][nx]) continue;
        flood(s_px, s_py, reach);
        if (!reach[sy * GW + sx]) continue;
        s_box[by][bx] = 0;
        s_box[ny][nx] = 1;
        s_px = bx;
        s_py = by;
    }

    // 7) 别生成"已经解开"的废关
    int dist = 0;
    for (int y = 0; y < GH; ++y) {
        for (int x = 0; x < GW; ++x) {
            if (!s_box[y][x]) continue;
            int best = 99;
            for (int gy = 0; gy < GH; ++gy) {
                for (int gx = 0; gx < GW; ++gx) {
                    if (!s_goal[gy][gx]) continue;
                    const int dd = abs(gx - x) + abs(gy - y);
                    if (dd < best) best = dd;
                }
            }
            dist += best;
        }
    }
    if (dist < 6) gen_level(level, seed * 1664525u + 1013904223u);
}

// ================= 绘制 =================
// 格子样式：0=地 1=墙 2=目标点 3=箱子 4=箱子在目标上
static int tile_at(int x, int y) {
    if (s_wall[y][x]) return 1;
    if (s_box[y][x]) return s_goal[y][x] ? 4 : 3;
    if (s_goal[y][x]) return 2;
    return 0;
}

static void render_cell(int x, int y) {
    lv_obj_t *cell = s_cell_obj[y][x];
    lv_obj_t *goal = s_goal_obj[y][x];
    if (cell == NULL) return;
    const int t = tile_at(x, y);
    if (s_drawn[y][x] == t) return;
    s_drawn[y][x] = (uint8_t)t;

    const int cx = 0, cy = 0; // goal 是格子的子对象，坐标相对格子
    if (t == 1) { // 墙：实心黑块
        lv_obj_set_style_bg_color(cell, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(cell, 0, 0);
    } else { // 地：白底 + 1px 淡格线
        lv_obj_set_style_bg_color(cell, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(cell, lv_color_black(), 0);
        lv_obj_set_style_border_width(cell, 1, 0);
    }

    if (goal != NULL) {
        if (t == 2) { // 目标点：中心实心小点
            lv_obj_clear_flag(goal, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(goal, 10, 10);
            lv_obj_set_pos(goal, cx + (CELL - 10) / 2, cy + (CELL - 10) / 2);
            lv_obj_set_style_bg_color(goal, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(goal, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(goal, LV_RADIUS_CIRCLE, 0);
        } else if (t == 4) { // 箱子在目标上：方框 + 中心点
            lv_obj_clear_flag(goal, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_size(goal, 10, 10);
            lv_obj_set_pos(goal, cx + (CELL - 10) / 2, cy + (CELL - 10) / 2);
            lv_obj_set_style_bg_color(goal, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(goal, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(goal, LV_RADIUS_CIRCLE, 0);
        } else {
            lv_obj_add_flag(goal, LV_OBJ_FLAG_HIDDEN);
        }
    }

    if (t == 3 || t == 4) { // 箱子：空心方框
        lv_obj_set_style_border_color(cell, lv_color_black(), 0);
        lv_obj_set_style_border_width(cell, 3, 0);
        lv_obj_set_style_border_side(cell, LV_BORDER_SIDE_FULL, 0);
        lv_obj_set_style_pad_all(cell, 6, 0); // 让边框往里收 → 呈现"箱子"而不是整格
        lv_obj_set_style_radius(cell, 2, 0);
    }
}

static void render_player(void) {
    if (s_player == NULL) return;
    lv_obj_set_pos(s_player, BOARD_X + s_px * CELL + (CELL - 24) / 2,
                   BOARD_Y + s_py * CELL + (CELL - 24) / 2);
}

static void update_info_line(void) {
    if (s_info_lbl == NULL) return;
    char buf[80];
    if (s_best > 0)
        snprintf(buf, sizeof(buf), "第 %d 关 · 步 %d · 最佳 %d", s_level, s_moves, s_best);
    else
        snprintf(buf, sizeof(buf), "第 %d 关 · 步 %d", s_level, s_moves);
    lv_label_set_text(s_info_lbl, buf);
}

static void update_status_line(void) {
    if (s_status_lbl == NULL) return;
    const char *t = "滑动屏幕或按方向键推箱子";
    if (s_solved) t = "这一关过了！按「下一关」继续";
    lv_label_set_text(s_status_lbl, t);
}

static void render_all(void) {
    for (int y = 0; y < GH; ++y)
        for (int x = 0; x < GW; ++x) {
            s_drawn[y][x] = 0xFF;
            render_cell(x, y);
        }
    render_player();
    update_info_line();
    update_status_line();
}

// ================= 持久化 =================
static void save_game(void) {
    FILE *fp = (FILE *)dynapp_fopen_data("sok.txt", "w");
    if (fp == NULL) return;
    fprintf(fp, "SOK1 %d %u %d %d %d %d %d %d\n", s_level, (unsigned)s_seed, s_moves, s_best, s_solved,
            s_px, s_py, s_undo_n);
    static const char kHex[] = "0123456789abcdef";
    for (int y = 0; y < GH; ++y) {
        for (int x = 0; x < GW; ++x) {
            int v = 0;
            if (s_wall[y][x]) v |= 1;
            if (s_goal[y][x]) v |= 2;
            if (s_box[y][x]) v |= 4;
            fputc(kHex[v], fp);
        }
    }
    fputc('\n', fp);
    for (int i = 0; i < s_undo_n; ++i) fputc('0' + s_undo[i], fp);
    fputc('\n', fp);
    fclose(fp);
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int load_game(void) {
    FILE *fp = (FILE *)dynapp_fopen_data("sok.txt", "r");
    if (fp == NULL) return 0;
    char magic[16] = {0};
    int level = 0, moves = 0, best = 0, solved = 0, px = 0, py = 0, undo_n = 0;
    unsigned seed = 0;
    if (fscanf(fp, "%15s", magic) != 1 || strcmp(magic, "SOK1") != 0) {
        fclose(fp);
        return 0;
    }
    if (fscanf(fp, "%d %u %d %d %d %d %d %d", &level, &seed, &moves, &best, &solved, &px, &py,
               &undo_n) != 8) {
        fclose(fp);
        return 0;
    }
    if (level < 1 || level > 9999 || undo_n < 0 || undo_n > MAX_UNDO) {
        fclose(fp);
        return 0;
    }
    for (int y = 0; y < GH; ++y) {
        for (int x = 0; x < GW; ++x) {
            int v = 0;
            if (fscanf(fp, "%1x", &v) != 1) {
                fclose(fp);
                return 0;
            }
            s_wall[y][x] = (uint8_t)(v & 1);
            s_goal[y][x] = (uint8_t)((v >> 1) & 1);
            s_box[y][x] = (uint8_t)((v >> 2) & 1);
        }
    }
    s_undo_n = 0;
    for (int i = 0; i < undo_n; ++i) {
        // 注意：fgetc 不在固件导出符号表里，动态应用引用它会在 dlopen 阶段失败
        char c = 0;
        if (fscanf(fp, "%1c", &c) != 1) break;
        if (c < '0' || c > '3') break;
        s_undo[s_undo_n++] = (uint8_t)(c - '0');
    }
    fclose(fp);
    s_level = level;
    s_seed = seed;
    s_moves = moves;
    s_best = best;
    s_solved = solved;
    s_px = px;
    s_py = py;
    return 1;
}

// ================= 游戏逻辑 =================
static int boxes_on_goal(void) {
    int n = 0;
    for (int y = 0; y < GH; ++y)
        for (int x = 0; x < GW; ++x)
            if (s_box[y][x] && s_goal[y][x]) n++;
    return n;
}

static int total_boxes(void) {
    int n = 0;
    for (int y = 0; y < GH; ++y)
        for (int x = 0; x < GW; ++x)
            if (s_box[y][x]) n++;
    return n;
}

static void try_move(int dir) {
    if (s_solved) return;
    const int nx = s_px + kDirDx[dir], ny = s_py + kDirDy[dir];
    if (!in_grid(nx, ny) || s_wall[ny][nx]) return;
    if (s_box[ny][nx]) {
        const int bx = nx + kDirDx[dir], by = ny + kDirDy[dir];
        if (!in_grid(bx, by) || s_wall[by][bx] || s_box[by][bx]) return;
        s_box[ny][nx] = 0;
        s_box[by][bx] = 1;
        render_cell(nx, ny);
        render_cell(bx, by);
    }
    s_px = nx;
    s_py = ny;
    s_moves++;
    s_move_count++;
    if (s_undo_n < MAX_UNDO) s_undo[s_undo_n++] = (uint8_t)dir;
    render_player();
    update_info_line();

    const int total = total_boxes();
    if (total > 0 && boxes_on_goal() == total) {
        s_solved = 1;
        if (s_best == 0 || s_moves < s_best) s_best = s_moves;
        update_info_line();
        update_status_line();
        save_game();
        dynapp_modal_alert("过关！", "所有箱子都推到目标点了。", "确定");
        return;
    }
    save_game();
}

static void undo_move(void) {
    if (s_undo_n <= 0) {
        dynapp_toast_show("没有可悔的步", 1200);
        return;
    }
    const int dir = s_undo[--s_undo_n];
    // 反向：人退回上一个位置，箱子跟着退
    const int px = s_px - kDirDx[dir], py = s_py - kDirDy[dir];
    if (in_grid(px, py) && s_box[py][px]) {
        const int bx = px - kDirDx[dir], by = py - kDirDy[dir];
        if (in_grid(bx, by)) {
            s_box[py][px] = 0;
            s_box[by][bx] = 1;
            render_cell(px, py);
            render_cell(bx, by);
        }
    }
    s_px = px;
    s_py = py;
    if (s_moves > 0) s_moves--;
    s_solved = 0;
    render_player();
    update_info_line();
    update_status_line();
    save_game();
}

static void load_level(int level) {
    s_level = level;
    s_seed = (uint32_t)esp_random() ^ ((uint32_t)level * 2654435761u);
    if (s_seed == 0) s_seed = 1;
    gen_level(level, s_seed);
    s_moves = 0;
    s_best = 0;
    s_solved = 0;
    s_undo_n = 0;
    memset(s_drawn, 0xFF, sizeof(s_drawn));
    render_all();
    save_game();
}

// ================= 交互 =================
static void on_dir_clicked(lv_event_t *e) {
    const int dir = (int)(intptr_t)lv_event_get_user_data(e);
    try_move(dir);
}

static void on_undo_clicked(lv_event_t *e) {
    (void)e;
    undo_move();
}

static void on_restart_clicked(lv_event_t *e) {
    (void)e;
    load_level(s_level);
    dynapp_toast_show("本关重开", 1000);
    dynapp_epd_full_refresh();
}

static void on_next_clicked(lv_event_t *e) {
    (void)e;
    load_level(s_level + 1);
    char b[32];
    snprintf(b, sizeof(b), "第 %d 关", s_level);
    dynapp_toast_show(b, 1200);
    dynapp_epd_full_refresh();
}

// 滑动屏幕推箱子（LVGL 原生手势事件）
static void on_screen_gesture(lv_event_t *e) {
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (indev == NULL) return;
    lv_indev_reset(indev, NULL);
    lv_indev_wait_release(indev);
    switch (lv_indev_get_gesture_dir(indev)) {
        case LV_DIR_LEFT: try_move(DIR_LEFT); break;
        case LV_DIR_RIGHT: try_move(DIR_RIGHT); break;
        case LV_DIR_TOP: try_move(DIR_UP); break;
        case LV_DIR_BOTTOM: try_move(DIR_DOWN); break;
        default: break;
    }
}

// 物理键回调跑在 esp_timer 上下文，必须转投 LVGL 线程
static void async_dir_cb(void *arg) { try_move((int)(intptr_t)arg); }

static int sok_on_vk_key(const char *key) {
    if (key == NULL) return 0;
    if (strcmp(key, "vk_home") == 0 || strcmp(key, "esc") == 0 || strcmp(key, "back") == 0) {
        save_game();
        dynapp_screen_go_home();
        return 1;
    }
    if (strcmp(key, "vk_prev") == 0) { // 上键 = 上
        dynapp_lv_async(async_dir_cb, (void *)(intptr_t)DIR_UP);
        return 1;
    }
    if (strcmp(key, "vk_next") == 0) { // 下键 = 下
        dynapp_lv_async(async_dir_cb, (void *)(intptr_t)DIR_DOWN);
        return 1;
    }
    return 0;
}

// ================= 界面 =================
static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                          lv_coord_t h, lv_event_cb_t cb, void *user_data) {
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
    s_player = NULL;
    memset(s_cell_obj, 0, sizeof(s_cell_obj));
    memset(s_goal_obj, 0, sizeof(s_goal_obj));
}

static void *sok_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    s_scr = scr;
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 800);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_screen_deleted, LV_EVENT_DELETE, NULL);
    lv_obj_add_event_cb(scr, on_screen_gesture, LV_EVENT_GESTURE, NULL);

    dynapp_screen_status_bar(scr);
    dynapp_vk_attach(scr, "sok", sok_create, sok_on_vk_key);

    if (!load_game()) {
        s_level = 1;
        s_seed = (uint32_t)esp_random();
        if (s_seed == 0) s_seed = 1;
        gen_level(1, s_seed);
        s_moves = 0;
        s_best = 0;
        s_solved = 0;
        s_undo_n = 0;
    }
    memset(s_drawn, 0xFF, sizeof(s_drawn));

    const lv_font_t *f_ui = ui_font();
    const lv_font_t *f_title = title_font();

    // 1. 标题
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "推箱子");
    if (f_title != NULL) lv_obj_set_style_text_font(title, f_title, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_set_pos(title, 16, 48);

    s_info_lbl = lv_label_create(scr);
    if (f_ui != NULL) lv_obj_set_style_text_font(s_info_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_info_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_align(s_info_lbl, LV_ALIGN_TOP_RIGHT, -16, 56);

    // 2. 棋盘
    for (int y = 0; y < GH; ++y) {
        for (int x = 0; x < GW; ++x) {
            lv_obj_t *cell = lv_obj_create(scr);
            lv_obj_remove_style_all(cell);
            lv_obj_set_pos(cell, BOARD_X + x * CELL, BOARD_Y + y * CELL);
            lv_obj_set_size(cell, CELL, CELL);
            lv_obj_set_style_bg_color(cell, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(cell, lv_color_black(), 0);
            lv_obj_set_style_border_width(cell, 1, 0);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
            s_cell_obj[y][x] = cell;

            lv_obj_t *goal = lv_obj_create(cell);
            lv_obj_remove_style_all(goal);
            lv_obj_set_size(goal, 10, 10);
            lv_obj_set_style_bg_color(goal, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(goal, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(goal, LV_RADIUS_CIRCLE, 0);
            lv_obj_clear_flag(goal, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(goal, LV_OBJ_FLAG_HIDDEN);
            s_goal_obj[y][x] = goal;
        }
    }

    // 3. 人
    s_player = lv_obj_create(scr);
    lv_obj_remove_style_all(s_player);
    lv_obj_set_size(s_player, 24, 24);
    lv_obj_set_style_bg_color(s_player, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_player, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_player, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(s_player, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    // 4. 状态行
    s_status_lbl = lv_label_create(scr);
    if (f_ui != NULL) lv_obj_set_style_text_font(s_status_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_status_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_pos(s_status_lbl, 16, 560);

    // 5. 方向键（十字）
    const lv_coord_t db = 60;
    make_btn(scr, "↑", 70, 600, db, db, on_dir_clicked, (void *)(intptr_t)DIR_UP);
    make_btn(scr, "←", 10, 660, db, db, on_dir_clicked, (void *)(intptr_t)DIR_LEFT);
    make_btn(scr, "↓", 70, 660, db, db, on_dir_clicked, (void *)(intptr_t)DIR_DOWN);
    make_btn(scr, "→", 130, 660, db, db, on_dir_clicked, (void *)(intptr_t)DIR_RIGHT);

    // 6. 功能按钮
    make_btn(scr, "悔棋", 200, 600, 125, 60, on_undo_clicked, NULL);
    make_btn(scr, "重开", 335, 600, 125, 60, on_restart_clicked, NULL);
    make_btn(scr, "下一关", 200, 670, 260, 60, on_next_clicked, NULL);

    render_all();
    save_game(); // 启动即落盘：关卡布局、种子、步数都要存下来
    return scr;
}

void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "sok";
    out->name = "推箱子";
    out->icon = "icon.png";
    out->create = sok_create;
}
