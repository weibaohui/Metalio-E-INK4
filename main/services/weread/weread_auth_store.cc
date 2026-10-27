#include "weread_auth_store.h"

#include <nvs.h>
#include <esp_log.h>
#include <esp_memory_utils.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

namespace {
static const char* kTag = "WereadAuthStore";
constexpr char kNamespace[] = "weread_auth";

bool ReadString(nvs_handle_t handle, const char* key, std::string* out) {
    size_t length = 0;
    if (nvs_get_str(handle, key, nullptr, &length) != ESP_OK || length == 0) return false;
    std::string value(length - 1, '\0');
    if (nvs_get_str(handle, key, value.data(), &length) != ESP_OK) return false;
    *out = std::move(value);
    return true;
}

template <typename F>
void RunInDramTaskIfNeeded(F&& fn) {
    int dummy = 0;
    if (esp_ptr_in_dram(&dummy)) {
        fn();
        return;
    }
    // Task stack is in PSRAM (e.g. LVGL worker). NVS flash cache requires DRAM stack.
    SemaphoreHandle_t sem = xSemaphoreCreateBinary();
    struct TaskArg {
        F* fn;
        SemaphoreHandle_t sem;
    } arg = { &fn, sem };

    xTaskCreate([](void* p) {
        auto* a = static_cast<TaskArg*>(p);
        (*a->fn)();
        xSemaphoreGive(a->sem);
        vTaskDelete(nullptr);
    }, "weread_nvs", 3072, &arg, tskIDLE_PRIORITY + 3, nullptr);

    xSemaphoreTake(sem, portMAX_DELAY);
    vSemaphoreDelete(sem);
}

} // namespace

bool WereadAuthStore::Load(WereadAuth* auth) const {
    if (auth == nullptr) return false;
    bool success = false;
    RunInDramTaskIfNeeded([&]() {
        nvs_handle_t handle = 0;
        if (nvs_open(kNamespace, NVS_READONLY, &handle) != ESP_OK) return;
        WereadAuth loaded;
        ReadString(handle, "vid", &loaded.vid);
        ReadString(handle, "skey", &loaded.skey);
        ReadString(handle, "gid", &loaded.gid);
        ReadString(handle, "rt", &loaded.refresh_token);
        nvs_close(handle);
        *auth = std::move(loaded);
        success = auth->IsLoggedIn();
    });
    ESP_LOGI(kTag, "Loaded auth: vid=%s, logged_in=%d", auth->vid.c_str(), auth->IsLoggedIn());
    return success;
}

bool WereadAuthStore::Save(const WereadAuth& auth) const {
    bool ok = false;
    RunInDramTaskIfNeeded([&]() {
        nvs_handle_t handle = 0;
        if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return;
        esp_err_t result = nvs_set_str(handle, "vid", auth.vid.c_str());
        if (result == ESP_OK) result = nvs_set_str(handle, "skey", auth.skey.c_str());
        if (result == ESP_OK) result = nvs_set_str(handle, "gid", auth.gid.c_str());
        if (result == ESP_OK) result = nvs_set_str(handle, "rt", auth.refresh_token.c_str());
        if (result == ESP_OK) result = nvs_commit(handle);
        nvs_close(handle);
        ok = (result == ESP_OK);
    });
    ESP_LOGI(kTag, "Saved auth: vid=%s, ok=%d", auth.vid.c_str(), ok);
    return ok;
}

bool WereadAuthStore::Clear() const {
    bool ok = false;
    RunInDramTaskIfNeeded([&]() {
        nvs_handle_t handle = 0;
        if (nvs_open(kNamespace, NVS_READWRITE, &handle) != ESP_OK) return;
        esp_err_t result = nvs_erase_all(handle);
        if (result == ESP_OK) result = nvs_commit(handle);
        nvs_close(handle);
        ok = (result == ESP_OK);
    });
    ESP_LOGI(kTag, "Cleared auth, ok=%d", ok);
    return ok;
}
