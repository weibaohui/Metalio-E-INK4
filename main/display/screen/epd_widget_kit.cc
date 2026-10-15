// main/display/screen/epd_widget_kit.cc
#include "epd_widget_kit.h"
#include "screen_common.h"
#include "display/modal_manager.h"
#include "display/status_bar_manager.h"
#include "haptic_feedback.h"
#include "application.h"
#include "assets/lang_config.h"
#include "fontpack_lvgl.h"
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "EpdWidgetKit"

namespace EpdWidgetKit {

lv_obj_t* EpdHeader::Create(lv_obj_t* parent, const char* title, lv_event_cb_t on_back) {
    if (parent == nullptr) return nullptr;

    lv_obj_t* header = lv_obj_create(parent);
    lv_obj_set_size(header, lv_pct(100), 56);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 6, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // 返回键 (大触摸热区 48x44)
    lv_obj_t* back_btn = lv_btn_create(header);
    lv_obj_set_size(back_btn, 48, 44);
    lv_obj_set_style_bg_opa(back_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(back_btn, 0, 0);
    lv_obj_set_style_shadow_width(back_btn, 0, 0);

    lv_obj_t* back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, "＜");
    lv_obj_center(back_lbl);

    if (on_back != nullptr) {
        lv_obj_add_event_cb(back_btn, on_back, LV_EVENT_CLICKED, nullptr);
    } else {
        lv_obj_add_event_cb(back_btn, [](lv_event_t*) {
            ScreenNavigateBack();
        }, LV_EVENT_CLICKED, nullptr);
    }

    // 主标题
    lv_obj_t* title_lbl = ScreenMakeLabel(header, title != nullptr ? title : "", 28, false);
    lv_obj_set_flex_grow(title_lbl, 1);

    return header;
}

lv_obj_t* EpdCell::CreateText(lv_obj_t* parent, const char* title, const char* value_text, lv_event_cb_t on_click) {
    if (parent == nullptr) return nullptr;

    lv_obj_t* cell = lv_obj_create(parent);
    lv_obj_set_size(cell, lv_pct(100), 62);
    lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_side(cell, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(cell, 1, 0);
    lv_obj_set_style_border_color(cell, lv_color_black(), 0);
    lv_obj_set_style_pad_hor(cell, 12, 0);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // 左侧标题
    ScreenMakeLabel(cell, title != nullptr ? title : "", 24, false);

    // 右侧内容/箭头
    lv_obj_t* val_lbl = ScreenMakeLabel(cell, value_text != nullptr ? value_text : "＞", 22, false);
    lv_obj_set_style_text_align(val_lbl, LV_TEXT_ALIGN_RIGHT, 0);

    if (on_click != nullptr) {
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(cell, on_click, LV_EVENT_CLICKED, nullptr);
    }
    return cell;
}

lv_obj_t* EpdCell::CreateSwitch(lv_obj_t* parent, const char* title, bool initial_state, lv_event_cb_t on_toggle) {
    if (parent == nullptr) return nullptr;

    lv_obj_t* cell = lv_obj_create(parent);
    lv_obj_set_size(cell, lv_pct(100), 62);
    lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_side(cell, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(cell, 1, 0);
    lv_obj_set_style_border_color(cell, lv_color_black(), 0);
    lv_obj_set_style_pad_hor(cell, 12, 0);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    // 左侧标题
    ScreenMakeLabel(cell, title != nullptr ? title : "", 24, false);

    // 右侧 Switch 控件 (墨水屏高对比度白底黑边)
    lv_obj_t* sw = lv_switch_create(cell);
    lv_obj_set_size(sw, 56, 30);
    if (initial_state) {
        lv_obj_add_state(sw, LV_STATE_CHECKED);
    } else {
        lv_obj_clear_state(sw, LV_STATE_CHECKED);
    }

    if (on_toggle != nullptr) {
        lv_obj_add_event_cb(sw, on_toggle, LV_EVENT_VALUE_CHANGED, nullptr);
    }
    return cell;
}

void EpdToast::Show(const char* message, uint32_t duration_ms) {
    if (message == nullptr || message[0] == '\0') return;
    StatusBarManager::GetInstance().ShowToast(message, duration_ms);
}

void EpdDialog::Show(const char* title, const char* message, const char* confirm_text, ResultCallback on_result) {
    struct DialogArg {
        std::string title;
        std::string message;
        std::string confirm_text;
        ResultCallback callback;
    };

    auto* arg = new DialogArg{
        title ? title : "提示",
        message ? message : "",
        confirm_text ? confirm_text : "确定",
        on_result
    };

    ScreenLvAsync([](void* p) {
        auto* a = static_cast<DialogArg*>(p);
        ModalManager::GetInstance().ShowDialog(
            a->title.c_str(), a->message.c_str(), a->confirm_text.c_str(), "取消", a->callback);
        delete a;
    }, arg);
}

// ==================== EpdRestartCountdown 重启倒计时模态弹窗 ====================

namespace {

struct RestartCountdownState {
    lv_obj_t* overlay = nullptr;
    lv_obj_t* msg_lbl = nullptr;
    lv_timer_t* timer = nullptr;
    int remaining = 0;
    std::string headline;
};

RestartCountdownState s_restart_state;

void RestartRebootTask(void* /*arg*/) {
    vTaskDelay(pdMS_TO_TICKS(200));
    Application::GetInstance().Reboot();
    vTaskDelete(nullptr);
}

void RestartTimerCallback(lv_timer_t* /*timer*/) {
    s_restart_state.remaining--;
    if (s_restart_state.remaining > 0) {
        char buf[128];
        snprintf(buf, sizeof(buf), Lang::Strings::SETTINGS_NET_REBOOT_COUNT_FMT,
                 s_restart_state.headline.c_str(), s_restart_state.remaining);
        if (s_restart_state.msg_lbl && lv_obj_is_valid(s_restart_state.msg_lbl)) {
            lv_label_set_text(s_restart_state.msg_lbl, buf);
        }
        return;
    }
    if (s_restart_state.timer != nullptr) {
        lv_timer_delete(s_restart_state.timer);
        s_restart_state.timer = nullptr;
    }
    if (s_restart_state.msg_lbl && lv_obj_is_valid(s_restart_state.msg_lbl)) {
        lv_label_set_text(s_restart_state.msg_lbl, Lang::Strings::SETTINGS_NET_REBOOTING);
    }
    xTaskCreate(RestartRebootTask, "net_reboot", 2048, nullptr, 5, nullptr);
}

} // namespace

void EpdRestartCountdown::Cancel() {
    if (s_restart_state.timer != nullptr) {
        lv_timer_delete(s_restart_state.timer);
        s_restart_state.timer = nullptr;
    }
    if (s_restart_state.overlay != nullptr && lv_obj_is_valid(s_restart_state.overlay)) {
        lv_obj_delete(s_restart_state.overlay);
    }
    s_restart_state.overlay = nullptr;
    s_restart_state.msg_lbl = nullptr;
    s_restart_state.headline.clear();
    s_restart_state.remaining = 0;
}

bool EpdRestartCountdown::IsActive() {
    return s_restart_state.timer != nullptr || (s_restart_state.overlay != nullptr && lv_obj_is_valid(s_restart_state.overlay));
}

void EpdRestartCountdown::Show(lv_obj_t* parent, const char* headline, int countdown_sec) {
    if (parent == nullptr) return;
    Cancel();

    s_restart_state.headline = headline ? headline : "";
    s_restart_state.remaining = countdown_sec > 0 ? countdown_sec : 3;

    lv_obj_t* mask = lv_obj_create(parent);
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(mask, 0, 0);
    ScreenApplyDotBackdrop(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    s_restart_state.overlay = mask;
    lv_obj_add_event_cb(mask, [](lv_event_t* /*e*/) {
        if (s_restart_state.timer != nullptr) {
            lv_timer_delete(s_restart_state.timer);
            s_restart_state.timer = nullptr;
        }
        s_restart_state.overlay = nullptr;
        s_restart_state.msg_lbl = nullptr;
        s_restart_state.headline.clear();
        s_restart_state.remaining = 0;
    }, LV_EVENT_DELETE, nullptr);

    lv_obj_t* card = lv_obj_create(mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_HOR_RES - 48, 160);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, 16, 0);

    lv_obj_t* lbl = lv_label_create(card);
    s_restart_state.msg_lbl = lbl;
    char buf[128];
    snprintf(buf, sizeof(buf), Lang::Strings::SETTINGS_NET_REBOOT_COUNT_FMT,
             s_restart_state.headline.c_str(), s_restart_state.remaining);
    lv_label_set_text(lbl, buf);
    lv_obj_set_width(lbl, LV_PCT(100));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl);

    s_restart_state.timer = lv_timer_create(RestartTimerCallback, 1000, nullptr);
}

// ==================== EpdText 自动折行文本实现 ====================

void EpdText::EnableAutoWrap(lv_obj_t* label, lv_coord_t width_pct) {
    if (label == nullptr) return;
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, lv_pct(width_pct));
}

lv_obj_t* EpdText::Create(lv_obj_t* parent, const char* text, int font_size, bool wrap) {
    return ScreenMakeLabel(parent, text, font_size, wrap);
}

// ==================== EpdScroll 底层翻页滚动实现 ====================

namespace EpdScroll {

bool PageTurn(lv_obj_t* scrollable, int dir, int overlap_px) {
    if (scrollable == nullptr || dir == 0) return false;
    lv_obj_update_layout(scrollable);
    const int32_t view_h = lv_obj_get_height(scrollable);
    if (view_h <= 0) return false;

    const int32_t cur = lv_obj_get_scroll_y(scrollable);
    const int32_t max_offset = lv_obj_get_scroll_top(scrollable) + lv_obj_get_scroll_bottom(scrollable);
    if (max_offset <= 0) return false;

    int32_t step = view_h - overlap_px;
    if (step < 40) step = view_h;

    int32_t target = cur + dir * step;
    if (target < 0) target = 0;
    if (target > max_offset) target = max_offset;

    if (target != cur) {
        lv_obj_scroll_to_y(scrollable, target, LV_ANIM_OFF);
        return true;
    }
    return false;
}

void ScrollToTop(lv_obj_t* scrollable) {
    if (scrollable == nullptr) return;
    lv_obj_update_layout(scrollable);
    lv_obj_scroll_to_y(scrollable, 0, LV_ANIM_OFF);
}

void ScrollToBottom(lv_obj_t* scrollable) {
    if (scrollable == nullptr) return;
    lv_obj_update_layout(scrollable);
    const int32_t max_offset = lv_obj_get_scroll_top(scrollable) + lv_obj_get_scroll_bottom(scrollable);
    if (max_offset > 0) {
        lv_obj_scroll_to_y(scrollable, max_offset, LV_ANIM_OFF);
    }
}

void GetPageInfo(lv_obj_t* scrollable, int* cur_page, int* total_pages, int overlap_px) {
    if (cur_page) *cur_page = 1;
    if (total_pages) *total_pages = 1;
    if (scrollable == nullptr) return;

    lv_obj_update_layout(scrollable);
    const int32_t view_h = lv_obj_get_height(scrollable);
    if (view_h <= 0) return;

    const int32_t max_offset = lv_obj_get_scroll_top(scrollable) + lv_obj_get_scroll_bottom(scrollable);
    int32_t step = view_h - overlap_px;
    if (step < 40) step = view_h;

    if (max_offset <= 0) {
        if (cur_page) *cur_page = 1;
        if (total_pages) *total_pages = 1;
        return;
    }

    int total = 1 + (max_offset + step - 1) / step;
    int cur = 1 + (lv_obj_get_scroll_y(scrollable) + step / 2) / step;
    if (cur < 1) cur = 1;
    if (cur > total) cur = total;

    if (cur_page) *cur_page = cur;
    if (total_pages) *total_pages = total;
}

struct SwipeData {
    lv_obj_t* scroll_obj;
    int overlap;
    std::function<void(int dir)> cb;
};

void AttachSwipeGesture(lv_obj_t* touch_target, lv_obj_t* scrollable, int overlap_px, std::function<void(int dir)> on_turn) {
    if (touch_target == nullptr) return;
    lv_obj_t* target = (scrollable != nullptr) ? scrollable : touch_target;

    auto* data = new SwipeData{target, overlap_px, on_turn};

    lv_obj_add_event_cb(touch_target, [](lv_event_t* e) {
        auto* d = static_cast<SwipeData*>(lv_event_get_user_data(e));
        if (!d || !d->scroll_obj || !lv_obj_is_valid(d->scroll_obj)) return;

        lv_indev_t* indev = lv_indev_active();
        if (indev == nullptr) return;

        lv_dir_t gesture = lv_indev_get_gesture_dir(indev);
        if (gesture == LV_DIR_TOP || gesture == LV_DIR_BOTTOM) {
            lv_indev_reset(indev, nullptr);
            lv_indev_wait_release(indev);

            // 手势方向与阅读方向: 手指上滑(LV_DIR_TOP)=查看后文(下一页, +1); 手指下滑(LV_DIR_BOTTOM)=查看前文(上一页, -1)
            int dir = (gesture == LV_DIR_TOP) ? 1 : -1;
            bool moved = PageTurn(d->scroll_obj, dir, d->overlap);
            if (moved && d->cb) {
                d->cb(dir);
            }
        }
    }, LV_EVENT_GESTURE, data);

    lv_obj_add_event_cb(touch_target, [](lv_event_t* e) {
        auto* d = static_cast<SwipeData*>(lv_event_get_user_data(e));
        delete d;
    }, LV_EVENT_DELETE, data);
}

} // namespace EpdScroll

// ==================== EpdPageScrollView 复合控件实现 ====================

namespace {

static lv_obj_t* CreateNavBtn(lv_obj_t* parent, const char* text, lv_event_cb_t cb, void* user_data) {
    lv_obj_t* btn = lv_btn_create(parent);
    lv_obj_set_size(btn, 110, 38);
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn, 1, 0);
    lv_obj_set_style_radius(btn, 6, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t* lbl = ScreenMakeLabel(btn, text, 22, false);
    lv_obj_center(lbl);
    return btn;
}

static void SetBtnEnabled(lv_obj_t* btn, bool enabled) {
    if (btn == nullptr) return;
    if (enabled) {
        lv_obj_clear_state(btn, LV_STATE_DISABLED);
        lv_obj_set_style_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    } else {
        lv_obj_add_state(btn, LV_STATE_DISABLED);
        lv_obj_set_style_opa(btn, LV_OPA_30, 0);
        lv_obj_set_style_border_color(btn, lv_color_make(180, 180, 180), 0);
    }
}

} // namespace

EpdPageScrollView* EpdPageScrollView::Create(lv_obj_t* parent, const Config& cfg) {
    if (parent == nullptr) return nullptr;

    auto* view = new EpdPageScrollView();
    view->cfg_ = cfg;

    // 1. 根容器 (采用纵向 flex 布局)
    view->root_ = lv_obj_create(parent);
    lv_obj_set_size(view->root_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_opa(view->root_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(view->root_, 0, 0);
    lv_obj_set_style_pad_all(view->root_, 0, 0);
    lv_obj_set_flex_flow(view->root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(view->root_, LV_OBJ_FLAG_SCROLLABLE);

    // 2. 绑定生命周期释放
    lv_obj_add_event_cb(view->root_, [](lv_event_t* e) {
        auto* self = static_cast<EpdPageScrollView*>(lv_event_get_user_data(e));
        delete self;
    }, LV_EVENT_DELETE, view);

    auto build_bar = [view]() {
        view->bar_ = lv_obj_create(view->root_);
        lv_obj_set_size(view->bar_, lv_pct(100), view->cfg_.bar_height);
        lv_obj_set_style_bg_color(view->bar_, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(view->bar_, LV_OPA_COVER, 0);
        lv_obj_set_style_border_side(view->bar_, view->cfg_.bar_at_bottom ? LV_BORDER_SIDE_TOP : LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(view->bar_, 1, 0);
        lv_obj_set_style_border_color(view->bar_, lv_color_black(), 0);
        lv_obj_set_flex_flow(view->bar_, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(view->bar_, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(view->bar_, 16, 0);
        lv_obj_set_style_pad_ver(view->bar_, 4, 0);
        lv_obj_clear_flag(view->bar_, LV_OBJ_FLAG_SCROLLABLE);

        view->prev_btn_ = CreateNavBtn(view->bar_, "▲ 上一页", [](lv_event_t* e) {
            auto* self = static_cast<EpdPageScrollView*>(lv_event_get_user_data(e));
            if (!self) return;
            if (self->on_page_nav_) {
                self->on_page_nav_(-1);
            } else {
                self->PageUp();
            }
        }, view);

        view->page_lbl_ = ScreenMakeLabel(view->bar_, "第 1 / 1 页", 22, false);

        view->next_btn_ = CreateNavBtn(view->bar_, "▼ 下一页", [](lv_event_t* e) {
            auto* self = static_cast<EpdPageScrollView*>(lv_event_get_user_data(e));
            if (!self) return;
            if (self->on_page_nav_) {
                self->on_page_nav_(1);
            } else {
                self->PageDown();
            }
        }, view);
    };

    if (cfg.show_page_bar && !cfg.bar_at_bottom) {
        build_bar();
    }

    // 3. 中间可滚动视口容器 (flex_grow 占满剩余所有高度)
    view->container_ = lv_obj_create(view->root_);
    lv_obj_set_width(view->container_, lv_pct(100));
    lv_obj_set_flex_grow(view->container_, 1);
    lv_obj_set_style_bg_opa(view->container_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(view->container_, 0, 0);
    lv_obj_set_style_pad_all(view->container_, 0, 0);
    lv_obj_set_scrollbar_mode(view->container_, LV_SCROLLBAR_MODE_OFF);

    if (cfg.show_page_bar && cfg.bar_at_bottom) {
        build_bar();
    }

    // 4. 绑定上下滑动手势
    if (cfg.enable_swipe) {
        EpdScroll::AttachSwipeGesture(view->container_, view->container_, cfg.overlap_px, [view](int) {
            view->UpdatePageInfo();
        });
    }

    view->UpdatePageInfo();
    return view;
}

bool EpdPageScrollView::PageUp() {
    bool moved = EpdScroll::PageTurn(container_, -1, cfg_.overlap_px);
    UpdatePageInfo();
    return moved;
}

bool EpdPageScrollView::PageDown() {
    bool moved = EpdScroll::PageTurn(container_, 1, cfg_.overlap_px);
    UpdatePageInfo();
    return moved;
}

void EpdPageScrollView::ScrollToTop() {
    EpdScroll::ScrollToTop(container_);
    UpdatePageInfo();
}

void EpdPageScrollView::ScrollToBottom() {
    EpdScroll::ScrollToBottom(container_);
    UpdatePageInfo();
}

void EpdPageScrollView::UpdatePageInfo() {
    if (manual_page_info_) {
        RefreshBarUI();
        return;
    }
    EpdScroll::GetPageInfo(container_, &cur_page_, &total_pages_, cfg_.overlap_px);
    RefreshBarUI();
    if (on_page_changed_) {
        on_page_changed_(cur_page_, total_pages_);
    }
}

void EpdPageScrollView::SetPageInfo(int cur_page, int total_pages) {
    manual_page_info_ = true;
    cur_page_ = (cur_page < 1) ? 1 : cur_page;
    total_pages_ = (total_pages < 1) ? 1 : total_pages;
    custom_page_text_.clear();
    RefreshBarUI();
    if (on_page_changed_) {
        on_page_changed_(cur_page_, total_pages_);
    }
}

void EpdPageScrollView::SetPageText(const char* text) {
    manual_page_info_ = true;
    custom_page_text_ = (text != nullptr) ? text : "";
    RefreshBarUI();
}

void EpdPageScrollView::ResetAutoPageInfo() {
    manual_page_info_ = false;
    custom_page_text_.clear();
    UpdatePageInfo();
}

void EpdPageScrollView::SetPageBarVisible(bool visible) {
    if (bar_ != nullptr) {
        if (visible) {
            lv_obj_clear_flag(bar_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(bar_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void EpdPageScrollView::SetNavButtonsEnabled(bool prev_enabled, bool next_enabled) {
    if (prev_btn_) SetBtnEnabled(prev_btn_, prev_enabled);
    if (next_btn_) SetBtnEnabled(next_btn_, next_enabled);
}

void EpdPageScrollView::RefreshBarUI() {
    if (!cfg_.show_page_bar || bar_ == nullptr || page_lbl_ == nullptr) return;

    // 单页时自动隐藏翻页控制栏，直接把空间返还给内容视口
    if (cfg_.auto_hide_single_page && total_pages_ <= 1) {
        lv_obj_add_flag(bar_, LV_OBJ_FLAG_HIDDEN);
        return;
    } else {
        lv_obj_clear_flag(bar_, LV_OBJ_FLAG_HIDDEN);
    }

    if (!custom_page_text_.empty()) {
        lv_label_set_text(page_lbl_, custom_page_text_.c_str());
    } else {
        char buf[32];
        snprintf(buf, sizeof(buf), "第 %d / %d 页", cur_page_, total_pages_);
        lv_label_set_text(page_lbl_, buf);
    }

    if (prev_btn_) {
        SetBtnEnabled(prev_btn_, cur_page_ > 1);
    }
    if (next_btn_) {
        SetBtnEnabled(next_btn_, cur_page_ < total_pages_);
    }
}

// ==================== EpdPageScaffold ====================

EpdPageScaffold* EpdPageScaffold::Create(const Config& cfg) {
    auto* scaffold = new EpdPageScaffold();
    scaffold->cfg_ = cfg;

    scaffold->scr_ = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scaffold->scr_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scaffold->scr_, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(scaffold->scr_, lv_color_black(), 0);
    lv_obj_clear_flag(scaffold->scr_, LV_OBJ_FLAG_SCROLLABLE);

    // 绑定生命周期：Screen 被 LVGL 销毁时自动回收 C++ 对象
    lv_obj_add_event_cb(scaffold->scr_, [](lv_event_t* e) {
        auto* self = static_cast<EpdPageScaffold*>(lv_event_get_user_data(e));
        delete self;
    }, LV_EVENT_DELETE, scaffold);

    lv_coord_t top_offset = 0;
    if (cfg.show_status_bar) {
        scaffold->status_bar_ = ScreenCreateStatusBar(scaffold->scr_);
        top_offset += scaffold->status_bar_.height;
    }

    if (cfg.show_header && cfg.title != nullptr) {
        scaffold->header_ = EpdHeader::Create(scaffold->scr_, cfg.title, cfg.on_back);
        if (scaffold->header_ != nullptr) {
            lv_obj_align(scaffold->header_, LV_ALIGN_TOP_MID, 0, top_offset);
            top_offset += 56; // EpdHeader height
            if (!cfg.show_back_btn) {
                lv_obj_t* back_btn = lv_obj_get_child(scaffold->header_, 0);
                if (back_btn) lv_obj_add_flag(back_btn, LV_OBJ_FLAG_HIDDEN);
            }
        }
    }

    const lv_coord_t body_h = LV_VER_RES - top_offset;
    scaffold->body_ = lv_obj_create(scaffold->scr_);
    lv_obj_remove_style_all(scaffold->body_);
    lv_obj_set_size(scaffold->body_, LV_HOR_RES, body_h);
    lv_obj_align(scaffold->body_, LV_ALIGN_TOP_MID, 0, top_offset);
    lv_obj_set_style_bg_opa(scaffold->body_, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(scaffold->body_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(scaffold->body_, LV_OBJ_FLAG_CLICKABLE);

    if (cfg.scrollable) {
        scaffold->scroll_view_ = EpdPageScrollView::Create(scaffold->body_, cfg.scroll_cfg);
        lv_obj_t* container = scaffold->scroll_view_->GetContainer();
        if (cfg.flex_column) {
            lv_obj_set_flex_flow(container, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_row(container, cfg.row_gap, 0);
        }
        lv_obj_set_style_pad_left(container, cfg.pad_h, 0);
        lv_obj_set_style_pad_right(container, cfg.pad_h, 0);
        lv_obj_set_style_pad_top(container, cfg.pad_v, 0);
        lv_obj_set_style_pad_bottom(container, cfg.pad_v, 0);
    } else {
        if (cfg.flex_column) {
            lv_obj_set_flex_flow(scaffold->body_, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_style_pad_row(scaffold->body_, cfg.row_gap, 0);
        }
        lv_obj_set_style_pad_left(scaffold->body_, cfg.pad_h, 0);
        lv_obj_set_style_pad_right(scaffold->body_, cfg.pad_h, 0);
        lv_obj_set_style_pad_top(scaffold->body_, cfg.pad_v, 0);
        lv_obj_set_style_pad_bottom(scaffold->body_, cfg.pad_v, 0);
    }

    return scaffold;
}

lv_obj_t* EpdPageScaffold::GetContentContainer() const {
    if (scroll_view_ != nullptr) {
        return scroll_view_->GetContainer();
    }
    return body_;
}

} // namespace EpdWidgetKit

