// main/display/screen/epd_widget_kit.cc
#include "epd_widget_kit.h"
#include "screen_common.h"
#include <esp_log.h>

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

    ScreenLvAsync([](void* arg) {
        auto* msg = static_cast<std::string*>(arg);
        lv_obj_t* top = lv_layer_top();
        if (top == nullptr) { delete msg; return; }

        lv_obj_t* toast = lv_obj_create(top);
        lv_obj_set_size(toast, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_max_width(toast, 400, 0);
        lv_obj_set_style_bg_color(toast, lv_color_white(), 0);
        lv_obj_set_style_border_width(toast, 2, 0);
        lv_obj_set_style_border_color(toast, lv_color_black(), 0);
        lv_obj_set_style_radius(toast, 8, 0);
        lv_obj_set_style_pad_all(toast, 12, 0);
        lv_obj_align(toast, LV_ALIGN_BOTTOM_MID, 0, -60);

        lv_obj_t* lbl = ScreenMakeLabel(toast, msg->c_str(), 24, true);
        lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);

        // 定时 2 秒自删
        lv_timer_t* t = lv_timer_create([](lv_timer_t* timer) {
            lv_obj_t* obj = static_cast<lv_obj_t*>(lv_timer_get_user_data(timer));
            if (obj && lv_obj_is_valid(obj)) {
                lv_obj_del(obj);
            }
            lv_timer_del(timer);
        }, 2000, toast);
        lv_timer_set_repeat_count(t, 1);

        delete msg;
    }, new std::string(message));
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
        lv_obj_t* top = lv_layer_top();
        if (top == nullptr) { delete a; return; }

        // 遮罩容器
        lv_obj_t* mask = lv_obj_create(top);
        lv_obj_set_size(mask, lv_pct(100), lv_pct(100));
        lv_obj_set_style_bg_color(mask, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(mask, LV_OPA_50, 0);
        lv_obj_set_style_border_width(mask, 0, 0);

        // 居中对话框卡片 (墨水屏高对比度白底黑边)
        lv_obj_t* card = lv_obj_create(mask);
        lv_obj_set_size(card, 380, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(card, lv_color_white(), 0);
        lv_obj_set_style_border_width(card, 2, 0);
        lv_obj_set_style_border_color(card, lv_color_black(), 0);
        lv_obj_set_style_radius(card, 8, 0);
        lv_obj_set_style_pad_all(card, 16, 0);
        lv_obj_center(card);

        // 标题
        ScreenMakeLabel(card, a->title.c_str(), 26, true);

        // 内容
        lv_obj_t* body = ScreenMakeLabel(card, a->message.c_str(), 22, true);
        lv_obj_set_style_pad_top(body, 12, 0);

        // 按钮行
        lv_obj_t* btn_row = lv_obj_create(card);
        lv_obj_set_size(btn_row, lv_pct(100), 50);
        lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(btn_row, 0, 0);
        lv_obj_set_style_pad_top(btn_row, 16, 0);
        lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        struct CbPayload {
            lv_obj_t* mask;
            ResultCallback cb;
        };

        // 取消按钮
        lv_obj_t* cancel_btn = lv_btn_create(btn_row);
        lv_obj_set_size(cancel_btn, 120, 42);
        lv_obj_set_style_bg_color(cancel_btn, lv_color_white(), 0);
        lv_obj_set_style_border_width(cancel_btn, 1, 0);
        lv_obj_set_style_border_color(cancel_btn, lv_color_black(), 0);
        lv_obj_t* cancel_lbl = lv_label_create(cancel_btn);
        lv_label_set_text(cancel_lbl, "取消");
        lv_obj_center(cancel_lbl);

        auto* cancel_p = new CbPayload{mask, a->callback};
        lv_obj_add_event_cb(cancel_btn, [](lv_event_t* e) {
            auto* p = static_cast<CbPayload*>(lv_event_get_user_data(e));
            if (p && p->cb) p->cb(false);
            if (p && p->mask && lv_obj_is_valid(p->mask)) lv_obj_del(p->mask);
            delete p;
        }, LV_EVENT_CLICKED, cancel_p);

        // 确认按钮 (反色黑底白字, 强调主操作)
        lv_obj_t* ok_btn = lv_btn_create(btn_row);
        lv_obj_set_size(ok_btn, 120, 42);
        lv_obj_set_style_bg_color(ok_btn, lv_color_black(), 0);
        lv_obj_set_style_border_width(ok_btn, 1, 0);
        lv_obj_set_style_border_color(ok_btn, lv_color_black(), 0);
        lv_obj_t* ok_lbl = lv_label_create(ok_btn);
        lv_label_set_text(ok_lbl, a->confirm_text.c_str());
        lv_obj_set_style_text_color(ok_lbl, lv_color_white(), 0);
        lv_obj_center(ok_lbl);

        auto* ok_p = new CbPayload{mask, a->callback};
        lv_obj_add_event_cb(ok_btn, [](lv_event_t* e) {
            auto* p = static_cast<CbPayload*>(lv_event_get_user_data(e));
            if (p && p->cb) p->cb(true);
            if (p && p->mask && lv_obj_is_valid(p->mask)) lv_obj_del(p->mask);
            delete p;
        }, LV_EVENT_CLICKED, ok_p);

        delete a;
    }, arg);
}

} // namespace EpdWidgetKit
