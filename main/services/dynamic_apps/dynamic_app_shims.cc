// main/services/dynamic_apps/dynamic_app_shims.cc
// 动态应用 C ABI 垫片实现：转发到 C++ 固件设施（见 dynamic_app_shims.h）。
#include "dynamic_app_shims.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <esp_log.h>

#include "display/lv_adapter_display.h"
#include "display/screen/screen_common.h"
#include "display/screen/vk_key_handler.h"
#include "sd_paths.h"

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

namespace {
std::string s_active_bundle_path;
std::string s_active_data_path;
constexpr const char* kShimTag = "DynAppShim";
}

void dynapp_set_active_bundle(const char* bundle_path) {
    if (bundle_path != nullptr) {
        s_active_bundle_path = bundle_path;
        s_active_data_path = s_active_bundle_path + "/data";
    } else {
        s_active_bundle_path.clear();
        s_active_data_path.clear();
    }
}

void dynapp_clear_active_bundle(void) {
    s_active_bundle_path.clear();
    s_active_data_path.clear();
}

const char* dynapp_get_bundle_path(void) {
    return s_active_bundle_path.c_str();
}

const char* dynapp_get_data_path(void) {
    if (s_active_data_path.empty()) {
        return "";
    }
    // 首次获取或确保 data 目录存在（0755）
    SdEnsureDir(s_active_data_path.c_str());
    return s_active_data_path.c_str();
}

void* dynapp_fopen_data(const char* rel_filename, const char* mode) {
    if (rel_filename == nullptr || rel_filename[0] == '\0' || rel_filename[0] == '/') {
        ESP_LOGE(kShimTag, "fopen_data: invalid relative filename");
        return nullptr;
    }
    if (strstr(rel_filename, "..") != nullptr || strchr(rel_filename, '\\') != nullptr) {
        ESP_LOGE(kShimTag, "fopen_data: path traversal attempt rejected: %s", rel_filename);
        return nullptr;
    }
    const char* data_dir = dynapp_get_data_path();
    if (data_dir == nullptr || data_dir[0] == '\0') {
        ESP_LOGE(kShimTag, "fopen_data: no active app data path");
        return nullptr;
    }
    std::string full_path = std::string(data_dir) + "/" + rel_filename;
    FILE* f = fopen(full_path.c_str(), mode);
    if (f == nullptr) {
        ESP_LOGD(kShimTag, "fopen_data %s (%s) failed", full_path.c_str(), mode ? mode : "");
    }
    return f;
}

int dynapp_is_path_allowed(const char* path, int write) {
    if (path == nullptr || path[0] == '\0') {
        return 0;
    }
    if (strstr(path, "..") != nullptr || strchr(path, '\\') != nullptr) {
        return 0;
    }
    const char* data_dir = dynapp_get_data_path();
    if (write) {
        // 写操作严格限制在自身 data/ 目录下
        if (data_dir != nullptr && data_dir[0] != '\0') {
            const size_t len = strlen(data_dir);
            if (strncmp(path, data_dir, len) == 0 && (path[len] == '/' || path[len] == '\0')) {
                return 1;
            }
        }
        ESP_LOGW(kShimTag, "sandbox: write access denied outside data dir: %s", path);
        return 0;
    }
    // 读操作允许读取自身 data/、自身 bundle 资源、系统公共字体及壁纸
    if (data_dir != nullptr && data_dir[0] != '\0') {
        const size_t len = strlen(data_dir);
        if (strncmp(path, data_dir, len) == 0 && (path[len] == '/' || path[len] == '\0')) {
            return 1;
        }
    }
    const char* bundle_dir = dynapp_get_bundle_path();
    if (bundle_dir != nullptr && bundle_dir[0] != '\0') {
        const size_t len = strlen(bundle_dir);
        if (strncmp(path, bundle_dir, len) == 0 && (path[len] == '/' || path[len] == '\0')) {
            return 1;
        }
    }
    if (strncmp(path, "/spiffs", 7) == 0) return 1;
    if (strncmp(path, "/sdcard/metalio/e-ink/fonts", 27) == 0) return 1;
    if (strncmp(path, "/sdcard/metalio/e-ink/wallpaper", 31) == 0) return 1;
    return 0;
}
