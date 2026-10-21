// tools/slink_app/main/slink_main.c
// 数回 动态应用：Slitherlink（数回），一步一刷的纸上逻辑谜题。
//
// 玩法：在点阵上画一条**单一闭合回路**。每个带数字的格子说明它的四条边中
//       有几条属于回路（0~3，4 很少见）。空格子不限制。
//       回路不能分叉、不能有多个圈。
//
// 为什么适合墨水屏：
//   - 这是"摆在那儿想"的谜题，不需要实时反馈，屏可以一直不动
//   - 画面是稀疏的线条 + 数字，1bpp 上极清晰
//   - 改一条边只改 2 个对象（边 + 相邻格的数字），脏区极小
//
// 关卡是**程序生成**的：
//   1. 在顶点图上做随机自避游走，闭合成一条简单回路（保证单圈、不分叉）
//   2. 按回路反算每个格子的边数 → 提示数字
//   3. 随机去掉约 1/4 的提示 → 难度中等
//   4. 随机自避游走失败时退回"随机矩形边界"（永远合法）
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "esp_random.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

#define NC 8  // 8x8 个格子
#define NV (NC + 1)
#define NEDGE_H (NV * NC)  // 水平边：NV 行 x NC 列
#define NEDGE_V (NC * NV)  // 垂直边：NC 行 x NV 列
#define NEDGE (NEDGE_H + NEDGE_V)
#define ECELL 46           // 格子边长
#define BOARD_X ((480 - (NC + 1) * ECELL) / 2)
#define BOARD_Y 116

// 边的编号：0..NEDGE_H-1 是水平边（行 r，列 c），之后是垂直边
// 水平边 h(r,c) 连接顶点 (r,c) 与 (r,c+1)
// 垂直边 v(r,c) 连接顶点 (r,c) 与 (r+1,c)

static uint8_t s_edge[NEDGE];  // 1=画上了
static uint8_t s_clue[NC * NC]; // 提示数字，0xFF=空
static uint8_t s_sol[NEDGE];   // 标准解（生成时用），0xFF=不用于判定
static int s_gen_seed = 0;
static int s_moves = 0;
static int s_won = 0;
static int s_check = 0;  // 0=没查过 1=数字对不上 2=回路没闭合 3=全对
static char s_check_msg[64] = {0};

// 悔棋栈：记这次改的边和改前的状态
typedef struct {
    uint16_t edge;
    uint8_t before;
} SlinkMove;
static SlinkMove s_hist[256];
static int s_hist_n = 0;

// 绘制对象
static lv_obj_t *s_bar[NEDGE];    // 画出来的线
static lv_obj_t *s_board = NULL; // 整块棋面的点击对象
static lv_obj_t *s_clue_lbl[NC * NC];
static lv_obj_t *s_info_lbl = NULL;
static lv_obj_t *s_status_lbl = NULL;
static lv_obj_t *s_rule_obj = NULL;
static lv_obj_t *s_err_lbl = NULL;
static lv_obj_t *s_mark = NULL; // 提示标记

static lv_obj_t *s_scr = NULL;

// ---- 字体 ----
static const lv_font_t *ui_font(void) {
    const lv_font_t *f = fontpack_lv_font_ui();
    return f != NULL ? f : fontpack_lv_font_get(25, 2);
}
static const lv_font_t *title_font(void) {
    const lv_font_t *f = fontpack_lv_font_get(30, 2);
    return f != NULL ? f : ui_font();
}
static const lv_font_t *num_font(void) {
    const lv_font_t *f = fontpack_lv_font_get(18, 2); // 纯数字
    return f != NULL ? f : ui_font();
}

// ================= 边的坐标 =================
static void edge_pos(int e, int *x, int *y) {
    if (e < NEDGE_H) {
        const int r = e / NC, c = e % NC;
        *x = BOARD_X + c * ECELL;
        *y = BOARD_Y + r * ECELL;
    } else {
        const int k = e - NEDGE_H;
        const int c = k / NC, r = k % NC;   // 垂直边：列*NC+行
        *x = BOARD_X + c * ECELL;
        *y = BOARD_Y + r * ECELL;
    }
}
static int edge_is_h(int e) { return e < NEDGE_H; }

// 边的线段端点（屏幕坐标）
static void edge_seg(int e, int *x0, int *y0, int *x1, int *y1) {
    if (e < NEDGE_H) {
        const int r = e / NC, c = e % NC;
        *x0 = BOARD_X + c * ECELL;
        *y0 = BOARD_Y + r * ECELL;
        *x1 = BOARD_X + (c + 1) * ECELL;
        *y1 = *y0;
    } else {
        const int k = e - NEDGE_H;
        const int c = k / NC, r = k % NC;
        *x0 = BOARD_X + c * ECELL;
        *y0 = BOARD_Y + r * ECELL;
        *x1 = *x0;
        *y1 = BOARD_Y + (r + 1) * ECELL;
    }
}

// 点到线段的距离
static int dist_point_seg(int px, int py, int x0, int y0, int x1, int y1) {
    const int dx = x1 - x0, dy = y1 - y0;
    const int len2 = dx * dx + dy * dy;
    int t = 0;
    if (len2 > 0) {
        t = ((px - x0) * dx + (py - y0) * dy) / len2;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
    }
    const int qx = x0 + t * dx, qy = y0 + t * dy;
    const int ex = px - qx, ey = py - qy;
    return ex * ex + ey * ey;
}

// 边的中点（提示标记用）：顶点是 4 条边的交点，热区放顶点上会互相重叠，
// 玩家点一下不知道点的是哪条边；放到线段中点就唯一了。
static void edge_mid(int e, int *x, int *y) {
    if (e < NEDGE_H) {
        const int r = e / NC, c = e % NC;
        *x = BOARD_X + c * ECELL + ECELL / 2;
        *y = BOARD_Y + r * ECELL;
    } else {
        const int k = e - NEDGE_H;
        const int c = k / NC, r = k % NC;
        *x = BOARD_X + c * ECELL;
        *y = BOARD_Y + r * ECELL + ECELL / 2;
    }
}

// 顶点 (vr,vc) 关联的 4 条边
// 顶点 (vr,vc) 周围的 4 条边。
// 水平边 h(r,c)：r 取 0..NC（9 行），c 取 0..NC-1
// 垂直边 v(r,c)：r 取 0..NC-1（8 行），c 取 0..NC（9 列）
// 两个方向的取值范围不一样，写混了会越界或漏边（漏了最底行的上边，
// 顶点度数就永远算不对，单圈判定就永远不成立）。
static void vertex_edges(int vr, int vc, int out[4]) {
    // 顶点 (vr,vc) 周围的 4 条边。方向别搞反：
    //   上/下 是**竖边** v(r,c)（竖着连 (r,c)-(r+1,c)）
    //   左/右 是**横边** h(r,c)（横着连 (r,c)-(r,c+1)）
    // 之前把横竖搞混了，顶点度数永远算不对 → 单圈判定永远不成立。
    // 取值范围也不同：横边 h 的行有 NC+1=9 行、列只有 NC=8 列；
    //                竖边 v 的行只有 NC=8 行、列有 NC+1=9 列。
    out[0] = (vr > 0) ? NEDGE_H + vc * NC + (vr - 1) : -1;   // 上
    out[1] = (vr < NC) ? NEDGE_H + vc * NC + vr : -1;         // 下
    out[2] = (vc > 0) ? vr * NC + (vc - 1) : -1;              // 左
    out[3] = (vc < NC) ? vr * NC + vc : -1;                   // 右
}

// ================= 随机 =================
static uint32_t s_rng = 1;
static void rng_seed(uint32_t s) { s_rng = s ? s : 1; }
static uint32_t rng_next(void) {
    uint32_t x = s_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    s_rng = x;
    return x;
}
static int rng_below(int n) { return n > 0 ? (int)(rng_next() % (uint32_t)n) : 0; }

// ================= 关卡生成 =================
static void edges_clear(uint8_t *e) { memset(e, 0, NEDGE); }

// 随机自避游走闭合成回路；成功返回 1
static int gen_random_loop(uint8_t *out) {
    for (int attempt = 0; attempt < 60; ++attempt) {
        edges_clear(out);
        uint8_t used[NV * NV];
        memset(used, 0, sizeof(used));
        int vr = rng_below(NV), vc = rng_below(NV);
        used[vr * NV + vc] = 1;
        int pr = vr, pc = vc;
        int closed = 0;
        for (int step = 0; step < 200; ++step) {
            // 能回到起点就收
            if (step > 6) {
                const int dr = vr - pr, dc = vc - pc;
                if ((dr == 0 && abs(dc) == 1) || (dc == 0 && abs(dr) == 1)) {
                    // 画这条边并闭合
                    int e = -1;
                    if (dr == 0) {
                        const int c = (dc > 0) ? pc : vc;
                        e = pr * NC + c;                 // 水平边
                    } else {
                        const int r = (dr > 0) ? pr : vr;
                        e = NEDGE_H + pc * NC + r;       // 垂直边
                    }
                    if (e >= 0 && e < NEDGE && !out[e]) {
                        out[e] = 1;
                        closed = 1;
                        break;
                    }
                }
            }
            // 走一步：随机方向，不能撞已访问的顶点
            static const int d[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
            int order[4] = {0, 1, 2, 3};
            for (int i = 3; i > 0; --i) { // 洗牌
                const int j = rng_below(i + 1);
                const int tmp = order[i];
                order[i] = order[j];
                order[j] = tmp;
            }
            int moved = 0;
            for (int k = 0; k < 4; ++k) {
                const int nr = pr + d[order[k]][0], nc = pc + d[order[k]][1];
                if (nr < 0 || nr >= NV || nc < 0 || nc >= NV) continue;
                if (used[nr * NV + nc]) continue;
                // 记下这条边
                int e = -1;
                if (d[order[k]][0] != 0) {
                    const int r = (d[order[k]][0] > 0) ? pr : nr;
                    e = r * NC + nc;
                } else {
                    const int c = (d[order[k]][1] > 0) ? pc : nc;
                    e = NEDGE_H + c * NC + pr; // 垂直边：列*NC+行
                }
                if (e < 0 || e >= NEDGE || out[e]) continue;
                out[e] = 1;
                used[nr * NV + nc] = 1;
                pr = nr;
                pc = nc;
                moved = 1;
                break;
            }
            if (!moved) break; // 卡住了
        }
        if (closed) {
            // 校验：每个顶点的回路度必须是 0 或 2
            int ok = 1;
            for (int v = 0; v < NV * NV && ok; ++v) {
                const int r = v / NV, c = v % NV;
                int es[4];
                vertex_edges(r, c, es);
                int deg = 0;
                for (int i = 0; i < 4; ++i)
                    if (es[i] >= 0 && out[es[i]]) deg++;
                if (deg != 0 && deg != 2) ok = 0;
            }
            if (ok) return 1;
        }
    }
    return 0;
}

// 兜底：随机矩形边界（永远合法）
static void gen_rect_loop(uint8_t *out) {
    edges_clear(out);
    const int w = 2 + rng_below(NC - 2); // 矩形宽（格）
    const int h = 2 + rng_below(NC - 2);
    const int x0 = rng_below(NC - w + 1), y0 = rng_below(NC - h + 1);
    for (int c = x0; c < x0 + w; ++c) {
        out[y0 * NC + c] = 1;              // 上边
        out[(y0 + h) * NC + c] = 1;        // 下边
    }
    for (int r = y0; r < y0 + h; ++r) {
        out[NEDGE_H + x0 * NC + r] = 1;    // 左边
        out[NEDGE_H + (x0 + w) * NC + r] = 1; // 右边
    }
}

// 由回路反算提示数字
static void compute_clues(const uint8_t *edges, uint8_t *clue) {
    for (int r = 0; r < NC; ++r) {
        for (int c = 0; c < NC; ++c) {
            int n = 0;
            if (edges[r * NC + c]) n++;                        // 上
            if (edges[(r + 1) * NC + c]) n++;                  // 下
            if (edges[NEDGE_H + c * NC + r]) n++;              // 左
            if (edges[NEDGE_H + (c + 1) * NC + r]) n++;        // 右
            clue[r * NC + c] = (uint8_t)n;
        }
    }
}

// 只按种子重算「标准解 + 提示数字」，不碰玩家画的线。
// RNG 消耗顺序必须与 gen_puzzle 完全一致，否则载入存档后提示会对不上。
static void gen_clues_from_seed(int seed) {
    rng_seed((uint32_t)seed);
    uint8_t sol[NEDGE];
    if (!gen_random_loop(sol)) {
        gen_rect_loop(sol);
    }
    memcpy(s_sol, sol, NEDGE);
    compute_clues(sol, s_clue);
    // 随机去掉约 1/4 的提示（4 的提示保留，它很关键）
    for (int i = 0; i < NC * NC; ++i) {
        if (s_clue[i] == 0xFF) continue;
        if (s_clue[i] >= 4) continue;
        if (rng_below(100) < 25) s_clue[i] = 0xFF;
    }
}

static void gen_puzzle(int seed) {
    s_gen_seed = seed;
    gen_clues_from_seed(seed);
    memset(s_edge, 0, NEDGE);
    s_moves = 0;
    s_hist_n = 0;
    s_won = 0;
    s_check = 0;
    s_check_msg[0] = '\0';
}

// ================= 判定 =================
static int clue_satisfied(int r, int c) {
    if (s_clue[r * NC + c] == 0xFF) return 1;
    int n = 0;
    if (s_edge[r * NC + c]) n++;
    if (s_edge[(r + 1) * NC + c]) n++;
    // 垂直边编号 = NEDGE_H + 列*NC + 行（列用 NC=8，不是 NV=9！用 NV 会每列错一位）
    if (s_edge[NEDGE_H + c * NC + r]) n++;
    if (s_edge[NEDGE_H + (c + 1) * NC + r]) n++;
    return n == s_clue[r * NC + c];
}

static int all_clues_ok(void) {
    for (int r = 0; r < NC; ++r)
        for (int c = 0; c < NC; ++c)
            if (!clue_satisfied(r, c)) return 0;
    return 1;
}

// 回路是否完整：所有用到边的顶点度数为 2，且这些顶点连成一条
static int is_single_loop(void) {
    int deg[NV * NV];
    int used_any = 0;
    for (int v = 0; v < NV * NV; ++v) {
        const int r = v / NV, c = v % NV;
        int es[4];
        vertex_edges(r, c, es);
        int d = 0;
        for (int i = 0; i < 4; ++i)
            if (es[i] >= 0 && s_edge[es[i]]) d++;
        deg[v] = d;
        if (d == 1) return 0; // 端点 → 不闭合
        if (d == 3) return 0; // 分叉
        if (d == 2) used_any = 1;
    }
    if (!used_any) return 0;
    // 连通性：从一个度为 2 的顶点 BFS
    int start = -1;
    for (int v = 0; v < NV * NV; ++v)
        if (deg[v] == 2) {
            start = v;
            break;
        }
    if (start < 0) return 0;
    uint8_t seen[NV * NV];
    memset(seen, 0, sizeof(seen));
    int q[NV * NV], head = 0, tail = 0;
    q[tail++] = start;
    seen[start] = 1;
    int count = 0;
    while (head < tail) {
        const int v = q[head++];
        count++;
        const int r = v / NV, c = v % NV;
        int es[4];
        vertex_edges(r, c, es);
        for (int i = 0; i < 4; ++i) {
            if (es[i] < 0 || !s_edge[es[i]]) continue;
            // 这条边的另一个端点
            int nv = -1;
            if (es[i] < NEDGE_H) {
                // 横边 h(r,c) 连的是顶点 (r,c) 与 (r,c+1)：另一端就是"当前顶点的对面"
                const int er = es[i] / NC, ec = es[i] % NC;
                const int a = er * NV + ec;              // 边的起点顶点
                nv = (v == a) ? a + 1 : a;               // 另一端
            } else {
                // 竖边 v(r,c) 连的是顶点 (r,c) 与 (r+1,c)
                const int k = es[i] - NEDGE_H;
                const int ec = k / NC, er = k % NC;     // 列 ec、行 er
                const int a = er * NV + ec;              // 边的起点顶点
                nv = (v == a) ? a + NV : a;              // 另一端（下一行，跨 NV）
            }
            if (nv < 0 || nv >= NV * NV || seen[nv]) continue;
            seen[nv] = 1;
            q[tail++] = nv;
        }
    }
    int deg2count = 0;
    for (int v = 0; v < NV * NV; ++v)
        if (deg[v] == 2) deg2count++;
    return count == deg2count;
}

// ================= 持久化 =================
static void save_game(void) {
    FILE *fp = (FILE *)dynapp_fopen_data("slink.txt", "w");
    if (fp == NULL) return;
    fprintf(fp, "SLK1 %d %d %d %d %d\n", s_gen_seed, s_moves, s_won, s_hist_n, s_check);
    for (int i = 0; i < NEDGE; ++i) fputc(s_edge[i] ? '1' : '0', fp);
    fputc('\n', fp);
    for (int i = 0; i < NC * NC; ++i) {
        if (s_clue[i] == 0xFF) fputc('.', fp);
        else fputc((char)('0' + s_clue[i]), fp);
    }
    fputc('\n', fp);
    for (int i = 0; i < s_hist_n; ++i) {
        fprintf(fp, "%d %d ", s_hist[i].edge, s_hist[i].before);
    }
    fputc('\n', fp);
    fclose(fp);
}

static int load_game(void) {
    FILE *fp = (FILE *)dynapp_fopen_data("slink.txt", "r");
    if (fp == NULL) return 0;
    char magic[16] = {0};
    int seed = 0, moves = 0, won = 0, hist_n = 0, check = 0;
    if (fscanf(fp, "%15s", magic) != 1 || strcmp(magic, "SLK1") != 0) {
        fclose(fp);
        return 0;
    }
    if (fscanf(fp, "%d %d %d %d %d", &seed, &moves, &won, &hist_n, &check) != 5 || hist_n < 0 ||
        hist_n > (int)(sizeof(s_hist) / sizeof(s_hist[0]))) {
        fclose(fp);
        return 0;
    }
    for (int i = 0; i < NEDGE; ++i) {
        int v = 0;
        if (fscanf(fp, "%1d", &v) != 1) {
            fclose(fp);
            return 0;
        }
        s_edge[i] = (uint8_t)(v ? 1 : 0);
    }
    char clues[NC * NC + 4] = {0};
    // 提示有 64 个字符，%63s 会丢掉最后一个，导致数字校验永远不过
    if (fscanf(fp, "%67s", clues) != 1) {
        fclose(fp);
        return 0;
    }
    for (int i = 0; i < NC * NC; ++i) {
        s_clue[i] = (clues[i] == '.') ? 0xFF : (uint8_t)(clues[i] - '0');
    }
    s_hist_n = 0;
    for (int i = 0; i < hist_n; ++i) {
        int e = 0, b = 0;
        if (fscanf(fp, "%d %d", &e, &b) != 2) break;
        if (e < 0 || e >= NEDGE) break;
        s_hist[s_hist_n].edge = (uint16_t)e;
        s_hist[s_hist_n].before = (uint8_t)b;
        s_hist_n++;
    }
    fclose(fp);
    s_gen_seed = seed;
    s_moves = moves;
    s_won = won;
    s_check = check;
    // 提示数字直接用存档里的（正常情况下与种子生成的一致）；
    // 只按种子复原标准解，供「提示」用
    rng_seed((uint32_t)seed);
    {
        uint8_t sol[NEDGE];
        if (!gen_random_loop(sol)) gen_rect_loop(sol);
        memcpy(s_sol, sol, NEDGE);
    }
    return 1;
}

// ================= 绘制 =================
static void render_edge(int e) {
    lv_obj_t *bar = s_bar[e];
    if (bar == NULL) return;
    int x = 0, y = 0;
    edge_pos(e, &x, &y);
    if (s_edge[e]) {
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_HIDDEN);
        if (edge_is_h(e)) {
            lv_obj_set_pos(bar, x, y - 3);
            lv_obj_set_size(bar, ECELL, 6);
        } else {
            lv_obj_set_pos(bar, x - 3, y);
            lv_obj_set_size(bar, 6, ECELL);
        }
    } else {
        lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
    }
}

static void update_info_line(void) {
    if (s_info_lbl == NULL) return;
    int n = 0;
    for (int i = 0; i < NEDGE; ++i) n += s_edge[i] ? 1 : 0;
    char buf[64];
    snprintf(buf, sizeof(buf), "第 %d 手 · %d 段", s_moves, n);
    lv_label_set_text(s_info_lbl, buf);
}

static void update_status_line(void) {
    if (s_status_lbl == NULL) return;
    const char *t = "点线段画/擦 · 画出一条单一闭合回路";
    if (s_won) t = "回路完成！";
    else if (s_check_msg[0] != '\0') t = s_check_msg;
    lv_label_set_text(s_status_lbl, t);
}

static void render_all(void) {
    for (int e = 0; e < NEDGE; ++e) render_edge(e);
    for (int r = 0; r < NC; ++r) {
        for (int c = 0; c < NC; ++c) {
            lv_obj_t *l = s_clue_lbl[r * NC + c];
            if (l == NULL) continue;
            if (s_clue[r * NC + c] == 0xFF) {
                lv_label_set_text(l, "");
            } else {
                char b[2] = {(char)('0' + s_clue[r * NC + c]), '\0'};
                lv_label_set_text(l, b);
            }
        }
    }
    update_info_line();
    update_status_line();
}

// ================= 交互 =================
static void check_win(void) {
    if (!all_clues_ok()) return;
    if (!is_single_loop()) return;
    s_won = 1;
    update_status_line();
    save_game();
    dynapp_modal_alert("完成！", "所有数字都对上，回路也是单一闭环。", "确定");
}

static void toggle_edge(int idx) {
    if (s_won) return;
    const int before = s_edge[idx] ? 1 : 0;
    s_edge[idx] = (uint8_t)(!before);
    if (s_hist_n < (int)(sizeof(s_hist) / sizeof(s_hist[0]))) {
        s_hist[s_hist_n].edge = (uint16_t)idx;
        s_hist[s_hist_n].before = (uint8_t)before;
        s_hist_n++;
    }
    s_moves++;
    s_check = 0;
    s_check_msg[0] = '\0';
    if (s_mark != NULL) lv_obj_add_flag(s_mark, LV_OBJ_FLAG_HIDDEN);
    render_edge(idx);
    update_info_line();
    save_game();
    check_win();
}

// 棋面点击：找离触摸点最近的那条边
static void on_board_clicked(lv_event_t *e) {
    lv_indev_t *indev = lv_event_get_indev(e);
    if (indev == NULL) return;
    lv_point_t pt;
    lv_indev_get_point(indev, &pt);
    int best = -1;
    int bestd = 0x7FFFFFFF;
    for (int i = 0; i < NEDGE; ++i) {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        edge_seg(i, &x0, &y0, &x1, &y1);
        const int d = dist_point_seg(pt.x, pt.y, x0, y0, x1, y1);
        if (d < bestd) {
            bestd = d;
            best = i;
        }
    }
    if (best >= 0) toggle_edge(best);
}

static void on_undo_clicked(lv_event_t *e) {
    (void)e;
    if (s_hist_n <= 0) {
        dynapp_toast_show("没有可悔的步骤", 1000);
        return;
    }
    const SlinkMove m = s_hist[--s_hist_n];
    s_edge[m.edge] = m.before;
    if (s_moves > 0) s_moves--;
    s_won = 0;
    s_check = 0;
    s_check_msg[0] = '\0';
    render_edge(m.edge);
    update_info_line();
    update_status_line();
    save_game();
}

static void on_new_clicked(lv_event_t *e) {
    (void)e;
    gen_puzzle((int)esp_random());
    render_all();
    save_game();
    dynapp_toast_show("换了一道新题", 1200);
    dynapp_epd_full_refresh();
}

static void on_hint_clicked(lv_event_t *e) {
    (void)e;
    if (s_mark != NULL) lv_obj_add_flag(s_mark, LV_OBJ_FLAG_HIDDEN);
    // 找一条"标准解里有、玩家还没画"的边，在屏幕上圈出来
    for (int i = 0; i < NEDGE; ++i) {
        if (s_sol[i] && !s_edge[i]) {
            if (s_mark != NULL) {
                int x = 0, y = 0;
                edge_mid(i, &x, &y);
                lv_obj_clear_flag(s_mark, LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_pos(s_mark, x - 7, y - 7);
            }
            dynapp_toast_show("这里还差一段", 1400);
            return;
        }
    }
    dynapp_toast_show("该画的都画了，检查一下有没有多画", 1600);
}

static void on_check_clicked(lv_event_t *e) {
    (void)e;
    const int clues_ok = all_clues_ok();
    const int loop_ok = is_single_loop();
    if (clues_ok && loop_ok) {
        s_won = 1;
        s_check = 3;
        snprintf(s_check_msg, sizeof(s_check_msg), "全部对上，回路完成！");
        update_status_line();
        save_game();
        dynapp_modal_alert("完成！", "所有数字都对上，回路也是单一闭环。", "确定");
        return;
    }
    // 结果常驻显示（墨水屏上 toast 一闪就没了，看不清）
    int bad = 0;
    for (int r = 0; r < NC; ++r)
        for (int c = 0; c < NC; ++c)
            if (!clue_satisfied(r, c)) bad++;
    s_check = clues_ok ? 2 : 1;
    if (bad > 0) {
        snprintf(s_check_msg, sizeof(s_check_msg), "有 %d 个数字对不上", bad);
    } else {
        snprintf(s_check_msg, sizeof(s_check_msg), "数字都对，但回路还没闭合");
    }
    update_status_line();
    save_game();
}

static void on_reload_clicked(lv_event_t *e) {
    (void)e;
    // 动态应用的界面实例被框架缓存复用：重进 app 不会重新读存档，
    // 外部改过的文件要点「重载」才生效（工牌/名片同理）。
    if (load_game()) {
        render_all();
        dynapp_toast_show("已重新载入", 1200);
        dynapp_epd_full_refresh();
    } else {
        dynapp_toast_show("没有可载入的存档", 1200);
    }
}

static void on_rule_clicked(lv_event_t *e) {
    (void)e;
    if (s_rule_obj == NULL) return;
    if (lv_obj_has_flag(s_rule_obj, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_clear_flag(s_rule_obj, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_rule_obj, LV_OBJ_FLAG_HIDDEN);
    }
    dynapp_epd_full_refresh();
}

static int slink_on_vk_key(const char *key) {
    if (key == NULL) return 0;
    if (strcmp(key, "vk_home") == 0 || strcmp(key, "esc") == 0 || strcmp(key, "back") == 0) {
        save_game();
        dynapp_screen_go_home();
        return 1;
    }
    if (strcmp(key, "vk_prev") == 0) {
        on_undo_clicked(NULL);
        return 1;
    }
    if (strcmp(key, "vk_next") == 0) {
        on_check_clicked(NULL);
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
    s_rule_obj = NULL;
    s_err_lbl = NULL;
    s_mark = NULL;
    memset(s_bar, 0, sizeof(s_bar));
    s_board = NULL;
    memset(s_clue_lbl, 0, sizeof(s_clue_lbl));
}

static void *slink_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    s_scr = scr;
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 800);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_screen_deleted, LV_EVENT_DELETE, NULL);

    dynapp_screen_status_bar(scr);
    dynapp_vk_attach(scr, "slink", slink_create, slink_on_vk_key);

    if (!load_game()) {
        gen_puzzle((int)esp_random());
    }

    const lv_font_t *f_ui = ui_font();
    const lv_font_t *f_title = title_font();
    const lv_font_t *f_num = num_font();

    // 1. 标题
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "数回");
    if (f_title != NULL) lv_obj_set_style_text_font(title, f_title, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_set_pos(title, 16, 48);

    s_info_lbl = lv_label_create(scr);
    if (f_ui != NULL) lv_obj_set_style_text_font(s_info_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_info_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_align(s_info_lbl, LV_ALIGN_TOP_RIGHT, -16, 56);

    // 1.5 网格线（1px 淡线）：让格子结构可见，回路线是 6px 粗线，视觉上能区分
    for (int i = 0; i <= NC; ++i) {
        lv_obj_t *vline = lv_obj_create(scr);
        lv_obj_remove_style_all(vline);
        lv_obj_set_pos(vline, BOARD_X + i * ECELL, BOARD_Y);
        lv_obj_set_size(vline, 1, (NC + 1) * ECELL);
        lv_obj_set_style_bg_color(vline, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(vline, LV_OPA_COVER, 0);
        lv_obj_clear_flag(vline, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *hline = lv_obj_create(scr);
        lv_obj_remove_style_all(hline);
        lv_obj_set_pos(hline, BOARD_X, BOARD_Y + i * ECELL);
        lv_obj_set_size(hline, (NC + 1) * ECELL, 1);
        lv_obj_set_style_bg_color(hline, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(hline, LV_OPA_COVER, 0);
        lv_obj_clear_flag(hline, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    // 2. 边：线段 + 看不见的点击热区
    for (int e = 0; e < NEDGE; ++e) {
        int x = 0, y = 0;
        edge_pos(e, &x, &y);

        lv_obj_t *bar = lv_obj_create(scr);
        lv_obj_remove_style_all(bar);
        lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        if (edge_is_h(e)) {
            lv_obj_set_pos(bar, x, y - 3);
            lv_obj_set_size(bar, ECELL, 6);
        } else {
            lv_obj_set_pos(bar, x - 3, y);
            lv_obj_set_size(bar, 6, ECELL);
        }
        lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);
        s_bar[e] = bar;

    }
    // 整块棋面只做**一个**可点对象：按触摸坐标算离哪条边最近。
    // （原来给 144 条边各建一个热区，会把固件的触觉区表(96项)撑爆，
    //   刷屏 "zone table full"，而且点顶点时四个热区重叠、点不准。）
    {
        lv_obj_t *board = lv_obj_create(scr);
        lv_obj_remove_style_all(board);
        lv_obj_set_pos(board, BOARD_X, BOARD_Y);
        lv_obj_set_size(board, (NC + 1) * ECELL, (NC + 1) * ECELL);
        lv_obj_set_style_bg_opa(board, LV_OPA_TRANSP, 0);
        lv_obj_add_flag(board, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(board, on_board_clicked, LV_EVENT_CLICKED, NULL);
        lv_obj_clear_flag(board, LV_OBJ_FLAG_SCROLLABLE);
        s_board = board;
    }

    // 2.5 提示标记（小方框）
    s_mark = lv_obj_create(scr);
    lv_obj_remove_style_all(s_mark);
    lv_obj_set_size(s_mark, 14, 14);
    lv_obj_set_style_bg_opa(s_mark, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_mark, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_mark, 2, 0);
    lv_obj_clear_flag(s_mark, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_mark, LV_OBJ_FLAG_HIDDEN);

    // 3. 提示数字
    for (int r = 0; r < NC; ++r) {
        for (int c = 0; c < NC; ++c) {
            lv_obj_t *l = lv_label_create(scr);
            lv_label_set_text(l, "");
            if (f_num != NULL) lv_obj_set_style_text_font(l, f_num, 0);
            lv_obj_set_style_text_color(l, lv_color_black(), 0);
            lv_obj_set_pos(l, BOARD_X + c * ECELL + ECELL / 2 - 6, BOARD_Y + r * ECELL + ECELL / 2 - 10);
            lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
            s_clue_lbl[r * NC + c] = l;
        }
    }

    // 4. 状态行
    s_status_lbl = lv_label_create(scr);
    if (f_ui != NULL) lv_obj_set_style_text_font(s_status_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_status_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_pos(s_status_lbl, 16, BOARD_Y + (NC + 1) * ECELL + 10);

    // 5. 规则浮层
    s_rule_obj = lv_obj_create(scr);
    lv_obj_remove_style_all(s_rule_obj);
    lv_obj_set_pos(s_rule_obj, 16, 96);
    lv_obj_set_size(s_rule_obj, 448, 400);
    lv_obj_set_style_bg_color(s_rule_obj, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_rule_obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_rule_obj, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_rule_obj, 2, 0);
    lv_obj_clear_flag(s_rule_obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    {
        const char *lines[] = {
            "数回怎么玩",
            "",
            "在点阵上画一条闭合回路。",
            "",
            "格子里的数字 = 这个格子的",
            "四条边里有几条属于回路。",
            "没有数字的格子不限制。",
            "",
            "要点：回路必须连成一条，",
            "不能分叉，也不能画成两个圈。",
        };
        for (int i = 0; i < (int)(sizeof(lines) / sizeof(lines[0])); ++i) {
            lv_obj_t *l = lv_label_create(s_rule_obj);
            lv_label_set_text(l, lines[i]);
            if (f_ui != NULL) lv_obj_set_style_text_font(l, f_ui, 0);
            lv_obj_set_style_text_color(l, lv_color_black(), 0);
            lv_obj_set_pos(l, 12, 12 + i * 30);
        }
    }
    lv_obj_add_flag(s_rule_obj, LV_OBJ_FLAG_HIDDEN);

    // 6. 按钮
    const lv_coord_t bw = 74, bh = 56, bgap = 4;
    const lv_coord_t by = 700;
    make_btn(scr, "撤销", 16, by, bw, bh, on_undo_clicked, NULL);
    make_btn(scr, "检查", 16 + bw + bgap, by, bw, bh, on_check_clicked, NULL);
    make_btn(scr, "提示", 16 + 2 * (bw + bgap), by, bw, bh, on_hint_clicked, NULL);
    make_btn(scr, "新题", 16 + 3 * (bw + bgap), by, bw, bh, on_new_clicked, NULL);
    make_btn(scr, "重载", 16 + 4 * (bw + bgap), by, bw, bh, on_reload_clicked, NULL);
    make_btn(scr, "规则", 16 + 5 * (bw + bgap), by, bw, bh, on_rule_clicked, NULL);

    render_all();
    save_game();
    return scr;
}

void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "slink";
    out->name = "数回";
    out->icon = "icon.png";
    out->create = slink_create;
}
