// tools/non_app/main/non_main.c
// 数织 动态应用：Nonogram / Picross，一步一刷，适配墨水屏低刷新率特性。
//
// 玩法：每行/每列旁边给出一串数字，表示这一行/列里"连续涂黑块"的个数与顺序，
//  据此把格子涂黑，还原出图案。
//
// 设计要点（墨水屏 480x800 / 1bpp 单色）：
//   - 格子只有"涂黑/空"两态（不做叉号标记，1bpp 上叉号要么用斜线旋转、
//     要么跟实心块混淆，反而不清晰）；用 1px 细网格线保持格子可辨。
//   - 填一格只改那一个对象 → 脏区极小；每 N 步全刷一次去残影。
//   - 图案全部内置为 ASCII art，提示数字由图案**自动算出**，不依赖外部素材；
//     关卡号 → 图案索引，存进存档可复现。
//   - 数字提示用 18@2 纯数字字体；标题与说明用 25@2。
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

#define GN 10  // 10x10
#define CELL 36
#define GRID_PX (GN * CELL)
#define GRID_X 100
#define GRID_Y 180
#define CLUE_LEFT 80  // 左侧行提示区宽度
#define CLUE_TOP 60   // 顶部列提示区高度

// ---- 图案库：10x10 的 ASCII art，'#'=涂黑 ----
#define PICTURE_COUNT 10
#define PICTURE_ROWS 10
#define PICTURE_COLS 10

// 上面的字符串手写容易出错，统一用行数组更稳：
// 纯字符数组（不要用 const char* 指针表！）：全局指针表里每个指针都要重定位，
// Xtensa PIC 下动态应用读这种表会触发 LoadProhibited 崩溃。
static const char kPicData[PICTURE_COUNT][PICTURE_ROWS][PICTURE_COLS + 1] = {
    { // 心
        ".##...##..",
        "##.....##.",
        "##.....##.",
        "#########.",
        "#########.",
        ".#######..",
        "..#####...",
        "...###....",
        "....#.....",
        "..........",
    },
    { // 星
        "....##....",
        "....##....",
        "...####...",
        "##########",
        ".########.",
        "..######..",
        "..##..##..",
        ".##....##.",
        "##......##",
        "..........",
    },
    { // 鱼
        "..........",
        ".##.......",
        "####...###",
        "##########",
        ".#########",
        "####...###",
        ".##.......",
        "..........",
        "..........",
        "..........",
    },
    { // 房子
        "...####...",
        "..######..",
        ".########.",
        "##########",
        "##########",
        "##.####.##",
        "##.####.##",
        "##......##",
        "##......##",
        "##########",
    },
    { // 树
        "....##....",
        "...####...",
        "..######..",
        ".########.",
        "..######..",
        ".########.",
        "....##....",
        "....##....",
        "....##....",
        "....##....",
    },
    { // 猫脸
        "##......##",
        "###....###",
        "##########",
        "##########",
        "##########",
        "##.####.##",
        "##.####.##",
        "###....###",
        "##......##",
        "..........",
    },
    { // 伞
        "...####...",
        "..######..",
        ".########.",
        "##########",
        "##########",
        "....##....",
        "....##....",
        "....##....",
        "...####...",
        "..........",
    },
    { // 笑脸
        "..######..",
        ".########.",
        "##..##..##",
        "##..##..##",
        "##########",
        "##########",
        "##########",
        ".########.",
        "..######..",
        "..........",
    },
    { // 花
        "...##..##.",
        "..#######.",
        "..#######.",
        "...####...",
        "..######..",
        ".########.",
        "..######..",
        "...####...",
        "....##....",
        "..........",
    },
    { // 帆船
        "....#.....",
        "....##....",
        "....###...",
        "....####..",
        "....###...",
        "....##....",
        "....#.....",
        ".########.",
        "##########",
        "..........",
    },
};

// ---- 状态 ----
static uint8_t s_mark[GN][GN];   // 玩家涂黑的格子
static uint8_t s_target[GN][GN]; // 目标图案（由 ASCII art 解出）
static uint8_t s_drawn[GN][GN];  // 上次渲染状态，0xFF=未画
static int s_pic = 0;            // 当前图案索引
static int s_moves = 0;
static int s_done[16];           // 各图案是否通关过
static int s_show_preview = 0;

// ---- 绘制对象 ----
static lv_obj_t *s_cell_obj[GN][GN];
static lv_obj_t *s_row_clue[GN];
static lv_obj_t *s_col_clue[GN];
static lv_obj_t *s_preview = NULL;
static lv_obj_t *s_info_lbl = NULL;
static lv_obj_t *s_status_lbl = NULL;

static lv_obj_t *s_scr = NULL;
static int s_move_count = 0;
static int s_last_full_refresh = 0;
#define FULL_REFRESH_EVERY 12

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

// ================= 图案与提示 =================
static void load_picture(int idx) {
    memset(s_target, 0, sizeof(s_target));
    memset(s_drawn, 0xFF, sizeof(s_drawn));
    s_pic = idx % PICTURE_COUNT;
    for (int r = 0; r < PICTURE_ROWS; ++r) {
        const char *line = kPicData[s_pic][r];
        for (int c = 0; c < PICTURE_COLS && line[c]; ++c) {
            s_target[r][c] = (line[c] == '#') ? 1 : 0;
        }
    }
    s_moves = 0;
    s_show_preview = 0;
}

// 一行（或一列）里连续黑块的个数
static int clue_count(const uint8_t *cells, int n, int *out) {
    int n_out = 0, run = 0;
    for (int i = 0; i < n; ++i) {
        if (cells[i]) {
            run++;
        } else {
            if (run > 0) out[n_out++] = run;
            run = 0;
        }
    }
    if (run > 0) out[n_out++] = run;
    return n_out;
}

// ================= 渲染 =================
static void render_cell(int r, int c) {
    lv_obj_t *o = s_cell_obj[r][c];
    if (o == NULL) return;
    const uint8_t v = s_mark[r][c];
    if (s_drawn[r][c] == v) return;
    s_drawn[r][c] = v;
    if (v) {
        lv_obj_set_style_bg_color(o, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_bg_color(o, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    }
}

static void render_clues(void) {
    int cl[GN];
    for (int r = 0; r < GN; ++r) {
        uint8_t line[GN];
        for (int c = 0; c < GN; ++c) line[c] = s_target[r][c];
        const int n = clue_count(line, GN, cl);
        char buf[64];
        buf[0] = '\0'; // 必须初始化：空提示行否则会显示上一轮栈上的残留
        int off = 0;
        for (int i = 0; i < n; ++i) {
            if (i > 0) off += snprintf(buf + off, sizeof(buf) - off, " ");
            off += snprintf(buf + off, sizeof(buf) - off, "%d", cl[i]);
        }
        lv_label_set_text(s_row_clue[r], buf);
    }
    for (int c = 0; c < GN; ++c) {
        uint8_t line[GN];
        for (int r = 0; r < GN; ++r) line[r] = s_target[r][c];
        const int n = clue_count(line, GN, cl);
        char buf[64];
        buf[0] = '\0';
        int off = 0;
        for (int i = 0; i < n; ++i) {
            if (i > 0) off += snprintf(buf + off, sizeof(buf) - off, " ");
            off += snprintf(buf + off, sizeof(buf) - off, "%d", cl[i]);
        }
        lv_label_set_text(s_col_clue[c], buf);
    }
}

static void update_info_line(void) {
    if (s_info_lbl == NULL) return;
    int nd = 0;
    for (int i = 0; i < PICTURE_COUNT; ++i) nd += s_done[i] ? 1 : 0;
    char buf[64];
    snprintf(buf, sizeof(buf), "第 %d/%d 题 · 已通 %d", s_pic + 1, PICTURE_COUNT, nd);
    lv_label_set_text(s_info_lbl, buf);
}

static void update_status_line(void) {
    if (s_status_lbl == NULL) return;
    const char *t = "点格子涂黑/擦除 · 按提示的数字还原图案";
    if (s_show_preview) t = "预览：左上角是目标图案";
    lv_label_set_text(s_status_lbl, t);
}

static void render_preview(void) {
    if (s_preview == NULL) return;
    if (!s_show_preview) {
        lv_obj_add_flag(s_preview, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_clear_flag(s_preview, LV_OBJ_FLAG_HIDDEN);
    // 预览用 10x10 个 4px 小块画在右上角
    for (int i = 0; i < GN * GN; ++i) {
        lv_obj_t *d = lv_obj_get_child(s_preview, i);
        if (d == NULL) continue;
        const int r = i / GN, c = i % GN;
        if (s_target[r][c]) {
            lv_obj_clear_flag(d, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void render_all(void) {
    for (int r = 0; r < GN; ++r)
        for (int c = 0; c < GN; ++c) {
            s_drawn[r][c] = 0xFF;
            render_cell(r, c);
        }
    render_clues();
    render_preview();
    update_info_line();
    update_status_line();
}

// ================= 持久化 =================
static void save_game(void) {
    FILE *fp = (FILE *)dynapp_fopen_data("non.txt", "w");
    if (fp == NULL) return;
    fprintf(fp, "NON1 %d %d\n", s_pic, s_moves);
    for (int r = 0; r < GN; ++r) {
        for (int c = 0; c < GN; ++c) fputc(s_mark[r][c] ? '1' : '0', fp);
    }
    fputc('\n', fp);
    for (int i = 0; i < PICTURE_COUNT; ++i) fputc(s_done[i] ? '1' : '0', fp);
    fputc('\n', fp);
    fclose(fp);
}

static int load_game(void) {
    FILE *fp = (FILE *)dynapp_fopen_data("non.txt", "r");
    if (fp == NULL) return 0;
    char magic[16] = {0};
    int pic = 0, moves = 0;
    if (fscanf(fp, "%15s", magic) != 1 || strcmp(magic, "NON1") != 0) {
        fclose(fp);
        return 0;
    }
    if (fscanf(fp, "%d %d", &pic, &moves) != 2 || pic < 0 || pic >= PICTURE_COUNT) {
        fclose(fp);
        return 0;
    }
    uint8_t marks[GN][GN];
    for (int r = 0; r < GN; ++r) {
        for (int c = 0; c < GN; ++c) {
            int v = 0;
            if (fscanf(fp, "%1d", &v) != 1) {
                fclose(fp);
                return 0;
            }
            marks[r][c] = (uint8_t)(v ? 1 : 0);
        }
    }
    char done[32] = {0};
    if (fscanf(fp, "%31s", done) == 1) {
        for (int i = 0; i < PICTURE_COUNT && done[i]; ++i) s_done[i] = (done[i] == '1');
    }
    fclose(fp);
    s_pic = pic;
    s_moves = moves;
    load_picture(pic);   // 设目标图案
    memcpy(s_mark, marks, sizeof(s_mark)); // 把玩家涂的格子贴回来
    return 1;
}

// ================= 交互 =================
static int is_complete(void) {
    for (int r = 0; r < GN; ++r)
        for (int c = 0; c < GN; ++c)
            if (s_mark[r][c] != s_target[r][c]) return 0;
    return 1;
}

static void on_cell_clicked(lv_event_t *e) {
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    const int r = i / GN, c = i % GN;
    s_mark[r][c] = (uint8_t)(s_mark[r][c] ? 0 : 1);
    s_moves++;
    s_move_count++;
    render_cell(r, c);
    update_info_line();
    if (is_complete()) {
        s_done[s_pic] = 1;
        save_game();
        update_status_line();
        dynapp_modal_alert("还原成功！", "图案和提示完全对上了。", "确定");
        return;
    }
    save_game();
}

static void on_next_clicked(lv_event_t *e) {
    (void)e;
    load_picture((s_pic + 1) % PICTURE_COUNT);
    memset(s_mark, 0, sizeof(s_mark));
    memset(s_drawn, 0xFF, sizeof(s_drawn));
    render_all();
    save_game();
    char b[32];
    snprintf(b, sizeof(b), "第 %d 题", s_pic + 1);
    dynapp_toast_show(b, 1200);
    dynapp_epd_full_refresh();
}

static void on_preview_clicked(lv_event_t *e) {
    (void)e;
    s_show_preview = !s_show_preview;
    render_preview();
    update_status_line();
    dynapp_toast_show(s_show_preview ? "显示目标图案" : "已隐藏目标图案", 1200);
    dynapp_epd_full_refresh();
}

static void on_clear_clicked(lv_event_t *e) {
    (void)e;
    memset(s_mark, 0, sizeof(s_mark));
    memset(s_drawn, 0xFF, sizeof(s_drawn));
    s_moves = 0;
    render_all();
    save_game();
    dynapp_toast_show("已清空", 1000);
    dynapp_epd_full_refresh();
}

static int non_on_vk_key(const char *key) {
    if (key == NULL) return 0;
    if (strcmp(key, "vk_home") == 0 || strcmp(key, "esc") == 0 || strcmp(key, "back") == 0) {
        save_game();
        dynapp_screen_go_home();
        return 1;
    }
    if (strcmp(key, "vk_next") == 0) { // 下键 = 下一题
        on_next_clicked(NULL);
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
    s_preview = NULL;
    memset(s_cell_obj, 0, sizeof(s_cell_obj));
    memset(s_row_clue, 0, sizeof(s_row_clue));
    memset(s_col_clue, 0, sizeof(s_col_clue));
}

static void *non_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    s_scr = scr;
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 800);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_screen_deleted, LV_EVENT_DELETE, NULL);

    dynapp_screen_status_bar(scr);
    dynapp_vk_attach(scr, "non", non_create, non_on_vk_key);

    if (!load_game()) {
        load_picture(0);
        memset(s_mark, 0, sizeof(s_mark));
        s_moves = 0;
    }
    memset(s_drawn, 0xFF, sizeof(s_drawn));

    const lv_font_t *f_ui = ui_font();
    const lv_font_t *f_title = title_font();
    const lv_font_t *f_num = num_font();

    // 1. 标题
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "数织");
    if (f_title != NULL) lv_obj_set_style_text_font(title, f_title, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_set_pos(title, 16, 48);

    s_info_lbl = lv_label_create(scr);
    if (f_ui != NULL) lv_obj_set_style_text_font(s_info_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_info_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_align(s_info_lbl, LV_ALIGN_TOP_RIGHT, -16, 56);

    // 2. 行提示（左侧）
    for (int r = 0; r < GN; ++r) {
        lv_obj_t *lab = lv_label_create(scr);
        lv_label_set_text(lab, "");
        if (f_num != NULL) lv_obj_set_style_text_font(lab, f_num, 0);
        lv_obj_set_style_text_color(lab, lv_color_black(), 0);
        lv_obj_set_pos(lab, GRID_X - CLUE_LEFT, GRID_Y + r * CELL + 8);
        lv_obj_set_width(lab, CLUE_LEFT - 6);
        lv_obj_set_style_text_align(lab, LV_TEXT_ALIGN_RIGHT, 0);
        s_row_clue[r] = lab;
    }

    // 3. 列提示（顶部）
    for (int c = 0; c < GN; ++c) {
        lv_obj_t *lab = lv_label_create(scr);
        lv_label_set_text(lab, "");
        if (f_num != NULL) lv_obj_set_style_text_font(lab, f_num, 0);
        lv_obj_set_style_text_color(lab, lv_color_black(), 0);
        lv_obj_set_pos(lab, GRID_X + c * CELL, GRID_Y - CLUE_TOP + 30);
        lv_obj_set_width(lab, CELL);
        lv_obj_set_style_text_align(lab, LV_TEXT_ALIGN_CENTER, 0);
        s_col_clue[c] = lab;
    }

    // 4. 网格
    for (int r = 0; r < GN; ++r) {
        for (int c = 0; c < GN; ++c) {
            lv_obj_t *cell = lv_obj_create(scr);
            lv_obj_remove_style_all(cell);
            lv_obj_set_pos(cell, GRID_X + c * CELL, GRID_Y + r * CELL);
            lv_obj_set_size(cell, CELL, CELL);
            lv_obj_set_style_bg_color(cell, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(cell, lv_color_black(), 0);
            lv_obj_set_style_border_width(cell, 1, 0);
            lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(cell, on_cell_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)(r * GN + c));
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
            s_cell_obj[r][c] = cell;
        }
    }

    // 5. 预览（右上角 10x10 的 4px 小块）
    s_preview = lv_obj_create(scr);
    lv_obj_remove_style_all(s_preview);
    lv_obj_set_pos(s_preview, 16, 88);
    lv_obj_set_size(s_preview, 44, 44);
    lv_obj_clear_flag(s_preview, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < GN * GN; ++i) {
        lv_obj_t *d = lv_obj_create(s_preview);
        lv_obj_remove_style_all(d);
        lv_obj_set_pos(d, (i % GN) * 4, (i / GN) * 4);
        lv_obj_set_size(d, 3, 3);
        lv_obj_set_style_bg_color(d, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(d, LV_OPA_COVER, 0);
        lv_obj_clear_flag(d, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(d, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(s_preview, LV_OBJ_FLAG_HIDDEN);

    // 6. 状态行
    s_status_lbl = lv_label_create(scr);
    if (f_ui != NULL) lv_obj_set_style_text_font(s_status_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_status_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_pos(s_status_lbl, 16, GRID_Y + GRID_PX + 12);

    // 7. 按钮
    const lv_coord_t bw = 107, bh = 56, bgap = 6;
    const lv_coord_t by = 620;
    lv_obj_t *b1 = make_btn(scr, "下一题", 16, by, bw, bh, on_next_clicked, NULL);
    lv_obj_t *b2 = make_btn(scr, "预览", 16 + bw + bgap, by, bw, bh, on_preview_clicked, NULL);
    lv_obj_t *b3 = make_btn(scr, "清空", 16 + 2 * (bw + bgap), by, bw, bh, on_clear_clicked, NULL);
    (void)b1;
    (void)b2;
    (void)b3;

    render_all();
    save_game();
    return scr;
}

void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "non";
    out->name = "数织";
    out->icon = "icon.png";
    out->create = non_create;
}
