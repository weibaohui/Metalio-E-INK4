// tools/minesweeper_app/main/minesweeper_main.c
// 扫雷 动态应用：回合制、一步一刷，适配墨水屏低刷新率特性。
//
// v2.0 界面重做——照经典 Windows 扫雷的三个招牌元素翻译到 1bpp：
//   1. 立体砖块：未翻开的格子 = 25% 网点灰面 + 右下 3px 黑边（凸起），
//      翻开后 = 纯白无边框（凹陷），一屏立刻有了「挖开」的层次
//   2. LED 计数器：左边剩雷数（雷-旗）、右边计时（秒），七段数码管画法
//      与番茄钟同源；计时只在操作时刷新显示——墨水屏不能每秒刷
//   3. 笑脸按钮：中间那张脸会变（玩=微笑 / 输=闭眼皱眉 / 赢=墨镜），点它开新局
//
// 其余 1bpp 取舍：
//   - 数字 1~8 没有颜色可用，改用大号粗体居中（去掉 v1 的数字下点阵——
//     50px 格子里点阵糊成省略号噪点）
//   - 踩雷的那格整格反色（黑底白雷 = 经典的红底炸弹）
//   - 旗子和雷都是几何图形，不依赖字体符号
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "esp_random.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"
#include "../../_shared/gray_dither.h"  // 1bpp 上的"灰"：Bayer 网点

#define N 9
#define CELL 50
#define BOARD (N * CELL)

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
static int s_hit_r = -1;     // 踩雷的那格（整格反色）
static int s_hit_c = -1;
static int s_timer_started = 0;   // 首次翻开才开始计时
static int s_timer_final = -1;    // 胜负分出后冻结的秒数

// ---- 绘制对象（旗/雷按需懒创建，NULL=还没画过）----
static lv_obj_t *s_cell_obj[N][N];
static lv_obj_t *s_num_lbl[N][N];
static lv_obj_t *s_flag_obj[N][N];
static lv_obj_t *s_mine_obj[N][N];

// 每格上次渲染的状态指纹：变了才真正去动 LVGL 对象。
// 墨水屏局刷按脏区面积计费，全表 81 格重绘一次要几百毫秒，
// 会把紧接着的下一次点击挤掉。
typedef struct {
    uint8_t val;    // bit0=open bit1=flag bit2=mine bit3..6=around bit7=踩雷反色
} RenderState;
static RenderState s_rendered[N][N];

static lv_obj_t *s_scr = NULL;
static int s_move_count = 0;
static int s_last_full_refresh = 0;
#define FULL_REFRESH_EVERY 12

// ---- 头部：LED 计数器 + 笑脸 ----
static lv_obj_t *s_cnt_seg[3][7], *s_cnt_ghost[3][7];
static lv_obj_t *s_tim_seg[3][7], *s_tim_ghost[3][7];
static lv_obj_t *s_face_btn = NULL;
static lv_obj_t *s_eye_l, *s_eye_r, *s_mouth_mid, *s_mouth_l, *s_mouth_r, *s_shades;
static lv_obj_t *s_diff_lbl = NULL;
static lv_obj_t *s_hint_lbl = NULL;

// ---- 字体 ----
static const lv_font_t *ui_font(void) {
    const lv_font_t *f = fontpack_lv_font_ui();
    return f != NULL ? f : fontpack_lv_font_get(25, 2);
}
static const lv_font_t *num_font(void) {
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
    s_hit_r = s_hit_c = -1;
    s_timer_started = 0;
    s_timer_final = -1;
    // 用 0xFF 哨兵而不是 0：指纹 val 的合法取值最大 0x8F，
    // "未翻开 + 周围无雷"的格子指纹正好是 0 —— 若用 0 初始化，
    // 这些格子首次渲染会被指纹短路跳过，砖面就缺一块。
    memset(s_rendered, 0xFF, sizeof(s_rendered));
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

// 秒表：首翻开始走，胜负冻结；显示只在操作时同步（墨水屏不能每秒刷）
// 恢复存档的旧局没有"首点"时刻，第一次任何操作补开表
static int s_timer_start_tick;
static void ensure_timer_started(void) {
    if (s_timer_started) return;
    s_timer_started = 1;
    s_timer_start_tick = (int)lv_tick_get();
}
static int elapsed_seconds(void) {
    if (s_timer_final >= 0) return s_timer_final;
    if (!s_timer_started) return 0;
    int sec = (int)(lv_tick_elaps((uint32_t)s_timer_start_tick) / 1000);
    if (sec > 999) sec = 999;
    return sec;
}

static void reveal_at(int r, int c) {
    if (s_dead || s_won) return;
    if (r < 0 || r >= N || c < 0 || c >= N) return;
    if (s_cells[r][c].flag) return;

    // 首点：把雷挪走，保证第一次不会踩雷；顺便启动计时
    if (s_first_click) {
        s_first_click = 0;
        place_mines(r, c);
    }
    ensure_timer_started();

    if (s_cells[r][c].mine) {
        s_dead = 1;
        s_cells[r][c].open = 1;
        s_hit_r = r;
        s_hit_c = c;
        s_timer_final = elapsed_seconds();
        // 翻开所有雷
        for (int rr = 0; rr < N; ++rr) {
            for (int cc = 0; cc < N; ++cc) {
                if (s_cells[rr][cc].mine) s_cells[rr][cc].open = 1;
            }
        }
        render_all();
        dynapp_modal_alert("踩雷了", "这局输了，点笑脸再来一次。", "确定");
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
    ensure_timer_started();
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
    ensure_timer_started();
    s_move_count++;
    render_all();
}

// ================= 七段 LED 计数器（画法与番茄钟同源） =================
// 段序: 0=a 1=b 2=c 3=d 4=e 5=f 6=g
//   --a--
//  f|   |b
//   --g--
//  e|   |c
//   --d--
static const uint8_t kSegMask[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F,
};
#define LED_W 26
#define LED_H 48
#define LED_S 6

static void seg_make_digit(lv_obj_t *parent, int x, int y,
                           lv_obj_t *out_seg[7], lv_obj_t *out_ghost[7]) {
    const int gap = 2;
    const int half = (LED_H - LED_S) / 2 - gap;
    struct {
        int x, y, w, h;
    } r[7];
    r[0] = (typeof(r[0])){x + LED_S / 2, y, LED_W - LED_S, LED_S};               // a 顶
    r[1] = (typeof(r[1])){x + LED_W - LED_S, y + LED_S / 2, LED_S, half};        // b 右上
    r[2] = (typeof(r[2])){x + LED_W - LED_S, y + (LED_H + LED_S) / 2, LED_S, half};  // c 右下
    r[3] = (typeof(r[3])){x + LED_S / 2, y + LED_H - LED_S, LED_W - LED_S, LED_S};   // d 底
    r[4] = (typeof(r[4])){x, y + (LED_H + LED_S) / 2, LED_S, half};              // e 左下
    r[5] = (typeof(r[5])){x, y + LED_S / 2, LED_S, half};                        // f 左上
    r[6] = (typeof(r[6])){x + LED_S / 2, y + (LED_H - LED_S) / 2, LED_W - LED_S, LED_S};  // g 中

    for (int i = 0; i < 7; ++i) {
        lv_obj_t *on = lv_obj_create(parent);
        lv_obj_remove_style_all(on);
        lv_obj_set_pos(on, r[i].x, r[i].y);
        lv_obj_set_size(on, r[i].w, r[i].h);
        lv_obj_set_style_bg_color(on, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(on, LV_OPA_COVER, 0);
        lv_obj_clear_flag(on, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        out_seg[i] = on;

        // 未点亮的段：段中央 1px 细线（粗了整只数字看着像空盒子）
        lv_obj_t *ghost = lv_obj_create(parent);
        lv_obj_remove_style_all(ghost);
        const int thin = 1;
        if (r[i].w > r[i].h) {
            lv_obj_set_pos(ghost, r[i].x, r[i].y + (r[i].h - thin) / 2);
            lv_obj_set_size(ghost, r[i].w, thin);
        } else {
            lv_obj_set_pos(ghost, r[i].x + (r[i].w - thin) / 2, r[i].y);
            lv_obj_set_size(ghost, thin, r[i].h);
        }
        lv_obj_set_style_bg_color(ghost, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(ghost, LV_OPA_COVER, 0);
        lv_obj_clear_flag(ghost, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        out_ghost[i] = ghost;
    }
}

// ch: '0'..'9' / ' ' 全灭（ghost 也藏——白底上空位的 ghost 远看像 0） / '-' 只有 g 段
static void seg_set_char(lv_obj_t *seg[7], lv_obj_t *ghost[7], char ch) {
    uint8_t mask = 0;
    if (ch >= '0' && ch <= '9') mask = kSegMask[ch - '0'];
    else if (ch == '-') mask = 0x40;
    for (int i = 0; i < 7; ++i) {
        if (seg[i] == NULL) continue;
        if (mask & (1u << i)) {
            lv_obj_clear_flag(seg[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ghost[i], LV_OBJ_FLAG_HIDDEN);
        } else if (ch == ' ') {
            lv_obj_add_flag(seg[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(ghost[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(seg[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(ghost[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void led_show(lv_obj_t *seg[3][7], lv_obj_t *ghost[3][7], int value) {
    char buf[4];
    if (value < 0) {
        if (value < -99) value = -99;
        snprintf(buf, sizeof(buf), "-%02d", -value);
    } else {
        if (value > 999) value = 999;
        snprintf(buf, sizeof(buf), "%3d", value);
    }
    for (int i = 0; i < 3; ++i) seg_set_char(seg[i], ghost[i], buf[i]);
}

// ================= 笑脸 =================
// 嘴 = 三段阶梯拼的「U」：微笑两端上翘，皱眉整体倒扣
static void sync_face(void) {
    if (s_face_btn == NULL) return;
    if (s_dead) {
        // 闭眼 + 皱眉
        lv_obj_set_size(s_eye_l, 12, 3);
        lv_obj_set_pos(s_eye_l, 16, 24);
        lv_obj_set_size(s_eye_r, 12, 3);
        lv_obj_set_pos(s_eye_r, 36, 24);
        lv_obj_set_size(s_mouth_l, 6, 3);
        lv_obj_set_size(s_mouth_r, 6, 3);
        lv_obj_set_pos(s_mouth_mid, 24, 40);
        lv_obj_set_pos(s_mouth_l, 17, 43);
        lv_obj_set_pos(s_mouth_r, 40, 43);
        lv_obj_add_flag(s_shades, LV_OBJ_FLAG_HIDDEN);
    } else if (s_won) {
        // 墨镜 + 微笑
        lv_obj_clear_flag(s_shades, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_eye_l, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_eye_r, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(s_mouth_l, 6, 3);
        lv_obj_set_size(s_mouth_r, 6, 3);
        lv_obj_set_pos(s_mouth_mid, 24, 43);
        lv_obj_set_pos(s_mouth_l, 17, 40);
        lv_obj_set_pos(s_mouth_r, 40, 40);
    } else {
        // 圆点眼 + 微笑
        lv_obj_set_size(s_eye_l, 6, 6);
        lv_obj_set_pos(s_eye_l, 19, 22);
        lv_obj_set_size(s_eye_r, 6, 6);
        lv_obj_set_pos(s_eye_r, 39, 22);
        lv_obj_clear_flag(s_eye_l, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_eye_r, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_shades, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_size(s_mouth_l, 6, 3);
        lv_obj_set_size(s_mouth_r, 6, 3);
        lv_obj_set_pos(s_mouth_mid, 24, 43);
        lv_obj_set_pos(s_mouth_l, 17, 40);
        lv_obj_set_pos(s_mouth_r, 41, 40);
    }
}

// ================= 格子绘制 =================
static lv_obj_t *ensure_flag(int r, int c) {
    if (s_flag_obj[r][c] != NULL) return s_flag_obj[r][c];
    lv_obj_t *f = lv_obj_create(s_cell_obj[r][c]);
    lv_obj_remove_style_all(f);
    lv_obj_set_size(f, 26, 26);
    lv_obj_set_pos(f, 11, 11);
    lv_obj_clear_flag(f, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    // 旗杆 + 底座
    lv_obj_t *pole = lv_obj_create(f);
    lv_obj_remove_style_all(pole);
    lv_obj_set_pos(pole, 18, 0);
    lv_obj_set_size(pole, 3, 20);
    lv_obj_set_style_bg_color(pole, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(pole, LV_OPA_COVER, 0);
    lv_obj_clear_flag(pole, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *base = lv_obj_create(f);
    lv_obj_remove_style_all(base);
    lv_obj_set_pos(base, 12, 22);
    lv_obj_set_size(base, 13, 4);
    lv_obj_set_style_bg_color(base, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(base, LV_OPA_COVER, 0);
    lv_obj_clear_flag(base, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    // 三角旗（三级阶梯拼出来，不用 transform_rotation——固件没导出该符号）
    const int cloth[3][4] = {
        {4, 0, 13, 6},
        {6, 6, 9, 6},
        {8, 12, 5, 6},
    };
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *cl = lv_obj_create(f);
        lv_obj_remove_style_all(cl);
        lv_obj_set_pos(cl, cloth[i][0], cloth[i][1]);
        lv_obj_set_size(cl, cloth[i][2], cloth[i][3]);
        lv_obj_set_style_bg_color(cl, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(cl, LV_OPA_COVER, 0);
        lv_obj_clear_flag(cl, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    s_flag_obj[r][c] = f;
    return f;
}

static lv_obj_t *ensure_mine(int r, int c) {
    if (s_mine_obj[r][c] != NULL) return s_mine_obj[r][c];
    lv_obj_t *m = lv_obj_create(s_cell_obj[r][c]);
    lv_obj_remove_style_all(m);
    lv_obj_set_size(m, 30, 30);
    lv_obj_set_pos(m, 10, 10);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    // 圆核（radius 拉满 = 圆），十字臂 + 四角点 = 经典八角雷
    lv_obj_t *core = lv_obj_create(m);
    lv_obj_remove_style_all(core);
    lv_obj_set_pos(core, 9, 9);
    lv_obj_set_size(core, 12, 12);
    lv_obj_set_style_radius(core, 6, 0);
    lv_obj_set_style_bg_color(core, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(core, LV_OPA_COVER, 0);
    lv_obj_clear_flag(core, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *vbar = lv_obj_create(m);
    lv_obj_remove_style_all(vbar);
    lv_obj_set_pos(vbar, 13, 0);
    lv_obj_set_size(vbar, 4, 30);
    lv_obj_set_style_bg_color(vbar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(vbar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(vbar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *hbar = lv_obj_create(m);
    lv_obj_remove_style_all(hbar);
    lv_obj_set_pos(hbar, 0, 13);
    lv_obj_set_size(hbar, 30, 4);
    lv_obj_set_style_bg_color(hbar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(hbar, LV_OPA_COVER, 0);
    lv_obj_clear_flag(hbar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    for (int i = 0; i < 4; ++i) {
        lv_obj_t *dot = lv_obj_create(m);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 5, 5);
        lv_obj_set_style_bg_color(dot, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_set_pos(dot, (i & 1) ? 21 : 4, (i & 2) ? 21 : 4);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    // 高光点（雷体左上留一粒白，立起来的关键）
    lv_obj_t *glint = lv_obj_create(m);
    lv_obj_remove_style_all(glint);
    lv_obj_set_pos(glint, 11, 11);
    lv_obj_set_size(glint, 3, 3);
    lv_obj_set_style_bg_color(glint, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(glint, LV_OPA_COVER, 0);
    lv_obj_clear_flag(glint, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    s_mine_obj[r][c] = m;
    return m;
}

// 踩雷格整格反色：黑底 + 雷体全部翻白（经典的红底炸弹在 1bpp 上的翻译）
static void recolor_mine_hit(lv_obj_t *m, int hit) {
    if (m == NULL) return;
    const uint32_t n = lv_obj_get_child_count(m);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t *ch = lv_obj_get_child(m, i);
        // 最后一个孩子是高光点：反色时它要从白点变黑点
        const int is_glint = ((int)i == (int)n - 1);
        lv_obj_set_style_bg_color(ch, (hit ^ is_glint) ? lv_color_white() : lv_color_black(), 0);
    }
}

static void render_cell(int r, int c) {
    lv_obj_t *cell = s_cell_obj[r][c];
    if (cell == NULL) return;
    const Cell *cc = &s_cells[r][c];
    const int hit = (s_dead && r == s_hit_r && c == s_hit_c) ? 1 : 0;

    // 指纹一致 = 这格外观没变 → 跳过，避免无谓的脏区
    const uint8_t val = (uint8_t)((cc->open ? 1 : 0) | (cc->flag ? 2 : 0) |
                                  (cc->mine ? 4 : 0) | (cc->around << 3) | (hit ? 0x80 : 0));
    if (s_rendered[r][c].val == val) {
        return;
    }
    s_rendered[r][c].val = val;

    // 立体砖 vs 凹陷：未翻开 = 网点灰面 + 右下黑边（凸起）；翻开 = 纯白无边（凹陷）
    if (cc->open) {
        gray_solid_fill(cell, hit ? lv_color_black() : lv_color_white());
        lv_obj_set_style_border_side(cell, LV_BORDER_SIDE_NONE, 0);
    } else {
        gray_dither_fill(cell, GRAY_DITHER_MID);
        lv_obj_set_style_border_side(cell, LV_BORDER_SIDE_BOTTOM | LV_BORDER_SIDE_RIGHT, 0);
        lv_obj_set_style_border_width(cell, 3, 0);
        lv_obj_set_style_border_color(cell, lv_color_black(), 0);
    }

    // 数字：大号粗体居中（1bpp 没有红蓝绿，靠字号重量说话）
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

    // 旗
    if (cc->flag && !cc->open) {
        lv_obj_clear_flag(ensure_flag(r, c), LV_OBJ_FLAG_HIDDEN);
    } else if (s_flag_obj[r][c] != NULL) {
        lv_obj_add_flag(s_flag_obj[r][c], LV_OBJ_FLAG_HIDDEN);
    }

    // 雷
    if (cc->open && cc->mine) {
        lv_obj_t *m = ensure_mine(r, c);
        recolor_mine_hit(m, hit);
        lv_obj_clear_flag(m, LV_OBJ_FLAG_HIDDEN);
    } else if (s_mine_obj[r][c] != NULL) {
        lv_obj_add_flag(s_mine_obj[r][c], LV_OBJ_FLAG_HIDDEN);
    }
}

static void render_all(void) {
    for (int r = 0; r < N; ++r) {
        for (int c = 0; c < N; ++c) render_cell(r, c);
    }
    led_show(s_cnt_seg, s_cnt_ghost, mine_total() - s_flags);
    led_show(s_tim_seg, s_tim_ghost, elapsed_seconds());
    sync_face();
    if (s_diff_lbl != NULL) {
        char buf[32];
        snprintf(buf, sizeof(buf), "难度 · %s", kDifficulties[s_diff_idx].name);
        lv_label_set_text(s_diff_lbl, buf);
    }
    if (s_hint_lbl != NULL) {
        lv_label_set_text(s_hint_lbl, s_flag_mode ? "插旗模式中 · 点格子放旗" : "点笑脸开新局 · 长按插旗");
    }
}

static void check_win(void) {
    if (s_dead || s_won) return;
    const int total = N * N - mine_total();
    if (s_revealed < total) return;
    s_won = 1;
    s_timer_final = elapsed_seconds();
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
    s_hit_r = s_hit_c = -1;
    s_timer_started = 0;
    s_timer_final = -1;
    memset(s_rendered, 0xFF, sizeof(s_rendered));
    compute_around();
    return 1;
}

// ================= 交互 =================
// ================= 空闲维护：落盘 与 全刷 分开 =================
//   落盘(3.5s)：操作只改内存，停手 3.5s 后写一次 SD。
//   全刷(6s)  ：全刷 480x800 = 38 万像素，墨水屏要 2~3 秒，期间 LVGL/触摸都被
//                阻塞，注入的"按下"会被 LVGL 当成手指移动 → CLICKED 丢失。
//                所以全刷必须等到"真空闲"（6 秒无操作）才做，绝不能在连续操作
//                途中触发 —— 否则一次全刷就会让后面一整串操作全部失效。
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
    if (s_save_timer != NULL) {
        lv_timer_delete(s_save_timer);
    }
    s_save_timer = lv_timer_create(idle_save_cb, 3500, NULL);

    if (s_refresh_timer != NULL) {
        lv_timer_delete(s_refresh_timer);
    }
    s_refresh_timer = lv_timer_create(idle_full_refresh_cb, 6000, NULL);
}

static void on_cell_clicked(lv_event_t *e) {
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const int r = idx / N, c = idx % N;
    if (s_flag_mode) {
        toggle_flag(r, c);
        schedule_idle_maintenance();
        return;
    }
    if (s_cells[r][c].open && !s_cells[r][c].mine) {
        chord_at(r, c);
    } else {
        reveal_at(r, c);
    }
    schedule_idle_maintenance();
}

static void on_cell_long_pressed(lv_event_t *e) {
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const int r = idx / N, c = idx % N;
    toggle_flag(r, c);
    schedule_idle_maintenance();
}

static void on_face_clicked(lv_event_t *e) {
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
    render_all();
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
    s_face_btn = NULL;
    s_diff_lbl = NULL;
    s_hint_lbl = NULL;
    memset(s_cell_obj, 0, sizeof(s_cell_obj));
    memset(s_num_lbl, 0, sizeof(s_num_lbl));
    memset(s_flag_obj, 0, sizeof(s_flag_obj));
    memset(s_mine_obj, 0, sizeof(s_mine_obj));
    memset(s_cnt_seg, 0, sizeof(s_cnt_seg));
    memset(s_cnt_ghost, 0, sizeof(s_cnt_ghost));
    memset(s_tim_seg, 0, sizeof(s_tim_seg));
    memset(s_tim_ghost, 0, sizeof(s_tim_ghost));
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

    // 1. 经典头部：剩雷 LED —— 笑脸 —— 计时 LED，外框下凹
    lv_obj_t *panel = lv_obj_create(scr);
    lv_obj_remove_style_all(panel);
    lv_obj_set_pos(panel, 16, 48);
    lv_obj_set_size(panel, 448, 78);
    lv_obj_set_style_border_side(panel, LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(panel, 3, 0);
    lv_obj_set_style_border_color(panel, lv_color_black(), 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < 3; ++i) {
        seg_make_digit(panel, 24 + i * (LED_W + 6), 15, s_cnt_seg[i], s_cnt_ghost[i]);
        seg_make_digit(panel, 338 + i * (LED_W + 6), 15, s_tim_seg[i], s_tim_ghost[i]);
    }

    // 笑脸按钮（点击 = 新游戏）
    s_face_btn = lv_obj_create(panel);
    lv_obj_remove_style_all(s_face_btn);
    lv_obj_set_size(s_face_btn, 68, 68);
    lv_obj_set_pos(s_face_btn, 190, 5);
    lv_obj_set_style_radius(s_face_btn, 34, 0);
    lv_obj_set_style_border_color(s_face_btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_face_btn, 2, 0);
    lv_obj_set_style_bg_color(s_face_btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_face_btn, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_face_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_face_btn, on_face_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_clear_flag(s_face_btn, LV_OBJ_FLAG_SCROLLABLE);

    s_eye_l = lv_obj_create(s_face_btn);
    lv_obj_remove_style_all(s_eye_l);
    lv_obj_set_size(s_eye_l, 6, 6);
    lv_obj_set_pos(s_eye_l, 19, 22);
    lv_obj_set_style_radius(s_eye_l, 3, 0);
    lv_obj_set_style_bg_color(s_eye_l, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_eye_l, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_eye_l, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    s_eye_r = lv_obj_create(s_face_btn);
    lv_obj_remove_style_all(s_eye_r);
    lv_obj_set_size(s_eye_r, 6, 6);
    lv_obj_set_pos(s_eye_r, 39, 22);
    lv_obj_set_style_radius(s_eye_r, 3, 0);
    lv_obj_set_style_bg_color(s_eye_r, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_eye_r, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_eye_r, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_mouth_mid = lv_obj_create(s_face_btn);
    lv_obj_remove_style_all(s_mouth_mid);
    lv_obj_set_size(s_mouth_mid, 16, 3);
    lv_obj_set_pos(s_mouth_mid, 24, 43);
    lv_obj_set_style_bg_color(s_mouth_mid, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_mouth_mid, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_mouth_mid, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    s_mouth_l = lv_obj_create(s_face_btn);
    lv_obj_remove_style_all(s_mouth_l);
    lv_obj_set_size(s_mouth_l, 7, 3);
    lv_obj_set_pos(s_mouth_l, 17, 40);
    lv_obj_set_style_bg_color(s_mouth_l, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_mouth_l, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_mouth_l, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    s_mouth_r = lv_obj_create(s_face_btn);
    lv_obj_remove_style_all(s_mouth_r);
    lv_obj_set_size(s_mouth_r, 7, 3);
    lv_obj_set_pos(s_mouth_r, 40, 40);
    lv_obj_set_style_bg_color(s_mouth_r, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_mouth_r, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_mouth_r, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    // 赢局墨镜：一整条黑镜面 + 两侧镜腿
    s_shades = lv_obj_create(s_face_btn);
    lv_obj_remove_style_all(s_shades);
    lv_obj_set_size(s_shades, 40, 11);
    lv_obj_set_pos(s_shades, 10, 21);
    lv_obj_set_style_radius(s_shades, 4, 0);
    lv_obj_set_style_bg_color(s_shades, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_shades, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_shades, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    {
        lv_obj_t *arm_l = lv_obj_create(s_shades);
        lv_obj_remove_style_all(arm_l);
        lv_obj_set_pos(arm_l, -4, 2);
        lv_obj_set_size(arm_l, 4, 5);
        lv_obj_set_style_bg_color(arm_l, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(arm_l, LV_OPA_COVER, 0);
        lv_obj_clear_flag(arm_l, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *arm_r = lv_obj_create(s_shades);
        lv_obj_remove_style_all(arm_r);
        lv_obj_set_pos(arm_r, 40, 2);
        lv_obj_set_size(arm_r, 4, 5);
        lv_obj_set_style_bg_color(arm_r, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(arm_r, LV_OPA_COVER, 0);
        lv_obj_clear_flag(arm_r, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    lv_obj_add_flag(s_shades, LV_OBJ_FLAG_HIDDEN);

    // 2. 棋盘：9x9 立体砖，砖块齐缝铺（砖的右下黑边自己拼出网格）
    lv_obj_t *board = lv_obj_create(scr);
    lv_obj_remove_style_all(board);
    lv_obj_set_pos(board, (480 - BOARD) / 2, 132);
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

            // 数字（大号粗体，居中）
            lv_obj_t *num = lv_label_create(cell);
            lv_label_set_text(num, "");
            const lv_font_t *nf = num_font();
            if (nf != NULL) lv_obj_set_style_text_font(num, nf, 0);
            lv_obj_set_style_text_color(num, lv_color_black(), 0);
            lv_obj_align(num, LV_ALIGN_CENTER, 0, 0);
            lv_obj_clear_flag(num, LV_OBJ_FLAG_CLICKABLE);
            s_num_lbl[r][c] = num;
        }
    }

    // 3. 操作提示
    s_hint_lbl = lv_label_create(scr);
    lv_label_set_text(s_hint_lbl, "点笑脸开新局 · 长按插旗");
    if (ui_font() != NULL) lv_obj_set_style_text_font(s_hint_lbl, ui_font(), 0);
    lv_obj_set_style_text_color(s_hint_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_pos(s_hint_lbl, 16, 594);

    // 4. 功能按钮（新游戏 = 点笑脸，经典操作）
    const lv_coord_t bw = 216, bh = 56;
    lv_obj_t *b1 = make_btn(scr, "难度 · 简单", bw, bh, on_diff_clicked, NULL);
    lv_obj_set_pos(b1, 16, 652);
    s_diff_lbl = lv_obj_get_child(b1, 0);
    lv_obj_t *b2 = make_btn(scr, "插旗模式", bw, bh, on_flag_mode_clicked, NULL);
    lv_obj_set_pos(b2, 248, 652);

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
