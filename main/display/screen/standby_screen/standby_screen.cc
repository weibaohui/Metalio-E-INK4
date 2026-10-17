#include "standby_screen/standby_screen.h"

#include "standby_screen/standby_classic.h"
#include "standby_screen/standby_wallpaper.h"
#include "services/wallpaper/wallpaper_active.h"

#include "application.h"
#include "core/app_manager.h"
#include "lv_adapter_display.h"
#include "power_policy.h"
#include "vk_key_handler.h"

#include <esp_log.h>
#include <lvgl.h>

namespace {

constexpr const char* TAG = "StandbyScreen";
constexpr const char* kScreenId = "standby";

enum class Variant : uint8_t { Classic, Wallpaper };
Variant s_variant = Variant::Classic;

bool s_host_alive = false;
bool s_as_overlay = false;
lv_obj_t* s_overlay_root = nullptr;
lv_obj_t* s_underlying_scr = nullptr;

bool PreferWallpaperVariant() {
    char path[192] = {};
    return wallpaper::TryResolveStandbyWallpaperPath(path, sizeof(path));
}

void ClearStatusBindings() {
    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->BindStatusWidgets(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
    }
}

void TeardownVariant() {
    if (s_variant == Variant::Wallpaper) {
        standby_wallpaper::Teardown();
    } else {
        standby_classic::Teardown();
    }
}

void MountVariant(lv_obj_t* root, bool as_overlay) {
    if (PreferWallpaperVariant()) {
        s_variant = Variant::Wallpaper;
        ESP_LOGI(TAG, "variant=wallpaper");
        standby_wallpaper::Build(root);
        standby_wallpaper::StartLoad();
    } else {
        s_variant = Variant::Classic;
        ESP_LOGI(TAG, "variant=classic");
        standby_classic::Build(root, as_overlay);
        standby_classic::StartRuntime();
    }
}

void TeardownHostState() {
    TeardownVariant();
    s_host_alive = false;
    s_as_overlay = false;
    s_overlay_root = nullptr;
    s_underlying_scr = nullptr;
}

void OnOverlayDeleted(lv_event_t* /*e*/) {
    if (!s_host_alive) {
        return;
    }
    TeardownHostState();
}

void OnScreenUnloaded(lv_event_t* /*e*/) {
    if (s_as_overlay) {
        return;
    }
    TeardownHostState();
}

bool StandbyOnVkKey(const char* /*key_name*/) {
    return true;
}

bool OnBootClick() {
    ESP_LOGI(TAG, "boot short -> ignore (stay standby)");
    return true;
}

bool OnBootLongPress() {
    ESP_LOGI(TAG, "boot long -> ignore");
    return true;
}

}  // namespace

lv_obj_t* StandbyScreen::Create() {
    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    ClearStatusBindings();
    s_as_overlay = false;
    s_host_alive = true;
    MountVariant(scr, false);
    lv_obj_add_event_cb(scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);
    VkKey_AttachScreen(scr, kScreenId,
                       VkKeyScreenDesc{StandbyScreen::Create, StandbyOnVkKey, OnBootClick,
                                       OnBootLongPress});
    return scr;
}

void StandbyScreen::Show() {
    if (s_host_alive) {
        return;
    }
    lv_obj_t* under = lv_screen_active();
    if (under == nullptr) {
        ESP_LOGW(TAG, "show: no active screen");
        return;
    }
    ESP_LOGI(TAG, "show overlay on screen=%p", static_cast<void*>(under));

    lv_obj_t* overlay = lv_obj_create(under);
    lv_obj_remove_style_all(overlay);
    lv_obj_set_size(overlay, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(overlay, 0, 0);
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);

    s_underlying_scr = under;
    s_overlay_root = overlay;
    s_as_overlay = true;
    s_host_alive = true;

    MountVariant(overlay, true);
    lv_obj_move_foreground(overlay);
    lv_obj_add_event_cb(overlay, OnOverlayDeleted, LV_EVENT_DELETE, nullptr);

    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->BeginStandbyEnterPaint();
    }

    if (auto app = AppManager::GetInstance().GetActiveApp()) {
        app->OnPause();
    }
    PowerPolicy::GetInstance().RequestReevaluate();
}

void StandbyScreen::Dismiss() {
    if (!s_host_alive) {
        return;
    }
    ExitEpdSleep();
    ESP_LOGI(TAG, "dismiss overlay (under=%p)", static_cast<void*>(s_underlying_scr));

    if (s_variant == Variant::Classic) {
        standby_classic::StopRuntime();
    }

    lv_obj_t* overlay = s_overlay_root;
    s_overlay_root = nullptr;
    s_host_alive = false;

    if (overlay != nullptr) {
        lv_obj_remove_event_cb(overlay, OnOverlayDeleted);
        TeardownVariant();
        lv_obj_delete(overlay);
    } else {
        TeardownVariant();
    }

    s_as_overlay = false;
    s_underlying_scr = nullptr;

    PowerPolicy::GetInstance().OnStandbyOverlayDismissed();
    if (auto app = AppManager::GetInstance().GetActiveApp()) {
        app->OnResume();
    }
}

bool StandbyScreen::IsActive() {
    return s_host_alive;
}

bool StandbyScreen::IsPaintReady() {
    if (!s_host_alive) {
        return false;
    }
    if (s_variant == Variant::Wallpaper) {
        return standby_wallpaper::IsContentReady();
    }
    return true;
}

void StandbyScreen::EnterEpdSleep()
{
    if (!s_host_alive) {
        return;
    }
    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->ParkEpdForStandby();
    }
}

void StandbyScreen::ExitEpdSleep()
{
    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->WakeEpdFromStandby();
    }
}

bool StandbyScreen::HandleBootClick() {
    if (!s_host_alive) {
        return false;
    }
    ESP_LOGI(TAG, "boot short -> ignore (stay standby)");
    return true;
}

bool StandbyScreen::HandleBootLongPress() {
    if (!s_host_alive) {
        return false;
    }
    ESP_LOGI(TAG, "boot long -> ignore");
    return true;
}

void StandbyScreen::EnsureWeatherCached() {
}
