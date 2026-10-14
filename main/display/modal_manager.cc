// main/display/modal_manager.cc
#include "modal_manager.h"
#include "haptic_feedback.h"
#include <esp_log.h>

static const char* TAG = "ModalManager";

ModalManager& ModalManager::GetInstance() {
    static ModalManager s_instance;
    return s_instance;
}

bool ModalManager::HasActiveModal() const {
    return !stack_.empty();
}

bool ModalManager::PopModal() {
    if (stack_.empty()) {
        return false;
    }

    ModalEntry entry = stack_.back();
    stack_.pop_back();

    if (entry.on_dismiss) {
        entry.on_dismiss();
    }

    if (entry.backdrop != nullptr && lv_obj_is_valid(entry.backdrop)) {
        lv_obj_delete_async(entry.backdrop);
    }
    ESP_LOGI(TAG, "已关闭顶层模态弹窗 (剩余=%d)", (int)stack_.size());
    return true;
}

void ModalManager::DismissAll() {
    while (!stack_.empty()) {
        PopModal();
    }
}

lv_obj_t* ModalManager::PushCustomModal(std::function<void(lv_obj_t* card)> builder,
                                        std::function<void()> on_dismiss) {
    lv_obj_t* active_scr = lv_screen_active();
    if (active_scr == nullptr) {
        ESP_LOGW(TAG, "无当前活跃屏幕，无法挂载模态弹窗");
        return nullptr;
    }

    // 1. 全屏防击穿遮罩背景 (Backdrop)
    // 墨水屏不使用半透明半调网点(避免严重残影与闪烁)，使用透明+强制拦截点击
    lv_obj_t* backdrop = lv_obj_create(active_scr);
    lv_obj_set_size(backdrop, LV_HOR_RES, LV_VER_RES);
    lv_obj_align(backdrop, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_opa(backdrop, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(backdrop, 0, 0);
    lv_obj_set_style_pad_all(backdrop, 0, 0);
    lv_obj_set_scrollbar_mode(backdrop, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(backdrop, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(backdrop, LV_OBJ_FLAG_SCROLLABLE);

    // 吞噬所有背景触摸，防误触
    lv_obj_add_event_cb(backdrop, [](lv_event_t*) {
        // 吸收点击
    }, LV_EVENT_CLICKED, nullptr);

    // 2. 居中高对比度墨水屏模态卡片 (Card)
    lv_obj_t* card = lv_obj_create(backdrop);
    lv_obj_set_width(card, LV_HOR_RES - 64); // 左右各留 32px
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_align(card, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 8, 0);
    lv_obj_set_style_pad_all(card, 20, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 16, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    if (builder) {
        builder(card);
    }

    ModalEntry entry{backdrop, card, std::move(on_dismiss)};
    stack_.push_back(entry);

    // 监听 backdrop 外部主动被删除时的安全回收
    lv_obj_add_event_cb(backdrop, [](lv_event_t* e) {
        auto* bd = static_cast<lv_obj_t*>(lv_event_get_target(e));
        auto& inst = ModalManager::GetInstance();
        for (auto it = inst.stack_.begin(); it != inst.stack_.end(); ++it) {
            if (it->backdrop == bd) {
                if (it->on_dismiss) it->on_dismiss();
                inst.stack_.erase(it);
                break;
            }
        }
    }, LV_EVENT_DELETE, nullptr);

    ESP_LOGI(TAG, "已挂载新模态弹窗 (栈深=%d)", (int)stack_.size());
    return card;
}

lv_obj_t* ModalManager::ShowDialog(const char* title,
                                   const char* message,
                                   const char* confirm_text,
                                   const char* cancel_text,
                                   std::function<void(bool confirmed)> on_result) {
    return PushCustomModal([title, message, confirm_text, cancel_text, on_result](lv_obj_t* card) {
        // 标题
        if (title != nullptr && title[0] != '\0') {
            lv_obj_t* t_lbl = lv_label_create(card);
            lv_label_set_text(t_lbl, title);
            lv_obj_set_style_text_align(t_lbl, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_width(t_lbl, lv_pct(100));
        }

        // 正文
        lv_obj_t* msg_lbl = lv_label_create(card);
        lv_label_set_text(msg_lbl, message != nullptr ? message : "");
        lv_obj_set_style_text_align(msg_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(msg_lbl, lv_pct(100));
        lv_label_set_long_mode(msg_lbl, LV_LABEL_LONG_WRAP);

        // 按钮栏
        lv_obj_t* btn_row = lv_obj_create(card);
        lv_obj_remove_style_all(btn_row);
        lv_obj_set_size(btn_row, lv_pct(100), 48);
        lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(btn_row, 12, 0);

        // 取消按钮
        lv_obj_t* cancel_btn = lv_btn_create(btn_row);
        lv_obj_set_flex_grow(cancel_btn, 1);
        lv_obj_set_height(cancel_btn, 44);
        lv_obj_set_style_bg_color(cancel_btn, lv_color_white(), 0);
        lv_obj_set_style_border_color(cancel_btn, lv_color_black(), 0);
        lv_obj_set_style_border_width(cancel_btn, 2, 0);
        lv_obj_set_style_radius(cancel_btn, 4, 0);
        HapticAttachClick(cancel_btn);

        lv_obj_t* cancel_lbl = lv_label_create(cancel_btn);
        lv_label_set_text(cancel_lbl, cancel_text != nullptr ? cancel_text : "取消");
        lv_obj_set_style_text_color(cancel_lbl, lv_color_black(), 0);
        lv_obj_center(cancel_lbl);

        lv_obj_add_event_cb(cancel_btn, [](lv_event_t* e) {
            auto* cb = static_cast<std::function<void(bool)>*>(lv_event_get_user_data(e));
            if (cb && *cb) (*cb)(false);
            ModalManager::GetInstance().PopModal();
        }, LV_EVENT_CLICKED, new std::function<void(bool)>(on_result));

        // 确定按钮
        lv_obj_t* confirm_btn = lv_btn_create(btn_row);
        lv_obj_set_flex_grow(confirm_btn, 1);
        lv_obj_set_height(confirm_btn, 44);
        lv_obj_set_style_bg_color(confirm_btn, lv_color_black(), 0);
        lv_obj_set_style_border_color(confirm_btn, lv_color_black(), 0);
        lv_obj_set_style_border_width(confirm_btn, 2, 0);
        lv_obj_set_style_radius(confirm_btn, 4, 0);
        HapticAttachClick(confirm_btn);

        lv_obj_t* confirm_lbl = lv_label_create(confirm_btn);
        lv_label_set_text(confirm_lbl, confirm_text != nullptr ? confirm_text : "确定");
        lv_obj_set_style_text_color(confirm_lbl, lv_color_white(), 0);
        lv_obj_center(confirm_lbl);

        lv_obj_add_event_cb(confirm_btn, [](lv_event_t* e) {
            auto* cb = static_cast<std::function<void(bool)>*>(lv_event_get_user_data(e));
            if (cb && *cb) (*cb)(true);
            ModalManager::GetInstance().PopModal();
        }, LV_EVENT_CLICKED, new std::function<void(bool)>(on_result));
    });
}

lv_obj_t* ModalManager::ShowAlert(const char* title,
                                  const char* message,
                                  const char* btn_text,
                                  std::function<void()> on_close) {
    return PushCustomModal([title, message, btn_text, on_close](lv_obj_t* card) {
        if (title != nullptr && title[0] != '\0') {
            lv_obj_t* t_lbl = lv_label_create(card);
            lv_label_set_text(t_lbl, title);
            lv_obj_set_style_text_align(t_lbl, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_width(t_lbl, lv_pct(100));
        }

        lv_obj_t* msg_lbl = lv_label_create(card);
        lv_label_set_text(msg_lbl, message != nullptr ? message : "");
        lv_obj_set_style_text_align(msg_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(msg_lbl, lv_pct(100));
        lv_label_set_long_mode(msg_lbl, LV_LABEL_LONG_WRAP);

        lv_obj_t* btn = lv_btn_create(card);
        lv_obj_set_width(btn, lv_pct(100));
        lv_obj_set_height(btn, 44);
        lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
        lv_obj_set_style_radius(btn, 4, 0);
        HapticAttachClick(btn);

        lv_obj_t* btn_lbl = lv_label_create(btn);
        lv_label_set_text(btn_lbl, btn_text != nullptr ? btn_text : "我知道了");
        lv_obj_set_style_text_color(btn_lbl, lv_color_white(), 0);
        lv_obj_center(btn_lbl);

        lv_obj_add_event_cb(btn, [](lv_event_t* e) {
            auto* cb = static_cast<std::function<void()>*>(lv_event_get_user_data(e));
            if (cb && *cb) (*cb)();
            ModalManager::GetInstance().PopModal();
        }, LV_EVENT_CLICKED, new std::function<void()>(on_close));
    });
}
