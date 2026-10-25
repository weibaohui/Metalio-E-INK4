// tools/sok_app/test/sok_gen_test.c
// 主机端关卡测试：直接编译 App 上机用的同一份 sok_gen.h。
//
// 干三件事：
//   1) 批量生成关卡（多关卡 × 多种子）
//   2) **构造式验解**：把生成时的「拉箱序列」倒过来放一遍，每一步都是一次真正的
//      推箱子；全部推完、且所有箱子都落在目标点上 → 这一关有解
//   3) 死箱体检：只看墙就推不到目标点的箱子（推不回来的死箱）
//
// 为什么要有这个：曾经把逆向生成的「拉」写成「推」，89%~98% 的关卡是死局，
// 而这种 bug 在真机上只表现为「这一关怎么都过不去」，极难定位。
//
// 编译（macOS / Linux）：
//   cc -O2 -o /tmp/sok_gen_test tools/sok_app/test/sok_gen_test.c
// 运行：
//   /tmp/sok_gen_test [每关种子数]
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../main/sok_gen.h"

// ================= 寻路（绕开墙和箱子） =================
static int walk_to(const sok_level_t *lv, const uint8_t box[SOK_GH][SOK_GW], int sx, int sy, int gx,
                   int gy) {
    static int queue[SOK_GW * SOK_GH];
    static int seen[SOK_GW * SOK_GH];
    memset(seen, 0, sizeof(seen));
    int head = 0, tail = 0;
    const int start = sy * SOK_GW + sx;
    seen[start] = 1;
    queue[tail++] = start;
    while (head < tail) {
        const int cur = queue[head++];
        const int cx = cur % SOK_GW, cy = cur / SOK_GW;
        if (cx == gx && cy == gy) return 1;
        for (int d = 0; d < 4; ++d) {
            const int nx = cx + sok_dx[d], ny = cy + sok_dy[d];
            if (!sok_in_grid(nx, ny)) continue;
            const int ni = ny * SOK_GW + nx;
            if (seen[ni] || lv->wall[ny][nx] || box[ny][nx]) continue;
            seen[ni] = 1;
            queue[tail++] = ni;
        }
    }
    return 0;
}

// ================= 构造式验解 =================
static int verify_solvable(const sok_level_t *lv, int *pushes_out) {
    uint8_t box[SOK_GH][SOK_GW];
    memcpy(box, lv->box, sizeof(box));
    int px = lv->px, py = lv->py;
    int pushes = 0;
    for (int i = lv->pull_n - 1; i >= 0; --i) {
        const int bx = lv->pull[i].bx, by = lv->pull[i].by, d = lv->pull[i].d;
        const int ax = bx - sok_dx[d], ay = by - sok_dy[d];      // 箱子现在该在 A
        const int cx = bx - 2 * sok_dx[d], cy = by - 2 * sok_dy[d]; // 推箱子的人要站 C
        if (!box[ay][ax]) return 0;
        if (box[by][bx] || lv->wall[by][bx]) return 0;
        if (!walk_to(lv, box, px, py, cx, cy)) return 0;
        box[ay][ax] = 0;
        box[by][bx] = 1;
        px = ax;
        py = ay;
        pushes++;
    }
    for (int y = 0; y < SOK_GH; ++y)
        for (int x = 0; x < SOK_GW; ++x)
            if ((box[y][x] != 0) != (lv->goal[y][x] != 0)) return 0; // 箱子与目标点必须一一对应
    if (pushes_out) *pushes_out = pushes;
    return 1;
}

static void print_board(const sok_level_t *lv) {
    for (int y = 0; y < SOK_GH; ++y) {
        for (int x = 0; x < SOK_GW; ++x) {
            if (x == lv->px && y == lv->py) {
                printf("☺");
            } else if (lv->wall[y][x]) {
                printf("█");
            } else if (lv->box[y][x] && lv->goal[y][x]) {
                printf("⊙");
            } else if (lv->box[y][x]) {
                printf("□");
            } else if (lv->goal[y][x]) {
                printf("·");
            } else {
                printf(" ");
            }
        }
        printf("\n");
    }
}

int main(int argc, char **argv) {
    const int per_level = (argc > 1) ? atoi(argv[1]) : 400;
    const int levels[] = {1, 2, 3, 5, 8, 12, 20};
    const int nlevels = (int)(sizeof(levels) / sizeof(levels[0]));

    int total_fail = 0;
    printf("=== 推箱子关卡生成器自检（每关 %d 个种子）===\n", per_level);
    for (int li = 0; li < nlevels; ++li) {
        const int level = levels[li];
        int solvable = 0, deadbox = 0, total_pulls = 0;
        for (int i = 0; i < per_level; ++i) {
            const uint32_t seed = (uint32_t)(20261021u + (uint32_t)i * 2654435761u);
            sok_level_t lv;
            if (sok_gen_level(level, seed, &lv) != 0) {
                printf("  生成失败 level=%d seed=%u\n", level, seed);
                total_fail++;
                continue;
            }
            int pushes = 0;
            if (verify_solvable(&lv, &pushes)) {
                solvable++;
                total_pulls += pushes;
            } else {
                total_fail++;
            }
            for (int y = 0; y < SOK_GH; ++y)
                for (int x = 0; x < SOK_GW; ++x)
                    if (lv.box[y][x] && sok_box_is_deadlocked(&lv, x, y)) {
                        deadbox++;
                        y = SOK_GH;
                        break;
                    }
        }
        printf("  第 %2d 关：%4d/%4d 有解（构造式验算，平均推箱 %.1f 次），死箱关卡 %d\n", level,
               solvable, per_level, solvable ? (double)total_pulls / solvable : 0.0, deadbox);
    }

    printf("\n=== 示例关卡（第 1 关，seed=20261021）===\n");
    sok_level_t demo;
    sok_gen_level(1, 20261021u, &demo);
    print_board(&demo);
    int pushes = 0;
    printf("  拉箱序列 %d 拉 → 倒放验算：%s（%d 次推箱即通关）\n", demo.pull_n,
           verify_solvable(&demo, &pushes) ? "有解" : "无解！", pushes);

    printf("\n%s（失败 %d）\n", total_fail == 0 ? "✅ 全部通过" : "❌ 有失败", total_fail);
    return total_fail == 0 ? 0 : 1;
}
