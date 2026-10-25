// tools/parking_app/main/parking_main.c
// Metalio 动态应用：挪车电话 (超大字纯粹展示)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "lvgl.h"
#include "metalio/abi.h"
#include "metalio/services.h"
#include "fontpack_lvgl.h"

#include "font_misans_regular_160_2.c"

#define SCR_W 480
#define SCR_H 800

typedef struct {
    char name[24];
    char number[16];
} SlotItem;

static int s_active_slot = 0;
static int s_edit_slot = 0;
static SlotItem s_slots[3] = {
    {"常用", "13800000000"},
    {"备用", "13900000000"},
    {"家人", "13700000000"}
};

static int s_edit_digits[11] = {1, 6, 6, 0, 1, 1, 0, 3, 6, 1, 8};

// UI 句柄
static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_num_part1_lbl = NULL; // "166"
static lv_obj_t *s_num_part2_lbl = NULL; // "0000"
static lv_obj_t *s_num_part3_lbl = NULL; // "0000"
static lv_obj_t *s_slot_btns[3] = {NULL};
static lv_obj_t *s_slot_lbls[3] = {NULL};

// 全屏壁纸预览 Overlay
static lv_obj_t *s_wall_modal = NULL;
static lv_obj_t *s_wp_part1_lbl = NULL;
static lv_obj_t *s_wp_part2_lbl = NULL;
static lv_obj_t *s_wp_part3_lbl = NULL;

// 弹窗组件
static lv_obj_t *s_modal = NULL;
static lv_obj_t *s_preview_lbl = NULL;
static lv_obj_t *s_digit_val_lbls[11] = {NULL};
static lv_obj_t *s_target_slot_btns[3] = {NULL};

// 纯图形粗黑三角形箭头 (16x10 L8 位图，100% 独立于字库)
static uint8_t s_arrow_up_l8[16 * 10];
static uint8_t s_arrow_dn_l8[16 * 10];
static lv_image_dsc_t s_arrow_up_dsc;
static lv_image_dsc_t s_arrow_dn_dsc;
static bool s_arrows_inited = false;

static void init_arrow_images(void) {
    if (s_arrows_inited) return;
    memset(s_arrow_up_l8, 0xFF, sizeof(s_arrow_up_l8)); // 0xFF = 白底
    memset(s_arrow_dn_l8, 0xFF, sizeof(s_arrow_dn_l8));

    // 向上箭头 ▲ (16 宽, 10 高)
    for (int y = 0; y < 10; ++y) {
        int half_w = y;
        int x0 = 8 - half_w;
        int x1 = 7 + half_w;
        if (x0 < 0) x0 = 0;
        if (x1 > 15) x1 = 15;
        for (int x = x0; x <= x1; ++x) {
            s_arrow_up_l8[y * 16 + x] = 0x00; // 0x00 = 纯黑
        }
    }

    // 向下箭头 ▼ (16 宽, 10 高)
    for (int y = 0; y < 10; ++y) {
        int half_w = 9 - y;
        int x0 = 8 - half_w;
        int x1 = 7 + half_w;
        if (x0 < 0) x0 = 0;
        if (x1 > 15) x1 = 15;
        for (int x = x0; x <= x1; ++x) {
            s_arrow_dn_l8[y * 16 + x] = 0x00; // 0x00 = 纯黑
        }
    }

    s_arrow_up_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_arrow_up_dsc.header.cf = LV_COLOR_FORMAT_L8;
    s_arrow_up_dsc.header.flags = 0;
    s_arrow_up_dsc.header.w = 16;
    s_arrow_up_dsc.header.h = 10;
    s_arrow_up_dsc.header.stride = 16;
    s_arrow_up_dsc.data_size = sizeof(s_arrow_up_l8);
    s_arrow_up_dsc.data = s_arrow_up_l8;

    s_arrow_dn_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_arrow_dn_dsc.header.cf = LV_COLOR_FORMAT_L8;
    s_arrow_dn_dsc.header.flags = 0;
    s_arrow_dn_dsc.header.w = 16;
    s_arrow_dn_dsc.header.h = 10;
    s_arrow_dn_dsc.header.stride = 16;
    s_arrow_dn_dsc.data_size = sizeof(s_arrow_dn_l8);
    s_arrow_dn_dsc.data = s_arrow_dn_l8;

    s_arrows_inited = true;
}

static void load_config(void) {
    void *fp = dynapp_fopen_data("config.json", "r");
    if (!fp) return;

    char buf[1024];
    size_t len = fread(buf, 1, sizeof(buf) - 1, (FILE*)fp);
    fclose((FILE*)fp);
    buf[len] = '\0';

    char *p = strstr(buf, "\"active_slot\":");
    if (p) {
        s_active_slot = atoi(p + 14);
        if (s_active_slot < 0 || s_active_slot > 2) s_active_slot = 0;
    }

    p = strstr(buf, "\"number\":");
    int idx = 0;
    while (p && idx < 3) {
        char *q = strchr(p + 9, '\"');
        if (q) {
            char *end = strchr(q + 1, '\"');
            if (end && (end - q - 1) == 11) {
                memcpy(s_slots[idx].number, q + 1, 11);
                s_slots[idx].number[11] = '\0';
                idx++;
            }
        }
        p = strstr(p + 9, "\"number\":");
    }
}

static void save_config(void) {
    void *fp = dynapp_fopen_data("config.json", "w");
    if (!fp) return;

    fprintf((FILE*)fp, "{\n  \"active_slot\": %d,\n  \"slots\": [\n", s_active_slot);
    for (int i = 0; i < 3; ++i) {
        fprintf((FILE*)fp, "    {\"name\": \"%s\", \"number\": \"%s\"}%s\n",
                s_slots[i].name, s_slots[i].number, (i < 2) ? "," : "");
    }
    fprintf((FILE*)fp, "  ]\n}\n");
    fclose((FILE*)fp);
}

// 刷新主屏超大号码与槽位高亮
static void refresh_main_view(void) {
    const char *raw = s_slots[s_active_slot].number;
    if (strlen(raw) == 11) {
        char part1[8], part2[8], part3[8];
        snprintf(part1, sizeof(part1), "%.3s", raw);
        snprintf(part2, sizeof(part2), "%.4s", raw + 3);
        snprintf(part3, sizeof(part3), "%.4s", raw + 7);

        if (s_num_part1_lbl) lv_label_set_text(s_num_part1_lbl, part1);
        if (s_num_part2_lbl) lv_label_set_text(s_num_part2_lbl, part2);
        if (s_num_part3_lbl) lv_label_set_text(s_num_part3_lbl, part3);

        if (s_wp_part1_lbl) lv_label_set_text(s_wp_part1_lbl, part1);
        if (s_wp_part2_lbl) lv_label_set_text(s_wp_part2_lbl, part2);
        if (s_wp_part3_lbl) lv_label_set_text(s_wp_part3_lbl, part3);
    }

    for (int i = 0; i < 3; ++i) {
        if (!s_slot_btns[i] || !s_slot_lbls[i]) continue;
        const char *num = s_slots[i].number;
        char label_text[32];
        if (strlen(num) == 11) {
            snprintf(label_text, sizeof(label_text), "[%d] %s\n%.3s***%.4s", i + 1, s_slots[i].name, num, num + 7);
        } else {
            snprintf(label_text, sizeof(label_text), "[%d] %s", i + 1, s_slots[i].name);
        }
        lv_label_set_text(s_slot_lbls[i], label_text);

        if (i == s_active_slot) {
            // 激活项：反白黑底白字
            lv_obj_set_style_bg_color(s_slot_btns[i], lv_color_black(), 0);
            lv_obj_set_style_bg_opa(s_slot_btns[i], LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(s_slot_lbls[i], lv_color_white(), 0);
            lv_obj_set_style_border_color(s_slot_btns[i], lv_color_black(), 0);
        } else {
            // 未激活项：白底黑字
            lv_obj_set_style_bg_color(s_slot_btns[i], lv_color_white(), 0);
            lv_obj_set_style_bg_opa(s_slot_btns[i], LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(s_slot_lbls[i], lv_color_black(), 0);
            lv_obj_set_style_border_color(s_slot_btns[i], lv_color_black(), 0);
        }
    }
}

static void on_slot_clicked(lv_event_t *e) {
    intptr_t idx = (intptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx > 2) return;
    if (idx == s_active_slot) return;

    s_active_slot = (int)idx;
    save_config();
    refresh_main_view();
    dynapp_epd_full_refresh();
}

static void draw_glyph_to_a2i1(uint8_t *bits, int W, int H, int stride,
                               const lv_font_fmt_txt_dsc_t *fdsc,
                               int gx, int gy, char ch) {
    if (ch < '0' || ch > '9') return;
    int gid = ch - '0' + 1;
    const lv_font_fmt_txt_glyph_dsc_t *g = &fdsc->glyph_dsc[gid];
    const uint8_t *bitmap = fdsc->glyph_bitmap;
    int start_x = gx + g->ofs_x;
    int start_y = gy + (124 - g->box_h - g->ofs_y);
    int bit_pos = 0;
    for (int r = 0; r < g->box_h; ++r) {
        for (int c = 0; c < g->box_w; ++c) {
            int byte_off = g->bitmap_index + (bit_pos >> 2);
            int shift = 6 - ((bit_pos & 3) << 1);
            int val = (bitmap[byte_off] >> shift) & 0x03;
            bit_pos++;
            if (val > 0) {
                int px = start_x + c;
                int py = start_y + r;
                if (px >= 0 && px < W && py >= 0 && py < H) {
                    bits[py * stride + (px >> 3)] &= ~(0x80 >> (px & 7)); // 0=黑
                }
            }
        }
    }
}

static void draw_line_to_a2i1(uint8_t *bits, int W, int H, int stride,
                              const lv_font_fmt_txt_dsc_t *fdsc,
                              int y_center, const char *str, int len) {
    int total_w = 0;
    for (int i = 0; i < len; ++i) {
        if (str[i] >= '0' && str[i] <= '9') {
            total_w += fdsc->glyph_dsc[str[i] - '0' + 1].adv_w / 16;
        }
    }
    int cur_x = (W - total_w) / 2;
    int y_top = y_center - 70;
    for (int i = 0; i < len; ++i) {
        if (str[i] >= '0' && str[i] <= '9') {
            draw_glyph_to_a2i1(bits, W, H, stride, fdsc, cur_x, y_top, str[i]);
            cur_x += fdsc->glyph_dsc[str[i] - '0' + 1].adv_w / 16;
        }
    }
}

static bool generate_parking_wallpaper(const char *number) {
    if (!number || strlen(number) != 11) return false;

    const int W = 480;
    const int H = 800;
    const int stride = 60; // (480 + 7) / 8
    const size_t bitmap_bytes = (size_t)stride * H; // 48000
    const size_t total_size = 20 + bitmap_bytes;    // 48020

    // 仅分配 48KB，彻底杜绝内存碎片或 OOM
    uint8_t *a2i1 = (uint8_t*)malloc(total_size);
    if (!a2i1) return false;

    memset(a2i1, 0, 20);
    // A2I1 格式头 (系统待机/关机壁纸原生支持格式)
    a2i1[0] = 'A'; a2i1[1] = '2'; a2i1[2] = 'I'; a2i1[3] = '1';
    a2i1[4] = (uint8_t)(W & 0xFF);
    a2i1[5] = (uint8_t)((W >> 8) & 0xFF);
    a2i1[6] = (uint8_t)(H & 0xFF);
    a2i1[7] = (uint8_t)((H >> 8) & 0xFF);
    a2i1[8] = (uint8_t)(stride & 0xFF);
    a2i1[9] = (uint8_t)((stride >> 8) & 0xFF);
    // 调色板: index0 黑, index1 白
    a2i1[12] = 0xFF; a2i1[13] = 0x00; a2i1[14] = 0x00; a2i1[15] = 0x00;
    a2i1[16] = 0xFF; a2i1[17] = 0xFF; a2i1[18] = 0xFF; a2i1[19] = 0xFF;

    uint8_t *bits = a2i1 + 20;
    memset(bits, 0xFF, bitmap_bytes); // 1 = 纯白背景

    const lv_font_fmt_txt_dsc_t *fdsc = (const lv_font_fmt_txt_dsc_t*)font_misans_regular_160_2.dsc;
    draw_line_to_a2i1(bits, W, H, stride, fdsc, 160, number, 3);
    draw_line_to_a2i1(bits, W, H, stride, fdsc, 380, number + 3, 4);
    draw_line_to_a2i1(bits, W, H, stride, fdsc, 600, number + 7, 4);

    FILE *fp = fopen("/sdcard/metalio/e-ink/wallpaper/parking_board.a2i1", "wb");
    if (!fp) {
        free(a2i1);
        return false;
    }
    fwrite(a2i1, 1, total_size, fp);
    fclose(fp);
    free(a2i1);

    dynapp_wallpaper_set_standby("parking_board.a2i1");
    return true;
}

static void open_wall_modal_cb(lv_event_t *e) {
    (void)e;
    if (!s_wall_modal) return;
    const char *raw = s_slots[s_active_slot].number;
    if (strlen(raw) == 11) {
        char part1[8], part2[8], part3[8];
        snprintf(part1, sizeof(part1), "%.3s", raw);
        snprintf(part2, sizeof(part2), "%.4s", raw + 3);
        snprintf(part3, sizeof(part3), "%.4s", raw + 7);

        if (s_wp_part1_lbl) lv_label_set_text(s_wp_part1_lbl, part1);
        if (s_wp_part2_lbl) lv_label_set_text(s_wp_part2_lbl, part2);
        if (s_wp_part3_lbl) lv_label_set_text(s_wp_part3_lbl, part3);
    }

    // 仅生成图片并保存为待机壁纸，不进入休眠
    generate_parking_wallpaper(raw);
    dynapp_toast_show("壁纸已保存并设为待机常显", 1500);

    lv_obj_remove_flag(s_wall_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_wall_modal);
    dynapp_epd_full_refresh();
}

static void close_wall_modal_cb(lv_event_t *e) {
    (void)e;
    if (s_wall_modal) {
        lv_obj_add_flag(s_wall_modal, LV_OBJ_FLAG_HIDDEN);
        dynapp_epd_full_refresh();
    }
}

static void update_modal_preview(void) {
    if (!s_preview_lbl) return;
    char raw[16];
    for (int i = 0; i < 11; ++i) raw[i] = (char)('0' + s_edit_digits[i]);
    raw[11] = '\0';

    char buf[64];
    snprintf(buf, sizeof(buf), "预览: %.3s %.4s %.4s", raw, raw + 3, raw + 7);
    lv_label_set_text(s_preview_lbl, buf);
    lv_obj_invalidate(s_preview_lbl);

    for (int i = 0; i < 11; ++i) {
        if (s_digit_val_lbls[i]) {
            char d_str[2] = {(char)('0' + s_edit_digits[i]), '\0'};
            lv_label_set_text(s_digit_val_lbls[i], d_str);
            lv_obj_invalidate(s_digit_val_lbls[i]);
        }
    }
}

static void on_digit_step_clicked(lv_event_t *e) {
    intptr_t arg = (intptr_t)lv_event_get_user_data(e);
    int col = (int)(arg % 100);
    bool is_down = (arg >= 100);

    if (col < 0 || col >= 11) return;

    // 滚轮视觉规则（遵从用户要求：8 上是 7，下是 9）：
    // 点击上按键 ▲：数字向上滚，变为上一个数字 (8 -> 7，减 1)
    // 点击下按键 ▼：数字向下滚，变为下一个数字 (8 -> 9，加 1)
    if (is_down) {
        s_edit_digits[col] = (s_edit_digits[col] + 1) % 10;
    } else {
        s_edit_digits[col] = (s_edit_digits[col] + 9) % 10;
    }
    update_modal_preview();
}

static void select_edit_slot(int slot) {
    if (slot < 0 || slot > 2) return;
    s_edit_slot = slot;

    for (int i = 0; i < 3; ++i) {
        if (!s_target_slot_btns[i]) continue;
        if (i == s_edit_slot) {
            lv_obj_set_style_bg_color(s_target_slot_btns[i], lv_color_black(), 0);
            lv_obj_set_style_bg_opa(s_target_slot_btns[i], LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(s_target_slot_btns[i], lv_color_white(), 0);
        } else {
            lv_obj_set_style_bg_color(s_target_slot_btns[i], lv_color_white(), 0);
            lv_obj_set_style_bg_opa(s_target_slot_btns[i], LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(s_target_slot_btns[i], lv_color_black(), 0);
        }
    }

    const char *num = s_slots[s_edit_slot].number;
    for (int i = 0; i < 11; ++i) {
        s_edit_digits[i] = (i < (int)strlen(num)) ? (num[i] - '0') : 0;
    }
    update_modal_preview();
}

static void on_target_slot_clicked(lv_event_t *e) {
    intptr_t slot = (intptr_t)lv_event_get_user_data(e);
    select_edit_slot((int)slot);
}

static void do_open_config_modal(void) {
    if (!s_modal) return;
    select_edit_slot(s_active_slot);
    lv_obj_remove_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_modal);
    dynapp_epd_full_refresh();
}

static void open_config_modal_cb(lv_event_t *e) {
    (void)e;
    do_open_config_modal();
}

static void do_close_config_modal(void) {
    if (s_modal) {
        lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);
        dynapp_epd_full_refresh();
    }
}

static void close_config_modal_cb(lv_event_t *e) {
    (void)e;
    do_close_config_modal();
}

static void on_save_clicked(lv_event_t *e) {
    (void)e;
    for (int i = 0; i < 11; ++i) {
        s_slots[s_edit_slot].number[i] = (char)('0' + s_edit_digits[i]);
    }
    s_slots[s_edit_slot].number[11] = '\0';
    s_active_slot = s_edit_slot;

    save_config();
    do_close_config_modal();
    refresh_main_view();
    dynapp_epd_full_refresh();
    dynapp_toast_show("已保存并生效", 1500);
}

static int parking_on_vk_key(const char *key) {
    if (!key) return 0;
    if (strcmp(key, "vk_home") == 0 || strcmp(key, "home") == 0 ||
        strcmp(key, "back") == 0 || strcmp(key, "esc") == 0) {
        if (s_wall_modal && !lv_obj_has_flag(s_wall_modal, LV_OBJ_FLAG_HIDDEN)) {
            close_wall_modal_cb(NULL);
            return 1;
        }
        if (s_modal && !lv_obj_has_flag(s_modal, LV_OBJ_FLAG_HIDDEN)) {
            do_close_config_modal();
            return 1;
        }
        dynapp_screen_go_home();
        return 1;
    }
    return 0;
}

static void parking_screen_delete_cb(lv_event_t *e) {
    (void)e;
    s_scr = NULL;
    s_num_part1_lbl = NULL;
    s_num_part2_lbl = NULL;
    s_num_part3_lbl = NULL;
    s_modal = NULL;
    s_wall_modal = NULL;
    s_wp_part1_lbl = NULL;
    s_wp_part2_lbl = NULL;
    s_wp_part3_lbl = NULL;
    s_preview_lbl = NULL;
    for (int i = 0; i < 3; ++i) {
        s_slot_btns[i] = NULL;
        s_slot_lbls[i] = NULL;
        s_target_slot_btns[i] = NULL;
    }
    for (int i = 0; i < 11; ++i) {
        s_digit_val_lbls[i] = NULL;
    }
}

static void* parking_create(void) {
    init_arrow_images();
    load_config();

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, SCR_W, SCR_H);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_scr = scr;
    lv_obj_add_event_cb(scr, parking_screen_delete_cb, LV_EVENT_DELETE, NULL);

    dynapp_screen_status_bar(scr);
    dynapp_vk_attach(scr, "parking", parking_create, parking_on_vk_key);

    const lv_font_t *font_ui = fontpack_lv_font_ui();

    // ================= 1. 核心超大号码展示区 (Y: 50 ~ 580) =================
    // 三行超级大字，每排高达 160 像素！占满整个视野中心，无任何废话！
    lv_obj_t *num_box = lv_obj_create(scr);
    lv_obj_remove_style_all(num_box);
    lv_obj_set_size(num_box, 460, 520);
    lv_obj_set_pos(num_box, 10, 50);
    lv_obj_set_flex_flow(num_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(num_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(num_box, 0, 0);

    // 第一行大字 (例如 166)
    s_num_part1_lbl = lv_label_create(num_box);
    lv_obj_set_style_text_font(s_num_part1_lbl, &font_misans_regular_160_2, 0);
    lv_obj_set_style_text_color(s_num_part1_lbl, lv_color_black(), 0);

    // 第二行大字 (例如 0000)
    s_num_part2_lbl = lv_label_create(num_box);
    lv_obj_set_style_text_font(s_num_part2_lbl, &font_misans_regular_160_2, 0);
    lv_obj_set_style_text_color(s_num_part2_lbl, lv_color_black(), 0);

    // 第三行大字 (例如 0000)
    s_num_part3_lbl = lv_label_create(num_box);
    lv_obj_set_style_text_font(s_num_part3_lbl, &font_misans_regular_160_2, 0);
    lv_obj_set_style_text_color(s_num_part3_lbl, lv_color_black(), 0);

    // ================= 2. 底部槽位选择与配置栏 (Y: 580 ~ 780) =================
    lv_obj_t *slots_row = lv_obj_create(scr);
    lv_obj_remove_style_all(slots_row);
    lv_obj_set_size(slots_row, 448, 76);
    lv_obj_set_pos(slots_row, 16, 580);
    lv_obj_set_flex_flow(slots_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(slots_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    for (int i = 0; i < 3; ++i) {
        lv_obj_t *btn = lv_obj_create(slots_row);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, 142, 70);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btn, lv_color_black(), 0);
        lv_obj_set_style_border_width(btn, 2, 0);
        lv_obj_set_style_radius(btn, 8, 0);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(btn, on_slot_clicked, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        lv_obj_t *lbl = lv_label_create(btn);
        if (font_ui) lv_obj_set_style_text_font(lbl, font_ui, 0);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_center(lbl);

        s_slot_btns[i] = btn;
        s_slot_lbls[i] = lbl;
    }

    // 底部操作栏：左边【调节号码】，右边【全屏壁纸预览】
    lv_obj_t *cfg_btn = lv_obj_create(scr);
    lv_obj_remove_style_all(cfg_btn);
    lv_obj_set_size(cfg_btn, 216, 62);
    lv_obj_set_pos(cfg_btn, 16, 670);
    lv_obj_set_style_bg_color(cfg_btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(cfg_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(cfg_btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(cfg_btn, 2, 0);
    lv_obj_set_style_radius(cfg_btn, 10, 0);
    lv_obj_add_flag(cfg_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cfg_btn, open_config_modal_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cfg_lbl = lv_label_create(cfg_btn);
    lv_label_set_text(cfg_lbl, "调节号码");
    if (font_ui) lv_obj_set_style_text_font(cfg_lbl, font_ui, 0);
    lv_obj_set_style_text_color(cfg_lbl, lv_color_black(), 0);
    lv_obj_center(cfg_lbl);

    lv_obj_t *wall_btn = lv_obj_create(scr);
    lv_obj_remove_style_all(wall_btn);
    lv_obj_set_size(wall_btn, 216, 62);
    lv_obj_set_pos(wall_btn, 248, 670);
    lv_obj_set_style_bg_color(wall_btn, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(wall_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(wall_btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(wall_btn, 2, 0);
    lv_obj_set_style_radius(wall_btn, 10, 0);
    lv_obj_add_flag(wall_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(wall_btn, open_wall_modal_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *wall_lbl = lv_label_create(wall_btn);
    lv_label_set_text(wall_lbl, "保存壁纸并预览");
    if (font_ui) lv_obj_set_style_text_font(wall_lbl, font_ui, 0);
    lv_obj_set_style_text_color(wall_lbl, lv_color_white(), 0);
    lv_obj_center(wall_lbl);

    // ================= 3. 11 位数字格子配置弹窗 (Modal) =================
    s_modal = lv_obj_create(scr);
    lv_obj_remove_style_all(s_modal);
    lv_obj_set_size(s_modal, SCR_W, SCR_H);
    lv_obj_set_pos(s_modal, 0, 0);
    lv_obj_set_style_bg_color(s_modal, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_modal, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_modal, LV_OBJ_FLAG_HIDDEN);

    // 弹窗顶栏
    lv_obj_t *m_top = lv_obj_create(s_modal);
    lv_obj_remove_style_all(m_top);
    lv_obj_set_size(m_top, 448, 60);
    lv_obj_set_pos(m_top, 16, 48);
    lv_obj_set_style_border_side(m_top, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(m_top, 2, 0);
    lv_obj_set_style_border_color(m_top, lv_color_black(), 0);

    lv_obj_t *m_title = lv_label_create(m_top);
    lv_label_set_text(m_title, "号码格子调节");
    if (font_ui) lv_obj_set_style_text_font(m_title, font_ui, 0);
    lv_obj_set_style_text_color(m_title, lv_color_black(), 0);
    lv_obj_align(m_title, LV_ALIGN_LEFT_MID, 0, 0);

    // 目标槽位选择胶囊
    lv_obj_t *slot_pick_row = lv_obj_create(m_top);
    lv_obj_remove_style_all(slot_pick_row);
    lv_obj_set_size(slot_pick_row, 220, 44);
    lv_obj_align(slot_pick_row, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_flex_flow(slot_pick_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(slot_pick_row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(slot_pick_row, 4, 0);

    const char *slot_names[3] = {"槽位1", "槽位2", "槽位3"};
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *t_btn = lv_obj_create(slot_pick_row);
        lv_obj_remove_style_all(t_btn);
        lv_obj_set_size(t_btn, 68, 38);
        lv_obj_set_style_bg_opa(t_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(t_btn, lv_color_black(), 0);
        lv_obj_set_style_border_width(t_btn, 2, 0);
        lv_obj_set_style_radius(t_btn, 6, 0);
        lv_obj_add_flag(t_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(t_btn, on_target_slot_clicked, LV_EVENT_CLICKED, (void*)(intptr_t)i);

        lv_obj_t *t_lbl = lv_label_create(t_btn);
        lv_label_set_text(t_lbl, slot_names[i]);
        if (font_ui) lv_obj_set_style_text_font(t_lbl, font_ui, 0);
        lv_obj_center(t_lbl);

        s_target_slot_btns[i] = t_btn;
    }

    // 实时预览标签
    s_preview_lbl = lv_label_create(s_modal);
    lv_label_set_text(s_preview_lbl, "预览: 138 0000 0000");
    if (font_ui) lv_obj_set_style_text_font(s_preview_lbl, font_ui, 0);
    lv_obj_set_style_text_color(s_preview_lbl, lv_color_black(), 0);
    lv_obj_set_pos(s_preview_lbl, 24, 120);

    // 11 位数字微调格子网格
    lv_obj_t *grid_cont = lv_obj_create(s_modal);
    lv_obj_remove_style_all(grid_cont);
    lv_obj_set_size(grid_cont, 448, 280);
    lv_obj_set_pos(grid_cont, 16, 170);
    lv_obj_set_flex_flow(grid_cont, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(grid_cont, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(grid_cont, 2, 0);

    const lv_font_t *font_latin36 = fontpack_lv_font_latin(36);
    if (!font_latin36) font_latin36 = font_ui;

    for (int col = 0; col < 11; ++col) {
        lv_obj_t *col_box = lv_obj_create(grid_cont);
        lv_obj_remove_style_all(col_box);
        lv_obj_set_size(col_box, 38, 260);
        lv_obj_set_flex_flow(col_box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col_box, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        // 上步进按钮 (图形实心黑三角 ▲，向上滚动，减1：如 8 -> 7)
        lv_obj_t *up_btn = lv_obj_create(col_box);
        lv_obj_remove_style_all(up_btn);
        lv_obj_set_size(up_btn, 36, 56);
        lv_obj_set_style_border_color(up_btn, lv_color_black(), 0);
        lv_obj_set_style_border_width(up_btn, 2, 0);
        lv_obj_set_style_radius(up_btn, 6, 0);
        lv_obj_set_style_bg_color(up_btn, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(up_btn, LV_OPA_COVER, 0);
        lv_obj_add_flag(up_btn, LV_OBJ_FLAG_CLICKABLE);
        intptr_t up_arg = (intptr_t)col; // 0..10 表示上按键
        lv_obj_add_event_cb(up_btn, on_digit_step_clicked, LV_EVENT_CLICKED, (void*)up_arg);

        lv_obj_t *up_img = lv_image_create(up_btn);
        lv_image_set_src(up_img, &s_arrow_up_dsc);
        lv_obj_center(up_img);
        lv_obj_clear_flag(up_img, LV_OBJ_FLAG_CLICKABLE);

        // 中间当前数字方块 (纯白底，纯黑字)
        lv_obj_t *digit_box = lv_obj_create(col_box);
        lv_obj_remove_style_all(digit_box);
        lv_obj_set_size(digit_box, 36, 80);
        lv_obj_set_style_border_color(digit_box, lv_color_black(), 0);
        lv_obj_set_style_border_width(digit_box, 2, 0);
        lv_obj_set_style_radius(digit_box, 8, 0);
        lv_obj_set_style_bg_color(digit_box, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(digit_box, LV_OPA_COVER, 0);

        lv_obj_t *d_lbl = lv_label_create(digit_box);
        char d_str[2] = {(char)('0' + s_edit_digits[col]), '\0'};
        lv_label_set_text(d_lbl, d_str);
        if (font_latin36) lv_obj_set_style_text_font(d_lbl, font_latin36, 0);
        lv_obj_set_style_text_color(d_lbl, lv_color_black(), 0);
        lv_obj_center(d_lbl);
        s_digit_val_lbls[col] = d_lbl;

        // 下步进按钮 (图形实心黑三角 ▼，向下滚动，加1：如 8 -> 9)
        lv_obj_t *dn_btn = lv_obj_create(col_box);
        lv_obj_remove_style_all(dn_btn);
        lv_obj_set_size(dn_btn, 36, 56);
        lv_obj_set_style_border_color(dn_btn, lv_color_black(), 0);
        lv_obj_set_style_border_width(dn_btn, 2, 0);
        lv_obj_set_style_radius(dn_btn, 6, 0);
        lv_obj_set_style_bg_color(dn_btn, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(dn_btn, LV_OPA_COVER, 0);
        lv_obj_add_flag(dn_btn, LV_OBJ_FLAG_CLICKABLE);
        intptr_t dn_arg = (intptr_t)(100 + col); // 100..110 表示下按键
        lv_obj_add_event_cb(dn_btn, on_digit_step_clicked, LV_EVENT_CLICKED, (void*)dn_arg);

        lv_obj_t *dn_img = lv_image_create(dn_btn);
        lv_image_set_src(dn_img, &s_arrow_dn_dsc);
        lv_obj_center(dn_img);
        lv_obj_clear_flag(dn_img, LV_OBJ_FLAG_CLICKABLE);
    }

    // 弹窗底栏操作
    lv_obj_t *m_bottom = lv_obj_create(s_modal);
    lv_obj_remove_style_all(m_bottom);
    lv_obj_set_size(m_bottom, 448, 70);
    lv_obj_set_pos(m_bottom, 16, 520);
    lv_obj_set_flex_flow(m_bottom, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(m_bottom, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *cancel_btn = lv_obj_create(m_bottom);
    lv_obj_remove_style_all(cancel_btn);
    lv_obj_set_size(cancel_btn, 180, 56);
    lv_obj_set_style_bg_opa(cancel_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(cancel_btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(cancel_btn, 2, 0);
    lv_obj_set_style_radius(cancel_btn, 10, 0);
    lv_obj_set_style_bg_color(cancel_btn, lv_color_white(), 0);
    lv_obj_add_flag(cancel_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(cancel_btn, close_config_modal_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *c_lbl = lv_label_create(cancel_btn);
    lv_label_set_text(c_lbl, "取消");
    if (font_ui) lv_obj_set_style_text_font(c_lbl, font_ui, 0);
    lv_obj_set_style_text_color(c_lbl, lv_color_black(), 0);
    lv_obj_center(c_lbl);

    lv_obj_t *save_btn = lv_obj_create(m_bottom);
    lv_obj_remove_style_all(save_btn);
    lv_obj_set_size(save_btn, 240, 56);
    lv_obj_set_style_bg_opa(save_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(save_btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(save_btn, 2, 0);
    lv_obj_set_style_radius(save_btn, 10, 0);
    lv_obj_set_style_bg_color(save_btn, lv_color_black(), 0);
    lv_obj_add_flag(save_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(save_btn, (lv_event_cb_t)on_save_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *s_lbl = lv_label_create(save_btn);
    lv_label_set_text(s_lbl, "保存并生效");
    if (font_ui) lv_obj_set_style_text_font(s_lbl, font_ui, 0);
    lv_obj_set_style_text_color(s_lbl, lv_color_white(), 0);
    lv_obj_center(s_lbl);

    // ================= 4. 全屏壁纸预览 Overlay =================
    s_wall_modal = lv_obj_create(scr);
    lv_obj_remove_style_all(s_wall_modal);
    lv_obj_set_size(s_wall_modal, SCR_W, SCR_H);
    lv_obj_set_pos(s_wall_modal, 0, 0);
    lv_obj_set_style_bg_color(s_wall_modal, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_wall_modal, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_wall_modal, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_wall_modal, close_wall_modal_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_wall_modal, LV_OBJ_FLAG_HIDDEN);

    // 三行大字满屏居中容器 (Y: 20 ~ 680，点击直接退出)
    lv_obj_t *wp_num_box = lv_obj_create(s_wall_modal);
    lv_obj_remove_style_all(wp_num_box);
    lv_obj_set_size(wp_num_box, 480, 650);
    lv_obj_set_pos(wp_num_box, 0, 20);
    lv_obj_set_flex_flow(wp_num_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(wp_num_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(wp_num_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(wp_num_box, close_wall_modal_cb, LV_EVENT_CLICKED, NULL);

    s_wp_part1_lbl = lv_label_create(wp_num_box);
    lv_obj_set_style_text_font(s_wp_part1_lbl, &font_misans_regular_160_2, 0);
    lv_obj_set_style_text_color(s_wp_part1_lbl, lv_color_black(), 0);
    lv_obj_add_flag(s_wp_part1_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_wp_part1_lbl, close_wall_modal_cb, LV_EVENT_CLICKED, NULL);

    s_wp_part2_lbl = lv_label_create(wp_num_box);
    lv_obj_set_style_text_font(s_wp_part2_lbl, &font_misans_regular_160_2, 0);
    lv_obj_set_style_text_color(s_wp_part2_lbl, lv_color_black(), 0);
    lv_obj_add_flag(s_wp_part2_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_wp_part2_lbl, close_wall_modal_cb, LV_EVENT_CLICKED, NULL);

    s_wp_part3_lbl = lv_label_create(wp_num_box);
    lv_obj_set_style_text_font(s_wp_part3_lbl, &font_misans_regular_160_2, 0);
    lv_obj_set_style_text_color(s_wp_part3_lbl, lv_color_black(), 0);
    lv_obj_add_flag(s_wp_part3_lbl, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_wp_part3_lbl, close_wall_modal_cb, LV_EVENT_CLICKED, NULL);

    // 底部通栏大返回按钮 (宽 448，高 62，X: 16, Y: 700)
    lv_obj_t *wp_back_btn = lv_obj_create(s_wall_modal);
    lv_obj_remove_style_all(wp_back_btn);
    lv_obj_set_size(wp_back_btn, 448, 62);
    lv_obj_set_pos(wp_back_btn, 16, 700);
    lv_obj_set_style_bg_opa(wp_back_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(wp_back_btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(wp_back_btn, 2, 0);
    lv_obj_set_style_radius(wp_back_btn, 10, 0);
    lv_obj_set_style_bg_color(wp_back_btn, lv_color_black(), 0);
    lv_obj_add_flag(wp_back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(wp_back_btn, close_wall_modal_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *wp_back_lbl = lv_label_create(wp_back_btn);
    lv_label_set_text(wp_back_lbl, "✓ 壁纸已保存 · 轻触屏幕返回");
    if (font_ui) lv_obj_set_style_text_font(wp_back_lbl, font_ui, 0);
    lv_obj_set_style_text_color(wp_back_lbl, lv_color_white(), 0);
    lv_obj_center(wp_back_lbl);

    refresh_main_view();
    return scr;
}

void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "parking";
    out->name = "挪车电话";
    out->icon = "icon.png";
    out->create = parking_create;
}
