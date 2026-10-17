// main/display/screen/tetris_screen/tetris_screen.cc —— 俄罗斯方块界面（480×800 竖屏 1-bit 墨水屏）。
// 布局:左盘右栏——棋盘 10×20(30px 格) + 右栏(下一个/暂存/得分/行数/等级/最高),
// 底部 暂停/重开/退出 + 速降(黑底反白)。
// 操作方式:
// 1. 盖板虚拟键(触摸条): vk_home=旋转, vk_next=左移, vk_prev=右移;
// 2. 屏幕触控: 点暂存框=暂存; 底部按键=速降/暂停/重开/退出。
// （音量键与 BOOT 键不参与游戏，保持系统原生功能；棋盘不响应点按）
// 刷新纪律:差量重绘——占格只在变化时改样式、文本只在变化时 set,墨水屏走局部刷新;
// 每 10 块落定请求一次全刷清残影。
#include "tetris_screen/tetris_screen.h"

#include <cstdio>
#include <cstring>

#include <esp_log.h>

#include "core/async_settings_store.h"
#include "display/lv_adapter_display.h"
#include "display/screen/epd_widget_kit.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "screen_common.h"
#include "tetris_screen/tetris_core.h"
#include "vk_key_handler.h"

namespace {

constexpr const char* TAG = "TetrisScreen";
constexpr const char* kScreenId = "tetris";

constexpr int kW = tetris::kW;
constexpr int kH = tetris::kH;
constexpr lv_coord_t kCell = 30;     // 棋盘格距
constexpr lv_coord_t kBoardX = 22;   // 棋盘框位置(body 内,坐标原点=body 内容区)
constexpr lv_coord_t kBoardY = 8;
constexpr lv_coord_t kSideX = 338;   // 右栏 X(棋盘 306 + 10 间距)
constexpr lv_coord_t kBtnH = 52;
constexpr int kFullRefreshEvery = 10;  // 每 N 块落定全刷一次

// 输入种类(lv_async 投递 user_data)
enum GameInput { kInLeft = 0, kInRight = 1, kInRotate = 2, kInDrop = 3, kInHold = 4 };

tetris::TetrisCore s_core;
bool s_alive = false;
lv_timer_t* s_timer = nullptr;
int s_last_full_piece = 0;  // 上次全刷时的落块数

EpdWidgetKit::EpdModalOverlay::Handle s_overlay;
bool s_overlay_open = false;

lv_obj_t* s_scr = nullptr;
lv_obj_t* s_board = nullptr;
lv_obj_t* s_cells[kH][kW];      // 200 占格
uint8_t s_render[kH][kW];       // 上次渲染的占格(差量重绘)
lv_obj_t* s_cur[4];             // 当前块
lv_obj_t* s_ghost[4];           // 落点预览
lv_obj_t* s_next_pv[4];         // 下一个块预览格
lv_obj_t* s_hold_pv[4];         // 暂存块预览格
int s_next_fp = -1;             // 预览形状指纹;-1=未画
int s_hold_fp = -1;             // -2=无暂存(隐藏)
lv_obj_t* s_score_lbl = nullptr;
lv_obj_t* s_lines_lbl = nullptr;
lv_obj_t* s_level_lbl = nullptr;
lv_obj_t* s_best_lbl = nullptr;
char s_score_txt[16] = "";
char s_lines_txt[8] = "";
char s_level_txt[8] = "";
lv_obj_t* s_pause_btn_lbl = nullptr;

const lv_font_t* UiFont() { return fontpack_lv_font_ui(); }

void DisableScroll(lv_obj_t* obj) {
    if (obj) lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

// ---- 差量渲染 ----

// 占格样式:实心黑(无边框)/空(透明 + 1px 浅点阵格线)
void SetCellStyle(lv_obj_t* cell, bool filled) {
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

void RedrawBoard() {
    if (!s_alive) return;
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const uint8_t v = s_core.filled(x, y) ? 1 : 0;
            if (s_render[y][x] == v || s_cells[y][x] == nullptr) continue;
            s_render[y][x] = v;
            SetCellStyle(s_cells[y][x], v != 0);
        }
    }
}

// 实心块格(棋盘绝对坐标 → 棋盘容器内像素,28×28 + 2px 白缝);y<0 时隐藏
void PlaceSolid(lv_obj_t* obj, int x, int y) {
    if (obj == nullptr) return;
    if (y < 0) {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(obj, static_cast<lv_coord_t>(4 + x * kCell), static_cast<lv_coord_t>(4 + y * kCell));
}

void HideAll(lv_obj_t** objs, int n) {
    for (int i = 0; i < n; ++i) {
        if (objs[i] != nullptr) lv_obj_add_flag(objs[i], LV_OBJ_FLAG_HIDDEN);
    }
}

void DrawCurrent() {
    if (!s_alive) return;
    if (s_core.state() != tetris::State::Running) {
        HideAll(s_cur, 4);
        return;
    }
    int cells[4][2];
    s_core.current_cells(cells);
    for (int i = 0; i < 4; ++i) PlaceSolid(s_cur[i], cells[i][0], cells[i][1]);
}

void DrawGhost() {
    if (!s_alive) return;
    bool show = s_core.state() == tetris::State::Running;
    int cur[4][2], gh[4][2];
    s_core.current_cells(cur);
    s_core.ghost_cells(gh);
    if (show) {
        for (int i = 0; i < 4; ++i) {
            if (gh[i][1] == cur[i][1]) show = false;  // 与当前块重叠时隐藏
        }
    }
    if (!show) {
        HideAll(s_ghost, 4);
        return;
    }
    for (int i = 0; i < 4; ++i) {
        if (s_ghost[i] == nullptr) continue;
        lv_obj_remove_flag(s_ghost[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(s_ghost[i], static_cast<lv_coord_t>(4 + gh[i][0] * kCell),
                       static_cast<lv_coord_t>(4 + gh[i][1] * kCell));
    }
}

// 预览框(110×110)内居中画形状;type<0 表示无块(全隐藏)
void DrawPreview(lv_obj_t** pv, bool is_hold, int type) {
    for (int i = 0; i < 4; ++i) {
        if (pv[i] == nullptr) continue;
        if (type < 0) {
            lv_obj_add_flag(pv[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        int cells[4][2];
        if (is_hold) s_core.hold_cells(cells);
        else s_core.next_cells(cells);
        // 形状外接框居中
        int minx = 9, maxx = -1, miny = 9, maxy = -1;
        for (int k = 0; k < 4; ++k) {
            if (cells[k][0] < minx) minx = cells[k][0];
            if (cells[k][0] > maxx) maxx = cells[k][0];
            if (cells[k][1] < miny) miny = cells[k][1];
            if (cells[k][1] > maxy) maxy = cells[k][1];
        }
        constexpr lv_coord_t kPv = 22;  // 预览格距
        const lv_coord_t ox = (110 - (maxx - minx + 1) * kPv) / 2;
        const lv_coord_t oy = (110 - (maxy - miny + 1) * kPv) / 2;
        lv_obj_remove_flag(pv[i], LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_pos(pv[i], static_cast<lv_coord_t>(ox + (cells[i][0] - minx) * kPv + 1),
                       static_cast<lv_coord_t>(oy + (cells[i][1] - miny) * kPv + 1));
    }
}

// 形状指纹(四格各 4×4 坐标折叠成一个 16 进制式整数,区分 7 种方块)
int ShapeFingerprint(const int cells[4][2]) {
    int fp = 0;
    for (int k = 0; k < 4; ++k) fp = fp * 16 + (cells[k][0] * 4 + cells[k][1]);
    return fp;
}

// 下一个/暂存预览只在形状变化时重画
void DrawPreviews() {
    if (!s_alive) return;
    int nc[4][2];
    s_core.next_cells(nc);
    const int nfp = ShapeFingerprint(nc);
    if (nfp != s_next_fp) {
        s_next_fp = nfp;
        DrawPreview(s_next_pv, false, 0);
    }
    int hfp;
    if (s_core.has_hold()) {
        int hc[4][2];
        s_core.hold_cells(hc);
        hfp = ShapeFingerprint(hc);
    } else {
        hfp = -2;
    }
    if (hfp != s_hold_fp) {
        s_hold_fp = hfp;
        DrawPreview(s_hold_pv, true, s_core.has_hold() ? 0 : -2);
    }
}

// 文本只在变化时 set,避免无谓的墨水屏刷新
void SetTextGuarded(lv_obj_t* lbl, char* last, size_t cap, int v) {
    if (lbl == nullptr) return;
    char buf[24];
    snprintf(buf, sizeof(buf), "%d", v);
    if (strncmp(buf, last, cap) == 0) return;
    strncpy(last, buf, cap - 1);
    last[cap - 1] = '\0';
    lv_label_set_text(lbl, last);
}

void UpdateLabels() {
    if (!s_alive) return;
    SetTextGuarded(s_score_lbl, s_score_txt, sizeof(s_score_txt), s_core.score());
    SetTextGuarded(s_lines_lbl, s_lines_txt, sizeof(s_lines_txt), s_core.lines());
    SetTextGuarded(s_level_lbl, s_level_txt, sizeof(s_level_txt), s_core.level());
}

void SyncBestLabel() {
    if (s_best_lbl == nullptr) return;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d", static_cast<int>(AsyncSettingsStore::GetInstance().GetInt("tetris", "hiscore", 0)));
    lv_label_set_text(s_best_lbl, buf);
}

void SyncPauseBtn() {
    if (s_pause_btn_lbl == nullptr) return;
    lv_label_set_text(s_pause_btn_lbl,
                      s_core.state() == tetris::State::Paused ? "继续" : "暂停");
}

// 重力周期变化时同步定时器
void SyncTimerPeriod() {
    if (s_timer == nullptr) return;
    const uint32_t p = static_cast<uint32_t>(s_core.drop_interval_ms());
    lv_timer_set_period(s_timer, p);
}

// 每 N 块落定全刷一次,清残影
void MaybeFullRefresh() {
    if (s_core.pieces() - s_last_full_piece < kFullRefreshEvery) return;
    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->RequestNextFullRefresh();
    }
    s_last_full_piece = s_core.pieces();
}

// ---- 最高分(NVS 异步落盘,键 7 字符 ≤15) ----

void SaveBestIfNeeded() {
    const int32_t best = AsyncSettingsStore::GetInstance().GetInt("tetris", "hiscore", 0);
    if (s_core.score() > best) {
        AsyncSettingsStore::GetInstance().SetInt("tetris", "hiscore", s_core.score());
    }
}

// ---- 游戏结束卡片 ----

void DoRestart() {
    if (s_overlay_open) {
        EpdWidgetKit::EpdModalOverlay::Close(s_overlay);
        s_overlay_open = false;
    }
    s_core.Reset();
    s_last_full_piece = 0;
    SyncTimerPeriod();
    if (s_timer != nullptr) lv_timer_resume(s_timer);
    memset(s_render, 0xFF, sizeof(s_render));  // 强制全量重画
    s_next_fp = -1;
    s_hold_fp = -1;
    RedrawBoard();
    DrawCurrent();
    DrawGhost();
    DrawPreviews();
    UpdateLabels();
    SyncBestLabel();
    SyncPauseBtn();
}

void OnOverlayClicked(lv_event_t*) {
    if (!s_alive) return;
    DoRestart();
}

void ShowGameOver() {
    SaveBestIfNeeded();
    static char msg[128];
    snprintf(msg, sizeof(msg), "游戏结束\n得分 %d · 最高 %d\n点按重新开始",
             s_core.score(), static_cast<int>(AsyncSettingsStore::GetInstance().GetInt("tetris", "hiscore", 0)));
    s_overlay = EpdWidgetKit::EpdModalOverlay::Show(s_scr, msg, 200);
    s_overlay_open = true;
    if (s_overlay.mask != nullptr) {
        lv_obj_add_event_cb(s_overlay.mask, OnOverlayClicked, LV_EVENT_CLICKED, nullptr);
    }
    if (s_overlay.card != nullptr) {
        lv_obj_add_event_cb(s_overlay.card, OnOverlayClicked, LV_EVENT_CLICKED, nullptr);
    }
    if (s_timer != nullptr) lv_timer_pause(s_timer);
}

// ---- 输入(全部在 LVGL 线程执行) ----

void ApplyInput(int input) {
    if (!s_alive) return;
    bool changed = false;
    switch (input) {
        case kInLeft:  changed = s_core.Move(-1); break;
        case kInRight: changed = s_core.Move(1); break;
        case kInRotate: changed = s_core.Rotate(); break;
        case kInDrop:
            if (s_core.state() == tetris::State::Running) {
                s_core.HardDrop();
                changed = true;
            }
            break;
        case kInHold: changed = s_core.Hold(); break;
        default: break;
    }
    if (!changed) return;
    if (s_core.state() == tetris::State::Over) {
        RedrawBoard();
        DrawCurrent();
        DrawGhost();
        UpdateLabels();
        ShowGameOver();
        return;
    }
    RedrawBoard();
    DrawCurrent();
    DrawGhost();
    DrawPreviews();
    UpdateLabels();
    SyncTimerPeriod();
    MaybeFullRefresh();
}

void GameInputAsync(void* arg) { ApplyInput(static_cast<int>(reinterpret_cast<uintptr_t>(arg))); }

// esp_timer 任务上下文的按键回调:只投递,不碰 LVGL/NVS
bool PostInput(int input) {
    if (!s_alive) return false;
    ScreenLvAsync(GameInputAsync, reinterpret_cast<void*>(static_cast<uintptr_t>(input)));
    return true;
}

// 盖板三键(y=900 触摸条,物理从左到右): vk_home=旋转, vk_next=左移, vk_prev=右移
bool OnVkKey(const char* key) {
    if (key == nullptr) return false;
    if (strcmp(key, "vk_home") == 0) return PostInput(kInRotate);
    if (strcmp(key, "vk_next") == 0) return PostInput(kInLeft);
    if (strcmp(key, "vk_prev") == 0) return PostInput(kInRight);
    return false;
}

// ---- 触控(事件回调,已在 LVGL 线程) ----

void OnBtnClicked(lv_event_t* e) {
    const uintptr_t kind = reinterpret_cast<uintptr_t>(lv_event_get_user_data(e));
    if (!s_alive) return;
    switch (kind) {
        case 0:  // 暂停/继续
            if (s_core.state() == tetris::State::Over) return;
            s_core.TogglePause();
            if (s_timer != nullptr) {
                if (s_core.state() == tetris::State::Paused) lv_timer_pause(s_timer);
                else lv_timer_resume(s_timer);
            }
            SyncPauseBtn();
            break;
        case 1:  // 重开
            DoRestart();
            break;
        case 2:  // 退出
            SaveBestIfNeeded();
            // 事件回调里删屏不安全,延后一拍
            lv_async_call([](void*) { ScreenRequestHome(); }, nullptr);
            break;
        case 3:  // 速降
            ApplyInput(kInDrop);
            break;
        default: break;
    }
}

void OnHoldBoxClicked(lv_event_t*) { ApplyInput(kInHold); }

void OnGameTick(lv_timer_t*) {
    if (!s_alive) return;
    if (s_core.state() != tetris::State::Running) return;
    s_core.Tick();
    if (s_core.state() == tetris::State::Over) {
        RedrawBoard();
        DrawCurrent();
        DrawGhost();
        UpdateLabels();
        ShowGameOver();
        return;
    }
    RedrawBoard();
    DrawCurrent();
    DrawGhost();
    DrawPreviews();
    UpdateLabels();
    SyncTimerPeriod();
    MaybeFullRefresh();
}

void OnScreenUnloaded(lv_event_t*) {
    if (!s_alive) return;
    s_alive = false;
    SaveBestIfNeeded();
    if (s_timer != nullptr) {
        lv_timer_delete(s_timer);
        s_timer = nullptr;
    }
    // 遮罩随屏销毁,勿再 Close;游戏保持暂停,回来自续
    if (s_core.state() == tetris::State::Running) s_core.TogglePause();
    s_overlay.mask = nullptr;
    s_overlay.card = nullptr;
    s_overlay.label = nullptr;
    s_overlay_open = false;
    for (int y = 0; y < kH; ++y)
        for (int x = 0; x < kW; ++x) s_cells[y][x] = nullptr;
    for (auto*& p : s_cur) p = nullptr;
    for (auto*& p : s_ghost) p = nullptr;
    for (auto*& p : s_next_pv) p = nullptr;
    for (auto*& p : s_hold_pv) p = nullptr;
    s_board = nullptr;
    s_score_lbl = s_lines_lbl = s_level_lbl = s_best_lbl = nullptr;
    s_pause_btn_lbl = nullptr;
    s_score_txt[0] = s_lines_txt[0] = s_level_txt[0] = '\0';
    s_next_fp = -1;
    s_hold_fp = -1;
}

// ---- UI 构建 ----

// 白底黑框方角按钮(带触觉反馈);返回内部 label
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
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl, w - 12);
    lv_obj_set_style_text_font(lbl, UiFont(), 0);
    lv_obj_set_style_text_color(lbl, invert ? lv_color_white() : lv_color_black(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return lbl;
}

// 实心块对象(28×28)
lv_obj_t* MakeBlock(lv_obj_t* parent) {
    lv_obj_t* blk = lv_obj_create(parent);
    lv_obj_remove_style_all(blk);
    lv_obj_set_size(blk, kCell - 2, kCell - 2);
    lv_obj_set_style_bg_color(blk, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(blk, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(blk, 0, 0);
    DisableScroll(blk);
    lv_obj_clear_flag(blk, LV_OBJ_FLAG_CLICKABLE);
    return blk;
}

// 空心预览格(落点预览用)
lv_obj_t* MakeGhost(lv_obj_t* parent) {
    lv_obj_t* blk = lv_obj_create(parent);
    lv_obj_remove_style_all(blk);
    lv_obj_set_size(blk, kCell - 2, kCell - 2);
    lv_obj_set_style_bg_opa(blk, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(blk, lv_color_black(), 0);
    lv_obj_set_style_border_width(blk, 2, 0);
    lv_obj_set_style_radius(blk, 0, 0);
    DisableScroll(blk);
    lv_obj_clear_flag(blk, LV_OBJ_FLAG_CLICKABLE);
    return blk;
}

lv_obj_t* MakeText(lv_obj_t* parent, const char* text, int font_size, lv_coord_t x, lv_coord_t y) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_pos(lbl, x, y);
    const lv_font_t* f = fontpack_lv_font_get(font_size, 2);
    lv_obj_set_style_text_font(lbl, f != nullptr ? f : UiFont(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return lbl;
}

}  // namespace

lv_obj_t* TetrisScreen::Create() {
    auto* scaffold = EpdWidgetKit::EpdPageScaffold::Create({
        .title = "俄罗斯方块",
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
    lv_obj_add_event_cb(s_scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOAD_START, nullptr);
    lv_obj_add_event_cb(s_scr, OnScreenUnloaded, LV_EVENT_DELETE, nullptr);

    // 上局结束:新局;暂停态回来自续
    if (s_core.state() == tetris::State::Over) {
        s_core.Reset();
    }

    // 棋盘框
    s_board = lv_obj_create(body);
    lv_obj_remove_style_all(s_board);
    lv_obj_set_pos(s_board, kBoardX, kBoardY);
    lv_obj_set_size(s_board, kW * kCell + 6, kH * kCell + 6);
    lv_obj_set_style_bg_color(s_board, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_board, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_board, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_board, 3, 0);
    lv_obj_set_style_radius(s_board, 0, 0);
    DisableScroll(s_board);
    lv_obj_clear_flag(s_board, LV_OBJ_FLAG_CLICKABLE);

    // 200 占格(差量重绘)
    memset(s_render, 0xFF, sizeof(s_render));
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            lv_obj_t* c = lv_obj_create(s_board);
            lv_obj_remove_style_all(c);
            lv_obj_set_size(c, kCell - 2, kCell - 2);
            lv_obj_set_pos(c, static_cast<lv_coord_t>(4 + x * kCell), static_cast<lv_coord_t>(4 + y * kCell));
            lv_obj_set_style_radius(c, 0, 0);
            DisableScroll(c);
            lv_obj_clear_flag(c, LV_OBJ_FLAG_CLICKABLE);
            s_cells[y][x] = c;
        }
    }

    // 落点预览 + 当前块(浮在占格上)
    for (int i = 0; i < 4; ++i) {
        s_ghost[i] = MakeGhost(s_board);
        lv_obj_add_flag(s_ghost[i], LV_OBJ_FLAG_HIDDEN);
        s_cur[i] = MakeBlock(s_board);
    }

    // 右栏
    MakeText(body, "下一个", 25, kSideX, 8);
    lv_obj_t* next_box = lv_obj_create(body);
    lv_obj_remove_style_all(next_box);
    lv_obj_set_pos(next_box, kSideX, 38);
    lv_obj_set_size(next_box, 110, 110);
    lv_obj_set_style_bg_color(next_box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(next_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(next_box, lv_color_black(), 0);
    lv_obj_set_style_border_width(next_box, 2, 0);
    lv_obj_set_style_radius(next_box, 0, 0);
    DisableScroll(next_box);
    lv_obj_clear_flag(next_box, LV_OBJ_FLAG_CLICKABLE);
    for (int i = 0; i < 4; ++i) {
        s_next_pv[i] = MakeBlock(next_box);
        lv_obj_set_size(s_next_pv[i], 20, 20);
        lv_obj_add_flag(s_next_pv[i], LV_OBJ_FLAG_HIDDEN);
    }

    MakeText(body, "暂存", 25, kSideX, 162);
    // 暂存框 = 暂存交换热区
    lv_obj_t* hold_box = lv_obj_create(body);
    lv_obj_remove_style_all(hold_box);
    lv_obj_set_pos(hold_box, kSideX, 192);
    lv_obj_set_size(hold_box, 110, 110);
    lv_obj_set_style_bg_color(hold_box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(hold_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(hold_box, lv_color_black(), 0);
    lv_obj_set_style_border_width(hold_box, 2, 0);
    lv_obj_set_style_radius(hold_box, 0, 0);
    DisableScroll(hold_box);
    lv_obj_add_flag(hold_box, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(hold_box);
    lv_obj_add_event_cb(hold_box, OnHoldBoxClicked, LV_EVENT_CLICKED, nullptr);
    for (int i = 0; i < 4; ++i) {
        s_hold_pv[i] = MakeBlock(hold_box);
        lv_obj_set_size(s_hold_pv[i], 20, 20);
        lv_obj_add_flag(s_hold_pv[i], LV_OBJ_FLAG_HIDDEN);
    }

    // 得分/行数/等级/最高
    lv_coord_t row_y = 316;
    s_score_lbl = MakeText(body, "0", 30, kSideX, row_y + 28);
    MakeText(body, "得分", 25, kSideX, row_y);
    row_y += 74;
    s_lines_lbl = MakeText(body, "0", 30, kSideX, row_y + 28);
    MakeText(body, "行数", 25, kSideX, row_y);
    row_y += 74;
    s_level_lbl = MakeText(body, "1", 30, kSideX, row_y + 28);
    MakeText(body, "等级", 25, kSideX, row_y);
    row_y += 74;
    s_best_lbl = MakeText(body, "0", 30, kSideX, row_y + 28);
    MakeText(body, "最高", 25, kSideX, row_y);
    SyncBestLabel();

    // 底部按键行:暂停 / 重开 / 退出 / 速降(黑底反白)
    lv_obj_t* row = lv_obj_create(body);
    lv_obj_remove_style_all(row);
    lv_obj_set_pos(row, kBoardX, kBoardY + kH * kCell + 6 + 14);
    lv_obj_set_size(row, 426, kBtnH);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    DisableScroll(row);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    s_pause_btn_lbl = MakeBtn(row, "暂停", 100, kBtnH, OnBtnClicked, (void*)0);
    MakeBtn(row, "重开", 100, kBtnH, OnBtnClicked, (void*)1);
    MakeBtn(row, "退出", 100, kBtnH, OnBtnClicked, (void*)2);
    MakeBtn(row, "速降", 100, kBtnH, OnBtnClicked, (void*)3, true);

    // 初始渲染
    RedrawBoard();
    DrawCurrent();
    DrawGhost();
    DrawPreviews();
    UpdateLabels();
    SyncPauseBtn();

    // 重力定时器(暂停态则先挂起)
    s_timer = lv_timer_create(OnGameTick, static_cast<uint32_t>(s_core.drop_interval_ms()), nullptr);
    if (s_core.state() == tetris::State::Paused) lv_timer_pause(s_timer);

    s_alive = true;
    VkKey_AttachScreen(s_scr, kScreenId, VkKeyScreenDesc{
        .factory = TetrisScreen::Create,
        .on_key = OnVkKey,
    });
    return s_scr;
}
