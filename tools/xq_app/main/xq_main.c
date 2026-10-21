// tools/xq_app/main/xq_main.c
// 中国象棋 动态应用：回合制、一步一刷，适配墨水屏低刷新率特性。
//
// 设计要点（墨水屏 480x800 / 1bpp 单色）：
//   - 走一步只重绘变化的两个点 → LVGL 局刷，残影可控；每 N 步全刷一次去残影
//   - 1bpp 单色没有红黑之分：空心方=白底黑字，实心方=黑底白字，再配合
//     帥/將 仕/士 相/象 兵/卒 的不同字形，双方一眼可分
//     界面文案也只用"空心/实心"，不用"红/黑"——墨水屏上"红"传达不了信息
//   - 棋盘 9x10 线，格 45px；楚河漢界写在河界内
//   - 引擎完整实现走法生成、将军判定、��死判定、飞将（白脸将）限制
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

#define XC_COLS 9
#define XC_ROWS 10
#define XC_CELL 45
#define XC_PIECE 40

// 1bpp 单色屏上没有"红/黑"可言，棋子上最明显的区分是牌面的明暗：
//   空心方 = 白底黑字（棋子在下方，帥仕相馬車炮兵），先行
//   实心方 = 黑底白字（棋子在上方，將士象馬車炮卒）
// 界面文案一律用"空心/实心"，别再用红/黑（墨水屏上"红"传达不了任何信息）
#define SIDE_HOLLOW 0 // 空心方（下，先行）
#define SIDE_SOLID 1  // 实心方（上）

// 棋子类型
enum {
    XP_NONE = 0,
    XP_KING,    // 帥/將
    XP_ADVISOR, // 仕/士
    XP_ELEPHANT,// 相/象
    XP_ROOK,    // 車
    XP_KNIGHT,  // 馬
    XP_CANNON,  // 炮
    XP_PAWN,    // 兵/卒
};

typedef struct {
    uint8_t type;
    uint8_t side;
} XPiece;

typedef struct {
    int8_t fr, fc, tr, tc;
} XMove;

#define XC_MAX_MOVES 128

static XPiece s_board[XC_ROWS][XC_COLS];
static int s_turn = SIDE_HOLLOW;      // 该谁走
static int s_winner = -1;          // -1 未结束 / 0 空心方胜 / 1 实心方胜
static int s_win_reason = 0;       // 1=将死 2=被将死 3=困毙
static int s_nmoves = 0;
static int s_check_side = -1;      // 正被将军的一方
static int s_user_side = SIDE_HOLLOW; // 用户执子方（界面常驻显示"你执X方"）

// 悔棋历史
static XMove s_hist[256];
static int s_hist_side[256];

// 绘制对象
static lv_obj_t *s_cell_obj[XC_ROWS][XC_COLS];   // 落子靶子（恒可见、透明）
static lv_obj_t *s_piece_obj[XC_ROWS][XC_COLS];  // 棋子圆（空时隐藏）
static lv_obj_t *s_piece_lbl[XC_ROWS][XC_COLS];  // 棋子字
static lv_obj_t *s_hint_obj[XC_ROWS][XC_COLS];   // 可走点标记
static lv_obj_t *s_sel_box = NULL;               // 选中框
static lv_obj_t *s_last_mark = NULL;             // 最后一手标记
static lv_obj_t *s_info_lbl = NULL;
static lv_obj_t *s_status_lbl = NULL;

static int s_sel_r = -1, s_sel_c = -1;
static uint8_t s_rendered[XC_ROWS][XC_COLS];
static uint8_t s_rendered_sel[3]; // 0=未渲染 1=无选中 2=有选中

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
static const lv_font_t *piece_font(void) {
    const lv_font_t *f = fontpack_lv_font_get(30, 2);
    return f != NULL ? f : ui_font();
}

// ================= 棋子字形 =================
// 1bpp 单色：空心方白底黑字，实心方黑底白字；字形也按阵营区分
static const char *piece_glyph(uint8_t type, uint8_t side) {
    if (side == SIDE_HOLLOW) {
        switch (type) {
            case XP_KING: return "帥";
            case XP_ADVISOR: return "仕";
            case XP_ELEPHANT: return "相";
            case XP_ROOK: return "車";
            case XP_KNIGHT: return "馬";
            case XP_CANNON: return "炮";
            case XP_PAWN: return "兵";
            default: return "";
        }
    }
    switch (type) {
        case XP_KING: return "將";
        case XP_ADVISOR: return "士";
        case XP_ELEPHANT: return "象";
        case XP_ROOK: return "車";
        case XP_KNIGHT: return "馬";
        case XP_CANNON: return "炮";
        case XP_PAWN: return "卒";
        default: return "";
    }
}

// 阵营显示名：墨水屏上只能靠明暗区分，别写"红/黑"
static const char *side_name(int side) { return side == SIDE_HOLLOW ? "空心方" : "实心方"; }

// ================= 棋局 =================
static int in_board(int r, int c) { return r >= 0 && r < XC_ROWS && c >= 0 && c < XC_COLS; }

// 九宫：实心方行 0-2，空心方行 7-9，列 3-5
static int in_palace(int r, int c, int side) {
    if (c < 3 || c > 5) return 0;
    return side == SIDE_SOLID ? (r <= 2) : (r >= 7);
}

// 未过河：空心方不能过河到 0-4 行，实心方不能到 5-9 行
static int in_own_half(int r, int side) {
    return side == SIDE_HOLLOW ? (r >= 5) : (r <= 4);
}

static void set_piece(int r, int c, uint8_t type, uint8_t side) {
    s_board[r][c].type = type;
    s_board[r][c].side = side;
}

static void init_board(void) {
    memset(s_board, 0, sizeof(s_board));
    // 实心方（上）
    const int bk[9] = {XP_ROOK, XP_KNIGHT, XP_ELEPHANT, XP_ADVISOR, XP_KING, XP_ADVISOR, XP_ELEPHANT, XP_KNIGHT, XP_ROOK};
    for (int c = 0; c < 9; ++c) set_piece(0, c, (uint8_t)bk[c], SIDE_SOLID);
    set_piece(2, 1, XP_CANNON, SIDE_SOLID);
    set_piece(2, 7, XP_CANNON, SIDE_SOLID);
    for (int c = 0; c < 9; c += 2) set_piece(3, c, XP_PAWN, SIDE_SOLID);
    // 空心方（下，先行）
    const int rk[9] = {XP_ROOK, XP_KNIGHT, XP_ELEPHANT, XP_ADVISOR, XP_KING, XP_ADVISOR, XP_ELEPHANT, XP_KNIGHT, XP_ROOK};
    for (int c = 0; c < 9; ++c) set_piece(9, c, (uint8_t)rk[c], SIDE_HOLLOW);
    set_piece(7, 1, XP_CANNON, SIDE_HOLLOW);
    set_piece(7, 7, XP_CANNON, SIDE_HOLLOW);
    for (int c = 0; c < 9; c += 2) set_piece(6, c, XP_PAWN, SIDE_HOLLOW);
}

// 找将/帅
static int find_king(int side) {
    for (int r = 0; r < XC_ROWS; ++r)
        for (int c = 0; c < XC_COLS; ++c)
            if (s_board[r][c].type == XP_KING && s_board[r][c].side == side) return r * 9 + c;
    return -1;
}

// ================= 走法生成（伪合法） =================
static int add_move(XMove *out, int n, int max, int fr, int fc, int tr, int tc) {
    if (n >= max) return n;
    out[n].fr = (int8_t)fr;
    out[n].fc = (int8_t)fc;
    out[n].tr = (int8_t)tr;
    out[n].tc = (int8_t)tc;
    return n + 1;
}

// 直线滑行（不含吃子）：返回可走到的空点
static int slide(int r, int c, int dr, int dc, int side, XMove *out, int n, int max) {
    int rr = r + dr, cc = c + dc;
    while (in_board(rr, cc)) {
        if (s_board[rr][cc].type == XP_NONE) {
            n = add_move(out, n, max, r, c, rr, cc);
        } else {
            if (s_board[rr][cc].side != side) n = add_move(out, n, max, r, c, rr, cc);
            break; // 遇到棋子（含敌方）停住
        }
        rr += dr;
        cc += dc;
    }
    return n;
}

// 炮吃子：越过恰好一个棋子（炮架），打到第二个棋子
static int cannon_capture(int r, int c, int dr, int dc, int side, XMove *out, int n, int max) {
    int rr = r + dr, cc = c + dc;
    int over = 0;
    while (in_board(rr, cc)) {
        if (!over) {
            if (s_board[rr][cc].type == XP_NONE) {
                rr += dr;
                cc += dc;
                continue;
            }
            over = 1; // 找到炮架
        } else {
            if (s_board[rr][cc].type != XP_NONE) {
                if (s_board[rr][cc].side != side) n = add_move(out, n, max, r, c, rr, cc);
                break;
            }
        }
        rr += dr;
        cc += dc;
    }
    return n;
}

static int gen_piece_moves(int r, int c, int side, XMove *out, int n, int max) {
    const uint8_t type = s_board[r][c].type;
    switch (type) {
        case XP_KING: { // 帥/將：九宫内走一步
            static const int dr[4] = {-1, 1, 0, 0};
            static const int dc[4] = {0, 0, -1, 1};
            for (int k = 0; k < 4; ++k) {
                const int rr = r + dr[k], cc = c + dc[k];
                if (in_palace(rr, cc, side) &&
                    (s_board[rr][cc].type == XP_NONE || s_board[rr][cc].side != side)) {
                    n = add_move(out, n, max, r, c, rr, cc);
                }
            }
            break;
        }
        case XP_ADVISOR: { // 仕/士：九宫内斜走一步
            static const int dr[4] = {-1, -1, 1, 1};
            static const int dc[4] = {-1, 1, -1, 1};
            for (int k = 0; k < 4; ++k) {
                const int rr = r + dr[k], cc = c + dc[k];
                if (in_palace(rr, cc, side) &&
                    (s_board[rr][cc].type == XP_NONE || s_board[rr][cc].side != side)) {
                    n = add_move(out, n, max, r, c, rr, cc);
                }
            }
            break;
        }
        case XP_ELEPHANT: { // 相/象：田字走，拐��不能有子（塞象眼），不过河
            static const int dr[4] = {-2, -2, 2, 2};
            static const int dc[4] = {-2, 2, -2, 2};
            for (int k = 0; k < 4; ++k) {
                const int rr = r + dr[k], cc = c + dc[k];
                if (!in_board(rr, cc) || !in_own_half(rr, side)) continue;
                if (s_board[r + dr[k] / 2][c + dc[k] / 2].type != XP_NONE) continue; // 塞象眼
                if (s_board[rr][cc].type != XP_NONE && s_board[rr][cc].side == side) continue;
                n = add_move(out, n, max, r, c, rr, cc);
            }
            break;
        }
        case XP_ROOK: { // 車：直线滑行
            n = slide(r, c, -1, 0, side, out, n, max);
            n = slide(r, c, 1, 0, side, out, n, max);
            n = slide(r, c, 0, -1, side, out, n, max);
            n = slide(r, c, 0, 1, side, out, n, max);
            break;
        }
        case XP_KNIGHT: { // 馬：日字，蹩马腿
            // 腿 = 长轴方向上的第一格（|dr|=2 取 (r+dr/2,c)，否则取 (r,c+dc/2)）
            static const int dr[8] = {-2, -2, 1, 1, -1, -1, 2, 2};
            static const int dc[8] = {-1, 1, -2, 2, -2, 2, -1, 1};
            static const int lr[8] = {-1, -1, 0, 0, 0, 0, 1, 1};
            static const int lc[8] = {0, 0, -1, 1, -1, 1, 0, 0};
            for (int k = 0; k < 8; ++k) {
                const int rr = r + dr[k], cc = c + dc[k];
                if (!in_board(rr, cc)) continue;
                const int leg_r = r + lr[k], leg_c = c + lc[k];
                if (!in_board(leg_r, leg_c)) continue; // 腿在棋盘外 → 走不了
                if (s_board[leg_r][leg_c].type != XP_NONE) continue; // 蹩马腿
                if (s_board[rr][cc].type != XP_NONE && s_board[rr][cc].side == side) continue;
                n = add_move(out, n, max, r, c, rr, cc);
            }
            break;
        }
        case XP_CANNON: { // 炮：不吃子走直线，吃子要隔一个炮架
            static const int dr[4] = {-1, 1, 0, 0};
            static const int dc[4] = {0, 0, -1, 1};
            for (int k = 0; k < 4; ++k) {
                int rr = r + dr[k], cc = c + dc[k];
                while (in_board(rr, cc) && s_board[rr][cc].type == XP_NONE) {
                    n = add_move(out, n, max, r, c, rr, cc);
                    rr += dr[k];
                    cc += dc[k];
                }
            }
            for (int k = 0; k < 4; ++k) {
                n = cannon_capture(r, c, dr[k], dc[k], side, out, n, max);
            }
            break;
        }
        case XP_PAWN: { // 兵/卒：只进不退，过河后可以横走
            const int fwd = (side == SIDE_HOLLOW) ? -1 : 1;
            const int rr = r + fwd;
            if (in_board(rr, c) && s_board[rr][c].type == XP_NONE) {
                n = add_move(out, n, max, r, c, rr, c);
            }
            if (!in_own_half(r, side)) { // 已过河
                if (in_board(r, c - 1) && s_board[r][c - 1].type == XP_NONE) {
                    n = add_move(out, n, max, r, c, r, c - 1);
                }
                if (in_board(r, c + 1) && s_board[r][c + 1].type == XP_NONE) {
                    n = add_move(out, n, max, r, c, r, c + 1);
                }
            }
            break;
        }
        default: break;
    }
    return n;
}

static int gen_side_moves(int side, XMove *out, int max) {
    int n = 0;
    for (int r = 0; r < XC_ROWS; ++r) {
        for (int c = 0; c < XC_COLS; ++c) {
            if (s_board[r][c].type == XP_NONE || s_board[r][c].side != side) continue;
            n = gen_piece_moves(r, c, side, out, n, max);
        }
    }
    return n;
}

// 某点是否被 by_side 攻击（用伪合法走法判定即可：只关心"能不能打到"）
static int is_attacked(int r, int c, int by_side) {
    XMove mv[XC_MAX_MOVES];
    const int n = gen_side_moves(by_side, mv, XC_MAX_MOVES);
    for (int i = 0; i < n; ++i) {
        if (mv[i].tr == r && mv[i].tc == c) return 1;
    }
    return 0;
}

// 飞将（白脸将）：两将同列且中间无子 → 非法局面
static int generals_face_each_other(void) {
    const int rk = find_king(SIDE_HOLLOW);
    const int bk = find_king(SIDE_SOLID);
    if (rk < 0 || bk < 0) return 0;
    const int rc = rk % 9, br = bk / 9, bc = bk % 9;
    if (rc != bc) return 0;
    const int lo = br < rk / 9 ? br : rk / 9;
    const int hi = br < rk / 9 ? rk / 9 : br;
    for (int r = lo + 1; r < hi; ++r) {
        if (s_board[r][rc].type != XP_NONE) return 0;
    }
    return 1;
}

static int side_in_check(int side) {
    const int k = find_king(side);
    if (k < 0) return 0;
    return is_attacked(k / 9, k % 9, side == SIDE_HOLLOW ? SIDE_SOLID : SIDE_HOLLOW);
}

// 走法是否合法（走后自己不被将军，且不构成白脸将）
static int move_is_legal(const XMove *m, int side) {
    const XPiece moved = s_board[m->fr][m->fc];
    const XPiece cap = s_board[m->tr][m->tc];
    s_board[m->tr][m->tc] = moved;
    s_board[m->fr][m->fc].type = XP_NONE;
    int ok = !side_in_check(side) && !generals_face_each_other();
    s_board[m->fr][m->fc] = moved;
    s_board[m->tr][m->tc] = cap;
    return ok;
}

static int legal_moves_of(int r, int c, int side, XMove *out, int max) {
    XMove tmp[XC_MAX_MOVES];
    const int n = gen_piece_moves(r, c, side, tmp, 0, XC_MAX_MOVES);
    int k = 0;
    for (int i = 0; i < n && k < max; ++i) {
        if (move_is_legal(&tmp[i], side)) out[k++] = tmp[i];
    }
    return k;
}

// ================= 渲染 =================
static void render_cell(int r, int c) {
    const XPiece *p = &s_board[r][c];
    const uint8_t sig = (uint8_t)((p->type << 1) | p->side);
    if (s_rendered[r][c] == sig) return;
    s_rendered[r][c] = sig;

    lv_obj_t *po = s_piece_obj[r][c];
    lv_obj_t *lb = s_piece_lbl[r][c];
    if (po == NULL || lb == NULL) return;
    if (p->type == XP_NONE) {
        lv_obj_add_flag(po, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(po, LV_OBJ_FLAG_HIDDEN);
    const bool red = (p->side == SIDE_HOLLOW);
    lv_obj_set_style_bg_color(po, red ? lv_color_white() : lv_color_black(), 0);
    lv_obj_set_style_bg_opa(po, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(po, lv_color_black(), 0);
    lv_obj_set_style_border_width(po, 2, 0);
    lv_obj_set_style_radius(po, LV_RADIUS_CIRCLE, 0);
    lv_label_set_text(lb, piece_glyph(p->type, p->side));
    lv_obj_set_style_text_color(lb, red ? lv_color_black() : lv_color_white(), 0);
}

static void update_info_line(void) {
    if (s_info_lbl == NULL) return;
    char buf[64];
    // 常驻告诉用户"你执哪一方"——墨水屏上双方只靠明暗区分，指路必须靠文字
    snprintf(buf, sizeof(buf), "你执%s", side_name(s_user_side));
    lv_label_set_text(s_info_lbl, buf);
}

static void update_status_line(void) {
    if (s_status_lbl == NULL) return;
    char buf[96];
    if (s_winner == SIDE_HOLLOW) {
        snprintf(buf, sizeof(buf), "%s", s_win_reason == 2 ? "实心方被将死，空心方胜" : "空心方胜");
    } else if (s_winner == SIDE_SOLID) {
        snprintf(buf, sizeof(buf), "%s", s_win_reason == 2 ? "空心方被将死，实心方胜" : "实心方胜");
    } else if (s_check_side == s_turn) {
        snprintf(buf, sizeof(buf), "%s须应将%s",
                 side_name(s_turn), s_turn == s_user_side ? "·该你了" : "");
    } else {
        // 状态行同时说清"谁的回合"和"该哪一方"：谁在走 + 这一方是谁
        snprintf(buf, sizeof(buf), "%s（%s）· 共 %d 手",
                 s_turn == s_user_side ? "该你落子" : "对方落子", side_name(s_turn), s_nmoves);
    }
    lv_label_set_text(s_status_lbl, buf);
}

static void render_all(void) {
    for (int r = 0; r < XC_ROWS; ++r)
        for (int c = 0; c < XC_COLS; ++c) render_cell(r, c);
    update_info_line();
    update_status_line();
}

static void clear_hint_marks(void) {
    for (int r = 0; r < XC_ROWS; ++r)
        for (int c = 0; c < XC_COLS; ++c)
            if (s_hint_obj[r][c] != NULL) lv_obj_add_flag(s_hint_obj[r][c], LV_OBJ_FLAG_HIDDEN);
}

static void update_sel_box(void) {
    if (s_sel_box == NULL) return;
    const uint8_t sig = (s_sel_r >= 0) ? 2 : 1;
    if (s_rendered_sel[0] == sig) return;
    s_rendered_sel[0] = sig;
    if (s_sel_r < 0) {
        lv_obj_add_flag(s_sel_box, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(s_sel_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_sel_box, s_sel_c * XC_CELL - 2, s_sel_r * XC_CELL - 2);
}

static void update_last_mark(int r, int c) {
    if (s_last_mark == NULL) return;
    if (r < 0) {
        lv_obj_add_flag(s_last_mark, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(s_last_mark, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_color(s_last_mark, s_board[r][c].side == SIDE_HOLLOW ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_pos(s_last_mark, c * XC_CELL + XC_CELL / 2 - 3, r * XC_CELL + XC_CELL / 2 - 3);
}

// ================= 持久化 =================
// 棋盘编码：0=空, 1..15 = type*2+side。用十六进制字符写盘
// （直接用 '0'+值 会越过数字区落到 ':' '<' '=' '>' '?'，解析端很容易踩坑）
static const char kHexDigits[] = "0123456789abcdef";

static void save_game(void) {
    FILE *fp = (FILE *)dynapp_fopen_data("xq.txt", "w");
    if (fp == NULL) return;
    fprintf(fp, "XQI2 %d %d %d %d %d\n", s_turn, s_winner, s_win_reason, s_nmoves, s_user_side);
    for (int r = 0; r < XC_ROWS; ++r) {
        for (int c = 0; c < XC_COLS; ++c) {
            const XPiece *p = &s_board[r][c];
            fputc(kHexDigits[p->type == XP_NONE ? 0 : (p->type * 2 + p->side)], fp);
        }
    }
    fputc('\n', fp);
    fclose(fp);
}

static int load_game(void) {
    FILE *fp = (FILE *)dynapp_fopen_data("xq.txt", "r");
    if (fp == NULL) return 0;
    char magic[16] = {0};
    int turn = 0, winner = -1, reason = 0, nmoves = 0, user_side = 0;
    if (fscanf(fp, "%15s", magic) != 1 || strcmp(magic, "XQI2") != 0) {
        fclose(fp);
        return 0;
    }
    if (fscanf(fp, "%d %d %d %d %d", &turn, &winner, &reason, &nmoves, &user_side) != 5) {
        fclose(fp);
        return 0;
    }
    if ((turn != SIDE_HOLLOW && turn != SIDE_SOLID) || winner < -1 || winner > 1) {
        fclose(fp);
        return 0;
    }
    for (int r = 0; r < XC_ROWS; ++r) {
        for (int c = 0; c < XC_COLS; ++c) {
            int v = 0;
            if (fscanf(fp, "%1x", &v) != 1 || v < 0 || v > 15) {
                fclose(fp);
                return 0;
            }
            if (v == 0) {
                s_board[r][c].type = XP_NONE;
                s_board[r][c].side = 0;
            } else {
                s_board[r][c].type = (uint8_t)(v / 2);
                s_board[r][c].side = (uint8_t)(v % 2);
                if (s_board[r][c].type > XP_PAWN) {
                    fclose(fp);
                    return 0;
                }
            }
        }
    }
    fclose(fp);
    s_turn = turn;
    s_winner = winner;
    s_win_reason = reason;
    s_nmoves = nmoves;
    s_user_side = user_side;
    s_sel_r = s_sel_c = -1;
    s_move_count = 0;
    s_last_full_refresh = 0;
    memset(s_rendered, 0xFF, sizeof(s_rendered));
    s_rendered_sel[0] = 0;
    s_check_side = side_in_check(SIDE_HOLLOW) ? SIDE_HOLLOW : (side_in_check(SIDE_SOLID) ? SIDE_SOLID : -1);
    return 1;
}

// ================= 交互 =================
static void after_move(int side) {
    s_nmoves++;
    s_move_count++;
    // 判定对方是否被将死 / 困毙
    const int opp = side == SIDE_HOLLOW ? SIDE_SOLID : SIDE_HOLLOW;
    s_check_side = -1;
    if (side_in_check(opp)) {
        s_check_side = opp;
        XMove mv[XC_MAX_MOVES];
        const int n = gen_side_moves(opp, mv, XC_MAX_MOVES);
        int has_legal = 0;
        for (int i = 0; i < n && !has_legal; ++i) {
            if (move_is_legal(&mv[i], opp)) has_legal = 1;
        }
        if (!has_legal) {
            s_winner = side;
            s_win_reason = 2;
        }
    } else {
        // 困毙：没被将军但无路可走（象棋规则算负）
        XMove mv[XC_MAX_MOVES];
        const int n = gen_side_moves(opp, mv, XC_MAX_MOVES);
        int has_legal = 0;
        for (int i = 0; i < n && !has_legal; ++i) {
            if (move_is_legal(&mv[i], opp)) has_legal = 1;
        }
        if (!has_legal) {
            s_winner = side;
            s_win_reason = 3;
        }
    }
    if (s_winner < 0) s_turn = opp;
}

static void do_move(const XMove *m) {
    const XPiece moved = s_board[m->fr][m->fc];
    s_hist[s_nmoves % 256] = *m;
    s_hist_side[s_nmoves % 256] = s_turn;
    s_board[m->tr][m->tc] = moved;
    s_board[m->fr][m->fc].type = XP_NONE;
    render_cell(m->fr, m->fc);
    render_cell(m->tr, m->tc);
    update_last_mark(m->tr, m->tc);
    after_move(s_turn);
    update_info_line();
    update_status_line();
}

static void schedule_idle_maintenance(void);

static void on_point_clicked(lv_event_t *e) {
    const int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const int r = idx / XC_COLS, c = idx % XC_COLS;
    if (s_winner >= 0) {
        dynapp_toast_show("本局已结束，点「新局」重来", 1600);
        return;
    }
    const XPiece *p = &s_board[r][c];
    if (s_sel_r < 0) {
        // 选己方棋子
        if (p->type == XP_NONE || p->side != s_turn) {
            dynapp_toast_show("该轮到你走自己的棋", 1400);
            return;
        }
        s_sel_r = r;
        s_sel_c = c;
        clear_hint_marks();
        update_sel_box();
        return;
    }
    // 已选中：判断是走子还是改选
    if (r == s_sel_r && c == s_sel_c) {
        s_sel_r = s_sel_c = -1;
        clear_hint_marks();
        update_sel_box();
        return;
    }
    if (p->type != XP_NONE && p->side == s_turn) {
        // 改选另一枚己方棋子
        s_sel_r = r;
        s_sel_c = c;
        clear_hint_marks();
        update_sel_box();
        return;
    }
    XMove mv[XC_MAX_MOVES];
    const int n = legal_moves_of(s_sel_r, s_sel_c, s_turn, mv, XC_MAX_MOVES);
    for (int i = 0; i < n; ++i) {
        if (mv[i].tr == r && mv[i].tc == c) {
            s_sel_r = s_sel_c = -1;
            clear_hint_marks();
            update_sel_box();
            do_move(&mv[i]);
            schedule_idle_maintenance();
            if (s_winner >= 0) {
                char msg[64];
                snprintf(msg, sizeof(msg), "%s", s_win_reason == 2
                                           ? (s_winner == SIDE_HOLLOW ? "实心方被将死。" : "空心方被将死。")
                                           : (s_winner == SIDE_HOLLOW ? "实心方无棋可走。" : "空心方无棋可走。"));
                dynapp_modal_alert(s_winner == SIDE_HOLLOW ? "空心方胜！" : "实心方胜！", msg, "确定");
            } else if (s_check_side == s_turn) {
                dynapp_toast_show("将军！", 1400);
            }
            return;
        }
    }
    dynapp_toast_show("这步不合规则", 1200);
}

static void on_new_game_clicked(lv_event_t *e) {
    (void)e;
    init_board();
    s_turn = SIDE_HOLLOW;
    s_winner = -1;
    s_win_reason = 0;
    s_nmoves = 0;
    s_sel_r = s_sel_c = -1;
    s_check_side = -1;
    s_move_count = 0;
    s_last_full_refresh = 0;
    memset(s_rendered, 0xFF, sizeof(s_rendered));
    s_rendered_sel[0] = 0;
    clear_hint_marks();
    update_sel_box();
    update_last_mark(-1, -1);
    render_all();
    save_game();
    dynapp_toast_show("新局，空心方先行", 1400);
    dynapp_epd_full_refresh();
}

static void on_side_clicked(lv_event_t *e) {
    (void)e;
    s_user_side = (s_user_side == SIDE_HOLLOW) ? SIDE_SOLID : SIDE_HOLLOW;
    update_info_line();
    update_status_line();
    save_game();
    char buf[48];
    snprintf(buf, sizeof(buf), "你执%s", side_name(s_user_side));
    dynapp_toast_show(buf, 1500);
    dynapp_epd_full_refresh();
}

static void on_undo_clicked(lv_event_t *e) {
    (void)e;
    if (s_nmoves <= 0) {
        dynapp_toast_show("还没有棋可悔", 1200);
        return;
    }
    // 悔一步：还原到上一步
    const XMove *m = &s_hist[(s_nmoves - 1) % 256];
    s_board[m->fr][m->fc] = s_board[m->tr][m->tc];
    s_board[m->tr][m->tc].type = XP_NONE;
    s_nmoves--;
    s_turn = s_hist_side[(s_nmoves) % 256];
    s_winner = -1;
    s_win_reason = 0;
    s_sel_r = s_sel_c = -1;
    s_check_side = side_in_check(SIDE_HOLLOW) ? SIDE_HOLLOW : (side_in_check(SIDE_SOLID) ? SIDE_SOLID : -1);
    render_cell(m->fr, m->fc);
    render_cell(m->tr, m->tc);
    if (s_nmoves > 0) {
        const XMove *lm = &s_hist[(s_nmoves - 1) % 256];
        update_last_mark(lm->tr, lm->tc);
    } else {
        update_last_mark(-1, -1);
    }
    clear_hint_marks();
    update_sel_box();
    update_info_line();
    update_status_line();
    save_game();
    dynapp_toast_show("已悔棋", 1200);
    dynapp_epd_full_refresh();
}

static void on_hint_clicked(lv_event_t *e) {
    (void)e;
    clear_hint_marks();
    if (s_sel_r < 0) {
        dynapp_toast_show("先点一枚己方棋子", 1400);
        return;
    }
    XMove mv[XC_MAX_MOVES];
    const int n = legal_moves_of(s_sel_r, s_sel_c, s_turn, mv, XC_MAX_MOVES);
    for (int i = 0; i < n; ++i) {
        lv_obj_t *h = s_hint_obj[mv[i].tr][mv[i].tc];
        if (h != NULL) lv_obj_clear_flag(h, LV_OBJ_FLAG_HIDDEN);
    }
    char buf[48];
    snprintf(buf, sizeof(buf), "可走 %d 个点", n);
    dynapp_toast_show(buf, 1400);
    dynapp_epd_full_refresh();
}

static int xq_on_vk_key(const char *key) {
    if (key == NULL) return 0;
    if (strcmp(key, "vk_home") == 0 || strcmp(key, "esc") == 0 || strcmp(key, "back") == 0) {
        dynapp_screen_go_home();
        return 1;
    }
    return 0;
}

// ================= 空闲维护 =================
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
    memset(s_piece_obj, 0, sizeof(s_piece_obj));
    memset(s_piece_lbl, 0, sizeof(s_piece_lbl));
    memset(s_hint_obj, 0, sizeof(s_hint_obj));
    s_sel_box = NULL;
    s_last_mark = NULL;
}

static void *xq_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    s_scr = scr;
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 800);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_screen_deleted, LV_EVENT_DELETE, NULL);

    dynapp_screen_status_bar(scr);
    dynapp_vk_attach(scr, "xq", xq_create, xq_on_vk_key);

    if (!load_game()) {
        init_board();
        s_turn = SIDE_HOLLOW;
        s_winner = -1;
        s_nmoves = 0;
        s_check_side = -1;
        memset(s_rendered, 0xFF, sizeof(s_rendered));
        s_rendered_sel[0] = 0;
    }

    // 1. 标题
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "中国象棋");
    const lv_font_t *f_title = title_font();
    if (f_title != NULL) lv_obj_set_style_text_font(title, f_title, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_set_pos(title, 16, 48);

    s_info_lbl = lv_label_create(scr);
    const lv_font_t *f_ui = ui_font();
    if (f_ui != NULL) lv_obj_set_style_text_font(s_info_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_info_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_align(s_info_lbl, LV_ALIGN_TOP_RIGHT, -16, 56);

    // 2. 棋盘：交点 (c,r) 在 board 内的 (PAD + c*CELL, PAD + r*CELL)，
    //    PAD = 棋子半径，棋子正好以交点为中心；容器尺寸要盖住最后一行的棋子
    const int PAD = XC_PIECE / 2;
    lv_obj_t *board = lv_obj_create(scr);
    lv_obj_remove_style_all(board);
    lv_obj_set_pos(board, 40, 92);
    lv_obj_set_size(board, 8 * XC_CELL + 2 * PAD, 9 * XC_CELL + 2 * PAD);
    lv_obj_clear_flag(board, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    // 竖线（9 条，穿过河界）
    for (int c = 0; c < XC_COLS; ++c) {
        lv_obj_t *line = lv_obj_create(board);
        lv_obj_remove_style_all(line);
        lv_obj_set_pos(line, PAD + c * XC_CELL, PAD);
        lv_obj_set_size(line, 1, 9 * XC_CELL);
        lv_obj_set_style_bg_color(line, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    // 横线（10 条）
    for (int r = 0; r < XC_ROWS; ++r) {
        lv_obj_t *line = lv_obj_create(board);
        lv_obj_remove_style_all(line);
        lv_obj_set_pos(line, PAD, PAD + r * XC_CELL);
        lv_obj_set_size(line, 8 * XC_CELL, 1);
        lv_obj_set_style_bg_color(line, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    // 九宫斜线（用点串画，固件未开 transform_rotation 画不了斜线）
    for (int side = 0; side < 2; ++side) {
        const int r0 = side == 0 ? 0 : 7;
        for (int k = 0; k < 2; ++k) {
            // 从 (3,r0) 到 (5,r0+2) / 从 (5,r0) 到 (3,r0+2)
            const int c_from = (k == 0) ? 3 : 5;
            const int c_to = (k == 0) ? 5 : 3;
            for (int i = 0; i <= 10; ++i) {
                const int t = i * 10; // 0..100 百分比
                const int rr = r0 + (2 * t) / 100;
                const int cc = c_from + ((c_to - c_from) * t) / 100;
                lv_obj_t *d = lv_obj_create(board);
                lv_obj_remove_style_all(d);
                lv_obj_set_size(d, 3, 3);
                lv_obj_set_style_bg_color(d, lv_color_black(), 0);
                lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
                lv_obj_set_style_radius(d, LV_RADIUS_CIRCLE, 0);
                lv_obj_set_pos(d, PAD + cc * XC_CELL - 1, PAD + rr * XC_CELL - 1);
                lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
            }
        }
    }
    // 楚河漢界
    {
        const char *txt[2] = {"楚河", "漢界"};
        const int xpos[2] = {PAD + 16, PAD + 6 * XC_CELL + 6};
        for (int i = 0; i < 2; ++i) {
            lv_obj_t *l = lv_label_create(board);
            lv_label_set_text(l, txt[i]);
            if (f_ui != NULL) lv_obj_set_style_text_font(l, f_ui, 0);
            lv_obj_set_style_text_color(l, lv_color_black(), 0);
            lv_obj_set_pos(l, xpos[i], PAD + 4 * XC_CELL + 8);
        }
    }

    // 交点：落子靶子 + 棋子 + 可走点标记
    for (int r = 0; r < XC_ROWS; ++r) {
        for (int c = 0; c < XC_COLS; ++c) {
            lv_obj_t *cell = lv_obj_create(board);
            lv_obj_remove_style_all(cell);
            lv_obj_set_pos(cell, c * XC_CELL, r * XC_CELL);
            lv_obj_set_size(cell, XC_CELL, XC_CELL);
            lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
            lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(cell, on_point_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)(r * XC_COLS + c));
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
            s_cell_obj[r][c] = cell;

            lv_obj_t *piece = lv_obj_create(board);
            lv_obj_remove_style_all(piece);
            lv_obj_set_size(piece, XC_PIECE, XC_PIECE);
            lv_obj_set_pos(piece, c * XC_CELL, r * XC_CELL);
            lv_obj_set_style_radius(piece, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_width(piece, 2, 0);
            lv_obj_set_style_border_color(piece, lv_color_black(), 0);
            lv_obj_clear_flag(piece, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(piece, LV_OBJ_FLAG_HIDDEN);
            s_piece_obj[r][c] = piece;

            lv_obj_t *plbl = lv_label_create(piece);
            lv_label_set_text(plbl, "");
            const lv_font_t *fp = piece_font();
            if (fp != NULL) lv_obj_set_style_text_font(plbl, fp, 0);
            lv_obj_set_style_text_color(plbl, lv_color_black(), 0);
            lv_obj_center(plbl);
            lv_obj_clear_flag(plbl, LV_OBJ_FLAG_CLICKABLE);
            s_piece_lbl[r][c] = plbl;

            lv_obj_t *hint = lv_obj_create(board);
            lv_obj_remove_style_all(hint);
            lv_obj_set_size(hint, 12, 12);
            lv_obj_set_pos(hint, c * XC_CELL + (XC_CELL - 12) / 2, r * XC_CELL + (XC_CELL - 12) / 2);
            lv_obj_set_style_bg_color(hint, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(hint, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_color(hint, lv_color_black(), 0);
            lv_obj_set_style_border_width(hint, 2, 0);
            lv_obj_clear_flag(hint, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_add_flag(hint, LV_OBJ_FLAG_HIDDEN);
            s_hint_obj[r][c] = hint;
        }
    }

    // 选中框
    s_sel_box = lv_obj_create(board);
    lv_obj_remove_style_all(s_sel_box);
    lv_obj_set_size(s_sel_box, XC_PIECE + 4, XC_PIECE + 4);
    lv_obj_set_style_bg_opa(s_sel_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(s_sel_box, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_sel_box, 2, 0);
    lv_obj_clear_flag(s_sel_box, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_sel_box, LV_OBJ_FLAG_HIDDEN);

    // 最后一手标记
    s_last_mark = lv_obj_create(board);
    lv_obj_remove_style_all(s_last_mark);
    lv_obj_set_size(s_last_mark, 6, 6);
    lv_obj_set_style_bg_opa(s_last_mark, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(s_last_mark, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(s_last_mark, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_last_mark, LV_OBJ_FLAG_HIDDEN);

    // 3. 状态行
    s_status_lbl = lv_label_create(scr);
    if (f_ui != NULL) lv_obj_set_style_text_font(s_status_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_status_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_pos(s_status_lbl, 16, 92 + 9 * XC_CELL + 2 * PAD + 4);

    lv_obj_t *hint = lv_label_create(scr);
    lv_label_set_text(hint, "点己方棋子选中 · 点目标点落子");
    if (f_ui != NULL) lv_obj_set_style_text_font(hint, f_ui, 0);
    lv_obj_set_style_text_color(hint, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_pos(hint, 16, 92 + 9 * XC_CELL + 2 * PAD + 34);

    // 4. 按钮
    const lv_coord_t bw = 107, bh = 56, bgap = 6;
    const lv_coord_t by = 620;
    lv_obj_t *b1 = make_btn(scr, "新局", bw, bh, on_new_game_clicked, NULL);
    lv_obj_set_pos(b1, 16, by);
    lv_obj_t *b2 = make_btn(scr, "悔棋", bw, bh, on_undo_clicked, NULL);
    lv_obj_set_pos(b2, 16 + bw + bgap, by);
    lv_obj_t *b3 = make_btn(scr, "提示", bw, bh, on_hint_clicked, NULL);
    lv_obj_set_pos(b3, 16 + 2 * (bw + bgap), by);
    lv_obj_t *b4 = make_btn(scr, "选边", bw, bh, on_side_clicked, NULL);
    lv_obj_set_pos(b4, 16 + 3 * (bw + bgap), by);

    render_all();
    return scr;
}

void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "xq";
    out->name = "中国象棋";
    out->icon = "icon.png";
    out->create = xq_create;
}
