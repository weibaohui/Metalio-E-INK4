// tools/sok_app/main/sok_main.c
// 推箱子 动态应用：经典 Sokoban，一步一刷，适配墨水屏低刷新率特性。
//
// 设计要点（墨水屏 480x800 / 1bpp 单色）：
//   - 一步一刷：走一步只改"人 + 那个箱子 + 露出的一张暗牌"三四个对象，
//     脏区很小；每 N 步全刷一次去残影（沿用数独/扫雷那套）。
//   - 关卡是**逆向生成**的：从"箱子都在目标点"的终局出发，反向**拉**箱子若干步，
//     得到的局面**保证有解**（构造出来的），不用手写关卡表。
//     ⚠ 关键几何：逆向是"拉"不是"推"——玩家退一步、箱子跟进。正向把这串拉倒过来
//     放才是解。写成"推"的话箱子会被顶到墙边、人要站到墙外推不动（89%~98% 死关）。
//     另有"死箱体检"兜底：只看墙都推不到目标点的箱子 → 直接换关。
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

#include "sok_gen.h"  // 关卡生成器（纯逻辑，与 PC 端测试共用同一份源码）
#include "../../_shared/gray_dither.h" // 1bpp 上的"灰"：Bayer 网点（复用固件首页那套做法）

#define GW SOK_GW
#define GH SOK_GH
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
// 视觉语言（1bpp 墨水屏，按成熟推箱子游戏的通用画法翻译过来 —— 每个格子是一个
// 独立的 tile，墙是深色 tile，箱子/人是画在 tile 上的物件；1bpp 没颜色，就用
// "填充 + 形状"分层，每个元素一个唯一形状）：
//   地板 = 白底 + 1px 淡格线      → 格子结构一眼可见（少了它，墙就成一坨黑背景）
//   墙   = 实心黑**圆角块，四周留 2px 白缝** → 一格一格的砖，不是连成一片的黑
//   箱子 = 实心黑方块 + 3 条白色横带（木板箱）→ 带纹理，跟纯黑的墙砖一眼分得开
//   目标 = 靶心（粗黑环 + 中心点）；箱子推上去后**反色**（白底黑条）= 已归位
//   人   = **一个小人**（圆头 + 身子 + 伸开的手臂 + 两条腿），见 mk_person_part
// ⚠ 别再把墙画成满格实心黑块：10x10 的棋盘一圈墙连起来就是一大片黑，
//   看着像"黑背景"，反而看不出格子和箱子在哪。
//
// 灰度模式（s_gray_on）：墨水屏物理只有黑白，"灰"靠 Bayer 网点模拟。
//   墙砖 → 25% 网点浅灰（不再是死黑一片），目标格 → 50% 网点中灰，
//   箱子/人 → 仍是纯黑（最深），层次是 白地板 < 浅灰墙 < 中灰目标 < 黑箱子。
static lv_obj_t *s_cell_obj[GH][GW];   // 格子容器
static lv_obj_t *s_wall_obj[GH][GW];   // 墙块
// 箱子用**对象池**（最多 6 个箱子）：每格一个箱子对象太费 LVGL 堆，
// 池子里 6 个箱子 + 各自的 3 条横带一共才 24 个对象。
#define S_MAX_CRATES 8
#define CRATE_STRIPES 3
static lv_obj_t *s_crate_obj[S_MAX_CRATES];             // 箱子主体
static lv_obj_t *s_crate_stripe[S_MAX_CRATES][CRATE_STRIPES]; // 箱子的横带
#define S_MAX_GOALS 8
static lv_obj_t *s_goal_ring[S_MAX_GOALS]; // 靶心外环
static lv_obj_t *s_goal_dot[S_MAX_GOALS];  // 靶心中心点
static lv_obj_t *s_player = NULL;
static lv_obj_t *s_info_lbl = NULL;
static lv_obj_t *s_status_lbl = NULL;
static lv_obj_t *s_gray_btn = NULL;
static int s_gray_on = 0; // 1 = 用网点灰画墙和目标格

static lv_obj_t *s_scr = NULL;
static int s_move_count = 0;
static int s_last_full_refresh = 0;
#define FULL_REFRESH_EVERY 8

// ---- 字体 ----
static const lv_font_t *ui_font(void) {
    const lv_font_t *f = fontpack_lv_font_ui();
    return f != NULL ? f : fontpack_lv_font_get(25, 2);
}
static const lv_font_t *title_font(void) {
    const lv_font_t *f = fontpack_lv_font_get(30, 2);
    return f != NULL ? f : ui_font();
}

// ================= 基础 =================
static const int kDirDx[4] = {0, 0, -1, 1};
static const int kDirDy[4] = {-1, 1, 0, 0};

static int in_grid(int x, int y) { return x >= 0 && x < GW && y >= 0 && y < GH; }

// ================= 关卡生成（逆向拉箱子，保证有解） =================
// 真正的生成逻辑在 sok_gen.h（纯逻辑、无 LVGL 依赖，PC 上可批量验算同一份源码）；
// 这里只把结果搬进 App 的棋盘状态，别在这里再写一份。
static void gen_level(int level, uint32_t seed) {
    sok_level_t lv;
    if (sok_gen_level(level, seed, &lv) != 0) return;
    memcpy(s_wall, lv.wall, sizeof(s_wall));
    memcpy(s_goal, lv.goal, sizeof(s_goal));
    memcpy(s_box, lv.box, sizeof(s_box));
    s_px = lv.px;
    s_py = lv.py;
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
    if (cell == NULL) return;
    const int t = tile_at(x, y);
    if (s_drawn[y][x] == t) return;
    s_drawn[y][x] = (uint8_t)t;

    // 格子底色：普通地板 = 白；目标格 = 中灰网点（灰度模式）。
    // 目标格的位置在一关里是固定的，所以这个底色基本不用重画。
    if (s_gray_on && s_goal[y][x] && !s_wall[y][x]) {
        gray_dither_fill(cell, GRAY_DITHER_DARK);
    } else {
        gray_solid_fill(cell, lv_color_white());
    }

    if (t == 1) { // 墙：40x40 圆角块（四周留白缝）
        // 黑白模式是实心黑砖；灰度模式换成 25% 网点浅灰砖 —— 这一下就把
        // "一大片死黑"变成"一片浅灰"，箱子（纯黑）立刻就跳出来了。
        if (s_gray_on) {
            gray_dither_fill(s_wall_obj[y][x], GRAY_DITHER_MID);
        } else {
            gray_solid_fill(s_wall_obj[y][x], lv_color_black());
        }
        lv_obj_clear_flag(s_wall_obj[y][x], LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_wall_obj[y][x], LV_OBJ_FLAG_HIDDEN);
    }
}

// 把箱子摆到各自的格子上：常态黑底白条，压在目标点上就反色（白底黑条 = 已归位）
static void sync_crates(void) {
    int bx[S_MAX_CRATES], by[S_MAX_CRATES];
    int n = 0;
    for (int y = 0; y < GH && n < S_MAX_CRATES; ++y) {
        for (int x = 0; x < GW && n < S_MAX_CRATES; ++x) {
            if (!s_box[y][x]) continue;
            bx[n] = x;
            by[n] = y;
            n++;
        }
    }
    for (int i = 0; i < S_MAX_CRATES; ++i) {
        lv_obj_t *c = s_crate_obj[i];
        if (c == NULL) continue;
        if (i >= n) {
            lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const int on_goal = s_goal[by[i]][bx[i]] ? 1 : 0;
        lv_obj_set_pos(c, BOARD_X + bx[i] * CELL + 6, BOARD_Y + by[i] * CELL + 6);
        lv_obj_set_style_bg_color(c, on_goal ? lv_color_white() : lv_color_black(), 0);
        for (int k = 0; k < CRATE_STRIPES; ++k) {
            lv_obj_t *st = s_crate_stripe[i][k];
            if (st == NULL) continue;
            lv_obj_set_style_bg_color(st, on_goal ? lv_color_black() : lv_color_white(), 0);
        }
        lv_obj_clear_flag(c, LV_OBJ_FLAG_HIDDEN);
    }
}

// 目标点靶心：整关最多 6 个，位置固定；箱子压上去时把靶心改成白色画在黑箱上
static void layout_goals(void) {
    int n = 0;
    for (int y = 0; y < GH && n < S_MAX_GOALS; ++y) {
        for (int x = 0; x < GW && n < S_MAX_GOALS; ++x) {
            if (!s_goal[y][x]) continue;
            const int on_crate = s_box[y][x] ? 1 : 0;
            lv_obj_t *ring = s_goal_ring[n];
            lv_obj_t *dot = s_goal_dot[n];
            // 灰度模式下靶心是「白环 + 黑心」：网点底上黑色细环会被网点吃掉，
            // 白色是网点里最"亮"的，白环才立得住（黑心再压住中心，靶心依旧清楚）。
            // 黑白模式维持「黑环 + 黑心」。
            const int ring_pad = s_gray_on ? 11 : 12;
            const int ring_size = s_gray_on ? 22 : 20;
            const int ring_w = s_gray_on ? 4 : 3;
            lv_obj_set_pos(ring, BOARD_X + x * CELL + ring_pad, BOARD_Y + y * CELL + ring_pad);
            lv_obj_set_size(ring, ring_size, ring_size);
            lv_obj_set_style_border_width(ring, ring_w, 0);
            lv_obj_set_pos(dot, BOARD_X + x * CELL + 18, BOARD_Y + y * CELL + 18);
            if (on_crate) {
                // 箱子压住了：靶心藏起来，箱子自己反色表示"已归位"
                lv_obj_add_flag(ring, LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_set_style_border_color(ring, s_gray_on ? lv_color_white() : lv_color_black(), 0);
                lv_obj_set_style_bg_color(dot, lv_color_black(), 0);
                lv_obj_clear_flag(ring, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clear_flag(dot, LV_OBJ_FLAG_HIDDEN);
            }
            n++;
        }
    }
    for (int i = n; i < S_MAX_GOALS; ++i) {
        lv_obj_add_flag(s_goal_ring[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_goal_dot[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void render_player(void) {
    if (s_player == NULL) return;
    lv_obj_set_pos(s_player, BOARD_X + s_px * CELL + 6, BOARD_Y + s_py * CELL + 6);
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
    layout_goals();
    sync_crates();
    render_player();
    update_info_line();
    update_status_line();
}

// ================= 持久化 =================
static void save_game(void) {
    FILE *fp = (FILE *)dynapp_fopen_data("sok.txt", "w");
    if (fp == NULL) return;
    // SOK3：多了灰度开关位。SOK2 的老存档也认（灰度默认关），别让老玩家丢进度。
    fprintf(fp, "SOK3 %d %u %d %d %d %d %d %d %d\n", s_level, (unsigned)s_seed, s_moves, s_best, s_solved,
            s_px, s_py, s_undo_n, s_gray_on);
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
    int level = 0, moves = 0, best = 0, solved = 0, px = 0, py = 0, undo_n = 0, gray = 0;
    unsigned seed = 0;
    // SOK2：生成器修好之前的老存档里存的是"死关"，直接作废重开一局。
    // SOK3：多了灰度开关位。SOK2 也照读，只是灰度默认关。
    if (fscanf(fp, "%15s", magic) != 1 ||
        (strcmp(magic, "SOK3") != 0 && strcmp(magic, "SOK2") != 0)) {
        fclose(fp);
        return 0;
    }
    const int is_v3 = (strcmp(magic, "SOK3") == 0);
    if (fscanf(fp, "%d %u %d %d %d %d %d %d", &level, &seed, &moves, &best, &solved, &px, &py,
               &undo_n) != 8) {
        fclose(fp);
        return 0;
    }
    if (is_v3) {
        if (fscanf(fp, "%d", &gray) != 1) gray = 0;
        if (gray != 0) gray = 1;
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
        // '0'..'7'：方向 0..3 | 推箱标志(4)。老存档存的是 0..3，照读无妨。
        if (c < '0' || c > '7') break;
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
    s_gray_on = gray;
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
    int pushed = 0;
    if (s_box[ny][nx]) {
        const int bx = nx + kDirDx[dir], by = ny + kDirDy[dir];
        if (!in_grid(bx, by) || s_wall[by][bx] || s_box[by][bx]) return;
        s_box[ny][nx] = 0;
        s_box[by][bx] = 1;
        pushed = 1;
        render_cell(nx, ny);
        render_cell(bx, by);
        layout_goals(); // 箱子压上/离开目标点 → 靶心要显隐
        sync_crates();  // 箱子挪位 + 归位反色
    }
    s_px = nx;
    s_py = ny;
    s_moves++;
    s_move_count++;
    // 悔棋栈：低 2 位记方向，第 3 位（值 4）记"这一步推了箱子"。
    // ⚠ 必须记这一位：光看方向没法区分"走过去"和"推着箱子走"，
    //   悔棋时会把没被推的箱子也拖回来。
    if (s_undo_n < MAX_UNDO) s_undo[s_undo_n++] = (uint8_t)(dir | (pushed ? 4 : 0));
    render_player();
    update_info_line();

    // 周期性全刷：墨水屏局刷把大块黑色抹掉后会留灰影（旧版定义了常量却从没调用），
    // 每走 N 步全刷一次清干净。
    if (s_move_count - s_last_full_refresh >= FULL_REFRESH_EVERY) {
        s_last_full_refresh = s_move_count;
        dynapp_epd_full_refresh();
    }

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
    const uint8_t rec = s_undo[--s_undo_n];
    const int dir = rec & 3;             // 低 2 位 = 方向
    const int pushed = (rec & 4) ? 1 : 0; // 第 3 位 = 这一步推了箱子
    // 反向：人退回上一个位置
    const int px = s_px - kDirDx[dir], py = s_py - kDirDy[dir];
    if (pushed) {
        // 箱子当时被推到「人现在这格 + 方向」，现在退回「人现在这格」。
        // ⚠ 老代码查的是"人上一格有没有箱子"——箱子推走后根本不在那儿，
        //   所以推箱子的那一步永远退不回来（HEAD 里就有的老 bug）。
        const int bx = s_px + kDirDx[dir], by = s_py + kDirDy[dir];
        if (in_grid(bx, by) && s_box[by][bx]) {
            s_box[by][bx] = 0;
            s_box[s_py][s_px] = 1;
            render_cell(s_px, s_py);
            render_cell(bx, by);
            layout_goals();
            sync_crates();
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

// 「灰度」开关：黑白 ↔ 网点灰，一键对比
static void update_gray_btn(void) {
    if (s_gray_btn == NULL) return;
    lv_obj_t *lbl = lv_obj_get_child(s_gray_btn, 0);
    if (s_gray_on) {
        lv_obj_set_style_bg_color(s_gray_btn, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(s_gray_btn, LV_OPA_COVER, 0);
        if (lbl != NULL) lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    } else {
        lv_obj_set_style_bg_color(s_gray_btn, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(s_gray_btn, LV_OPA_COVER, 0);
        if (lbl != NULL) lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    }
}

static void on_gray_clicked(lv_event_t *e) {
    (void)e;
    s_gray_on = !s_gray_on;
    update_gray_btn();
    // 整套重画：墙砖要换填充、目标格底色要换、箱子和靶心的显隐也要重算
    memset(s_drawn, 0xFF, sizeof(s_drawn));
    render_all();
    save_game();
    dynapp_epd_full_refresh();
    dynapp_toast_show(s_gray_on ? "网点灰 已开" : "网点灰 已关", 1200);
}

// 画小人身上的一个零件（实心黑块）
//   round = 0  → 方块
//   round = 1  → 正圆（头）
//   round > 1  → 圆角矩形，半径 = round 像素（身子）
// ⚠ LVGL 会把半径夹到 min(w,h)/2，所以 round=4 的 10x8 身子 = 圆胖腰身。
static lv_obj_t *mk_person_part(lv_obj_t *parent, int x, int y, int w, int h, int round) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    if (round == 1) {
        lv_obj_set_style_radius(p, LV_RADIUS_CIRCLE, 0);
    } else if (round > 1) {
        lv_obj_set_style_radius(p, round, 0);
    }
    lv_obj_clear_flag(p, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

// 画小人身上的白色零件（眼睛 / 嘴）
// ⚠ 1bpp 没有灰度，"可爱"只能靠**形状**：大头 + 白色方眼 + 笑嘴 + 呆毛。
//   五官是"黑头上的白"，所以必须**创建在头之后**（LVGL 后建的画在上层）。
static lv_obj_t *mk_person_white(lv_obj_t *parent, int x, int y, int w, int h) {
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_remove_style_all(p);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    return p;
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
    // ⚠ 只清理"当前屏"自己的状态。
    // 固件对"同 app 重开"的处理是：**新屏先加载、旧屏随后卸载**
    // （见 main/core/app_manager.cc 的 DispatchScreenLifecycle 注释）。
    // 如果这里无条件清空 s_* 指针，旧屏卸载会把**新屏**的对象指针一起清掉 →
    // App 变僵尸：画面停在旧状态、点击无反应、标签不再更新。
    // 所以：删的不是当前屏，就什么都不做。
    if (lv_event_get_target(e) != s_scr) return;
    save_game();
    s_scr = NULL;
    s_info_lbl = NULL;
    s_status_lbl = NULL;
    s_player = NULL;
    s_gray_btn = NULL;
    memset(s_cell_obj, 0, sizeof(s_cell_obj));
    memset(s_wall_obj, 0, sizeof(s_wall_obj));
    memset(s_crate_obj, 0, sizeof(s_crate_obj));
    memset(s_crate_stripe, 0, sizeof(s_crate_stripe));
    memset(s_goal_ring, 0, sizeof(s_goal_ring));
    memset(s_goal_dot, 0, sizeof(s_goal_dot));
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
    // 3. 棋盘格：容器（透明）+ 墙块 + 箱子 + 箱孔
    for (int y = 0; y < GH; ++y) {
        for (int x = 0; x < GW; ++x) {
            lv_obj_t *cell = lv_obj_create(scr);
            lv_obj_remove_style_all(cell);
            lv_obj_set_pos(cell, BOARD_X + x * CELL, BOARD_Y + y * CELL);
            lv_obj_set_size(cell, CELL, CELL);
            // 地板：白底 + 1px 格线（格子结构可见；少了格线墙就变成一大片黑）
            lv_obj_set_style_bg_color(cell, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(cell, lv_color_black(), 0);
            lv_obj_set_style_border_width(cell, 1, 0);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
            s_cell_obj[y][x] = cell;

            // 墙：40x40 圆角黑块，四周留 2px 白缝 → 一格一格的砖，不连成黑片
            lv_obj_t *wall = lv_obj_create(cell);
            lv_obj_remove_style_all(wall);
            lv_obj_set_pos(wall, 2, 2);
            lv_obj_set_size(wall, CELL - 4, CELL - 4);
            lv_obj_set_style_bg_color(wall, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(wall, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(wall, 6, 0);
            lv_obj_clear_flag(wall, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(wall, LV_OBJ_FLAG_HIDDEN);
            s_wall_obj[y][x] = wall;

        }
    }

    // 4. 箱子对象池：每个箱子 32x32 黑底 + 3 条白色横带（木板箱）
    //    横带用子对象画（纯矩形，1bpp 下最干净；真斜纹要重刷固件才能画）
    for (int i = 0; i < S_MAX_CRATES; ++i) {
        lv_obj_t *crate = lv_obj_create(scr);
        lv_obj_remove_style_all(crate);
        lv_obj_set_size(crate, CELL - 12, CELL - 12);
        lv_obj_set_style_bg_color(crate, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(crate, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(crate, lv_color_black(), 0);
        lv_obj_set_style_border_width(crate, 2, 0); // 黑底时看不出，反色时就是边框
        lv_obj_clear_flag(crate, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(crate, LV_OBJ_FLAG_HIDDEN);
        s_crate_obj[i] = crate;

        for (int k = 0; k < CRATE_STRIPES; ++k) {
            lv_obj_t *strip = lv_obj_create(crate);
            lv_obj_remove_style_all(strip);
            lv_obj_set_pos(strip, 0, 2 + k * 9);
            lv_obj_set_size(strip, CELL - 16, 4);
            lv_obj_set_style_bg_color(strip, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(strip, LV_OPA_COVER, 0);
            lv_obj_clear_flag(strip, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
            s_crate_stripe[i][k] = strip;
        }
    }

    // 4. 目标点靶心（环 + 点）：整关固定位置，箱子压上去时转白
    for (int i = 0; i < S_MAX_GOALS; ++i) {
        lv_obj_t *ring = lv_obj_create(scr);
        lv_obj_remove_style_all(ring);
        lv_obj_set_size(ring, 20, 20);
        lv_obj_set_style_bg_opa(ring, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(ring, lv_color_black(), 0);
        lv_obj_set_style_border_width(ring, 3, 0);
        lv_obj_set_style_radius(ring, LV_RADIUS_CIRCLE, 0);
        lv_obj_clear_flag(ring, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(ring, LV_OBJ_FLAG_HIDDEN);
        s_goal_ring[i] = ring;

        lv_obj_t *dot = lv_obj_create(scr);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_bg_color(dot, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
        s_goal_dot[i] = dot;
    }

    // 5. 人：画一个可爱的小人
    //    原来画的是"实心黑点 + 白描边"，一眼看不出哪个是自己 → 改成小人。
    //    1bpp 下画小人只能靠"圆 + 矩形"拼（没有斜线），这正好是国际通用的
    //    人形图标（头 + 展开的手臂 + 分开的腿），缩到 32px 也认得出。
    //    ⚠ 不用再画白描边了：现在是 tile 式布局，墙砖四周留了 3~4px 白缝，
    //      小人和墙/箱子之间天然隔着白，不会粘连。
    //
    //    「可爱版」造型（32x32 画布，坐标 = 容器内相对位置）：
    //      呆毛  2x3  @(15, 0)   头顶一撮翘毛，最提神的一笔
    //      头    圆13 @(10, 2)   大头 = Q 版可爱的前提
    //      眼    2x2 白 @(13,7) @(18,7)
    //      嘴    4x1 白 @(14,11)  一条白线当笑嘴
    //      身子  10x8 @(11,15)  圆角 4px = 圆胖腰身
    //      手臂  18x3 @(7, 17)   左右各伸 4px，别太长（太长像"十字"）
    //      腿    4x8  @(11,23) @(17,23)  接在身子下沿，中间留 2px 缝
    //    ⚠ 五官是"黑头上的白"，必须**创建在头之后**才能盖住头。
    s_player = lv_obj_create(scr);
    lv_obj_remove_style_all(s_player);
    lv_obj_set_size(s_player, 32, 32);
    lv_obj_clear_flag(s_player, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    // 头（圆）
    mk_person_part(s_player, 10, 2, 13, 13, 1);
    // 呆毛（压住头顶）
    mk_person_part(s_player, 15, 0, 2, 3, 0);
    // 两只眼睛（白）
    mk_person_white(s_player, 13, 7, 2, 2);
    mk_person_white(s_player, 18, 7, 2, 2);
    // 笑嘴（白）
    mk_person_white(s_player, 14, 11, 4, 1);
    // 身子（圆角）
    mk_person_part(s_player, 11, 15, 10, 8, 4);
    // 两只手臂（横着伸出去）
    mk_person_part(s_player, 7, 17, 18, 3, 0);
    // 两条腿（接在身子下沿，中间留 2px 缝）
    mk_person_part(s_player, 11, 23, 4, 8, 0);
    mk_person_part(s_player, 17, 23, 4, 8, 0);

    // 6. 状态行
    s_status_lbl = lv_label_create(scr);
    if (f_ui != NULL) lv_obj_set_style_text_font(s_status_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_status_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_pos(s_status_lbl, 16, 560);

    // 7. 方向键（十字）
    const lv_coord_t db = 60;
    make_btn(scr, "↑", 70, 600, db, db, on_dir_clicked, (void *)(intptr_t)DIR_UP);
    make_btn(scr, "←", 10, 660, db, db, on_dir_clicked, (void *)(intptr_t)DIR_LEFT);
    make_btn(scr, "↓", 70, 660, db, db, on_dir_clicked, (void *)(intptr_t)DIR_DOWN);
    make_btn(scr, "→", 130, 660, db, db, on_dir_clicked, (void *)(intptr_t)DIR_RIGHT);

    // 8. 功能按钮
    make_btn(scr, "悔棋", 200, 600, 125, 60, on_undo_clicked, NULL);
    make_btn(scr, "重开", 335, 600, 125, 60, on_restart_clicked, NULL);
    make_btn(scr, "下一关", 200, 670, 260, 60, on_next_clicked, NULL);

    // 9. 灰度开关：黑白 ↔ 网点灰（1bpp 没有真灰，只能用 Bayer 网点模拟）
    s_gray_btn = make_btn(scr, "灰度", 16, 740, 160, 48, on_gray_clicked, NULL);
    update_gray_btn();

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
