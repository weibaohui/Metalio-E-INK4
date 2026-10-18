// tools/fangfang_app/main/fangfang_main.c
// 「方方」动态应用：方块战争（俄罗斯方块）的 .so 移植版，内容与内置 tetris 一致。
// 纯 C + LVGL，仅调用固件符号表内 API；点开 dlopen，退出 dlclose。
//
// 移植差异（相对 main/display/screen/tetris_screen/）：
//   - EpdPageScaffold → lv_obj 直建 + dynapp_screen_status_bar 垫片
//   - VkKey_AttachScreen / ScreenLvAsync / ScreenRequestHome / 全刷请求 → dynapp_* 垫片
//   - 最高分 NVS → SD 文件 apps/fangfang/hiscore.txt（fopen 系，已进符号表）
//   - HapticAttachClick 触觉反馈省略
// 输入：盖板三键 vk_home=旋转 vk_prev=左移 vk_next=右移；屏上按钮=暂停/重开/退出/速降。
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "lvgl.h"
#include "esp_random.h"

#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

// ---- 核心参数（tetris_core 同款）----
#define FW 10
#define FH 20
#define TYPES 7
#define STATE_RUNNING 0
#define STATE_PAUSED 1
#define STATE_OVER 2

// 7 种方块 × 4 旋转态，rot0 为出场朝向；y 向下
static const int k_shapes[TYPES][4][4][2] = {
    {  { {0,1},{1,1},{2,1},{3,1} }, { {2,0},{2,1},{2,2},{2,3} },
       { {0,2},{1,2},{2,2},{3,2} }, { {1,0},{1,1},{1,2},{1,3} } },  // I
    {  { {1,0},{2,0},{1,1},{2,1} }, { {1,0},{2,0},{1,1},{2,1} },
       { {1,0},{2,0},{1,1},{2,1} }, { {1,0},{2,0},{1,1},{2,1} } },  // O
    {  { {1,0},{0,1},{1,1},{2,1} }, { {1,0},{1,1},{2,1},{1,2} },
       { {0,1},{1,1},{2,1},{1,2} }, { {1,0},{0,1},{1,1},{1,2} } },  // T
    {  { {1,0},{2,0},{0,1},{1,1} }, { {1,0},{1,1},{2,1},{2,2} },
       { {1,1},{2,1},{0,2},{1,2} }, { {0,0},{0,1},{1,1},{1,2} } },  // S
    {  { {0,0},{1,0},{1,1},{2,1} }, { {2,0},{1,1},{2,1},{1,2} },
       { {0,1},{1,1},{1,2},{2,2} }, { {1,0},{0,1},{1,1},{0,2} } },  // Z
    {  { {2,0},{0,1},{1,1},{2,1} }, { {1,0},{1,1},{1,2},{2,2} },
       { {0,1},{1,1},{2,1},{0,2} }, { {0,0},{1,0},{1,1},{1,2} } },  // L
    {  { {0,0},{0,1},{1,1},{2,1} }, { {1,0},{2,0},{1,1},{1,2} },
       { {0,1},{1,1},{2,1},{2,2} }, { {1,0},{1,1},{0,2},{1,2} } },  // J
};

static uint8_t board[FH][FW];
static int type_, rot_, px_, py_, next_, hold_, can_hold_;
static int state_ = STATE_OVER, score_, lines_, level_, pieces_, drop_ms_;

static int rnd7(void) { return (int)(esp_random() % TYPES); }

static int core_fits(int t, int r, int x, int y) {
    for (int i = 0; i < 4; ++i) {
        const int cx = x + k_shapes[t][r][i][0];
        const int cy = y + k_shapes[t][r][i][1];
        if (cx < 0 || cx >= FW || cy >= FH) return 0;
        if (cy < 0) continue;
        if (board[cy][cx] != 0) return 0;
    }
    return 1;
}

static int core_ghost_y(void) {
    int y = py_;
    while (core_fits(type_, rot_, px_, y + 1)) ++y;
    return y;
}

static void core_spawn(void) {
    type_ = next_;
    next_ = rnd7();
    rot_ = 0;
    px_ = 3;
    py_ = 0;
    if (!core_fits(type_, rot_, px_, py_)) state_ = STATE_OVER;
}

static void core_reset(void) {
    memset(board, 0, sizeof(board));
    type_ = rot_ = px_ = py_ = 0;
    next_ = rnd7();
    hold_ = -1;
    can_hold_ = 1;
    state_ = STATE_RUNNING;
    score_ = lines_ = pieces_ = 0;
    level_ = 1;
    drop_ms_ = 800;
    core_spawn();
}

static int core_clear_lines(void) {
    int cleared = 0;
    for (int y = FH - 1; y >= 0; --y) {
        int full = 1;
        for (int x = 0; x < FW; ++x) {
            if (board[y][x] == 0) { full = 0; break; }
        }
        if (!full) continue;
        ++cleared;
        for (int yy = y; yy > 0; --yy) {
            for (int x = 0; x < FW; ++x) board[yy][x] = board[yy - 1][x];
        }
        for (int x = 0; x < FW; ++x) board[0][x] = 0;
        ++y;
    }
    if (cleared > 0) {
        static const int k_pts[4] = {100, 300, 500, 800};
        score_ += k_pts[cleared - 1] * level_;
        lines_ += cleared;
        level_ = lines_ / 10 + 1;
        drop_ms_ = 800 - (level_ - 1) * 70;
        if (drop_ms_ < 150) drop_ms_ = 150;
    }
    return cleared;
}

static void core_tick(void) {
    if (state_ != STATE_RUNNING) return;
    if (core_fits(type_, rot_, px_, py_ + 1)) { ++py_; return; }
    int lock_out = 0;
    for (int i = 0; i < 4; ++i) {
        const int x = px_ + k_shapes[type_][rot_][i][0];
        const int y = py_ + k_shapes[type_][rot_][i][1];
        if (y < 0) { lock_out = 1; continue; }
        board[y][x] = 1;
    }
    core_clear_lines();
    can_hold_ = 1;
    ++pieces_;
    core_spawn();
    if (lock_out) state_ = STATE_OVER;
}

static int core_move(int dx) {
    if (state_ != STATE_RUNNING) return 0;
    if (!core_fits(type_, rot_, px_ + dx, py_)) return 0;
    px_ += dx;
    return 1;
}

static int core_rotate(void) {
    if (state_ != STATE_RUNNING) return 0;
    const int nr = (rot_ + 1) % 4;
    static const int k_kick[4][2] = {{0,0},{-1,0},{1,0},{0,-1}};
    for (int i = 0; i < 4; ++i) {
        if (core_fits(type_, nr, px_ + k_kick[i][0], py_ + k_kick[i][1])) {
            rot_ = nr;
            px_ += k_kick[i][0];
            py_ += k_kick[i][1];
            return 1;
        }
    }
    return 0;
}

static int core_hold(void) {
    if (state_ != STATE_RUNNING || !can_hold_) return 0;
    if (hold_ < 0) {
        hold_ = type_;
        core_spawn();
    } else {
        const int t = type_;
        type_ = hold_;
        hold_ = t;
        rot_ = 0;
        px_ = 3;
        py_ = 0;
        if (!core_fits(type_, rot_, px_, py_)) state_ = STATE_OVER;
    }
    can_hold_ = 0;
    return 1;
}

static void core_soft_drop(void) {
    if (state_ == STATE_RUNNING && core_fits(type_, rot_, px_, py_ + 1)) ++py_;
}

static void core_hard_drop(void) {
    if (state_ != STATE_RUNNING) return;
    py_ = core_ghost_y();
    core_tick();
}

static void core_toggle_pause(void) {
    if (state_ == STATE_RUNNING) state_ = STATE_PAUSED;
    else if (state_ == STATE_PAUSED) state_ = STATE_RUNNING;
}

static void core_cells(const int shape[TYPES][4][2] /*未用*/, int t, int r, int px, int py, int out[4][2]) {
    for (int i = 0; i < 4; ++i) {
        out[i][0] = px + k_shapes[t][r][i][0];
        out[i][1] = py + k_shapes[t][r][i][1];
    }
}

// ---- UI 状态 ----
#define CELL_PX 30
#define BOARD_X 22
#define BOARD_Y 12
#define SIDE_X 338
#define BTN_H 60
#define FULL_REFRESH_EVERY 10

static const char* k_screen_id = "fangfang";
static const char* k_hiscore_filename = "hiscore.txt";

static int s_alive = 0;
static lv_obj_t* s_scr;
static lv_obj_t* s_board;
static lv_obj_t* s_cells[FH][FW];
static uint8_t s_render[FH][FW];
static lv_obj_t* s_cur[4];
static lv_obj_t* s_ghost[4];
static lv_obj_t* s_next_pv[4];
static lv_obj_t* s_hold_pv[4];
static int s_next_fp;
static int s_hold_fp;
static lv_obj_t* s_hold_tip;
static lv_obj_t* s_score_lbl;
static lv_obj_t* s_lines_lbl;
static lv_obj_t* s_level_lbl;
static lv_obj_t* s_best_lbl;
static char s_score_txt[16];
static char s_lines_txt[8];
static char s_level_txt[8];
static lv_obj_t* s_pause_lbl;
static lv_timer_t* s_timer;
static int s_last_full_piece;
static lv_obj_t* s_mask;
static lv_obj_t* s_card;
static int s_overlay_open;

static const lv_font_t* ui_font(void) {
    const lv_font_t* f = fontpack_lv_font_ui();
    if (f == NULL) f = fontpack_lv_font_get(25, 2);
    return f;
}

static void disable_scroll(lv_obj_t* obj) {
    if (obj) lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

// ---- 差量渲染 ----
static void set_cell_style(lv_obj_t* cell, int filled) {
    if (filled) {
        lv_obj_set_style_bg_color(cell, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(cell, 0, 0);
    } else {
        lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(cell, lv_color_hex(0xAAAAAA), 0);
        lv_obj_set_style_border_width(cell, 1, 0);
    }
}

static void redraw_board(void) {
    if (!s_alive) return;
    for (int y = 0; y < FH; ++y) {
        for (int x = 0; x < FW; ++x) {
            const uint8_t v = board[y][x] != 0 ? 1 : 0;
            if (s_render[y][x] == v || s_cells[y][x] == NULL) continue;
            s_render[y][x] = v;
            set_cell_style(s_cells[y][x], v != 0);
        }
    }
}

static void place_solid(lv_obj_t* obj, int x, int y) {
    if (obj == NULL) return;
    if (y < 0) { lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN); return; }
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(obj, (lv_coord_t)(4 + x * CELL_PX), (lv_coord_t)(4 + y * CELL_PX));
}

static void hide_all(lv_obj_t** objs, int n) {
    for (int i = 0; i < n; ++i) {
        if (objs[i] != NULL) lv_obj_add_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void draw_current(void) {
    if (!s_alive) return;
    if (state_ != STATE_RUNNING) { hide_all(s_cur, 4); return; }
    int cells[4][2];
    core_cells(NULL, type_, rot_, px_, py_, cells);
    for (int i = 0; i < 4; ++i) place_solid(s_cur[i], cells[i][0], cells[i][1]);
}

static void draw_ghost(void) {
    if (!s_alive) return;
    int show = state_ == STATE_RUNNING;
    int cur[4][2], gh[4][2];
    core_cells(NULL, type_, rot_, px_, py_, cur);
    const int gy = core_ghost_y();
    core_cells(NULL, type_, rot_, px_, gy, gh);
    if (show) {
        for (int i = 0; i < 4; ++i) {
            if (gh[i][1] == cur[i][1]) { show = 0; break; }
        }
    }
    if (!show) { hide_all(s_ghost, 4); return; }
    for (int i = 0; i < 4; ++i) {
        if (s_ghost[i] == NULL) continue;
        lv_obj_remove_flag(s_ghost[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_ghost[i], (lv_coord_t)(4 + gh[i][0] * CELL_PX),
                       (lv_coord_t)(4 + gh[i][1] * CELL_PX));
    }
}

static void draw_preview(lv_obj_t** pv, int is_hold) {
    int cells[4][2];
    if (is_hold) {
        if (hold_ < 0) { hide_all(pv, 4); return; }
        for (int i = 0; i < 4; ++i) {
            cells[i][0] = k_shapes[hold_][0][i][0];
            cells[i][1] = k_shapes[hold_][0][i][1];
        }
    } else {
        for (int i = 0; i < 4; ++i) {
            cells[i][0] = k_shapes[next_][0][i][0];
            cells[i][1] = k_shapes[next_][0][i][1];
        }
    }
    for (int i = 0; i < 4; ++i) {
        if (pv[i] == NULL) continue;
        int minx = 9, maxx = -1, miny = 9, maxy = -1;
        for (int k = 0; k < 4; ++k) {
            if (cells[k][0] < minx) minx = cells[k][0];
            if (cells[k][0] > maxx) maxx = cells[k][0];
            if (cells[k][1] < miny) miny = cells[k][1];
            if (cells[k][1] > maxy) maxy = cells[k][1];
        }
        const lv_coord_t pv_step = 22;
        const lv_coord_t ox = (110 - (maxx - minx + 1) * pv_step) / 2;
        const lv_coord_t oy = (110 - (maxy - miny + 1) * pv_step) / 2;
        lv_obj_remove_flag(pv[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(pv[i], (lv_coord_t)(ox + (cells[i][0] - minx) * pv_step + 1),
                       (lv_coord_t)(oy + (cells[i][1] - miny) * pv_step + 1));
    }
}

static int shape_fingerprint(const int cells[4][2]) {
    int fp = 0;
    for (int k = 0; k < 4; ++k) fp = fp * 16 + (cells[k][0] * 4 + cells[k][1]);
    return fp;
}

static void draw_previews(void) {
    if (!s_alive) return;
    int nc[4][2];
    for (int i = 0; i < 4; ++i) {
        nc[i][0] = k_shapes[next_][0][i][0];
        nc[i][1] = k_shapes[next_][0][i][1];
    }
    const int nfp = shape_fingerprint(nc);
    if (nfp != s_next_fp) {
        s_next_fp = nfp;
        draw_preview(s_next_pv, 0);
    }
    int hfp;
    if (hold_ >= 0) {
        int hc[4][2];
        for (int i = 0; i < 4; ++i) {
            hc[i][0] = k_shapes[hold_][0][i][0];
            hc[i][1] = k_shapes[hold_][0][i][1];
        }
        hfp = shape_fingerprint(hc);
    } else {
        hfp = -2;
    }
    if (hfp != s_hold_fp) {
        s_hold_fp = hfp;
        draw_preview(s_hold_pv, 1);
        if (s_hold_tip != NULL) {
            if (hold_ >= 0) lv_obj_add_flag(s_hold_tip, LV_OBJ_FLAG_HIDDEN);
            else lv_obj_remove_flag(s_hold_tip, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void set_text_guarded(lv_obj_t* lbl, char* last, size_t cap, int v) {
    if (lbl == NULL) return;
    char buf[24];
    snprintf(buf, sizeof(buf), "%d", v);
    if (strncmp(buf, last, cap) == 0) return;
    strncpy(last, buf, cap - 1);
    last[cap - 1] = '\0';
    lv_label_set_text(lbl, last);
}

static void update_labels(void) {
    if (!s_alive) return;
    set_text_guarded(s_score_lbl, s_score_txt, sizeof(s_score_txt), score_);
    set_text_guarded(s_lines_lbl, s_lines_txt, sizeof(s_lines_txt), lines_);
    set_text_guarded(s_level_lbl, s_level_txt, sizeof(s_level_txt), level_);
}

// ---- 最高分（应用 data 目录文件：支持热重装不覆盖、沙箱隔离）----
static int load_best(void) {
    FILE* f = (FILE*)dynapp_fopen_data(k_hiscore_filename, "r");
    if (f == NULL) return 0;
    char line[32] = {0};
    fgets(line, sizeof(line), f);
    fclose(f);
    return (int)strtol(line, NULL, 10);
}

static void save_best_if_needed(void) {
    const int best = load_best();
    if (score_ > best) {
        FILE* f = (FILE*)dynapp_fopen_data(k_hiscore_filename, "w");
        if (f != NULL) {
            fprintf(f, "%d\n", score_);
            fclose(f);
        }
    }
}

static void sync_best_label(void) {
    if (s_best_lbl == NULL) return;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", load_best());
    lv_label_set_text(s_best_lbl, buf);
}

static void sync_pause_btn(void) {
    if (s_pause_lbl == NULL) return;
    lv_label_set_text(s_pause_lbl, state_ == STATE_PAUSED ? "继续" : "暂停");
}

static void sync_timer_period(void) {
    if (s_timer == NULL) return;
    lv_timer_set_period(s_timer, (uint32_t)drop_ms_);
}

static void maybe_full_refresh(void) {
    if (pieces_ - s_last_full_piece < FULL_REFRESH_EVERY) return;
    dynapp_epd_full_refresh();
    s_last_full_piece = pieces_;
}

// ---- 游戏结束卡片（EpdModalOverlay 的 LVGL 直建替代）----
static void do_restart(void) {
    if (s_overlay_open) {
        if (s_mask != NULL) lv_obj_delete(s_mask);
        s_mask = NULL;
        s_card = NULL;
        s_overlay_open = 0;
    }
    core_reset();
    s_last_full_piece = 0;
    sync_timer_period();
    if (s_timer != NULL) lv_timer_resume(s_timer);
    memset(s_render, 0xFF, sizeof(s_render));
    s_next_fp = -1;
    s_hold_fp = -1;
    redraw_board();
    draw_current();
    draw_ghost();
    draw_previews();
    update_labels();
    sync_best_label();
    sync_pause_btn();
}

static void on_overlay_clicked(lv_event_t* e) {
    (void)e;
    if (!s_alive) return;
    do_restart();
}

static void show_game_over(void) {
    save_best_if_needed();
    static char msg[128];
    snprintf(msg, sizeof(msg), "游戏结束\n得分 %d · 最高 %d\n点按重新开始",
             score_, load_best());

    s_mask = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_mask);
    lv_obj_set_size(s_mask, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_mask, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_mask, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_mask, LV_OBJ_FLAG_CLICKABLE);
    disable_scroll(s_mask);
    lv_obj_add_event_cb(s_mask, on_overlay_clicked, LV_EVENT_CLICKED, NULL);

    s_card = lv_obj_create(s_mask);
    lv_obj_remove_style_all(s_card);
    lv_obj_set_size(s_card, 320, 160);
    lv_obj_align(s_card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_border_color(s_card, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_card, 3, 0);
    lv_obj_set_style_bg_color(s_card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_card, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_card, LV_OBJ_FLAG_CLICKABLE);
    disable_scroll(s_card);
    lv_obj_add_event_cb(s_card, on_overlay_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t* lbl = lv_label_create(s_card);
    lv_label_set_text(lbl, msg);
    const lv_font_t* f = fontpack_lv_font_get(25, 2);
    if (f != NULL) lv_obj_set_style_text_font(lbl, f, 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl);

    s_overlay_open = 1;
    if (s_timer != NULL) lv_timer_pause(s_timer);
}

// ---- 输入（全部 LVGL 线程执行）----
enum { K_IN_LEFT = 0, K_IN_RIGHT = 1, K_IN_ROTATE = 2, K_IN_DROP = 3, K_IN_HOLD = 4 };

static void apply_input(int input) {
    if (!s_alive) return;
    int changed = 0;
    switch (input) {
        case K_IN_LEFT:  changed = core_move(-1); break;
        case K_IN_RIGHT: changed = core_move(1); break;
        case K_IN_ROTATE: changed = core_rotate(); break;
        case K_IN_DROP:
            if (state_ == STATE_RUNNING) { core_hard_drop(); changed = 1; }
            break;
        case K_IN_HOLD: changed = core_hold(); break;
        default: break;
    }
    if (!changed) return;
    redraw_board();
    draw_current();
    draw_ghost();
    draw_previews();
    update_labels();
    sync_timer_period();
    maybe_full_refresh();
    if (state_ == STATE_OVER) {
        redraw_board();
        draw_current();
        draw_ghost();
        update_labels();
        show_game_over();
    }
}

static void input_async_cb(void* arg) { apply_input((int)(intptr_t)arg); }

// esp_timer 上下文回调：只投递（线程安全垫片），不碰 LVGL
static int on_vk_key(const char* key) {
    if (key == NULL || !s_alive) return 0;
    int input = -1;
    if (strcmp(key, "vk_home") == 0) input = K_IN_ROTATE;
    else if (strcmp(key, "vk_prev") == 0) input = K_IN_LEFT;
    else if (strcmp(key, "vk_next") == 0) input = K_IN_RIGHT;
    if (input < 0) return 0;
    return dynapp_lv_async(input_async_cb, (void*)(intptr_t)input);
}

// ---- 触控（事件回调，已在 LVGL 线程）----
static void go_home_async(void* arg) {
    (void)arg;
    dynapp_screen_go_home();
}

static void on_btn_clicked(lv_event_t* e) {
    const intptr_t kind = (intptr_t)lv_event_get_user_data(e);
    if (!s_alive) return;
    switch (kind) {
        case 0:  // 暂停/继续
            if (state_ == STATE_OVER) return;
            core_toggle_pause();
            if (s_timer != NULL) {
                if (state_ == STATE_PAUSED) lv_timer_pause(s_timer);
                else lv_timer_resume(s_timer);
            }
            sync_pause_btn();
            break;
        case 1:  // 重开
            do_restart();
            break;
        case 2:  // 退出：事件回调里删屏不安全，延后一拍
            save_best_if_needed();
            lv_async_call(go_home_async, NULL);
            break;
        case 3:  // 速降
            apply_input(K_IN_DROP);
            break;
        default: break;
    }
}

static void on_hold_box_clicked(lv_event_t* e) {
    (void)e;
    apply_input(K_IN_HOLD);
}

static void on_game_tick(lv_timer_t* t) {
    (void)t;
    if (!s_alive) return;
    if (state_ != STATE_RUNNING) return;
    core_tick();
    redraw_board();
    draw_current();
    draw_ghost();
    draw_previews();
    update_labels();
    sync_timer_period();
    maybe_full_refresh();
    if (state_ == STATE_OVER) {
        redraw_board();
        draw_current();
        draw_ghost();
        update_labels();
        show_game_over();
    }
}

static void on_screen_unloaded(lv_event_t* e) {
    (void)e;
    if (!s_alive) return;
    s_alive = 0;
    save_best_if_needed();
    if (s_timer != NULL) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (state_ == STATE_RUNNING) core_toggle_pause();
    s_mask = NULL;
    s_card = NULL;
    for (int y = 0; y < FH; ++y)
        for (int x = 0; x < FW; ++x) s_cells[y][x] = NULL;
    for (int i = 0; i < 4; ++i) {
        s_cur[i] = NULL;
        s_ghost[i] = NULL;
        s_next_pv[i] = NULL;
        s_hold_pv[i] = NULL;
    }
    s_hold_tip = NULL;
    s_board = NULL;
    s_score_lbl = s_lines_lbl = s_level_lbl = s_best_lbl = NULL;
    s_pause_lbl = NULL;
    s_score_txt[0] = s_lines_txt[0] = s_level_txt[0] = '\0';
    s_next_fp = -1;
    s_hold_fp = -1;
}

// ---- UI 构建 ----
static lv_obj_t* make_btn(lv_obj_t* parent, const char* text, lv_coord_t w, lv_coord_t h,
                          lv_event_cb_t cb, void* user_data, int invert) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_bg_color(btn, invert ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl, w - 12);
    lv_obj_set_style_text_font(lbl, ui_font(), 0);
    lv_obj_set_style_text_color(lbl, invert ? lv_color_white() : lv_color_black(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return lbl;
}

static lv_obj_t* make_block(lv_obj_t* parent) {
    lv_obj_t* blk = lv_obj_create(parent);
    lv_obj_remove_style_all(blk);
    lv_obj_set_size(blk, CELL_PX - 2, CELL_PX - 2);
    lv_obj_set_style_bg_color(blk, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(blk, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(blk, 0, 0);
    disable_scroll(blk);
    lv_obj_clear_flag(blk, LV_OBJ_FLAG_CLICKABLE);
    return blk;
}

static lv_obj_t* make_ghost(lv_obj_t* parent) {
    lv_obj_t* blk = lv_obj_create(parent);
    lv_obj_remove_style_all(blk);
    lv_obj_set_size(blk, CELL_PX - 2, CELL_PX - 2);
    lv_obj_set_style_bg_opa(blk, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(blk, lv_color_black(), 0);
    lv_obj_set_style_border_width(blk, 2, 0);
    lv_obj_set_style_radius(blk, 0, 0);
    disable_scroll(blk);
    lv_obj_clear_flag(blk, LV_OBJ_FLAG_CLICKABLE);
    return blk;
}

static lv_obj_t* make_text(lv_obj_t* parent, const char* text, int font_size, lv_coord_t x, lv_coord_t y) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_pos(lbl, x, y);
    const lv_font_t* f = fontpack_lv_font_get(font_size, 2);
    lv_obj_set_style_text_font(lbl, f != NULL ? f : ui_font(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return lbl;
}

static lv_obj_t* make_plain_box(lv_obj_t* parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h,
                                lv_coord_t border) {
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_bg_color(box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(box, lv_color_black(), 0);
    lv_obj_set_style_border_width(box, border, 0);
    lv_obj_set_style_radius(box, 0, 0);
    disable_scroll(box);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
    return box;
}

static void* fangfang_create(void) {
    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    dynapp_screen_status_bar(s_scr);
    lv_obj_add_event_cb(s_scr, on_screen_unloaded, LV_EVENT_SCREEN_UNLOAD_START, NULL);
    lv_obj_add_event_cb(s_scr, on_screen_unloaded, LV_EVENT_SCREEN_UNLOADED, NULL);
    lv_obj_add_event_cb(s_scr, on_screen_unloaded, LV_EVENT_DELETE, NULL);

    if (state_ == STATE_OVER) core_reset();

    // 棋盘框 + 200 占格（差量重绘）
    s_board = make_plain_box(s_scr, BOARD_X, BOARD_Y, FW * CELL_PX + 6, FH * CELL_PX + 6, 3);
    memset(s_render, 0xFF, sizeof(s_render));
    for (int y = 0; y < FH; ++y) {
        for (int x = 0; x < FW; ++x) {
            lv_obj_t* c = lv_obj_create(s_board);
            lv_obj_remove_style_all(c);
            lv_obj_set_size(c, CELL_PX - 2, CELL_PX - 2);
            lv_obj_set_pos(c, (lv_coord_t)(4 + x * CELL_PX), (lv_coord_t)(4 + y * CELL_PX));
            lv_obj_set_style_radius(c, 0, 0);
            disable_scroll(c);
            lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
            s_cells[y][x] = c;
        }
    }
    for (int i = 0; i < 4; ++i) {
        s_ghost[i] = make_ghost(s_board);
        lv_obj_add_flag(s_ghost[i], LV_OBJ_FLAG_HIDDEN);
        s_cur[i] = make_block(s_board);
    }

    // 右栏：下一个 / 暂存（点击交换）/ 得分 / 行数 / 等级 / 最高
    make_text(s_scr, "下一个", 25, SIDE_X, 8);
    lv_obj_t* next_box = make_plain_box(s_scr, SIDE_X, 38, 110, 110, 2);
    for (int i = 0; i < 4; ++i) {
        s_next_pv[i] = make_block(next_box);
        lv_obj_set_size(s_next_pv[i], 20, 20);
        lv_obj_add_flag(s_next_pv[i], LV_OBJ_FLAG_HIDDEN);
    }

    make_text(s_scr, "暂存", 25, SIDE_X, 162);
    lv_obj_t* hold_box = make_plain_box(s_scr, SIDE_X, 192, 110, 110, 2);
    lv_obj_add_flag(hold_box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(hold_box, on_hold_box_clicked, LV_EVENT_CLICKED, NULL);

    s_hold_tip = lv_label_create(hold_box);
    lv_label_set_text(s_hold_tip, "点击\n暂存");
    lv_obj_set_style_text_font(s_hold_tip, ui_font(), 0);
    lv_obj_set_style_text_color(s_hold_tip, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_style_text_align(s_hold_tip, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_hold_tip);
    lv_obj_clear_flag(s_hold_tip, LV_OBJ_FLAG_CLICKABLE);
    if (hold_ >= 0) lv_obj_add_flag(s_hold_tip, LV_OBJ_FLAG_HIDDEN);

    for (int i = 0; i < 4; ++i) {
        s_hold_pv[i] = make_block(hold_box);
        lv_obj_set_size(s_hold_pv[i], 20, 20);
        lv_obj_add_flag(s_hold_pv[i], LV_OBJ_FLAG_HIDDEN);
    }

    lv_coord_t row_y = 316;
    s_score_lbl = make_text(s_scr, "0", 30, SIDE_X, row_y + 28);
    make_text(s_scr, "得分", 25, SIDE_X, row_y);
    row_y += 74;
    s_lines_lbl = make_text(s_scr, "0", 30, SIDE_X, row_y + 28);
    make_text(s_scr, "行数", 25, SIDE_X, row_y);
    row_y += 74;
    s_level_lbl = make_text(s_scr, "1", 30, SIDE_X, row_y + 28);
    make_text(s_scr, "等级", 25, SIDE_X, row_y);
    row_y += 74;
    s_best_lbl = make_text(s_scr, "0", 30, SIDE_X, row_y + 28);
    make_text(s_scr, "最高", 25, SIDE_X, row_y);
    sync_best_label();

    // 底部按键行：暂停 / 重开 / 退出 / 速降（黑底反白）
    lv_obj_t* row = lv_obj_create(s_scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 436, BTN_H);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    disable_scroll(row);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    s_pause_lbl = make_btn(row, "暂停", 102, BTN_H, on_btn_clicked, (void*)0, 0);
    make_btn(row, "重开", 102, BTN_H, on_btn_clicked, (void*)1, 0);
    make_btn(row, "退出", 102, BTN_H, on_btn_clicked, (void*)2, 0);
    make_btn(row, "速降", 102, BTN_H, on_btn_clicked, (void*)3, 1);

    redraw_board();
    draw_current();
    draw_ghost();
    draw_previews();
    update_labels();
    sync_pause_btn();

    s_timer = lv_timer_create(on_game_tick, (uint32_t)drop_ms_, NULL);
    if (state_ == STATE_PAUSED) lv_timer_pause(s_timer);

    s_alive = 1;
    // 盖板三键：vk_home=旋转 vk_prev=左移 vk_next=右移；factory 供返回栈重建
    dynapp_vk_attach(s_scr, k_screen_id, fangfang_create, on_vk_key);
    return s_scr;
}

// 固件 dlsym("metalio_app_entry") 的入口：填 desc 后返回（无副作用）。
void metalio_app_entry(metalio_app_desc_t* out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "fangfang";
    out->name = "方方";
    out->icon = "icon.png";
    out->create = fangfang_create;
}
