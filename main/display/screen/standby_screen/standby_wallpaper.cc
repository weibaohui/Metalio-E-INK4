#include "standby_screen/standby_wallpaper.h"

#include "services/wallpaper/wallpaper_active.h"
#include "reader/image_util.h"
#include "reader_core/reader_types.h"

#include <atomic>
#include <cstdio>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>
#include "assets/lang_config.h"
#include "utils/psr_worker.h"

namespace standby_wallpaper {
namespace {

constexpr const char* TAG = "StandbyWp";
constexpr int kDecodeMaxW = 480;
constexpr int kDecodeMaxH = 800;
constexpr uint32_t kLoadStack = 12 * 1024;

enum class LoadState : uint8_t { Idle = 0, Loading = 1, Ready = 2, Failed = 3 };

struct UiState {
    lv_obj_t* root = nullptr;
    lv_obj_t* img = nullptr;
};

UiState s_ui;
bool s_alive = false;
uint32_t s_epoch = 0;
reader::RasterImage* s_raster = nullptr;
std::atomic<bool> s_load_busy{false};
std::atomic<LoadState> s_load_state{LoadState::Idle};

struct LoadWork {
    uint32_t epoch = 0;
};

struct LoadResult {
    uint32_t epoch = 0;
    bool ok = false;
    char err[64] = {};
    reader::RasterImage* img = nullptr;
};

void MarkLoadDone(LoadState st) {
    s_load_state.store(st, std::memory_order_release);
}

void DetachImage() {
    if (s_ui.img != nullptr) {
        lv_image_set_src(s_ui.img, nullptr);
        lv_obj_add_flag(s_ui.img, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_raster != nullptr) {
        delete s_raster;
        s_raster = nullptr;
    }
}

void ApplyLoadAsync(void* p) {
    auto* msg = static_cast<LoadResult*>(p);
    s_load_busy.store(false, std::memory_order_release);
    if (msg == nullptr) {
        return;
    }
    if (!s_alive || msg->epoch != s_epoch) {
        delete msg->img;
        delete msg;
        return;
    }
    if (!msg->ok || msg->img == nullptr || msg->img->empty()) {
        ESP_LOGW(TAG, "load failed: %s", msg->err[0] != '\0' ? msg->err : "unknown");
        MarkLoadDone(LoadState::Failed);
        delete msg->img;
        delete msg;
        return;
    }

    DetachImage();
    s_raster = msg->img;
    msg->img = nullptr;
    if (s_ui.img != nullptr) {
        lv_image_set_src(s_ui.img, &s_raster->dsc);
        lv_obj_set_size(s_ui.img, s_raster->width, s_raster->height);
        lv_obj_center(s_ui.img);
        lv_obj_clear_flag(s_ui.img, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_ui.root != nullptr) {
        lv_obj_invalidate(s_ui.root);
        lv_refr_now(nullptr);
    }
    MarkLoadDone(LoadState::Ready);
    delete msg;
}

void LoadTask(void* arg) {
    auto* work = static_cast<LoadWork*>(arg);
    auto* msg = new LoadResult{};
    msg->epoch = work != nullptr ? work->epoch : 0;
    delete work;

    char path[192] = {};
    if (!wallpaper::TryResolveStandbyWallpaperPath(path, sizeof(path))) {
        std::snprintf(msg->err, sizeof(msg->err), "%s", "待机壁纸文件未找到");
    } else {
        auto* img = new reader::RasterImage();
        if (!reader::DecodeImageFileToL8(path, kDecodeMaxW, kDecodeMaxH, *img) || img->empty()) {
            std::snprintf(msg->err, sizeof(msg->err), "%s", "待机壁纸解码失败");
            delete img;
        } else {
            msg->ok = true;
            msg->img = img;
            ESP_LOGI(TAG, "decoded standby wallpaper %s (%ux%u)", path,
                     static_cast<unsigned>(img->width), static_cast<unsigned>(img->height));
        }
    }

    if (lv_async_call(ApplyLoadAsync, msg) != LV_RESULT_OK) {
        delete msg->img;
        delete msg;
        s_load_busy.store(false, std::memory_order_release);
        MarkLoadDone(LoadState::Failed);
    }
}

}  // namespace

void Build(lv_obj_t* root) {
    if (root == nullptr) {
        return;
    }
    ++s_epoch;
    s_alive = true;
    s_ui = UiState{};
    MarkLoadDone(LoadState::Idle);

    lv_obj_set_style_bg_color(root, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.root = root;
    s_ui.img = lv_image_create(root);
    lv_obj_add_flag(s_ui.img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_ui.img, LV_OBJ_FLAG_CLICKABLE);
}

void StartLoad() {
    if (!s_alive) {
        return;
    }
    if (s_load_busy.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    MarkLoadDone(LoadState::Loading);
    auto* work = new LoadWork{s_epoch};
    if (!SpawnPsrWorker("stb_wp_load", kLoadStack, 5, LoadTask, work, nullptr, 0)) {
        delete work;
        s_load_busy.store(false, std::memory_order_release);
        MarkLoadDone(LoadState::Failed);
        ESP_LOGW(TAG, "load task create failed");
    }
}

bool IsContentReady() {
    const LoadState st = s_load_state.load(std::memory_order_acquire);
    return st == LoadState::Ready || st == LoadState::Failed;
}

void Teardown() {
    ++s_epoch;
    s_alive = false;
    DetachImage();
    s_ui = UiState{};
    MarkLoadDone(LoadState::Idle);
}

}  // namespace standby_wallpaper
