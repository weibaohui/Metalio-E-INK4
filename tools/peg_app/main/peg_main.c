// tools/peg_app/main/peg_main.c
// 孔明棋 动态应用：Peg Solitaire（英文十字盘 33 洞），一步一刷，适配墨水屏。
//
// 玩法：棋盘上有 32 枚棋子、中心 1 个空洞。每次把一枚棋子跳过相邻的棋子
//       跳进它后面的空洞，被跳过的棋子被吃掉。目标是最后只剩 1 枚棋子。
//
// 设计要点（墨水屏 480x800 / 1bpp 单色）：
//   - 纯几何：洞=空心小圆圈，棋子=实心小圆点。零字体依赖，墨水屏上极清晰。
//   - 一次只改 3 个对象（起点/被吃的/落点）→ 脏区极小，一次跳子只刷一小片。
//   - 跳子的合法性靠"直线两格"判定：不需要格子图，坐标算即可。
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "lvgl.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

#define GRID 7   // 盘面是 7x7 的格
#define NHOLES 33
#define CELL 56
#define BOARD_X ((480 - GRID * CELL) / 2)
#define BOARD_Y 168
#define HOLE_R 11

// 英文十字盘：中央 3x7 加上上下各 3x3 的臂
//   . . x x x . .
//   . . x x x . .
//   x x x x x x x
//   x x x x x x x
//   x x x x x x x
//   . . x x x . .
//   . . x x x . .
static const char kBoardMap[GRID * GRID] = {
    0, 0, 1, 1, 1, 0, 0,
    0, 0, 1, 1, 1, 0, 0,
    1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1,
    0, 0, 1, 1, 1, 0, 0,
    0, 0, 1, 1, 1, 0, 0,
};

// 每个洞的格坐标（行列）
static int8_t s_hx[NHOLES];
static int8_t s_hy[NHOLES];
// 洞索引表：按格坐标查洞号，-1 = 不是洞
static int8_t s_hole_at[GRID * GRID];

static uint8_t s_peg[NHOLES]; // 1=有棋子
static int s_moves = 0;
static int s_sel = -1;   // 选中的棋子（洞号）
static int s_hint_to = -1; // 提示的落点
static int s_won = 0;

// 悔棋栈：每步记 (起点洞, 落点洞)
typedef struct {
    uint8_t from;
    uint8_t to;
} PegMove;
static PegMove s_hist[256];
static int s_hist_n = 0;

// ---- 绘制对象 ----
static lv_obj_t *s_hole_obj[NHOLES];  // 洞（空心圈）
static lv_obj_t *s_peg_obj[NHOLES];   // 棋子（实心点）
static lv_obj_t *s_mark_obj = NULL;   // 提示落点的标记
static lv_obj_t *s_info_lbl = NULL;
static lv_obj_t *s_status_lbl = NULL;
static lv_obj_t *s_rule_obj = NULL;

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

// ================= 盘面 =================
static void build_board(void) {
    memset(s_hole_at, -1, sizeof(s_hole_at));
    int n = 0;
    for (int i = 0; i < GRID * GRID; ++i) {
        if (!kBoardMap[i]) continue;
        s_hole_at[i] = (int8_t)n;
        s_hx[n] = (int8_t)(i % GRID);
        s_hy[n] = (int8_t)(i / GRID);
        n++;
    }
}

static int hole_index(int gx, int gy) {
    if (gx < 0 || gx >= GRID || gy < 0 || gy >= GRID) return -1;
    return s_hole_at[gy * GRID + gx];
}

static void new_game(void) {
    memset(s_peg, 0, sizeof(s_peg));
    // 除中心（3,3）外全放棋子
    const int center = hole_index(3, 3);
    for (int i = 0; i < NHOLES; ++i) s_peg[i] = (i == center) ? 0 : 1;
    s_moves = 0;
    s_hist_n = 0;
    s_sel = -1;
    s_hint_to = -1;
    s_won = 0;
}

static int count_pegs(void) {
    int n = 0;
    for (int i = 0; i < NHOLES; ++i) n += s_peg[i] ? 1 : 0;
    return n;
}

// 从 from 跳到 to 是否合法（直线两格，中间有子、落点空）
static int can_jump(int from, int to) {
    if (from < 0 || to < 0) return 0;
    if (!s_peg[from] || s_peg[to]) return 0;
    const int dx = s_hx[to] - s_hx[from];
    const int dy = s_hy[to] - s_hy[from];
    if (dx != 0 && dy != 0) return 0;         // 只能横竖
    if (abs(dx) != 2 && abs(dy) != 2) return 0; // 必须隔一个
    const int mx = (s_hx[from] + s_hx[to]) / 2;
    const int my = (s_hy[from] + s_hy[to]) / 2;
    const int mid = hole_index(mx, my);
    if (mid < 0) return 0;
    return s_peg[mid] ? 1 : 0;
}

// 有没有一步可走
static int find_any_jump(int *out_from, int *out_to) {
    static const int dx[4] = {0, 0, -1, 1};
    static const int dy[4] = {-1, 1, 0, 0};
    for (int i = 0; i < NHOLES; ++i) {
        if (!s_peg[i]) continue;
        for (int d = 0; d < 4; ++d) {
            const int tx = hole_index(s_hx[i] + dx[d] * 2, s_hy[i] + dy[d] * 2);
            if (can_jump(i, tx)) {
                if (out_from) *out_from = i;
                if (out_to) *out_to = tx;
                return 1;
            }
        }
    }
    return 0;
}

// ================= 绘制 =================
static void render_hole(int i) {
    lv_obj_t *hole = s_hole_obj[i];
    lv_obj_t *peg = s_peg_obj[i];
    if (hole == NULL || peg == NULL) return;
    const int x = BOARD_X + s_hx[i] * CELL + CELL / 2;
    const int y = BOARD_Y + s_hy[i] * CELL + CELL / 2;
    if (s_peg[i]) {
        // 棋子：实心圆点
        lv_obj_clear_flag(peg, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(peg, x - HOLE_R, y - HOLE_R);
        // 选中的棋子加一圈描边
        lv_obj_set_style_border_width(peg, (i == s_sel) ? 2 : 0, 0);
        lv_obj_set_style_border_color(peg, lv_color_black(), 0);
        lv_obj_set_style_radius(peg, LV_RADIUS_CIRCLE, 0);
    } else {
        // 空洞：空心圈
        lv_obj_clear_flag(hole, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(hole, x - HOLE_R, y - HOLE_R);
        lv_obj_set_style_bg_opa(hole, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(hole, lv_color_black(), 0);
        lv_obj_set_style_border_width(hole, (i == s_hint_to) ? 3 : 1, 0);
        lv_obj_set_style_radius(hole, LV_RADIUS_CIRCLE, 0);
        lv_obj_add_flag(peg, LV_OBJ_FLAG_HIDDEN);
    }
}

static void render_all(void) {
    for (int i = 0; i < NHOLES; ++i) render_hole(i);
    if (s_info_lbl != NULL) {
        char buf[64];
        snprintf(buf, sizeof(buf), "第 %d 手 · 剩 %d 枚", s_moves, count_pegs());
        lv_label_set_text(s_info_lbl, buf);
    }
    if (s_status_lbl != NULL) {
        const char *t = "点棋子选中，再点落点跳过去";
        if (s_won) t = "只剩一枚，漂亮！";
        else if (s_hint_to >= 0) t = "有一步可走：已圈出落点";
        lv_label_set_text(s_status_lbl, t);
    }
}

// ================= 持久化 =================
static void save_game(void) {
    FILE *fp = (FILE *)dynapp_fopen_data("peg.txt", "w");
    if (fp == NULL) return;
    fprintf(fp, "PEG1 %d %d %d\n", s_moves, s_hist_n, s_won);
    for (int i = 0; i < NHOLES; ++i) fputc(s_peg[i] ? '1' : '0', fp);
    fputc('\n', fp);
    for (int i = 0; i < s_hist_n; ++i) {
        fprintf(fp, "%d %d ", s_hist[i].from, s_hist[i].to);
    }
    fputc('\n', fp);
    fclose(fp);
}

static int load_game(void) {
    FILE *fp = (FILE *)dynapp_fopen_data("peg.txt", "r");
    if (fp == NULL) return 0;
    char magic[16] = {0};
    int moves = 0, hist_n = 0, won = 0;
    if (fscanf(fp, "%15s", magic) != 1 || strcmp(magic, "PEG1") != 0) {
        fclose(fp);
        return 0;
    }
    if (fscanf(fp, "%d %d %d", &moves, &hist_n, &won) != 3 || hist_n < 0 ||
        hist_n > (int)(sizeof(s_hist) / sizeof(s_hist[0]))) {
        fclose(fp);
        return 0;
    }
    for (int i = 0; i < NHOLES; ++i) {
        int v = 0;
        if (fscanf(fp, "%1d", &v) != 1) {
            fclose(fp);
            return 0;
        }
        s_peg[i] = (uint8_t)(v ? 1 : 0);
    }
    s_hist_n = 0;
    for (int i = 0; i < hist_n; ++i) {
        int a = 0, b = 0;
        if (fscanf(fp, "%d %d", &a, &b) != 2) break;
        if (a < 0 || a >= NHOLES || b < 0 || b >= NHOLES) break;
        s_hist[s_hist_n].from = (uint8_t)a;
        s_hist[s_hist_n].to = (uint8_t)b;
        s_hist_n++;
    }
    fclose(fp);
    s_moves = moves;
    s_won = won;
    s_sel = -1;
    s_hint_to = -1;
    return 1;
}

// ================= 交互 =================
static void do_jump(int from, int to) {
    const int dx = s_hx[to] - s_hx[from];
    const int dy = s_hy[to] - s_hy[from];
    const int mid = hole_index((s_hx[from] + s_hx[to]) / 2, (s_hy[from] + s_hy[to]) / 2);
    s_peg[from] = 0;
    s_peg[mid] = 0;
    s_peg[to] = 1;
    if (s_hist_n < (int)(sizeof(s_hist) / sizeof(s_hist[0]))) {
        s_hist[s_hist_n].from = (uint8_t)from;
        s_hist[s_hist_n].to = (uint8_t)to;
        s_hist_n++;
    }
    s_moves++;
    s_sel = -1;
    s_hint_to = -1;
    render_hole(from);
    render_hole(mid);
    render_hole(to);
    if (count_pegs() == 1) {
        s_won = 1;
        render_all();
        save_game();
        dynapp_modal_alert("通关！", "最后只剩一枚棋子。", "确定");
        return;
    }
    render_all();
    save_game();
    (void)dx;
    (void)dy;
}

static void on_hole_clicked(lv_event_t *e) {
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (s_won) return;
    if (s_peg[i]) {
        // 点棋子：选中它（或改选别的）
        s_sel = (s_sel == i) ? -1 : i;
        s_hint_to = -1;
        render_all();
        return;
    }
    // 点空洞：如果是提示的落点，优先走提示；否则尝试用选中的棋子跳
    if (s_sel >= 0 && can_jump(s_sel, i)) {
        do_jump(s_sel, i);
        return;
    }
    if (s_hint_to == i && s_sel < 0) {
        // 提示模式：点落点就执行那一步
        int from = -1, to = -1;
        if (find_any_jump(&from, &to) && to == i) {
            s_sel = from;
            do_jump(from, to);
            return;
        }
    }
    if (s_sel >= 0) {
        dynapp_toast_show("这一步不合法", 1000);
    }
}

static void on_undo_clicked(lv_event_t *e) {
    (void)e;
    if (s_hist_n <= 0) {
        dynapp_toast_show("没有可悔的步", 1000);
        return;
    }
    const PegMove m = s_hist[--s_hist_n];
    const int dx = s_hx[m.to] - s_hx[m.from];
    const int dy = s_hy[m.to] - s_hy[m.from];
    const int mid = hole_index((s_hx[m.from] + s_hx[m.to]) / 2, (s_hy[m.from] + s_hy[m.to]) / 2);
    s_peg[m.from] = 1;
    s_peg[mid] = 1;
    s_peg[m.to] = 0;
    if (s_moves > 0) s_moves--;
    s_won = 0;
    s_sel = -1;
    s_hint_to = -1;
    render_all();
    save_game();
    (void)dx;
    (void)dy;
}

static void on_restart_clicked(lv_event_t *e) {
    (void)e;
    new_game();
    render_all();
    save_game();
    dynapp_toast_show("新局", 1000);
    dynapp_epd_full_refresh();
}

static void on_hint_clicked(lv_event_t *e) {
    (void)e;
    int from = -1, to = -1;
    if (!find_any_jump(&from, &to)) {
        dynapp_toast_show("没有可走的步了", 1400);
        return;
    }
    s_sel = from;
    s_hint_to = to;
    render_all();
    dynapp_toast_show("圈出的是落点", 1400);
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

static int peg_on_vk_key(const char *key) {
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
        on_hint_clicked(NULL);
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
    memset(s_hole_obj, 0, sizeof(s_hole_obj));
    memset(s_peg_obj, 0, sizeof(s_peg_obj));
}

static void *peg_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    s_scr = scr;
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 800);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_screen_deleted, LV_EVENT_DELETE, NULL);

    dynapp_screen_status_bar(scr);
    dynapp_vk_attach(scr, "peg", peg_create, peg_on_vk_key);

    build_board();
    if (!load_game()) new_game();

    const lv_font_t *f_ui = ui_font();
    const lv_font_t *f_title = title_font();

    // 1. 标题
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "孔明棋");
    if (f_title != NULL) lv_obj_set_style_text_font(title, f_title, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_set_pos(title, 16, 48);

    s_info_lbl = lv_label_create(scr);
    if (f_ui != NULL) lv_obj_set_style_text_font(s_info_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_info_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_align(s_info_lbl, LV_ALIGN_TOP_RIGHT, -16, 56);

    // 2. 盘面：每个洞一个可点的空心圈 + 一个实心棋子
    for (int i = 0; i < NHOLES; ++i) {
        const int x = BOARD_X + s_hx[i] * CELL + CELL / 2;
        const int y = BOARD_Y + s_hy[i] * CELL + CELL / 2;

        lv_obj_t *hole = lv_obj_create(scr);
        lv_obj_remove_style_all(hole);
        lv_obj_set_pos(hole, x - HOLE_R, y - HOLE_R);
        lv_obj_set_size(hole, HOLE_R * 2, HOLE_R * 2);
        lv_obj_set_style_bg_opa(hole, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(hole, lv_color_black(), 0);
        lv_obj_set_style_border_width(hole, 1, 0);
        lv_obj_set_style_radius(hole, LV_RADIUS_CIRCLE, 0);
        lv_obj_add_flag(hole, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(hole, on_hole_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_clear_flag(hole, LV_OBJ_FLAG_SCROLLABLE);
        s_hole_obj[i] = hole;

        lv_obj_t *peg = lv_obj_create(scr);
        lv_obj_remove_style_all(peg);
        lv_obj_set_pos(peg, x - HOLE_R + 2, y - HOLE_R + 2);
        lv_obj_set_size(peg, HOLE_R * 2 - 4, HOLE_R * 2 - 4);
        lv_obj_set_style_bg_color(peg, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(peg, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(peg, LV_RADIUS_CIRCLE, 0);
        lv_obj_clear_flag(peg, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        s_peg_obj[i] = peg;
    }

    // 3. 状态行
    s_status_lbl = lv_label_create(scr);
    if (f_ui != NULL) lv_obj_set_style_text_font(s_status_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_status_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_pos(s_status_lbl, 16, BOARD_Y + GRID * CELL + 16);

    // 4. 规则浮层
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
            "孔明棋怎么玩",
            "",
            "盘上 32 枚棋子，中心空着。",
            "",
            "一次跳：点一枚棋子选中，",
            "再点它前方两格远的空洞，",
            "中间的棋子被吃掉。",
            "",
            "目标：最后只剩 1 枚棋子。",
            "",
            "「提示」会圈出一步可走的位置。",
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

    // 5. 按钮
    const lv_coord_t bw = 107, bh = 56, bgap = 6;
    const lv_coord_t by = 700;
    make_btn(scr, "撤销", 16, by, bw, bh, on_undo_clicked, NULL);
    make_btn(scr, "提示", 16 + bw + bgap, by, bw, bh, on_hint_clicked, NULL);
    make_btn(scr, "重开", 16 + 2 * (bw + bgap), by, bw, bh, on_restart_clicked, NULL);
    make_btn(scr, "规则", 16 + 3 * (bw + bgap), by, bw, bh, on_rule_clicked, NULL);

    render_all();
    save_game(); // 启动即落盘：初始局面也要存下来
    return scr;
}

void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "peg";
    out->name = "孔明棋";
    out->icon = "icon.png";
    out->create = peg_create;
}
