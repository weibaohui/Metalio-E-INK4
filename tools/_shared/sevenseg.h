// tools/_shared/sevenseg.h —— 七段数码管（墨水屏上大字温度/数字用）
//
// 纯几何画：7 段实心条 = 点亮的段；未点亮的段只画中央一条细线
// （整段空心矩形在 1bpp 上太重，像一堆空盒子）。
//
// 用法：
//   SevenSeg ss;
//   sevenseg_init(&ss, parent, 3, x, y, dw, dh, stroke);
//   sevenseg_set_mask(&ss, 18, 0, 0x01);  // 显示 18，最高位（第 0 位）留空
#ifndef SEVENSEG_H
#define SEVENSEG_H

#include <stdint.h>
#include <string.h>

#include "lvgl.h"

#define SEG_MAX_DIGITS 3

typedef struct {
    lv_obj_t *on[SEG_MAX_DIGITS][7];
    lv_obj_t *ghost[SEG_MAX_DIGITS][7];
    lv_obj_t *minus;
    int digits;
    int gap;
} SevenSeg;

static const uint8_t kSegMaskTab[10] = {
    0x3F, // 0
    0x06, // 1
    0x5B, // 2
    0x4F, // 3
    0x66, // 4
    0x6D, // 5
    0x7D, // 6
    0x07, // 7
    0x7F, // 8
    0x6F, // 9
};

static void sevenseg_init(SevenSeg *ss, lv_obj_t *parent, int digits, int x, int y, int w, int h,
                          int stroke) {
    if (ss == NULL || parent == NULL) return;
    memset(ss, 0, sizeof(*ss));
    ss->digits = digits > SEG_MAX_DIGITS ? SEG_MAX_DIGITS : digits;
    const int gap = 3;
    // 七段管几何（别照抄番茄钟那版：它的中段 g 被画成了竖条，"5" 会变成怪字形）：
    //   纵向切成 上竖(a下沿) / 中横(g) / 下竖 三段，中间留 gap。
    const int mid_y = (h - stroke) / 2;                          // 中横的上沿
    const int upper_h = mid_y - stroke - gap;                     // 上竖高度
    const int lower_h = (h - stroke) - (mid_y + stroke) - gap;    // 下竖高度
    ss->gap = w + 10;
    for (int d = 0; d < ss->digits; ++d) {
        const int dx = x + d * ss->gap;
        struct {
            int x, y, w, h;
        } r[7];
        r[0] = (typeof(r[0])){dx + stroke / 2, y, w - stroke, stroke};                    // a 顶(横)
        r[1] = (typeof(r[1])){dx + w - stroke, y + stroke, stroke, upper_h};              // b 右上(竖)
        r[2] = (typeof(r[2])){dx + w - stroke, y + mid_y + stroke, stroke, lower_h};      // c 右下(竖)
        r[3] = (typeof(r[3])){dx + stroke / 2, y + h - stroke, w - stroke, stroke};        // d 底(横)
        r[4] = (typeof(r[4])){dx, y + mid_y + stroke, stroke, lower_h};                    // e 左下(竖)
        r[5] = (typeof(r[5])){dx, y + stroke, stroke, upper_h};                            // f 左上(竖)
        r[6] = (typeof(r[6])){dx + stroke / 2, y + mid_y, w - stroke, stroke};             // g 中(横)

        for (int i = 0; i < 7; ++i) {
            lv_obj_t *on = lv_obj_create(parent);
            lv_obj_remove_style_all(on);
            lv_obj_set_pos(on, r[i].x, r[i].y);
            lv_obj_set_size(on, r[i].w, r[i].h);
            lv_obj_set_style_bg_color(on, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(on, LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(on, 0, 0);
            lv_obj_clear_flag(on, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
            ss->on[d][i] = on;

            lv_obj_t *ghost = lv_obj_create(parent);
            lv_obj_remove_style_all(ghost);
            const int thin = 3;
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
            ss->ghost[d][i] = ghost;
        }
    }
    // 负号（横杠），默认隐藏
    ss->minus = lv_obj_create(parent);
    lv_obj_remove_style_all(ss->minus);
    lv_obj_set_size(ss->minus, w / 2, stroke);
    lv_obj_set_style_bg_color(ss->minus, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(ss->minus, LV_OPA_COVER, 0);
    lv_obj_clear_flag(ss->minus, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(ss->minus, LV_OBJ_FLAG_HIDDEN);
}

// 显示一个整数；blank_mask 的第 d 位为 1 时，第 d 位（从左往右数，0=最高位）整位留空。
// 用途：隐藏前导零（温度 8 度不该显示成 "  8"，也不该留一个 "8" 的鬼影）。
static void sevenseg_set_mask(SevenSeg *ss, int value, int negative, uint8_t blank_mask) {
    if (ss == NULL) return;
    int neg = negative && value < 0;
    int v = neg ? -value : value;
    if (v > 999) v = 999;
    for (int d = 0; d < ss->digits; ++d) {
        // 显示位 d 是从左往右第 d 位：0=最高位（百位），digits-1=个位。
        // 别和"数值位序"搞反了——digits-1-d 才是它对应的 10 的幂次。
        const int place = ss->digits - 1 - d;
        int rem = v;
        for (int k = 0; k < place; ++k) rem /= 10;
        const int digit = rem % 10;
        const uint8_t mask = kSegMaskTab[digit];
        const int blank = (blank_mask >> d) & 1;
        for (int i = 0; i < 7; ++i) {
            if (ss->on[d][i] == NULL) continue;
            if (!blank && (mask & (1 << i))) {
                lv_obj_clear_flag(ss->on[d][i], LV_OBJ_FLAG_HIDDEN);
                lv_obj_add_flag(ss->ghost[d][i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(ss->on[d][i], LV_OBJ_FLAG_HIDDEN);
                if (blank) {
                    lv_obj_add_flag(ss->ghost[d][i], LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_clear_flag(ss->ghost[d][i], LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
    }
    if (ss->minus != NULL) {
        if (neg) {
            lv_obj_clear_flag(ss->minus, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(ss->minus, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void sevenseg_set_minus_pos(SevenSeg *ss, int x, int y) {
    if (ss != NULL && ss->minus != NULL) lv_obj_set_pos(ss->minus, x, y);
}

// 只显示未点亮的鬼影骨架（还没数据时用：告诉用户"这里等会儿会显示数字"，
// 而不是留一块空白，看着像渲染坏了）
static void sevenseg_show_ghost(SevenSeg *ss) {
    if (ss == NULL) return;
    for (int d = 0; d < ss->digits; ++d) {
        for (int i = 0; i < 7; ++i) {
            if (ss->on[d][i] != NULL) lv_obj_add_flag(ss->on[d][i], LV_OBJ_FLAG_HIDDEN);
            if (ss->ghost[d][i] != NULL) lv_obj_clear_flag(ss->ghost[d][i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (ss->minus != NULL) lv_obj_add_flag(ss->minus, LV_OBJ_FLAG_HIDDEN);
}

// 显示一个整数（自动补位）；negative=1 显示负号
static void sevenseg_set(SevenSeg *ss, int value, int negative) {
    sevenseg_set_mask(ss, value, negative, 0);
}

#endif /* SEVENSEG_H */
