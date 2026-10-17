#include "services/wallpaper/wallpaper_active.h"

#include "boards/common/SdCardManager.hpp"
#include "boards/common/sd_paths.h"
#include "core/settings.h"
#include "display/screen/screen_common.h"
#include "reader/image_util.h"
#include "reader_core/reader_types.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <sys/stat.h>
#include <unistd.h>
#include "assets/lang_config.h"

namespace wallpaper {
namespace {

constexpr const char* TAG = "WpActive";
constexpr const char* kNvsNs = "wallpaper";
constexpr const char* kNvsKeyShutdown = "shutdown";
constexpr const char* kNvsKeyStandby = "standby";
constexpr size_t kNameMax = 95;
constexpr const char* kCachedShutdownA2i1 = SD_PATH_WALLPAPER "/.shutdown.a2i1";

enum class Slot : uint8_t { Shutdown = 0, Standby = 1, Count = 2 };

char s_names[static_cast<size_t>(Slot::Count)][kNameMax + 1] = {};
std::atomic<bool> s_cache_ready{false};
std::atomic<bool> s_hydrate_busy{false};

struct HydrateWork {
    HydrateDoneFn done = nullptr;
    void* user = nullptr;
};

const char* NvsKeyFor(Slot slot) {
    return slot == Slot::Standby ? kNvsKeyStandby : kNvsKeyShutdown;
}

const char* SlotTag(Slot slot) {
    return slot == Slot::Standby ? "standby" : "shutdown";
}

bool FileExistsRegular(const char* path) {
    struct stat st {};
    return path != nullptr && stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0;
}

std::string AbsPathForName(const std::string& name) {
    std::string path = SD_PATH_WALLPAPER;
    if (!path.empty() && path.back() != '/') {
        path.push_back('/');
    }
    path.append(name);
    return path;
}

bool IsSafeWallpaperBasename(const char* name) {
    if (name == nullptr || name[0] == '\0') {
        return false;
    }
    const size_t n = std::strlen(name);
    if (n > kNameMax) {
        return false;
    }
    if (std::strcmp(name, ".") == 0 || std::strcmp(name, "..") == 0) {
        return false;
    }
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (c < 0x20 || c == 0x7F) {
            return false;
        }
        if (c == '/' || c == '\\' || c == ':' || c == '\0') {
            return false;
        }
    }
    return true;
}

std::string BasenameOf(const char* path_or_name) {
    if (path_or_name == nullptr || path_or_name[0] == '\0') {
        return {};
    }
    const char* slash = std::strrchr(path_or_name, '/');
    const char* base = slash != nullptr ? slash + 1 : path_or_name;
    if (!IsSafeWallpaperBasename(base)) {
        return {};
    }
    return base;
}

void SetSlotCache(Slot slot, const char* name) {
    const size_t i = static_cast<size_t>(slot);
    if (name == nullptr || name[0] == '\0') {
        s_names[i][0] = '\0';
    } else {
        std::snprintf(s_names[i], sizeof(s_names[i]), "%s", name);
    }
}

void MarkCacheReady() {
    s_cache_ready.store(true, std::memory_order_release);
}

void PersistSlotToNvs(Slot slot, const char* name) {
    Settings settings(kNvsNs, true);
    const char* key = NvsKeyFor(slot);
    if (name == nullptr || name[0] == '\0') {
        settings.EraseKey(key);
    } else {
        settings.SetString(key, name);
    }
}

std::string ReadSlotNameFromNvs(Slot slot, bool* should_erase) {
    if (should_erase != nullptr) {
        *should_erase = false;
    }
    Settings settings(kNvsNs, false);
    const std::string name = settings.GetString(NvsKeyFor(slot), "");
    if (name.empty()) {
        return {};
    }
    if (!IsSafeWallpaperBasename(name.c_str())) {
        if (should_erase != nullptr) {
            *should_erase = true;
        }
        return {};
    }
    return name;
}

void LoadAllFromNvsIntoCache() {
    bool defer_ready = false;
    for (uint8_t s = 0; s < static_cast<uint8_t>(Slot::Count); ++s) {
        const Slot slot = static_cast<Slot>(s);
        bool erase = false;
        const std::string name = ReadSlotNameFromNvs(slot, &erase);
        if (erase) {
            PersistSlotToNvs(slot, nullptr);
            SetSlotCache(slot, "");
            continue;
        }
        if (name.empty()) {
            SetSlotCache(slot, "");
            continue;
        }
        const std::string path = AbsPathForName(name);
        if (FileExistsRegular(path.c_str())) {
            SetSlotCache(slot, name.c_str());
            ESP_LOGI(TAG, "hydrate %s=%s", SlotTag(slot), name.c_str());
            continue;
        }
        if (SdCardManager::GetInstance().IsMounted()) {
            ESP_LOGW(TAG, "hydrate %s NVS=%s missing on SD -> clear", SlotTag(slot), name.c_str());
            PersistSlotToNvs(slot, nullptr);
            SetSlotCache(slot, "");
        } else {
            SetSlotCache(slot, "");
            defer_ready = true;
            ESP_LOGW(TAG, "hydrate SD unmounted, keep NVS %s=%s", SlotTag(slot), name.c_str());
        }
    }
    if (!defer_ready) {
        MarkCacheReady();
    }
}

void InvokeDoneAsync(HydrateDoneFn done, void* user) {
    if (done == nullptr) {
        return;
    }
    auto* cb = new HydrateWork{done, user};
    if (!ScreenLvAsync(
            [](void* p) {
                auto* c = static_cast<HydrateWork*>(p);
                if (c->done != nullptr) {
                    c->done(c->user);
                }
                delete c;
            },
            cb)) {
        delete cb;
    }
}

void HydrateTask(void* arg) {
    auto* work = static_cast<HydrateWork*>(arg);
    LoadAllFromNvsIntoCache();
    const HydrateDoneFn done = work != nullptr ? work->done : nullptr;
    void* user = work != nullptr ? work->user : nullptr;
    delete work;
    s_hydrate_busy.store(false, std::memory_order_release);
    InvokeDoneAsync(done, user);
    vTaskDelete(nullptr);
}

void WaitThenDoneTask(void* arg) {
    auto* work = static_cast<HydrateWork*>(arg);
    for (int i = 0; i < 200 && !s_cache_ready.load(std::memory_order_acquire); ++i) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    const HydrateDoneFn done = work != nullptr ? work->done : nullptr;
    void* user = work != nullptr ? work->user : nullptr;
    delete work;
    InvokeDoneAsync(done, user);
    vTaskDelete(nullptr);
}

std::string GetSlotFilename(Slot slot) {
    if (!s_cache_ready.load(std::memory_order_acquire)) {
        return {};
    }
    const char* name = s_names[static_cast<size_t>(slot)];
    if (name[0] == '\0') {
        return {};
    }
    const std::string path = AbsPathForName(name);
    if (!FileExistsRegular(path.c_str())) {
        return {};
    }
    return name;
}

bool EnsureShutdownA2i1Cached(const std::string& src_path) {
    // 检查原图是否已是 A2I1
    FILE* fp = fopen(src_path.c_str(), "rb");
    if (fp != nullptr) {
        uint8_t magic[4] = {};
        if (fread(magic, 1, 4, fp) == 4 && magic[0] == 'A' && magic[1] == '2' && magic[2] == 'I' && magic[3] == '1') {
            fclose(fp);
            return true; // 自身已经是 A2I1，直接使用
        }
        fclose(fp);
    }

    // 非 A2I1 图（PNG/JPG/BMP 等），解码并转码为 .shutdown.a2i1
    reader::RasterImage img;
    if (!reader::DecodeImageFileToL8(src_path.c_str(), 480, 800, img) || img.empty()) {
        ESP_LOGE(TAG, "failed to decode %s for shutdown A2I1", src_path.c_str());
        return false;
    }
    std::vector<uint8_t> a2i1;
    if (!reader::EncodeL8ToA2i1(img, a2i1) || a2i1.empty()) {
        ESP_LOGE(TAG, "failed to encode A2I1 for shutdown");
        return false;
    }
    if (!reader::WriteFileAtomic(kCachedShutdownA2i1, a2i1.data(), a2i1.size())) {
        ESP_LOGE(TAG, "failed to write cached shutdown A2I1 to %s", kCachedShutdownA2i1);
        return false;
    }
    ESP_LOGI(TAG, "generated cached shutdown A2I1 (%u bytes) from %s", static_cast<unsigned>(a2i1.size()), src_path.c_str());
    return true;
}

bool SetSlotFromFile(Slot slot, const char* path_or_name, std::string& err_out) {
    err_out.clear();
    const std::string name = BasenameOf(path_or_name);
    if (name.empty()) {
        err_out = Lang::Strings::WALLPAPER_NAME_INVALID;
        return false;
    }

    const std::string src = AbsPathForName(name);
    const std::string prefix = std::string(SD_PATH_WALLPAPER) + "/";
    if (src.rfind(prefix, 0) != 0) {
        err_out = Lang::Strings::WALLPAPER_PATH_INVALID;
        return false;
    }
    if (!FileExistsRegular(src.c_str())) {
        err_out = Lang::Strings::WALLPAPER_FILE_MISSING;
        return false;
    }

    if (slot == Slot::Shutdown) {
        if (!EnsureShutdownA2i1Cached(src)) {
            err_out = Lang::Strings::WALLPAPER_ENABLE_FAIL;
            return false;
        }
    }

    if (!s_cache_ready.load(std::memory_order_acquire)) {
        LoadAllFromNvsIntoCache();
    }
    SetSlotCache(slot, name.c_str());
    MarkCacheReady();
    PersistSlotToNvs(slot, name.c_str());
    ESP_LOGI(TAG, "%s wallpaper -> %s", SlotTag(slot), name.c_str());
    return true;
}

void ClearSlot(Slot slot) {
    if (!s_cache_ready.load(std::memory_order_acquire)) {
        LoadAllFromNvsIntoCache();
    }
    SetSlotCache(slot, "");
    MarkCacheReady();
    PersistSlotToNvs(slot, nullptr);
    if (slot == Slot::Shutdown) {
        unlink(kCachedShutdownA2i1);
    }
    ESP_LOGI(TAG, "cleared %s wallpaper", SlotTag(slot));
}

void ClearSlotIfMatches(Slot slot, const char* filename, bool* any_cleared) {
    if (filename == nullptr || filename[0] == '\0') {
        return;
    }

    bool match = false;
    if (s_cache_ready.load(std::memory_order_acquire)) {
        match = (std::strcmp(s_names[static_cast<size_t>(slot)], filename) == 0);
    }
    {
        Settings settings(kNvsNs, false);
        if (settings.GetString(NvsKeyFor(slot), "") == filename) {
            match = true;
        }
    }
    if (!match) {
        return;
    }

    SetSlotCache(slot, "");
    PersistSlotToNvs(slot, nullptr);
    if (slot == Slot::Shutdown) {
        unlink(kCachedShutdownA2i1);
    }
    ESP_LOGI(TAG, "cleared %s wallpaper (%s)", SlotTag(slot), filename);
    if (any_cleared != nullptr) {
        *any_cleared = true;
    }
}

bool TryResolveSlotPath(Slot slot, char* out_path, size_t out_len) {
    if (out_path == nullptr || out_len < 8) {
        return false;
    }
    out_path[0] = '\0';

    std::string name;
    const char* cached = s_names[static_cast<size_t>(slot)];
    if (s_cache_ready.load(std::memory_order_acquire) && cached[0] != '\0' &&
        IsSafeWallpaperBasename(cached)) {
        name = cached;
    } else {
        bool erase = false;
        name = ReadSlotNameFromNvs(slot, &erase);
        (void)erase;
    }
    if (name.empty() || !IsSafeWallpaperBasename(name.c_str())) {
        return false;
    }

    // 关机壁纸：优先检查是否存在转码后的 .shutdown.a2i1 或自身为 a2i1
    if (slot == Slot::Shutdown) {
        if (FileExistsRegular(kCachedShutdownA2i1)) {
            std::snprintf(out_path, out_len, "%s", kCachedShutdownA2i1);
            return true;
        }
    }

    const std::string path = AbsPathForName(name);
    if (path.size() + 1 > out_len) {
        ESP_LOGW(TAG, "%s path too long", SlotTag(slot));
        return false;
    }
    if (!FileExistsRegular(path.c_str())) {
        ESP_LOGW(TAG, "%s path missing: %s", SlotTag(slot), path.c_str());
        return false;
    }
    std::snprintf(out_path, out_len, "%s", path.c_str());
    return true;
}

}  // namespace

std::string GetActiveFilename() {
    return GetSlotFilename(Slot::Shutdown);
}

std::string GetStandbyFilename() {
    return GetSlotFilename(Slot::Standby);
}

bool IsActiveFilename(const char* filename) {
    if (filename == nullptr || filename[0] == '\0') {
        return false;
    }
    const std::string active = GetActiveFilename();
    return !active.empty() && active == filename;
}

bool IsStandbyFilename(const char* filename) {
    if (filename == nullptr || filename[0] == '\0') {
        return false;
    }
    const std::string active = GetStandbyFilename();
    return !active.empty() && active == filename;
}

bool SetActiveFromFile(const char* path_or_name, std::string& err_out) {
    return SetSlotFromFile(Slot::Shutdown, path_or_name, err_out);
}

bool SetStandbyFromFile(const char* path_or_name, std::string& err_out) {
    return SetSlotFromFile(Slot::Standby, path_or_name, err_out);
}

void ClearShutdownWallpaper() {
    ClearSlot(Slot::Shutdown);
}

void ClearStandbyWallpaper() {
    ClearSlot(Slot::Standby);
}

void ClearActiveIfMatches(const char* filename) {
    const bool was_ready = s_cache_ready.load(std::memory_order_acquire);
    bool any = false;
    ClearSlotIfMatches(Slot::Shutdown, filename, &any);
    ClearSlotIfMatches(Slot::Standby, filename, &any);
    if (any && was_ready) {
        MarkCacheReady();
    }
}

bool TryResolveActiveWallpaperPath(char* out_path, size_t out_len) {
    return TryResolveSlotPath(Slot::Shutdown, out_path, out_len);
}

bool TryResolveStandbyWallpaperPath(char* out_path, size_t out_len) {
    return TryResolveSlotPath(Slot::Standby, out_path, out_len);
}

void RequestHydrateFromNvs(HydrateDoneFn done, void* user) {
    if (s_cache_ready.load(std::memory_order_acquire)) {
        InvokeDoneAsync(done, user);
        return;
    }

    auto* work = new HydrateWork{done, user};
    if (s_hydrate_busy.exchange(true, std::memory_order_acq_rel)) {
        if (xTaskCreate(WaitThenDoneTask, "wp_hyd_wait", 3072, work, 5, nullptr) != pdPASS) {
            delete work;
            InvokeDoneAsync(done, user);
        }
        return;
    }

    if (xTaskCreate(HydrateTask, "wp_hydrate", 4096, work, 5, nullptr) != pdPASS) {
        delete work;
        s_hydrate_busy.store(false, std::memory_order_release);
        ESP_LOGE(TAG, "hydrate task create failed");
        InvokeDoneAsync(done, user);
    }
}

void HydrateFromNvsNow() {
    if (s_cache_ready.load(std::memory_order_acquire)) {
        return;
    }
    if (s_hydrate_busy.exchange(true, std::memory_order_acq_rel)) {
        for (int i = 0; i < 200 && !s_cache_ready.load(std::memory_order_acquire); ++i) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        return;
    }
    LoadAllFromNvsIntoCache();
    s_hydrate_busy.store(false, std::memory_order_release);
}

}  // namespace wallpaper
