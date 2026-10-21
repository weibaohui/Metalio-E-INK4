// tools/sol_app/main/sol_main.c
// 纸牌接龙 动态应用：回合制、一步一刷，适配墨水屏低刷新率特性。
//
// 设计要点（墨水屏 480x800 / 1bpp 单色）：
//   - 52 张牌常驻 52 个对象，移动时只重排受影响的 1~2 个堆 → 脏区小，局刷快
//   - 牌面：点数用 18@2（数字 + A/J/Q/K），花色用 30@2（♠♥♦♣）——字体包里都有
//   - 1bpp 单色没有红黑之分：红桃/方块用"花色字 + 牌框内白底"区分不了颜色，
//     故红花色(♥♦)用黑底白字、黑花色(♠♣)用白底黑字，一眼可分
//   - 暗牌 = 实心黑牌（1bpp 下最省事也最像"背面"）
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "esp_random.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

#define NSUIT 4
#define NRANK 13
#define NCARD (NSUIT * NRANK)
#define NCOL 7
#define NFND 4

#define CARD_W 58
#define CARD_H 82
#define GAP_DOWN 20 // 暗牌叠放间距
#define GAP_UP 28   // 明牌叠放间距

// 堆 id
#define PILE_COL0 0
#define PILE_FND0 (PILE_COL0 + NCOL)
#define PILE_STOCK (PILE_FND0 + NFND)
#define PILE_WASTE (PILE_STOCK + 1)
#define PILE_COUNT (PILE_WASTE + 1)

static const char *kSuitText[NSUIT] = {"♠", "♥", "♦", "♣"};
static const char *kRankText[NRANK] = {"A", "2", "3", "4", "5", "6", "7", "8", "9", "10", "J", "Q", "K"};

typedef struct {
    uint8_t card; // 0..51，编码 = rank*4+suit
    uint8_t up;   // 1=正面朝上
} Card;

static Card s_pile[PILE_COUNT][NCARD];
static int s_pile_n[PILE_COUNT];
static int s_nmoves = 0;
static int s_won = 0;

// 悔棋栈：一次移动 = (来源堆, 目标堆, 起始牌在来源堆中的下标)
typedef struct {
    uint8_t from;
    uint8_t to;
    uint8_t idx;
} MoveRec;
static MoveRec s_hist[512];
static int s_hist_n = 0;

// 绘制对象
static lv_obj_t *s_pile_obj[PILE_COUNT];
static int s_pile_x[PILE_COUNT]; // 堆原点的绝对坐标
static int s_pile_y[PILE_COUNT];
static lv_obj_t *s_card_obj[NCARD];
static lv_obj_t *s_card_rank[NCARD];
static lv_obj_t *s_card_suit[NCARD];
static lv_obj_t *s_info_lbl = NULL;
static lv_obj_t *s_status_lbl = NULL;

// 选中的牌（在来源堆中的下标），-1 表示没选
static int s_sel_pile = -1;
static int s_sel_idx = -1;
static int s_hint_card = -1; // 提示高亮的牌 id
static uint8_t s_rendered_card[NCARD]; // 每张牌上次渲染的样式（0xFF=未渲染）

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
static const lv_font_t *rank_font(void) {
    const lv_font_t *f = fontpack_lv_font_get(18, 2); // 数字 + A/J/Q/K
    return f != NULL ? f : ui_font();
}
static const lv_font_t *suit_font(void) {
    const lv_font_t *f = fontpack_lv_font_get(30, 2); // ♠♥♦♣
    return f != NULL ? f : ui_font();
}

static int card_rank(int card) { return card / NSUIT; }  // 0=A .. 12=K
static int card_suit(int card) { return card % NSUIT; }  // 0♠ 1♥ 2♦ 3♣
static int is_red(int card) { const int s = card_suit(card); return s == 1 || s == 2; }

// ================= 牌局 =================
static void shuffle(int *deck) {
    for (int i = NCARD - 1; i > 0; --i) {
        const int j = (int)(esp_random() % (uint32_t)(i + 1));
        const int t = deck[i];
        deck[i] = deck[j];
        deck[j] = t;
    }
}

static void new_game(void) {
    int deck[NCARD];
    for (int i = 0; i < NCARD; ++i) deck[i] = i;
    shuffle(deck);

    memset(s_pile, 0, sizeof(s_pile));
    memset(s_pile_n, 0, sizeof(s_pile_n));
    int k = 0;
    // 发牌：第 i 列发 i+1 张，最后一张正面朝上
    for (int c = 0; c < NCOL; ++c) {
        for (int j = 0; j <= c; ++j) {
            s_pile[PILE_COL0 + c][j].card = (uint8_t)deck[k++];
            s_pile[PILE_COL0 + c][j].up = (j == c) ? 1 : 0;
        }
        s_pile_n[PILE_COL0 + c] = c + 1;
    }
    // 剩下的进牌堆（从上往下发，s_pile[STOCK] 从后往前抽）
    for (int i = 0; i < NCARD - k; ++i) {
        s_pile[PILE_STOCK][i].card = (uint8_t)deck[k + i];
        s_pile[PILE_STOCK][i].up = 0;
    }
    s_pile_n[PILE_STOCK] = NCARD - k;
    s_nmoves = 0;
    s_won = 0;
    s_hist_n = 0;
    s_sel_pile = s_sel_idx = -1;
    s_hint_card = -1;
    memset(s_rendered_card, 0xFF, sizeof(s_rendered_card));
    s_move_count = 0;
    s_last_full_refresh = 0;
}

// ================= 规则 =================
static int pile_top(int p) { return s_pile_n[p] > 0 ? s_pile[p][s_pile_n[p] - 1].card : -1; }

// 能否把 card 放到第 col 列
static int can_put_col(int card, int col) {
    const int top = pile_top(PILE_COL0 + col);
    if (top < 0) {
        return card_rank(card) == NRANK - 1; // 空列只放 K
    }
    if (card_rank(card) + 1 != card_rank(top)) return 0;
    return is_red(card) != is_red(top); // 花色交替
}

// 能否把 card 放到第 f 个基础堆
static int can_put_fnd(int card, int f) {
    if (card_suit(card) != f) return 0;
    const int top = pile_top(PILE_FND0 + f);
    if (top < 0) return card_rank(card) == 0; // 空堆只放 A
    return card_rank(card) == card_rank(top) + 1;
}

// 选中的那组牌（从 s_sel_idx 到该堆末尾）能否整体放到目标堆
static int group_can_go(int target_pile) {
    if (s_sel_pile < 0 || s_sel_idx < 0) return 0;
    const int card = s_pile[s_sel_pile][s_sel_idx].card;
    if (target_pile == PILE_STOCK) return 0;
    if (target_pile == PILE_WASTE) return 0;
    if (target_pile >= PILE_FND0) return can_put_fnd(card, target_pile - PILE_FND0);
    return can_put_col(card, target_pile - PILE_COL0);
}

// ================= 绘制 =================
static void render_card(int id) {
    lv_obj_t *o = s_card_obj[id];
    if (o == NULL) return;
    const int rank = card_rank(id);
    const int suit = card_suit(id);
    const uint8_t style = (uint8_t)((rank << 2) | suit);
    if (s_rendered_card[id] == style) return;
    s_rendered_card[id] = style;

    const int red = is_red(id);
    // 红花色：黑底白字；黑花色：白底黑字（1bpp 无颜色，用明暗区分阵营）
    lv_obj_set_style_bg_color(o, red ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(o, red ? lv_color_white() : lv_color_black(), 0);
    lv_obj_set_style_border_width(o, 1, 0);

    lv_label_set_text(s_card_rank[id], kRankText[rank]);
    lv_obj_set_style_text_color(s_card_rank[id], red ? lv_color_white() : lv_color_black(), 0);
    lv_obj_clear_flag(s_card_rank[id], LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_card_suit[id], LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_card_suit[id], kSuitText[suit]);
    lv_obj_set_style_text_color(s_card_suit[id], red ? lv_color_white() : lv_color_black(), 0);
}

// 把某堆的牌摆到画面上（只动这一堆）
static void relayout_pile(int p) {
    if (p == PILE_STOCK) {
        // 牌堆只显示最上面一张（背面），其余隐藏
        for (int i = 0; i < s_pile_n[p]; ++i) {
            const int id = s_pile[p][i].card;
            if (s_card_obj[id] == NULL) continue;
            lv_obj_set_parent(s_card_obj[id], s_scr);
            lv_obj_set_pos(s_card_obj[id], s_pile_x[p], s_pile_y[p]);
            const bool top = (i == s_pile_n[p] - 1);
            if (top) {
                lv_obj_clear_flag(s_card_obj[id], LV_OBJ_FLAG_HIDDEN);
                lv_obj_set_style_bg_color(s_card_obj[id], lv_color_black(), 0);
                lv_obj_set_style_bg_opa(s_card_obj[id], LV_OPA_COVER, 0);
                lv_obj_set_style_border_color(s_card_obj[id], lv_color_white(), 0);
                lv_obj_set_style_border_width(s_card_obj[id], 1, 0);
            } else {
                lv_obj_add_flag(s_card_obj[id], LV_OBJ_FLAG_HIDDEN);
            }
        }
        return;
    }
    if (p == PILE_WASTE) {
        // 废牌堆只显示最上面一张（其余压在下边）
        for (int i = 0; i < s_pile_n[p]; ++i) {
            const int id = s_pile[p][i].card;
            if (s_card_obj[id] == NULL) continue;
            lv_obj_set_parent(s_card_obj[id], s_scr);
            lv_obj_set_pos(s_card_obj[id], s_pile_x[p], s_pile_y[p]);
            const bool top = (i == s_pile_n[p] - 1);
            if (top) {
                lv_obj_clear_flag(s_card_obj[id], LV_OBJ_FLAG_HIDDEN);
                render_card(id);
            } else {
                lv_obj_add_flag(s_card_obj[id], LV_OBJ_FLAG_HIDDEN);
            }
        }
        return;
    }
    int y = 0;
    for (int i = 0; i < s_pile_n[p]; ++i) {
        const int id = s_pile[p][i].card;
        if (s_card_obj[id] == NULL) continue;
        lv_obj_set_parent(s_card_obj[id], s_scr);
        lv_obj_set_pos(s_card_obj[id], s_pile_x[p], s_pile_y[p] + y);
        // 叠放的牌必须按从下到上的顺序置顶，否则上层牌会被下层牌盖住、
        // 点击也会命中下面那张（牌 id 与叠放顺序无关，z 序是随机的）
        lv_obj_move_foreground(s_card_obj[id]);
        if (s_pile[p][i].up) {
            lv_obj_clear_flag(s_card_obj[id], LV_OBJ_FLAG_HIDDEN);
            render_card(id);
            y += GAP_UP;
        } else {
            // 暗牌：画成黑牌背（不能隐藏，否则看不到这列还压着几张暗牌）
            lv_obj_clear_flag(s_card_obj[id], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(s_card_obj[id], lv_color_black(), 0);
            lv_obj_set_style_bg_opa(s_card_obj[id], LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(s_card_obj[id], lv_color_white(), 0);
            lv_obj_set_style_border_width(s_card_obj[id], 1, 0);
            lv_obj_add_flag(s_card_rank[id], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_card_suit[id], LV_OBJ_FLAG_HIDDEN);
            y += GAP_DOWN;
        }
    }
}

static void relayout_all(void) {
    for (int p = 0; p < PILE_COUNT; ++p) relayout_pile(p);
}

static void update_info_line(void) {
    if (s_info_lbl == NULL) return;
    char buf[64];
    const int stock_left = s_pile_n[PILE_STOCK];
    snprintf(buf, sizeof(buf), "牌堆 %d · 步 %d", stock_left, s_nmoves);
    lv_label_set_text(s_info_lbl, buf);
}

static void update_status_line(void) {
    if (s_status_lbl == NULL) return;
    char buf[96];
    if (s_won) {
        snprintf(buf, sizeof(buf), "全部归位，恭喜通关");
    } else if (s_sel_pile >= 0) {
        snprintf(buf, sizeof(buf), "已选 %s%s，点目标堆放下", kRankText[card_rank(s_pile[s_sel_pile][s_sel_idx].card)],
                 "");
    } else {
        snprintf(buf, sizeof(buf), "点牌选中 · 点堆放下 · 暗牌自动翻开");
    }
    lv_label_set_text(s_status_lbl, buf);
}

// ================= 持久化 =================
static void save_game(void) {
    FILE *fp = (FILE *)dynapp_fopen_data("sol.txt", "w");
    if (fp == NULL) return;
    fprintf(fp, "SOL1 %d %d", s_nmoves, s_won);
    for (int p = 0; p < PILE_COUNT; ++p) fprintf(fp, " %d", s_pile_n[p]);
    fputc('\n', fp);
    // 牌序：每个堆写 count 张（2 位十六进制 = rank*4+suit）
    static const char kHex[] = "0123456789abcdef";
    for (int p = 0; p < PILE_COUNT; ++p) {
        for (int i = 0; i < s_pile_n[p]; ++i) {
            fputc(kHex[s_pile[p][i].card >> 4], fp);
            fputc(kHex[s_pile[p][i].card & 0xF], fp);
        }
    }
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
    FILE *fp = (FILE *)dynapp_fopen_data("sol.txt", "r");
    if (fp == NULL) return 0;
    char magic[16] = {0};
    int nmoves = 0, won = 0;
    if (fscanf(fp, "%15s", magic) != 1 || strcmp(magic, "SOL1") != 0) {
        fclose(fp);
        return 0;
    }
    if (fscanf(fp, "%d %d", &nmoves, &won) != 2) {
        fclose(fp);
        return 0;
    }
    int counts[PILE_COUNT];
    for (int p = 0; p < PILE_COUNT; ++p) {
        if (fscanf(fp, "%d", &counts[p]) != 1 || counts[p] < 0 || counts[p] > NCARD) {
            fclose(fp);
            return 0;
        }
    }
    int total = 0;
    for (int p = 0; p < PILE_COUNT; ++p) total += counts[p];
    if (total != NCARD) {
        fclose(fp);
        return 0;
    }
    memset(s_pile, 0, sizeof(s_pile));
    memset(s_pile_n, 0, sizeof(s_pile_n));
    for (int p = 0; p < PILE_COUNT; ++p) {
        for (int i = 0; i < counts[p]; ++i) {
            int hi = 0, lo = 0;
            if (fscanf(fp, "%1x%1x", &hi, &lo) != 2) {
                fclose(fp);
                return 0;
            }
            const int card = (hi << 4) | lo;
            if (card < 0 || card >= NCARD) {
                fclose(fp);
                return 0;
            }
            s_pile[p][i].card = (uint8_t)card;
            s_pile[p][i].up = 1;
        }
        s_pile_n[p] = counts[p];
    }
    fclose(fp);
    // 暗牌：每列除最后一张外都是暗牌
    for (int c = 0; c < NCOL; ++c) {
        const int p = PILE_COL0 + c;
        for (int i = 0; i + 1 < s_pile_n[p]; ++i) s_pile[p][i].up = 0;
    }
    for (int i = 0; i < s_pile_n[PILE_STOCK]; ++i) s_pile[PILE_STOCK][i].up = 0;
    s_nmoves = nmoves;
    s_won = won;
    s_hist_n = 0;
    s_sel_pile = s_sel_idx = -1;
    s_hint_card = -1;
    memset(s_rendered_card, 0xFF, sizeof(s_rendered_card));
    s_move_count = 0;
    s_last_full_refresh = 0;
    return 1;
}

// ================= 交互 =================
static void schedule_idle_maintenance(void);
static void clear_hint(void);

static int check_win(void) {
    for (int f = 0; f < NFND; ++f) {
        if (s_pile_n[PILE_FND0 + f] != NRANK) return 0;
    }
    return 1;
}

static void do_move(int from, int to, int idx) {
    clear_hint();
    const int cnt = s_pile_n[from] - idx;
    if (cnt <= 0) return;
    // 记录
    if (s_hist_n < (int)(sizeof(s_hist) / sizeof(s_hist[0]))) {
        s_hist[s_hist_n].from = (uint8_t)from;
        s_hist[s_hist_n].to = (uint8_t)to;
        s_hist[s_hist_n].idx = (uint8_t)idx;
        s_hist_n++;
    }
    for (int i = 0; i < cnt; ++i) {
        s_pile[to][s_pile_n[to]++] = s_pile[from][idx + i];
    }
    s_pile_n[from] = idx;
    // 暗牌自动翻开
    if (from >= PILE_COL0 && from < PILE_COL0 + NCOL && s_pile_n[from] > 0) {
        s_pile[from][s_pile_n[from] - 1].up = 1;
    }
    s_nmoves++;
    s_move_count++;
    relayout_pile(from);
    relayout_pile(to);
    if (check_win() && !s_won) {
        s_won = 1;
        update_info_line();
        update_status_line();
        save_game();
        dynapp_modal_alert("通关！", "四门牌都归位了。", "确定");
        return;
    }
    update_info_line();
    update_status_line();
    schedule_idle_maintenance();
}

static void draw_from_stock(void) {
    clear_hint();
    if (s_pile_n[PILE_STOCK] <= 0) {
        dynapp_toast_show("牌堆已空", 1200);
        return;
    }
    // 从牌堆末尾抽一张，抽出后从牌堆数组移除（同一张牌不能同时存在于两堆）
    const int last = s_pile_n[PILE_STOCK] - 1;
    const int waste_idx = s_pile_n[PILE_WASTE];
    s_pile[PILE_WASTE][s_pile_n[PILE_WASTE]++] = s_pile[PILE_STOCK][last];
    s_pile[PILE_WASTE][s_pile_n[PILE_WASTE] - 1].up = 1;
    s_pile_n[PILE_STOCK] = last;
    // 抽牌也要能悔：按"牌堆 -> 废牌堆"记，悔棋才能把牌放回牌堆末尾
    if (s_hist_n < (int)(sizeof(s_hist) / sizeof(s_hist[0]))) {
        s_hist[s_hist_n].from = PILE_STOCK;
        s_hist[s_hist_n].to = PILE_WASTE;
        s_hist[s_hist_n].idx = (uint8_t)waste_idx;
        s_hist_n++;
    }
    s_nmoves++;
    s_move_count++;
    relayout_pile(PILE_STOCK);
    relayout_pile(PILE_WASTE);
    update_info_line();
    update_status_line();
    schedule_idle_maintenance();
}

static void on_card_clicked(lv_event_t *e) {
    const int id = (int)(intptr_t)lv_event_get_user_data(e);
    // 找到这张牌在哪个堆、第几张
    int found_pile = -1, found_idx = -1;
    for (int p = 0; p < PILE_COUNT && found_pile < 0; ++p) {
        for (int i = 0; i < s_pile_n[p]; ++i) {
            if (s_pile[p][i].card == id) {
                found_pile = p;
                found_idx = i;
                break;
            }
        }
    }
    if (found_pile < 0) return;
    // 牌堆：顶牌是暗牌（背面），点它就是抽牌 —— 必须先于"暗牌不能选"的判断
    if (found_pile == PILE_STOCK) {
        if (found_idx != s_pile_n[PILE_STOCK] - 1) {
            dynapp_toast_show("只能抽最上面一张", 1200);
            return;
        }
        draw_from_stock();
        return;
    }
    // 已经选过牌 → 先试落子。
    // 目标可以是一列的任意位置（点在那列的暗牌区也���"放到这一列"），
    // 所以这里不能先用"暗牌不能选"挡掉。
    if (s_sel_pile >= 0) {
        if (s_sel_pile == found_pile && s_sel_idx == found_idx) {
            s_sel_pile = s_sel_idx = -1; // 取消选择
            update_status_line();
            return;
        }
        if (group_can_go(found_pile)) {
            do_move(s_sel_pile, found_pile, s_sel_idx);
            s_sel_pile = s_sel_idx = -1;
            return;
        }
        // 落不了：若点的是己方明牌，改选它
        const int mine_col = (found_pile >= PILE_COL0 && found_pile < PILE_COL0 + NCOL);
        const int mine_waste = (found_pile == PILE_WASTE && found_idx == s_pile_n[PILE_WASTE] - 1);
        if (s_pile[found_pile][found_idx].up && (mine_col || mine_waste)) {
            s_sel_pile = found_pile;
            s_sel_idx = found_idx;
            update_status_line();
            return;
        }
        dynapp_toast_show("这步不合规则", 1200);
        return;
    }
    if (!s_pile[found_pile][found_idx].up) {
        dynapp_toast_show("暗牌要先翻开", 1200);
        return;
    }
    // 选中：只能选"可移动"的那部分（堆顶，或废牌堆顶）
    if (found_pile == PILE_WASTE) {
        if (found_idx != s_pile_n[PILE_WASTE] - 1) {
            dynapp_toast_show("只能选最上面一张", 1200);
            return;
        }
    } else if (found_pile >= PILE_COL0 && found_pile < PILE_COL0 + NCOL) {
        if (found_idx != s_pile_n[found_pile] - 1) {
            // 该牌下面还有明牌 → 整组一起移动，选中这一组的起点
            s_sel_pile = found_pile;
            s_sel_idx = found_idx;
            update_status_line();
            return;
        }
    }
    s_sel_pile = found_pile;
    s_sel_idx = found_idx;
    update_status_line();
}

static void on_pile_clicked(lv_event_t *e) {
    const int p = (int)(intptr_t)lv_event_get_user_data(e);
    if (p == PILE_STOCK) {
        draw_from_stock();
        return;
    }
    if (p == PILE_WASTE) {
        if (s_pile_n[PILE_WASTE] > 0) {
            s_sel_pile = PILE_WASTE;
            s_sel_idx = s_pile_n[PILE_WASTE] - 1;
            update_status_line();
        }
        return;
    }
    if (s_sel_pile < 0) {
        if (p >= PILE_FND0 && p < PILE_FND0 + NFND && s_pile_n[p] > 0) {
            s_sel_pile = p;
            s_sel_idx = s_pile_n[p] - 1;
            update_status_line();
        }
        return;
    }
    if (group_can_go(p)) {
        do_move(s_sel_pile, p, s_sel_idx);
        s_sel_pile = s_sel_idx = -1;
        return;
    }
    dynapp_toast_show("这步不合规则", 1200);
}

static void on_new_game_clicked(lv_event_t *e) {
    (void)e;
    new_game();
    relayout_all();
    update_info_line();
    update_status_line();
    save_game();
    dynapp_toast_show("新局", 1200);
    dynapp_epd_full_refresh();
}

static void on_undo_clicked(lv_event_t *e) {
    (void)e;
    if (s_hist_n <= 0) {
        dynapp_toast_show("没有可悔的步骤", 1200);
        return;
    }
    const MoveRec *m = &s_hist[s_hist_n - 1];
    const int from = m->from, to = m->to, idx = m->idx;
    const int cnt = s_pile_n[to] - idx;
    for (int i = cnt - 1; i >= 0; --i) {
        s_pile[from][s_pile_n[from]++] = s_pile[to][idx + i];
        // 还原明暗状态
        if (from >= PILE_COL0 && from < PILE_COL0 + NCOL) {
            s_pile[from][s_pile_n[from] - 1].up = (s_pile_n[from] - 1 == idx) ? 1 : 0;
        } else if (from == PILE_STOCK) {
            s_pile[from][s_pile_n[from] - 1].up = 0; // 撤回牌堆的牌是暗牌
        }
    }
    s_pile_n[to] = idx;
    s_hist_n--;
    if (s_nmoves > 0) s_nmoves--;
    s_won = 0;
    s_sel_pile = s_sel_idx = -1;
    relayout_pile(from);
    relayout_pile(to);
    update_info_line();
    update_status_line();
    save_game();
    dynapp_toast_show("已悔一步", 1200);
    dynapp_epd_full_refresh();
}

static void clear_hint(void) {
    if (s_hint_card < 0) return;
    if (s_card_obj[s_hint_card] != NULL) {
        s_rendered_card[s_hint_card] = 0xFF; // 强制重绘，恢复 1px 边框
        render_card(s_hint_card);
    }
    s_hint_card = -1;
}

static void mark_hint(int card) {
    clear_hint();
    if (s_card_obj[card] == NULL) return;
    s_hint_card = card;
    lv_obj_set_style_border_width(s_card_obj[card], 3, 0);
}

static void on_hint_clicked(lv_event_t *e) {
    (void)e;
    clear_hint();
    // 1) 优先找能进基础堆的牌
    for (int p = PILE_WASTE; p >= 0; --p) {
        if (p == PILE_STOCK) continue;
        if (s_pile_n[p] == 0) continue;
        if (p >= PILE_COL0 && p < PILE_COL0 + NCOL) {
            if (!s_pile[p][s_pile_n[p] - 1].up) continue;
        }
        const int card = pile_top(p);
        for (int f = 0; f < NFND; ++f) {
            if (can_put_fnd(card, f)) {
                mark_hint(card);
                char buf[48];
                snprintf(buf, sizeof(buf), "%s%s 可进基础堆", kRankText[card_rank(card)], "");
                dynapp_toast_show(buf, 1600);
                dynapp_epd_full_refresh();
                return;
            }
        }
    }
    // 2) 找能在列之间移动的牌
    for (int c = 0; c < NCOL; ++c) {
        const int p = PILE_COL0 + c;
        if (s_pile_n[p] == 0) continue;
        const int card = pile_top(p);
        for (int t = 0; t < NCOL; ++t) {
            if (t == c) continue;
            if (can_put_col(card, t)) {
                mark_hint(card);
                dynapp_toast_show("有一张牌可以移到别的列", 1600);
                dynapp_epd_full_refresh();
                return;
            }
        }
    }
    // 3) 牌堆还有牌
    if (s_pile_n[PILE_STOCK] > 0) {
        dynapp_toast_show("可以先翻一张牌堆", 1600);
        return;
    }
    dynapp_toast_show("暂时没有可走的棋", 1600);
}

static int sol_on_vk_key(const char *key) {
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
    memset(s_card_obj, 0, sizeof(s_card_obj));
    memset(s_card_rank, 0, sizeof(s_card_rank));
    memset(s_card_suit, 0, sizeof(s_card_suit));
    memset(s_pile_obj, 0, sizeof(s_pile_obj));
}

static void *sol_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    s_scr = scr;
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 800);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_screen_deleted, LV_EVENT_DELETE, NULL);

    dynapp_screen_status_bar(scr);
    dynapp_vk_attach(scr, "sol", sol_create, sol_on_vk_key);

    if (!load_game()) new_game();

    // 1. 标题
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "纸牌接龙");
    const lv_font_t *f_title = title_font();
    if (f_title != NULL) lv_obj_set_style_text_font(title, f_title, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_set_pos(title, 16, 48);

    s_info_lbl = lv_label_create(scr);
    const lv_font_t *f_ui = ui_font();
    if (f_ui != NULL) lv_obj_set_style_text_font(s_info_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_info_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_align(s_info_lbl, LV_ALIGN_TOP_RIGHT, -16, 56);

    // 2. 牌堆容器
    // 牌堆 / 废牌堆 在左上，4 个基础堆在右上，7 列在下
    const int top_y = 88;
    const int col_x0 = 16, col_dx = 65;
    for (int c = 0; c < NCOL; ++c) {
        lv_obj_t *pile = lv_obj_create(scr);
        lv_obj_remove_style_all(pile);
        lv_obj_set_pos(pile, col_x0 + c * col_dx, 186);
        lv_obj_set_size(pile, CARD_W, CARD_H);
        lv_obj_set_style_bg_opa(pile, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(pile, lv_color_black(), 0);
        lv_obj_set_style_border_width(pile, 1, 0);
        lv_obj_add_flag(pile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(pile, on_pile_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)(PILE_COL0 + c));
        lv_obj_clear_flag(pile, LV_OBJ_FLAG_SCROLLABLE);
        s_pile_obj[PILE_COL0 + c] = pile;
        s_pile_x[PILE_COL0 + c] = col_x0 + c * col_dx;
        s_pile_y[PILE_COL0 + c] = 186;
    }
    for (int f = 0; f < NFND; ++f) {
        lv_obj_t *pile = lv_obj_create(scr);
        lv_obj_remove_style_all(pile);
        lv_obj_set_pos(pile, 220 + f * 62, top_y);
        lv_obj_set_size(pile, CARD_W, CARD_H);
        lv_obj_set_style_bg_opa(pile, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(pile, lv_color_black(), 0);
        lv_obj_set_style_border_width(pile, 1, 0);
        lv_obj_add_flag(pile, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(pile, on_pile_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)(PILE_FND0 + f));
        lv_obj_clear_flag(pile, LV_OBJ_FLAG_SCROLLABLE);
        s_pile_obj[PILE_FND0 + f] = pile;
        s_pile_x[PILE_FND0 + f] = 220 + f * 62;
        s_pile_y[PILE_FND0 + f] = top_y;
    }
    {
        const int px[2] = {16, 80};
        const int pid[2] = {PILE_STOCK, PILE_WASTE};
        for (int i = 0; i < 2; ++i) {
            lv_obj_t *pile = lv_obj_create(scr);
            lv_obj_remove_style_all(pile);
            lv_obj_set_pos(pile, px[i], top_y);
            lv_obj_set_size(pile, CARD_W, CARD_H);
            lv_obj_set_style_bg_opa(pile, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_color(pile, lv_color_black(), 0);
            lv_obj_set_style_border_width(pile, 1, 0);
            lv_obj_add_flag(pile, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(pile, on_pile_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)pid[i]);
            lv_obj_clear_flag(pile, LV_OBJ_FLAG_SCROLLABLE);
            s_pile_obj[pid[i]] = pile;
            s_pile_x[pid[i]] = px[i];
            s_pile_y[pid[i]] = top_y;
        }
    }

    // 3. 52 张牌常驻对象
    for (int id = 0; id < NCARD; ++id) {
        lv_obj_t *card = lv_obj_create(scr);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, CARD_W, CARD_H);
        lv_obj_set_style_radius(card, 4, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, on_card_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)id);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        s_card_obj[id] = card;

        lv_obj_t *rk = lv_label_create(card);
        lv_label_set_text(rk, "");
        const lv_font_t *fr = rank_font();
        if (fr != NULL) lv_obj_set_style_text_font(rk, fr, 0);
        lv_obj_set_pos(rk, 4, 2);
        lv_obj_clear_flag(rk, LV_OBJ_FLAG_CLICKABLE);
        s_card_rank[id] = rk;

        lv_obj_t *su = lv_label_create(card);
        lv_label_set_text(su, "");
        const lv_font_t *fs = suit_font();
        if (fs != NULL) lv_obj_set_style_text_font(su, fs, 0);
        lv_obj_center(su);
        lv_obj_clear_flag(su, LV_OBJ_FLAG_CLICKABLE);
        s_card_suit[id] = su;
    }

    // 4. 状态行
    s_status_lbl = lv_label_create(scr);
    if (f_ui != NULL) lv_obj_set_style_text_font(s_status_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_status_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_pos(s_status_lbl, 16, 686);

    // 5. 按钮
    const lv_coord_t bw = 144, bh = 56, bgap = 8;
    const lv_coord_t by = 716;
    lv_obj_t *b1 = make_btn(scr, "新局", bw, bh, on_new_game_clicked, NULL);
    lv_obj_set_pos(b1, 16, by);
    lv_obj_t *b2 = make_btn(scr, "悔棋", bw, bh, on_undo_clicked, NULL);
    lv_obj_set_pos(b2, 16 + bw + bgap, by);
    lv_obj_t *b3 = make_btn(scr, "提示", bw, bh, on_hint_clicked, NULL);
    lv_obj_set_pos(b3, 16 + 2 * (bw + bgap), by);

    relayout_all();
    update_info_line();
    update_status_line();
    return scr;
}

void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "sol";
    out->name = "纸牌接龙";
    out->icon = "icon.png";
    out->create = sol_create;
}
