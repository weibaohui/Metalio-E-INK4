// main/display/screen/power_screen/power_screen.cc —— 电源管理 APP (全面升级为新架构)
#include "power_screen/power_screen.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "boot_key_handler.h"
#include "display/modal_manager.h"
#include "display/screen/epd_widget_kit.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "power_policy.h"
#include "screen_common.h"
#include "vk_key_handler.h"

namespace {

constexpr const char* TAG = "PowerScreen";
constexpr const char* kScreenId = "power";

struct PowerUi {
    lv_obj_t* screen = nullptr;
    lv_obj_t* footer_lbl = nullptr;
};
PowerUi s_ui;

const lv_font_t* UiFont() { return fontpack_lv_font_ui(); }

void SetFooter(const char* text) {
    if (s_ui.footer_lbl != nullptr) {
        lv_label_set_text(s_ui.footer_lbl, text);
    }
}

// 息屏: 进入待机(EPD 休眠 + 轻睡眠 LP; BOOT/POWER 键唤醒)
void OnSleepClicked(lv_event_t*) {
    HapticPulseIfEnabled();
    ESP_LOGI(TAG, "息屏(进入待机)");
    PowerKey_OnClick();
}

// 重启: 标准弹窗二次确认
void OnRebootClicked(lv_event_t*) {
    HapticPulseIfEnabled();
    ModalManager::GetInstance().ShowDialog(
        "重启设备",
        "确定要重启设备吗？未保存的数据可能会丢失。",
        "确定重启",
        "取消",
        [](bool confirmed) {
            if (confirmed) {
                SetFooter("正在重启…");
                Application::GetInstance().Schedule([]() {
                    ESP_LOGW(TAG, "电源页确认重启");
                    vTaskDelay(pdMS_TO_TICKS(300));
                    Application::GetInstance().Reboot();
                });
            }
        });
}

// 关机: 标准弹窗二次确认
void OnShutdownClicked(lv_event_t*) {
    HapticPulseIfEnabled();
    ModalManager::GetInstance().ShowDialog(
        "关闭设备",
        "确定要关机吗？关机后需长按电源键开机。",
        "确定关机",
        "取消",
        [](bool confirmed) {
            if (confirmed) {
                SetFooter("正在关机…");
                PowerPolicy::GetInstance().RequestPowerOff();
            }
        });
}

lv_obj_t* MakeBigButton(lv_obj_t* parent, const char* text, lv_event_cb_t cb) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, lv_pct(100), 96);
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn, 3, 0);
    lv_obj_set_style_radius(btn, 18, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    const lv_font_t* big_font = fontpack_lv_font_get(34, 2);
    lv_obj_set_style_text_font(lbl, big_font != nullptr ? big_font : UiFont(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

void OnScreenUnloaded(lv_event_t*) {
    s_ui = {};
}

}  // namespace

lv_obj_t* PowerScreen::Create() {
    s_ui = {};
    ScreenSetIsHome(false);

    // 基于标准 EpdPageScaffold 脚手架构建
    auto* scaffold = EpdWidgetKit::EpdPageScaffold::Create({
        .title = "电源管理",
        .show_status_bar = true,
        .show_back_btn = true,
        .pad_h = 16,
        .pad_v = 16,
        .row_gap = 14,
    });

    s_ui.screen = scaffold->GetScreen();
    lv_obj_t* body = scaffold->GetBody();
    lv_obj_add_event_cb(s_ui.screen, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);

    MakeBigButton(body, "息屏待机", OnSleepClicked);
    MakeBigButton(body, "重启系统", OnRebootClicked);
    MakeBigButton(body, "关机", OnShutdownClicked);

    s_ui.footer_lbl = ScreenMakeLabel(body, "息屏后按 BOOT/POWER 键唤醒；关机需长按电源键开机", 24, true);
    lv_obj_set_style_text_align(s_ui.footer_lbl, LV_TEXT_ALIGN_CENTER, 0);

    VkKey_AttachScreen(s_ui.screen, kScreenId, VkKeyScreenDesc{PowerScreen::Create, nullptr});
    return s_ui.screen;
}
