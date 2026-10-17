// main/services/dynamic_apps/dynamic_app_shims.cc
// 动态应用 C ABI 垫片实现：转发到 C++ 固件设施（见 dynamic_app_shims.h）。
#include "dynamic_app_shims.h"

#include "display/lv_adapter_display.h"
#include "display/screen/screen_common.h"
#include "display/screen/vk_key_handler.h"

void* dynapp_screen_status_bar(void* scr) {
    if (scr == nullptr) {
        return nullptr;
    }
    return ScreenCreateStatusBar(static_cast<lv_obj_t*>(scr)).bar;
}

void dynapp_vk_attach(void* scr, const char* name,
                      void* (*factory)(void), int (*on_key)(const char* key)) {
    if (scr == nullptr || name == nullptr) {
        return;
    }
    VkKeyScreenDesc desc;
    desc.factory = reinterpret_cast<ScreenFactory>(factory);
    desc.on_key = reinterpret_cast<VkKeyHandler>(on_key);
    VkKey_AttachScreen(static_cast<lv_obj_t*>(scr), name, desc);
}

int dynapp_lv_async(void (*cb)(void*), void* user_data) {
    return ScreenLvAsync(cb, user_data) ? 1 : 0;
}

void dynapp_screen_go_home(void) {
    ScreenRequestHome();
}

void dynapp_epd_full_refresh(void) {
    if (auto* disp = LVAdapterDisplay::Instance()) {
        disp->RequestNextFullRefresh();
    }
}
