// main/display/screen/power_screen/power_screen.cc —— 见 .h(电源管理 APP)。
#include "power_screen/power_screen.h"

#include <esp_timer.h>
#include <esp_log.h>

#include "application.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "power_policy.h"
#include "screen_common.h"
#include "boot_key_handler.h"
#include "standby_screen/standby_screen.h"
#include "vk_key_handler.h"

namespace {

constexpr const char* TAG = "PowerScreen";
constexpr const char* kScreenId = "power";
constexpr int64_t kConfirmWindowUs = 3000000;  // 二次确认有效窗口 3s

struct PowerUi {
    lv_obj_t* screen = nullptr;
    lv_obj_t* reboot_lbl = nullptr;
    lv_obj_t* shutdown_lbl = nullptr;
    lv_obj_t* footer_lbl = nullptr;
};
PowerUi s_ui;
bool s_alive = false;

// 二次确认:0=无,1=重启,2=关机
int s_armed = 0;
int64_t s_armed_at = 0;

const lv_font_t* UiFont() { return fontpack_lv_font_ui(); }
void DisableScroll(lv_obj_t* o) { lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE); }

lv_obj_t* MakeLabel(lv_obj_t* parent, const char* text, int font_size = 25) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text != nullptr ? text : "");
    const lv_font_t* font = font_size > 25 ? fontpack_lv_font_get(font_size, 2) : UiFont();
    lv_obj_set_style_text_font(lbl, font != nullptr ? font : UiFont(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, lv_pct(100));
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return lbl;
}

void SetFooter(const char* text) {
    if (s_ui.footer_lbl != nullptr) lv_label_set_text(s_ui.footer_lbl, text);
}

void Disarm() {
    s_armed = 0;
    if (s_ui.reboot_lbl != nullptr) lv_label_set_text(s_ui.reboot_lbl, "重启");
    if (s_ui.shutdown_lbl != nullptr) lv_label_set_text(s_ui.shutdown_lbl, "关机");
}

// 息屏:进入待机(EPD 休眠 + 轻睡眠 LP;BOOT/POWER 键唤醒)。
// ⚠️ 必须走 PowerKey_OnClick(= PreparePowerKeyStandby + LvAsyncUrgent 排队进
// StandbyScreen::Show)。不能在点击回调里同步调 Show():待机转换会在 LVGL 事件
// 分发中途进入轻睡眠,LVGL worker 被冻在渲染半途,唤醒链路失效 → 整机假死
// (2026-10-09 实测:进电源 APP 点息屏,系统卡死只能 BOOT+USB 硬复位)。
void OnSleepClicked(lv_event_t*) {
    HapticPulseIfEnabled();
    ESP_LOGI(TAG, "息屏(进入待机)");
    PowerKey_OnClick();
}

// 重启:二次确认 → 走 main 任务 Application::Reboot(与 MCP self.reboot 同路径)
void OnRebootClicked(lv_event_t*) {
    HapticPulseIfEnabled();
    const int64_t now = esp_timer_get_time();
    if (s_armed == 1 && now - s_armed_at < kConfirmWindowUs) {
        SetFooter("正在重启…");
        Disarm();
        Application::GetInstance().Schedule([]() {
            ESP_LOGW(TAG, "电源页确认重启");
            vTaskDelay(pdMS_TO_TICKS(300));
            Application::GetInstance().Reboot();
        });
        return;
    }
    s_armed = 1;
    s_armed_at = now;
    if (s_ui.reboot_lbl != nullptr) lv_label_set_text(s_ui.reboot_lbl, "再点确认");
    SetFooter("3 秒内再点一次「重启」确认");
}

// 关机:二次确认 → PowerPolicy::RequestPowerOff(与 POWER 长按同路径,走硬关机)
void OnShutdownClicked(lv_event_t*) {
    HapticPulseIfEnabled();
    const int64_t now = esp_timer_get_time();
    if (s_armed == 2 && now - s_armed_at < kConfirmWindowUs) {
        SetFooter("正在关机…");
        Disarm();
        PowerPolicy::GetInstance().RequestPowerOff();
        return;
    }
    s_armed = 2;
    s_armed_at = now;
    if (s_ui.shutdown_lbl != nullptr) lv_label_set_text(s_ui.shutdown_lbl, "再点确认");
    SetFooter("3 秒内再点一次「关机」确认");
}

lv_obj_t* MakeBigButton(lv_obj_t* parent, const char* text, lv_event_cb_t cb, lv_obj_t** lbl_out) {
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
    lv_obj_set_style_text_font(lbl, fontpack_lv_font_get(34, 2), 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    if (lbl_out != nullptr) *lbl_out = lbl;
    return btn;
}

void OnScreenUnloaded(lv_event_t*) {
    s_alive = false;
    s_armed = 0;
    s_ui = {};
}

}  // namespace

lv_obj_t* PowerScreen::Create() {
    s_ui = {};
    s_alive = true;
    s_armed = 0;
    ScreenSetIsHome(false);

    lv_obj_t* screen = lv_obj_create(nullptr);
    s_ui.screen = screen;
    lv_obj_remove_style_all(screen);
    lv_obj_set_size(screen, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);
    lv_obj_set_style_text_font(screen, UiFont(), 0);
    DisableScroll(screen);
    lv_obj_add_event_cb(screen, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);

    const EpdStatusBar status = ScreenCreateStatusBar(screen);
    lv_obj_t* body = lv_obj_create(screen);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, LV_VER_RES - status.height);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, status.height);
    lv_obj_set_style_pad_all(body, 16, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 14, 0);
    DisableScroll(body);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);

    MakeLabel(body, "电源管理", 30);
    MakeBigButton(body, "息屏", OnSleepClicked, nullptr);
    MakeBigButton(body, "重启", OnRebootClicked, &s_ui.reboot_lbl);
    MakeBigButton(body, "关机", OnShutdownClicked, &s_ui.shutdown_lbl);

    s_ui.footer_lbl = MakeLabel(body, "息屏后按 BOOT/POWER 键唤醒;重启/关机需二次确认");
    lv_obj_set_style_text_align(s_ui.footer_lbl, LV_TEXT_ALIGN_CENTER, 0);

    VkKey_AttachScreen(screen, kScreenId, VkKeyScreenDesc{PowerScreen::Create, nullptr});
    return screen;
}
