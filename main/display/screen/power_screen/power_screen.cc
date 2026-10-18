// main/display/screen/power_screen/power_screen.cc
// 内置系统应用：电源管理
#include "power_screen/power_screen.h"

#include <cstdio>
#include <cstring>

#include "application.h"
#include "boards/common/power_policy/power_policy.h"
#include "core/haptic_feedback.h"
#include "display/modal_manager.h"
#include "display/status_bar_manager.h"
#include "display/screen/boot_key_handler.h"
#include "display/screen/screen_common.h"
#include "display/screen/vk_key_handler.h"
#include "fontpack_lvgl.h"

namespace {

constexpr const char* kScreenId = "power";

lv_obj_t* s_scr = nullptr;
lv_obj_t* s_footer_lbl = nullptr;

void OnSleepClicked(lv_event_t* e) {
    (void)e;
    PowerKey_OnClick();
}

void OnRebootConfirmed(bool confirmed) {
    if (confirmed) {
        if (s_footer_lbl != nullptr) {
            lv_label_set_text(s_footer_lbl, "正在重启…");
        }
        StatusBarManager::GetInstance().ShowToast("正在重启…", 2000);
        xTaskCreate([](void*) {
            vTaskDelay(pdMS_TO_TICKS(500));
            Application::GetInstance().Reboot();
            vTaskDelete(nullptr);
        }, "reboot_task", 2048, nullptr, 5, nullptr);
    }
}

void OnRebootClicked(lv_event_t* e) {
    (void)e;
    ModalManager::GetInstance().ShowDialog(
        "重启设备",
        "确定要重启设备吗？未保存的数据可能会丢失。",
        "确定重启",
        "取消",
        OnRebootConfirmed);
}

void OnShutdownConfirmed(bool confirmed) {
    if (confirmed) {
        if (s_footer_lbl != nullptr) {
            lv_label_set_text(s_footer_lbl, "正在关机…");
        }
        StatusBarManager::GetInstance().ShowToast("正在关机…", 2000);
        xTaskCreate([](void*) {
            vTaskDelay(pdMS_TO_TICKS(500));
            PowerPolicy::GetInstance().RequestPowerOff();
            vTaskDelete(nullptr);
        }, "shutdown_task", 2048, nullptr, 5, nullptr);
    }
}

void OnShutdownClicked(lv_event_t* e) {
    (void)e;
    ModalManager::GetInstance().ShowDialog(
        "关闭设备",
        "确定要关机吗？关机后需长按电源键开机。",
        "确定关机",
        "取消",
        OnShutdownConfirmed);
}

bool PowerOnVkKey(const char* key) {
    if (key == nullptr) return false;
    if (strcmp(key, "home") == 0 || strcmp(key, "vk_home") == 0 || strcmp(key, "prev") == 0 ||
        strcmp(key, "vk_prev") == 0 || strcmp(key, "esc") == 0 || strcmp(key, "back") == 0) {
        ScreenNavigateBack();
        return true;
    }
    return false;
}

void PowerScreenDeleteCb(lv_event_t* e) {
    (void)e;
    s_scr = nullptr;
    s_footer_lbl = nullptr;
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
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    HapticAttachClick(btn);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    const lv_font_t* big_font = fontpack_lv_font_get(34, 2);
    if (big_font == nullptr) big_font = fontpack_lv_font_ui();
    if (big_font != nullptr) lv_obj_set_style_text_font(lbl, big_font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

}  // namespace

lv_obj_t* PowerScreen::Create() {
    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 800);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_scr = scr;
    lv_obj_add_event_cb(scr, PowerScreenDeleteCb, LV_EVENT_DELETE, nullptr);

    // 1. 系统顶部状态栏
    ScreenCreateStatusBar(scr);

    // 2. 虚拟键与返回栈
    VkKeyScreenDesc desc = {};
    desc.factory = PowerScreen::Create;
    desc.on_key = PowerOnVkKey;
    VkKey_AttachScreen(scr, kScreenId, desc);

    // 3. 内容容器 (无顶栏，直连状态栏，全屏空间更充裕)
    lv_obj_t* body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, 448, 720);
    lv_obj_set_pos(body, 16, 52);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(body, 28, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    MakeBigButton(body, "息屏待机", OnSleepClicked);
    MakeBigButton(body, "重启系统", OnRebootClicked);
    MakeBigButton(body, "关机", OnShutdownClicked);

    // 5. 提示说明
    s_footer_lbl = lv_label_create(body);
    lv_label_set_text(s_footer_lbl, "息屏后按 BOOT/POWER 键唤醒\n关机需长按电源键开机");
    const lv_font_t* f20 = fontpack_lv_font_get(20, 0);
    if (f20 == nullptr) f20 = fontpack_lv_font_ui();
    if (f20 != nullptr) lv_obj_set_style_text_font(s_footer_lbl, f20, 0);
    lv_obj_set_style_text_color(s_footer_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_style_text_align(s_footer_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(s_footer_lbl, 420);
    lv_obj_set_style_pad_top(s_footer_lbl, 24, 0);
    lv_obj_clear_flag(s_footer_lbl, LV_OBJ_FLAG_CLICKABLE);

    return scr;
}
