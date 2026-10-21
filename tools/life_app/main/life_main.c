// tools/life_app/main/life_main.c
// 生命游戏 动态应用： Conway's Game of Life，一步一刷，适配墨水屏低刷新率特性。
//
// 设计要点（墨水屏 480x800 / 1bpp 单色）：
//   - 一代一刷：每推进一代只重绘"状态变化的格子"，整屏最多刷一次
//     （墨水屏局刷按脏区面积计费，一代里通常有 10~30% 的格子翻转，
//      脏区几乎铺满整块棋盘，所以一次刷完比逐格刷快得多）
//   - 格子用实心黑方块表示"活"、白底表示"死"，配 1px 网格线方便点格编辑
//   - 不做常驻秒级刷新：自动播放用 3 秒一代的节奏（比墨水屏刷新还慢，
//     保证每代都能看清）
//   - 边界按"死域"处理（棋盘外恒为死），滑翔机等图案会飞出边界，符合直觉
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "esp_random.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

#define LCOLS 24
#define LROWS 30
#define LCELL 18
#define LBOARD_W (LCOLS * LCELL) /* 432 */
#define LBOARD_H (LROWS * LCELL) /* 540 */
#define LBOARD_X 24
#define LBOARD_Y 96

// ---- 棋局状态 ----
static uint8_t s_alive[LCOLS * LROWS];
static uint8_t s_next[LCOLS * LROWS];
static uint8_t s_rendered[LCOLS * LROWS]; // 上次画出来的状态（0xFF=未画）
static int s_gen = 0;
static int s_auto = 0;

// ---- 绘制对象 ----
static lv_obj_t *s_cell_obj[LCOLS * LROWS];
static lv_obj_t *s_info_lbl = NULL;
static lv_obj_t *s_status_lbl = NULL;

static void auto_tick(lv_timer_t *tmr);
static lv_timer_t *s_auto_timer = NULL;

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

// ================= 图案预设 =================
// 每个预设：名字 + 宽高 + 图案（'#' 活，'.' 死）
typedef struct {
    const char *name;
    const char *rows[6];
} Preset;

// 图案行以 NULL 结束，不要写 "..." 占位行（会被当成空行截断图案）
static const Preset kPresets[] = {
    {"滑翔机", {".#.", "..#", "###", NULL}},
    {"闪烁", {".#.", ".#.", ".#.", NULL}},
    {"蟾蜍", {".###", "####", NULL}},
    {"R型五格", {"..##", ".##.", ".#..", NULL}},
    {"十字", {"..#..", "..#..", "#####", "..#..", "..#..", NULL}},
    {"小爆炸", {".#.#.", "#####", ".###.", "..#..", NULL}},
};
#define PRESET_COUNT ((int)(sizeof(kPresets) / sizeof(kPresets[0])))
static int s_preset_idx = 0;

static void clear_board(void) {
    memset(s_alive, 0, sizeof(s_alive));
    memset(s_rendered, 0xFF, sizeof(s_rendered));
    s_gen = 0;
}

static void apply_preset(int idx) {
    clear_board();
    const Preset *p = &kPresets[idx];
    int h = 0;
    while (h < 6 && p->rows[h] != NULL && p->rows[h][0] != '\0') h++;
    int w = 0;
    if (h > 0) {
        w = (int)strlen(p->rows[0]);
    }
    const int x0 = (LCOLS - w) / 2;
    const int y0 = (LROWS - h) / 2;
    for (int r = 0; r < h; ++r) {
        for (int c = 0; c < w && p->rows[r][c]; ++c) {
            if (p->rows[r][c] != '#') continue;
            const int x = x0 + c, y = y0 + r;
            if (x < 0 || x >= LCOLS || y < 0 || y >= LROWS) continue;
            s_alive[y * LCOLS + x] = 1;
        }
    }
    s_preset_idx = idx;
}

static void random_board(void) {
    for (int i = 0; i < LCOLS * LROWS; ++i) {
        s_alive[i] = (uint8_t)(esp_random() % 100u < 28u ? 1 : 0);
    }
    memset(s_rendered, 0xFF, sizeof(s_rendered));
    s_gen = 0;
}

// ================= 世代演算 =================
static int neighbors(int x, int y) {
    int n = 0;
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            if (dx == 0 && dy == 0) continue;
            const int nx = x + dx, ny = y + dy;
            if (nx < 0 || nx >= LCOLS || ny < 0 || ny >= LROWS) continue; // 棋盘外恒为死
            if (s_alive[ny * LCOLS + nx]) n++;
        }
    }
    return n;
}

// 推进一代：活细胞有 2/3 个邻居则存活，死细胞恰有 3 个邻居则诞生
static void step_generation(void) {
    for (int y = 0; y < LROWS; ++y) {
        for (int x = 0; x < LCOLS; ++x) {
            const int n = neighbors(x, y);
            const uint8_t alive = s_alive[y * LCOLS + x];
            s_next[y * LCOLS + x] = (uint8_t)((alive && (n == 2 || n == 3)) || (!alive && n == 3));
        }
    }
    memcpy(s_alive, s_next, sizeof(s_alive));
    s_gen++;
}

// ================= 绘制 =================
static void render_cell(int i) {
    lv_obj_t *o = s_cell_obj[i];
    if (o == NULL) return;
    const uint8_t v = s_alive[i];
    if (s_rendered[i] == v) return; // 没变就别动 LVGL（省脏区）
    s_rendered[i] = v;
    if (v) {
        lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_color(o, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    } else {
        // 死格子不能 HIDDEN：LVGL 命中测试会跳过隐藏对象，空格子就点不到了
        // （要点格子把死格点活）。改成透明底，网格线照常透出来。
        lv_obj_clear_flag(o, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
    }
}

static int count_alive(void) {
    int n = 0;
    for (int i = 0; i < LCOLS * LROWS; ++i) n += s_alive[i] ? 1 : 0;
    return n;
}

static void update_info_line(void) {
    if (s_info_lbl == NULL) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "第 %d 代 · %d 格", s_gen, count_alive());
    lv_label_set_text(s_info_lbl, buf);
}

static void update_status_line(void) {
    if (s_status_lbl == NULL) return;
    const char *t = s_auto ? "自动播放中 · 再按暂停" : "点格子改写 · 按「下一帧」推进";
    lv_label_set_text(s_status_lbl, t);
}

static void render_all(void) {
    for (int i = 0; i < LCOLS * LROWS; ++i) render_cell(i);
    update_info_line();
    update_status_line();
}

// ================= 持久化 =================
static void save_game(void) {
    FILE *fp = (FILE *)dynapp_fopen_data("life.txt", "w");
    if (fp == NULL) return;
    fprintf(fp, "LIFE1 %d %d %d\n", s_gen, s_auto, s_preset_idx);
    // 棋盘压成十六进制：4 格 = 1 个字符
    static const char kHex[] = "0123456789abcdef";
    for (int i = 0; i < LCOLS * LROWS; i += 4) {
        int v = 0;
        for (int k = 0; k < 4; ++k) {
            if (i + k < LCOLS * LROWS && s_alive[i + k]) v |= (1 << k);
        }
        fputc(kHex[v], fp);
    }
    fputc('\n', fp);
    fclose(fp);
}

static int load_game(void) {
    FILE *fp = (FILE *)dynapp_fopen_data("life.txt", "r");
    if (fp == NULL) return 0;
    char magic[16] = {0};
    int gen = 0, auto_ = 0, preset = 0;
    if (fscanf(fp, "%15s", magic) != 1 || strcmp(magic, "LIFE1") != 0) {
        fclose(fp);
        return 0;
    }
    if (fscanf(fp, "%d %d %d", &gen, &auto_, &preset) != 3) {
        fclose(fp);
        return 0;
    }
    const int need = (LCOLS * LROWS + 3) / 4;
    int hex[512];
    int got = 0;
    // 注意：fgetc 不在固件导出符号表里（只有 fscanf/fprintf/fputc 等），
    // 动态应用引用未导出的符号会在 dlopen 阶段直接失败
    while (got < need) {
        int v = 0;
        if (fscanf(fp, "%1x", &v) != 1) break;
        hex[got++] = v;
    }
    fclose(fp);
    if (got < need) return 0;
    for (int i = 0; i < LCOLS * LROWS; ++i) {
        const int v = hex[i / 4];
        s_alive[i] = (uint8_t)((v >> (i % 4)) & 1);
    }
    s_gen = gen;
    s_auto = auto_ ? 1 : 0;
    s_preset_idx = (preset >= 0 && preset < PRESET_COUNT) ? preset : 0;
    memset(s_rendered, 0xFF, sizeof(s_rendered));
    // 恢复了"自动播放中"就必须把定时器也拉起来，否则界面在骗人：
    // 状态写着自动播放、实际不播，而且第一次点「自动」反而只是把它关掉。
    if (s_auto) {
        s_auto_timer = lv_timer_create(auto_tick, 3000, NULL);
    }
    return 1;
}

// ================= 交互 =================
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

static void stop_auto(void) {
    if (s_auto_timer != NULL) {
        lv_timer_delete(s_auto_timer);
        s_auto_timer = NULL;
    }
    s_auto = 0;
}

static void auto_tick(lv_timer_t *tmr) {
    (void)tmr;
    step_generation();
    render_all();
    s_move_count++;
    // 自动播放每 3 秒一代，比 3.5s 的"空闲落盘"还快：靠空闲定时器存盘会被
    // 一直重置，永远存不上（代数在涨但盘上是旧的）。这里直接落盘。
    save_game();
    // 同理，全刷定时器也会被一直重置，这里直接做周期性全刷
    if (s_move_count - s_last_full_refresh >= FULL_REFRESH_EVERY) {
        s_last_full_refresh = s_move_count;
        dynapp_epd_full_refresh();
    }
}

static void on_cell_clicked(lv_event_t *e) {
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= LCOLS * LROWS) return;
    stop_auto();
    s_alive[i] = (uint8_t)(s_alive[i] ? 0 : 1);
    render_cell(i);
    update_info_line();
    update_status_line();
    s_move_count++;
    schedule_idle_maintenance();
}

static void on_step_clicked(lv_event_t *e) {
    (void)e;
    stop_auto();
    step_generation();
    render_all();
    s_move_count++;
    schedule_idle_maintenance();
}

static void on_auto_clicked(lv_event_t *e) {
    (void)e;
    if (s_auto) {
        stop_auto();
        update_status_line();
        dynapp_toast_show("已暂停", 1000);
        return;
    }
    s_auto = 1;
    // 3 秒一代：比墨水屏刷新(1~2s)慢，保证每代都看得清
    s_auto_timer = lv_timer_create(auto_tick, 3000, NULL);
    update_status_line();
    dynapp_toast_show("自动播放", 1000);
}

static void on_preset_clicked(lv_event_t *e) {
    (void)e;
    stop_auto();
    s_preset_idx = (s_preset_idx + 1) % PRESET_COUNT;
    apply_preset(s_preset_idx);
    render_all();
    s_move_count++;
    save_game();
    dynapp_toast_show(kPresets[s_preset_idx].name, 1400);
    dynapp_epd_full_refresh();
}

static void on_clear_clicked(lv_event_t *e) {
    (void)e;
    stop_auto();
    clear_board();
    render_all();
    s_move_count++;
    save_game();
    dynapp_toast_show("已清空", 1000);
    dynapp_epd_full_refresh();
}

static int life_on_vk_key(const char *key) {
    if (key == NULL) return 0;
    if (strcmp(key, "vk_home") == 0 || strcmp(key, "esc") == 0 || strcmp(key, "back") == 0) {
        stop_auto();
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
    stop_auto();
    save_game();
    s_scr = NULL;
    s_info_lbl = NULL;
    s_status_lbl = NULL;
    memset(s_cell_obj, 0, sizeof(s_cell_obj));
}

static void *life_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    s_scr = scr;
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 800);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_screen_deleted, LV_EVENT_DELETE, NULL);

    dynapp_screen_status_bar(scr);
    dynapp_vk_attach(scr, "life", life_create, life_on_vk_key);

    if (!load_game()) {
        apply_preset(0);
    }

    // 1. 标题
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "生命游戏");
    const lv_font_t *f_title = title_font();
    if (f_title != NULL) lv_obj_set_style_text_font(title, f_title, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_set_pos(title, 16, 48);

    s_info_lbl = lv_label_create(scr);
    const lv_font_t *f_ui = ui_font();
    if (f_ui != NULL) lv_obj_set_style_text_font(s_info_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_info_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_align(s_info_lbl, LV_ALIGN_TOP_RIGHT, -16, 56);

    // 2. 网格线（1px，压在格子下面）
    for (int c = 0; c <= LCOLS; ++c) {
        lv_obj_t *line = lv_obj_create(scr);
        lv_obj_remove_style_all(line);
        lv_obj_set_pos(line, LBOARD_X + c * LCELL, LBOARD_Y);
        lv_obj_set_size(line, 1, LBOARD_H);
        lv_obj_set_style_bg_color(line, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }
    for (int r = 0; r <= LROWS; ++r) {
        lv_obj_t *line = lv_obj_create(scr);
        lv_obj_remove_style_all(line);
        lv_obj_set_pos(line, LBOARD_X, LBOARD_Y + r * LCELL);
        lv_obj_set_size(line, LBOARD_W, 1);
        lv_obj_set_style_bg_color(line, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    }

    // 3. 格子（活=黑方块；死=隐藏，透出网格线）
    for (int r = 0; r < LROWS; ++r) {
        for (int c = 0; c < LCOLS; ++c) {
            const int i = r * LCOLS + c;
            lv_obj_t *cell = lv_obj_create(scr);
            lv_obj_remove_style_all(cell);
            lv_obj_set_pos(cell, LBOARD_X + c * LCELL, LBOARD_Y + r * LCELL);
            lv_obj_set_size(cell, LCELL, LCELL);
            lv_obj_set_style_bg_color(cell, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
            lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_event_cb(cell, on_cell_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
            s_cell_obj[i] = cell;
        }
    }

    // 4. 状态行
    s_status_lbl = lv_label_create(scr);
    if (f_ui != NULL) lv_obj_set_style_text_font(s_status_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_status_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_pos(s_status_lbl, 16, LBOARD_Y + LBOARD_H + 8);

    // 5. 按钮
    const lv_coord_t bw = 107, bh = 56, bgap = 6;
    const lv_coord_t by = 700;
    lv_obj_t *b1 = make_btn(scr, "下一帧", bw, bh, on_step_clicked, NULL);
    lv_obj_set_pos(b1, 16, by);
    lv_obj_t *b2 = make_btn(scr, "自动", bw, bh, on_auto_clicked, NULL);
    lv_obj_set_pos(b2, 16 + bw + bgap, by);
    lv_obj_t *b3 = make_btn(scr, "图案", bw, bh, on_preset_clicked, NULL);
    lv_obj_set_pos(b3, 16 + 2 * (bw + bgap), by);
    lv_obj_t *b4 = make_btn(scr, "清空", bw, bh, on_clear_clicked, NULL);
    lv_obj_set_pos(b4, 16 + 3 * (bw + bgap), by);

    render_all();
    return scr;
}

void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "life";
    out->name = "生命游戏";
    out->icon = "icon.png";
    out->create = life_create;
}
