// main/display/screen/game_2048_screen/game_2048_screen.cc
#include "game_2048_screen/game_2048_screen.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include <esp_log.h>

#include "core/async_settings_store.h"
#include "display/lv_adapter_display.h"
#include "display/screen/epd_widget_kit.h"
#include "fontpack_lvgl.h"
#include "game_2048_screen/game_2048_core.h"
#include "haptic_feedback.h"
#include "screen_common.h"
#include "vk_key_handler.h"

namespace {

constexpr const char* TAG = "Game2048";
constexpr const char* kScreenId = "game2048";

constexpr int kGrid = game_2048::kGridSize;
constexpr lv_coord_t kBoardSize = 440;
constexpr lv_coord_t kCellSize = 96;
constexpr lv_coord_t kCellGap = 10;
constexpr lv_coord_t kCellPad = 13;
constexpr int kFullRefreshEvery = 15;  // 每移动 15 次全局刷新一次，消除墨水屏残影

game_2048::Game2048Core s_core;
bool s_alive = false;
int s_move_count = 0;
int s_last_full_refresh = 0;

EpdWidgetKit::EpdModalOverlay::Handle s_overlay;
bool s_overlay_open = false;

lv_obj_t* s_scr = nullptr;
lv_obj_t* s_board = nullptr;
lv_obj_t* s_cells[kGrid][kGrid];
lv_obj_t* s_cell_lbls[kGrid][kGrid];
int s_rendered[kGrid][kGrid];

lv_obj_t* s_score_lbl = nullptr;
lv_obj_t* s_best_lbl = nullptr;
char s_score_txt[16] = "";
char s_best_txt[16] = "";

lv_point_t s_touch_start{0, 0};
bool s_touch_active = false;

const lv_font_t* UiFont() { return fontpack_lv_font_ui(); }

void DisableScroll(lv_obj_t* obj) {
    if (obj) lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

// 文本更新（带值比对守护，避免多余局部刷新）
void SetTextGuarded(lv_obj_t* lbl, char* last, size_t cap, int v) {
    if (lbl == nullptr) return;
    char buf[24];
    snprintf(buf, sizeof(buf), "%d", v);
    if (strncmp(buf, last, cap) == 0) return;
    strncpy(last, buf, cap - 1);
    last[cap - 1] = '\0';
    lv_label_set_text(lbl, last);
}

// 样式与层级匹配（为墨水屏专属定制视觉）
void ApplyTileStyle(lv_obj_t* cell, lv_obj_t* lbl, int val) {
    if (cell == nullptr || lbl == nullptr) return;

    if (val == 0) {
        lv_obj_set_style_bg_color(cell, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(cell, lv_palette_lighten(LV_PALETTE_GREY, 2), 0);
        lv_obj_set_style_border_width(cell, 1, 0);
        lv_label_set_text(lbl, "");
        return;
    }

    char buf[16];
    snprintf(buf, sizeof(buf), "%d", val);
    lv_label_set_text(lbl, buf);

    // 针对位数智能选择字体大小
    int font_sz = 34;
    if (val >= 1024) font_sz = 24;
    else if (val >= 128) font_sz = 28;
    else if (val >= 16) font_sz = 32;

    const lv_font_t* f = fontpack_lv_font_get(font_sz, 2);
    lv_obj_set_style_text_font(lbl, f != nullptr ? f : UiFont(), 0);

    // 依数值深浅与反白设计
    if (val == 2) {
        lv_obj_set_style_bg_color(cell, lv_color_white(), 0);
        lv_obj_set_style_border_color(cell, lv_color_black(), 0);
        lv_obj_set_style_border_width(cell, 2, 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    } else if (val == 4) {
        lv_obj_set_style_bg_color(cell, lv_color_white(), 0);
        lv_obj_set_style_border_color(cell, lv_color_black(), 0);
        lv_obj_set_style_border_width(cell, 3, 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    } else if (val == 8) {
        lv_obj_set_style_bg_color(cell, lv_palette_lighten(LV_PALETTE_GREY, 4), 0);
        lv_obj_set_style_border_color(cell, lv_color_black(), 0);
        lv_obj_set_style_border_width(cell, 3, 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    } else if (val == 16) {
        lv_obj_set_style_bg_color(cell, lv_palette_lighten(LV_PALETTE_GREY, 3), 0);
        lv_obj_set_style_border_color(cell, lv_color_black(), 0);
        lv_obj_set_style_border_width(cell, 3, 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    } else if (val == 32) {
        lv_obj_set_style_bg_color(cell, lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
        lv_obj_set_style_border_color(cell, lv_color_black(), 0);
        lv_obj_set_style_border_width(cell, 3, 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    } else if (val == 64) {
        lv_obj_set_style_bg_color(cell, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
        lv_obj_set_style_border_color(cell, lv_color_black(), 0);
        lv_obj_set_style_border_width(cell, 3, 0);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    } else {
        // 128 及以上：纯黑底白字反白王者质感
        lv_obj_set_style_bg_color(cell, lv_color_black(), 0);
        lv_obj_set_style_border_color(cell, lv_color_black(), 0);
        lv_obj_set_style_border_width(cell, val >= 1024 ? 4 : 2, 0);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    }
}

void SaveBestIfNeeded() {
    const int32_t best = AsyncSettingsStore::GetInstance().GetInt("game2048", "hiscore", 0);
    if (s_core.score() > best) {
        AsyncSettingsStore::GetInstance().SetInt("game2048", "hiscore", s_core.score());
    }
}

void SyncBestLabel() {
    if (s_best_lbl == nullptr) return;
    const int best = static_cast<int>(AsyncSettingsStore::GetInstance().GetInt("game2048", "hiscore", 0));
    s_core.set_best_score(best);
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", best);
    lv_label_set_text(s_best_lbl, buf);
}

void RedrawBoard() {
    if (!s_alive) return;
    for (int r = 0; r < kGrid; ++r) {
        for (int c = 0; c < kGrid; ++c) {
            const int v = s_core.cell(r, c);
            if (s_rendered[r][c] == v) continue;
            s_rendered[r][c] = v;
            ApplyTileStyle(s_cells[r][c], s_cell_lbls[r][c], v);
        }
    }
    SetTextGuarded(s_score_lbl, s_score_txt, sizeof(s_score_txt), s_core.score());
    if (s_core.score() > s_core.best_score()) {
        SyncBestLabel();
    }
}

void MaybeFullRefresh() {
    if (s_move_count - s_last_full_refresh < kFullRefreshEvery) return;
    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->RequestNextFullRefresh();
    }
    s_last_full_refresh = s_move_count;
}

void DoRestart();

void OnOverlayClicked(lv_event_t*) {
    if (!s_alive) return;
    DoRestart();
}

void ShowGameOverModal() {
    if (!s_alive || s_overlay_open) return;
    SaveBestIfNeeded();
    char msg[128];
    snprintf(msg, sizeof(msg), "游戏结束\n得分 %d · 最高 %d\n点按重新开始",
             s_core.score(), static_cast<int>(AsyncSettingsStore::GetInstance().GetInt("game2048", "hiscore", 0)));
    s_overlay = EpdWidgetKit::EpdModalOverlay::Show(s_scr, msg, 200);
    s_overlay_open = true;
    if (s_overlay.mask != nullptr) {
        lv_obj_add_event_cb(s_overlay.mask, OnOverlayClicked, LV_EVENT_CLICKED, nullptr);
    }
    if (s_overlay.card != nullptr) {
        lv_obj_add_event_cb(s_overlay.card, OnOverlayClicked, LV_EVENT_CLICKED, nullptr);
    }
}

void ShowVictoryModal() {
    if (!s_alive || s_overlay_open) return;
    SaveBestIfNeeded();
    char msg[128];
    snprintf(msg, sizeof(msg), "恭喜达成 2048！\n当前得分 %d\n点按继续挑战更高分", s_core.score());
    s_overlay = EpdWidgetKit::EpdModalOverlay::Show(s_scr, msg, 200);
    s_overlay_open = true;
    if (s_overlay.mask != nullptr) {
        lv_obj_add_event_cb(s_overlay.mask, [](lv_event_t*) {
            if (s_overlay_open) {
                EpdWidgetKit::EpdModalOverlay::Close(s_overlay);
                s_overlay_open = false;
            }
        }, LV_EVENT_CLICKED, nullptr);
    }
}

void HandleMove(game_2048::Direction dir) {
    if (!s_alive) return;
    if (s_overlay_open) return;

    if (s_core.Move(dir)) {
        ++s_move_count;
        RedrawBoard();
        MaybeFullRefresh();

        if (s_core.has_won() && !s_overlay_open) {
            static bool s_celebrated = false;
            if (!s_celebrated) {
                s_celebrated = true;
                ShowVictoryModal();
                return;
            }
        }

        if (s_core.is_over()) {
            ShowGameOverModal();
        }
    }
}

void DoRestart() {
    if (s_overlay_open) {
        EpdWidgetKit::EpdModalOverlay::Close(s_overlay);
        s_overlay_open = false;
    }
    s_core.Reset();
    s_move_count = 0;
    s_last_full_refresh = 0;
    std::memset(s_rendered, 0xFF, sizeof(s_rendered));
    RedrawBoard();
    SyncBestLabel();
}

void OnBtnClicked(lv_event_t* e) {
    const uintptr_t kind = reinterpret_cast<uintptr_t>(lv_event_get_user_data(e));
    if (!s_alive) return;
    switch (kind) {
        case 0:  // 撤销
            if (s_core.Undo()) {
                RedrawBoard();
            }
            break;
        case 1:  // 重开
            DoRestart();
            break;
        case 2:  // 退出
            SaveBestIfNeeded();
            lv_async_call([](void*) { ScreenRequestHome(); }, nullptr);
            break;
        default: break;
    }
}

// 触摸滑动手势检测
void OnBoardTouch(lv_event_t* e) {
    const lv_event_code_t code = lv_event_get_code(e);
    lv_indev_t* indev = lv_indev_active();
    if (indev == nullptr) return;

    if (code == LV_EVENT_PRESSED) {
        lv_indev_get_point(indev, &s_touch_start);
        s_touch_active = true;
    } else if (code == LV_EVENT_RELEASED && s_touch_active) {
        s_touch_active = false;
        lv_point_t p;
        lv_indev_get_point(indev, &p);
        const lv_coord_t dx = p.x - s_touch_start.x;
        const lv_coord_t dy = p.y - s_touch_start.y;
        constexpr lv_coord_t kMinSwipe = 35;

        if (std::abs(dx) >= kMinSwipe || std::abs(dy) >= kMinSwipe) {
            if (std::abs(dx) > std::abs(dy)) {
                HandleMove(dx > 0 ? game_2048::Direction::kRight : game_2048::Direction::kLeft);
            } else {
                HandleMove(dy > 0 ? game_2048::Direction::kDown : game_2048::Direction::kUp);
            }
        }
    }
}

// 屏幕原生手势备援
void OnScreenGesture(lv_event_t* e) {
    lv_indev_t* indev = lv_indev_active();
    if (indev == nullptr) return;
    lv_indev_reset(indev, nullptr);
    lv_indev_wait_release(indev);

    switch (lv_indev_get_gesture_dir(indev)) {
        case LV_DIR_LEFT:   HandleMove(game_2048::Direction::kLeft); break;
        case LV_DIR_RIGHT:  HandleMove(game_2048::Direction::kRight); break;
        case LV_DIR_TOP:    HandleMove(game_2048::Direction::kUp); break;
        case LV_DIR_BOTTOM: HandleMove(game_2048::Direction::kDown); break;
        default: break;
    }
}

// 盖板虚拟按键：vk_prev=左移, vk_next=右移, vk_home=撤销
bool OnVkKey(const char* key) {
    if (key == nullptr || !s_alive) return false;
    if (strcmp(key, "vk_prev") == 0) {
        ScreenLvAsync([](void*) { HandleMove(game_2048::Direction::kLeft); }, nullptr);
        return true;
    }
    if (strcmp(key, "vk_next") == 0) {
        ScreenLvAsync([](void*) { HandleMove(game_2048::Direction::kRight); }, nullptr);
        return true;
    }
    if (strcmp(key, "vk_home") == 0) {
        ScreenLvAsync([](void*) {
            if (s_core.Undo()) RedrawBoard();
        }, nullptr);
        return true;
    }
    return false;
}

void OnScreenUnloaded(lv_event_t*) {
    if (!s_alive) return;
    s_alive = false;
    SaveBestIfNeeded();
    s_overlay.mask = nullptr;
    s_overlay.card = nullptr;
    s_overlay.label = nullptr;
    s_overlay_open = false;
    s_board = nullptr;
    for (int r = 0; r < kGrid; ++r) {
        for (int c = 0; c < kGrid; ++c) {
            s_cells[r][c] = nullptr;
            s_cell_lbls[r][c] = nullptr;
        }
    }
    s_score_lbl = s_best_lbl = nullptr;
    s_score_txt[0] = s_best_txt[0] = '\0';
}

// 按钮创建辅助函数
lv_obj_t* MakeBtn(lv_obj_t* parent, const char* text, lv_coord_t w, lv_coord_t h,
                  lv_event_cb_t cb, void* user_data, bool invert = false) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_bg_color(btn, invert ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, UiFont(), 0);
    lv_obj_set_style_text_color(lbl, invert ? lv_color_white() : lv_color_black(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return lbl;
}

// 得分展示卡片辅助函数
lv_obj_t* MakeScoreBox(lv_obj_t* parent, const char* title, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h) {
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_radius(box, 6, 0);
    lv_obj_set_style_border_color(box, lv_color_black(), 0);
    lv_obj_set_style_border_width(box, 2, 0);
    lv_obj_set_style_bg_color(box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    DisableScroll(box);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* t_lbl = lv_label_create(box);
    lv_label_set_text(t_lbl, title);
    const lv_font_t* f18 = fontpack_lv_font_get(18, 2);
    lv_obj_set_style_text_font(t_lbl, f18 != nullptr ? f18 : UiFont(), 0);
    lv_obj_set_style_text_color(t_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_align(t_lbl, LV_ALIGN_TOP_MID, 0, 4);

    lv_obj_t* v_lbl = lv_label_create(box);
    lv_label_set_text(v_lbl, "0");
    const lv_font_t* f24 = fontpack_lv_font_get(24, 2);
    lv_obj_set_style_text_font(v_lbl, f24 != nullptr ? f24 : UiFont(), 0);
    lv_obj_set_style_text_color(v_lbl, lv_color_black(), 0);
    lv_obj_align(v_lbl, LV_ALIGN_BOTTOM_MID, 0, -4);

    return v_lbl;
}

}  // namespace

lv_obj_t* Game2048Screen::Create() {
    auto* scaffold = EpdWidgetKit::EpdPageScaffold::Create({
        .title = "2048",
        .show_header = false,
        .show_status_bar = true,
        .show_back_btn = false,
        .on_back = nullptr,
        .scrollable = false,
        .scroll_cfg = {},
        .pad_h = 0,
        .pad_v = 0,
        .flex_column = false,
        .row_gap = 0,
    });
    s_scr = scaffold->GetScreen();
    lv_obj_t* body = scaffold->GetBody();

    ScreenSetIsHome(false);
    lv_obj_add_event_cb(s_scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);
    lv_obj_add_event_cb(s_scr, OnScreenUnloaded, LV_EVENT_DELETE, nullptr);
    lv_obj_add_event_cb(s_scr, OnScreenGesture, LV_EVENT_GESTURE, nullptr);

    s_alive = true;
    s_core.Reset();
    s_move_count = 0;
    s_last_full_refresh = 0;

    // 头部区域：左侧大标题与副标，右侧得分与最高卡片
    lv_obj_t* title_lbl = lv_label_create(body);
    lv_label_set_text(title_lbl, "2048");
    const lv_font_t* f34 = fontpack_lv_font_get(34, 2);
    lv_obj_set_style_text_font(title_lbl, f34 != nullptr ? f34 : UiFont(), 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_black(), 0);
    lv_obj_set_pos(title_lbl, 24, 12);

    lv_obj_t* sub_lbl = lv_label_create(body);
    lv_label_set_text(sub_lbl, "滑屏合并，达成2048");
    const lv_font_t* f18 = fontpack_lv_font_get(18, 2);
    lv_obj_set_style_text_font(sub_lbl, f18 != nullptr ? f18 : UiFont(), 0);
    lv_obj_set_style_text_color(sub_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_pos(sub_lbl, 24, 48);

    s_score_lbl = MakeScoreBox(body, "得分", 260, 10, 94, 56);
    s_best_lbl = MakeScoreBox(body, "最高", 362, 10, 94, 56);

    // 4×4 棋盘大框（带点击与滑动手势绑定）
    s_board = lv_obj_create(body);
    lv_obj_remove_style_all(s_board);
    lv_obj_set_pos(s_board, 20, 78);
    lv_obj_set_size(s_board, kBoardSize, kBoardSize);
    lv_obj_set_style_bg_color(s_board, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_board, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_board, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_board, 3, 0);
    lv_obj_set_style_radius(s_board, 8, 0);
    DisableScroll(s_board);
    lv_obj_add_flag(s_board, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_board, OnBoardTouch, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(s_board, OnBoardTouch, LV_EVENT_RELEASED, nullptr);

    // 16 占格创建
    std::memset(s_rendered, 0xFF, sizeof(s_rendered));
    for (int r = 0; r < kGrid; ++r) {
        for (int c = 0; c < kGrid; ++c) {
            lv_obj_t* cell = lv_obj_create(s_board);
            lv_obj_remove_style_all(cell);
            lv_obj_set_size(cell, kCellSize, kCellSize);
            const lv_coord_t px = static_cast<lv_coord_t>(kCellPad + c * (kCellSize + kCellGap));
            const lv_coord_t py = static_cast<lv_coord_t>(kCellPad + r * (kCellSize + kCellGap));
            lv_obj_set_pos(cell, px, py);
            lv_obj_set_style_radius(cell, 6, 0);
            DisableScroll(cell);
            lv_obj_clear_flag(cell, LV_OBJ_FLAG_CLICKABLE);

            lv_obj_t* lbl = lv_label_create(cell);
            lv_label_set_text(lbl, "");
            lv_obj_center(lbl);
            lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

            s_cells[r][c] = cell;
            s_cell_lbls[r][c] = lbl;
        }
    }

    // 中下方：精巧的微型方向辅助键（方便单手点按，亦提示支持手势滑动）
    lv_coord_t dpad_y = 528;
    lv_obj_t* up_btn = MakeBtn(body, "▲", 56, 40, [](lv_event_t*) { HandleMove(game_2048::Direction::kUp); }, nullptr);
    lv_obj_set_pos(up_btn, 212, dpad_y);

    lv_obj_t* left_btn = MakeBtn(body, "◀", 56, 40, [](lv_event_t*) { HandleMove(game_2048::Direction::kLeft); }, nullptr);
    lv_obj_set_pos(left_btn, 148, dpad_y + 44);

    lv_obj_t* hint = lv_label_create(body);
    lv_label_set_text(hint, "或滑动");
    lv_obj_set_style_text_font(hint, f18 != nullptr ? f18 : UiFont(), 0);
    lv_obj_set_style_text_color(hint, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_pos(hint, 216, dpad_y + 54);

    lv_obj_t* right_btn = MakeBtn(body, "▶", 56, 40, [](lv_event_t*) { HandleMove(game_2048::Direction::kRight); }, nullptr);
    lv_obj_set_pos(right_btn, 276, dpad_y + 44);

    lv_obj_t* down_btn = MakeBtn(body, "▼", 56, 40, [](lv_event_t*) { HandleMove(game_2048::Direction::kDown); }, nullptr);
    lv_obj_set_pos(down_btn, 212, dpad_y + 88);

    // 底部按键行：贴紧屏幕最底边，去除多余空白
    lv_obj_t* row = lv_obj_create(body);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 436, 56);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    DisableScroll(row);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

    MakeBtn(row, "撤销", 136, 56, OnBtnClicked, (void*)0);
    MakeBtn(row, "重开", 136, 56, OnBtnClicked, (void*)1);
    MakeBtn(row, "退出", 136, 56, OnBtnClicked, (void*)2);

    // 虚拟按键监听
    VkKey_AttachScreen(s_scr, kScreenId, VkKeyScreenDesc{
        .factory = Game2048Screen::Create,
        .on_key = OnVkKey,
    });

    // 初始渲染
    SyncBestLabel();
    RedrawBoard();

    return s_scr;
}
