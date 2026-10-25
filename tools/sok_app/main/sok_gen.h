// tools/sok_app/main/sok_gen.h
// 推箱子 App 的关卡生成器：纯逻辑，不依赖 LVGL / 固件 / SDK。
//
// 为什么单独成文件：关卡必须**保证有解**，而这是整个 App 最容易被改坏的地方
// （曾经把"拉"写成"推"，89%~98% 的关卡是死局）。抽成无依赖的头文件后，
// 同一份源码可以在 PC 上直接编译、批量生成、批量验解（tools/sok_app/test/）。
//
// ⚠ 逆向生成的正确姿势是「拉」不是「推」，改之前先读下面的几何说明。
#pragma once

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define SOK_GW 10
#define SOK_GH 10
#define SOK_MAX_PULLS 512

typedef struct {
    uint8_t wall[SOK_GH][SOK_GW];
    uint8_t goal[SOK_GH][SOK_GW];
    uint8_t box[SOK_GH][SOK_GW];
    int px, py;   // 人的位置
    int level;
    uint32_t seed;
    // 逆向「拉」的序列：第 i 拉把箱子从 (bx,by) 拉到 (bx-dx, by-dy)。
    // 正向把这串倒过来放就是这一关的解 —— PC 上据此可做构造式验解。
    int pull_n;
    struct {
        int8_t bx, by, d;
    } pull[SOK_MAX_PULLS];
} sok_level_t;

static const int sok_dx[4] = {0, 0, -1, 1};
static const int sok_dy[4] = {-1, 1, 0, 0};

static inline int sok_in_grid(int x, int y) {
    return x >= 0 && x < SOK_GW && y >= 0 && y < SOK_GH;
}

// ================= 随机数（xorshift32，可复现） =================
typedef struct {
    uint32_t s;
} sok_rng_t;

static inline void sok_rng_seed(sok_rng_t *r, uint32_t seed) { r->s = seed ? seed : 1u; }

static inline uint32_t sok_rng_next(sok_rng_t *r) {
    uint32_t x = r->s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->s = x;
    return x;
}

static inline int sok_rng_below(sok_rng_t *r, int n) {
    return n > 0 ? (int)(sok_rng_next(r) % (uint32_t)n) : 0;
}

// ================= 洪水填充 =================
// 只绕墙
static void sok_flood(const sok_level_t *lv, int x, int y, uint8_t *used) {
    static int qx[SOK_GW * SOK_GH], qy[SOK_GW * SOK_GH];
    int head = 0, tail = 0;
    memset(used, 0, SOK_GW * SOK_GH);
    if (!sok_in_grid(x, y) || lv->wall[y][x]) return;
    used[y * SOK_GW + x] = 1;
    qx[tail] = x;
    qy[tail] = y;
    tail++;
    while (head < tail) {
        const int cx = qx[head], cy = qy[head];
        head++;
        for (int d = 0; d < 4; ++d) {
            const int nx = cx + sok_dx[d], ny = cy + sok_dy[d];
            if (!sok_in_grid(nx, ny) || lv->wall[ny][nx] || used[ny * SOK_GW + nx]) continue;
            used[ny * SOK_GW + nx] = 1;
            qx[tail] = nx;
            qy[tail] = ny;
            tail++;
        }
    }
}

// 墙 + 箱子都绕开（逆向生成时用：正向玩家走回头路时，箱子是挡路的）
static void sok_flood_free(const sok_level_t *lv, int x, int y, uint8_t *used) {
    static int qx[SOK_GW * SOK_GW], qy[SOK_GW * SOK_GW];
    int head = 0, tail = 0;
    memset(used, 0, SOK_GW * SOK_GW);
    if (!sok_in_grid(x, y) || lv->wall[y][x] || lv->box[y][x]) return;
    used[y * SOK_GW + x] = 1;
    qx[tail] = x;
    qy[tail] = y;
    tail++;
    while (head < tail) {
        const int cx = qx[head], cy = qy[head];
        head++;
        for (int d = 0; d < 4; ++d) {
            const int nx = cx + sok_dx[d], ny = cy + sok_dy[d];
            if (!sok_in_grid(nx, ny) || lv->wall[ny][nx] || lv->box[ny][nx] ||
                used[ny * SOK_GW + nx])
                continue;
            used[ny * SOK_GW + nx] = 1;
            qx[tail] = nx;
            qy[tail] = ny;
            tail++;
        }
    }
}

// 死箱体检：只看墙，乐观估计这个箱子还能被推到哪些格子。
// 连墙这一关都过不去（推不到任何目标点）= 死箱，这一关作废。
static int sok_box_is_deadlocked(const sok_level_t *lv, int bx, int by) {
    static uint8_t seen[SOK_GW * SOK_GH];
    static int qx[SOK_GW * SOK_GH], qy[SOK_GW * SOK_GH];
    memset(seen, 0, sizeof(seen));
    int head = 0, tail = 0;
    qx[tail] = bx;
    qy[tail] = by;
    tail++;
    seen[by * SOK_GW + bx] = 1;
    int has_goal = 0;
    while (head < tail) {
        const int cx = qx[head], cy = qy[head];
        head++;
        if (lv->goal[cy][cx]) has_goal = 1;
        for (int d = 0; d < 4; ++d) {
            const int nx = cx + sok_dx[d], ny = cy + sok_dy[d]; // 箱子往 d 推
            const int sx = cx - sok_dx[d], sy = cy - sok_dy[d]; // 推的人要站的位置
            if (!sok_in_grid(nx, ny) || lv->wall[ny][nx]) continue;
            if (!sok_in_grid(sx, sy) || lv->wall[sy][sx]) continue; // 站不上去 → 这方向推不动
            if (seen[ny * SOK_GW + nx]) continue;
            seen[ny * SOK_GW + nx] = 1;
            qx[tail] = nx;
            qy[tail] = ny;
            tail++;
        }
    }
    return !has_goal;
}

// ================= 关卡生成 =================
// 返回 0 = 成功，1 = 失败（理论上不会发生）
static int sok_gen_level(int level, uint32_t seed, sok_level_t *out) {
    sok_rng_t rng;
    sok_rng_seed(&rng, seed);
    memset(out, 0, sizeof(*out));
    out->level = level;
    out->seed = seed;

    // 1) 外墙
    for (int x = 0; x < SOK_GW; ++x) {
        out->wall[0][x] = 1;
        out->wall[SOK_GH - 1][x] = 1;
    }
    for (int y = 0; y < SOK_GH; ++y) {
        out->wall[y][0] = 1;
        out->wall[y][SOK_GW - 1] = 1;
    }

    // 2) 内部随机墙
    const int inner = (SOK_GW - 2) * (SOK_GH - 2);
    int walls = 5 + level * 2;
    if (walls > inner / 4) walls = inner / 4;
    for (int i = 0; i < walls; ++i) {
        const int x = 1 + sok_rng_below(&rng, SOK_GW - 2);
        const int y = 1 + sok_rng_below(&rng, SOK_GH - 2);
        out->wall[y][x] = 1;
    }

    // 3) 找最大的连通空地
    static uint8_t visited[SOK_GW * SOK_GH];
    static uint8_t region[SOK_GW * SOK_GH];
    static uint8_t best_region[SOK_GW * SOK_GH];
    memset(visited, 0, sizeof(visited));
    memset(best_region, 0, sizeof(best_region));
    int best_cnt = 0;
    for (int y = 1; y < SOK_GH - 1; ++y) {
        for (int x = 1; x < SOK_GW - 1; ++x) {
            if (out->wall[y][x] || visited[y * SOK_GW + x]) continue;
            sok_flood(out, x, y, region);
            int cnt = 0;
            for (int i = 0; i < SOK_GW * SOK_GH; ++i)
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
        memset(out->wall, 0, sizeof(out->wall));
        for (int x = 0; x < SOK_GW; ++x) {
            out->wall[0][x] = 1;
            out->wall[SOK_GH - 1][x] = 1;
        }
        for (int y = 0; y < SOK_GH; ++y) {
            out->wall[y][0] = 1;
            out->wall[y][SOK_GW - 1] = 1;
        }
        memset(best_region, 0, sizeof(best_region));
        for (int y = 1; y < SOK_GH - 1; ++y)
            for (int x = 1; x < SOK_GW - 1; ++x) best_region[y * SOK_GW + x] = 1;
        best_cnt = inner;
    }

    // 4) 目标点（箱子先放目标上 = 终局）
    int nbox = 3 + level / 2;
    if (nbox > 6) nbox = 6;
    if (nbox > best_cnt / 5) nbox = best_cnt / 5;
    if (nbox < 1) nbox = 1;

    int cx[SOK_GW * SOK_GH], cy[SOK_GW * SOK_GH];
    int nc = 0;
    for (int i = 0; i < SOK_GW * SOK_GH; ++i) {
        if (best_region[i]) {
            cx[nc] = i % SOK_GW;
            cy[nc] = i / SOK_GW;
            nc++;
        }
    }
    for (int i = 0; i < nbox && nc > 0; ++i) {
        const int k = sok_rng_below(&rng, nc);
        out->goal[cy[k]][cx[k]] = 1;
        out->box[cy[k]][cx[k]] = 1;
        cx[k] = cx[nc - 1];
        cy[k] = cy[nc - 1];
        nc--;
    }

    // 5) 人的位置
    int px = 1, py = 1;
    for (int tries = 0; tries < 200; ++tries) {
        px = 1 + sok_rng_below(&rng, SOK_GW - 2);
        py = 1 + sok_rng_below(&rng, SOK_GH - 2);
        if (!out->wall[py][px] && !out->box[py][px]) break;
    }

    // 6) 逆向拉箱子（真·拉）：玩家退一步、箱子跟进 → 得到的局面保证可解
    //
    //    【别改回"推"！】正向解法 = 把这串"拉"倒过来放。每一拉 (bx,by,d) 表示：
    //       逆向：玩家站到 A=(bx-dx, by-dy)，退到 C=(bx-2dx, by-2dy)，箱子落到 A；
    //       正向：玩家站在 C，把 A 上的箱子往 +d 推一格 → 箱子正好回到 B=(bx,by)（即目标点）。
    //    所以每一拉必须保证：
    //       · A、C 都不是墙也不是箱子；
    //       · 玩家能**绕开箱子**走到 A（正向回头路才走得通）。
    //    若写成"推"（玩家站在箱子反方向、把箱子推离目标点），正向就再也推不回来：
    //    箱子被顶到墙边、人要站到墙外去推，89%~98% 的关卡直接死。
    static uint8_t reach[SOK_GW * SOK_GH];
    const int pulls = 20 + level * 12;
    for (int i = 0; i < pulls; ++i) {
        int bx = -1, by = -1;
        for (int tries = 0; tries < 30; ++tries) {
            const int x = 1 + sok_rng_below(&rng, SOK_GW - 2);
            const int y = 1 + sok_rng_below(&rng, SOK_GH - 2);
            if (out->box[y][x]) {
                bx = x;
                by = y;
                break;
            }
        }
        if (bx < 0) continue;
        const int d = sok_rng_below(&rng, 4);
        const int ax = bx - sok_dx[d];     // 箱子落到这里（= 逆向玩家的站位）
        const int ay = by - sok_dy[d];
        const int cx2 = bx - 2 * sok_dx[d]; // 玩家退到这里（= 正向推箱子时的人站位）
        const int cy2 = by - 2 * sok_dy[d];
        if (!sok_in_grid(ax, ay) || !sok_in_grid(cx2, cy2)) continue;
        if (out->wall[ay][ax] || out->box[ay][ax]) continue;
        if (out->wall[cy2][cx2] || out->box[cy2][cx2]) continue;
        sok_flood_free(out, px, py, reach);
        if (!reach[ay * SOK_GW + ax]) continue;
        out->box[by][bx] = 0;
        out->box[ay][ax] = 1;
        px = cx2;
        py = cy2;
        if (out->pull_n < SOK_MAX_PULLS) {
            out->pull[out->pull_n].bx = (int8_t)bx;
            out->pull[out->pull_n].by = (int8_t)by;
            out->pull[out->pull_n].d = (int8_t)d;
            out->pull_n++;
        }
    }

    // 7) 死箱体检：只看墙都推不到任何目标点的箱子 = 死箱，这一关作废重来（兜底，正常不触发）
    for (int y = 0; y < SOK_GH; ++y) {
        for (int x = 0; x < SOK_GW; ++x) {
            if (out->box[y][x] && sok_box_is_deadlocked(out, x, y)) {
                return sok_gen_level(level, seed * 1664525u + 1013904223u, out);
            }
        }
    }

    // 8) 别生成"已经解开"的废关
    int dist = 0;
    for (int y = 0; y < SOK_GH; ++y) {
        for (int x = 0; x < SOK_GW; ++x) {
            if (!out->box[y][x]) continue;
            int best = 99;
            for (int gy = 0; gy < SOK_GH; ++gy) {
                for (int gx = 0; gx < SOK_GW; ++gx) {
                    if (!out->goal[gy][gx]) continue;
                    const int dd = abs(gx - x) + abs(gy - y);
                    if (dd < best) best = dd;
                }
            }
            dist += best;
        }
    }
    if (dist < 6) {
        return sok_gen_level(level, seed * 1664525u + 1013904223u, out);
    }

    out->px = px;
    out->py = py;
    return 0;
}
