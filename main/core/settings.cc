#include "settings.h"

#include <esp_log.h>
#include <esp_memory_utils.h>
#include <nvs_flash.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>
#include <functional>
#include <new>

#include "application.h"

#define TAG "Settings"

namespace {

// §2.3 SafeSettings:PSRAM 栈任务禁直接 NVS/flash——写 flash 关 cache 的窗口里
// 访问 PSRAM 栈会触发 cache 断言崩机(收音机 NVS、GLM 各咬过一次)。此前这条
// 纪律靠 30 个文件各自写注释 + 手动 Schedule 自觉遵守;现在统一收进 Settings:
// 每次访问前探测当前栈位置,PSRAM 栈 → 操作转发 main 任务(内部栈)同步执行。
// 内部栈任务(含 main)零开销直访。
bool CurrentStackInPsram() {
    // 取局部变量地址即当前栈帧位置;地址逃逸进调用,编译器不会把它优化掉
    char probe = 0;
    (void)probe;
    return esp_ptr_external_ram(&probe);
}

constexpr TickType_t kForwardTimeoutTicks = pdMS_TO_TICKS(3000);

// 转发状态。生命周期走"双方接力":超时的调用方置 timed_out 后立刻脱手,绝不再
// 碰状态(main 侧迟到的执行看到标志负责收尾);正常路径双方各 fetch_add,最后
// 一方清理。main 若 3s 都没消费(等于卡死的病态),状态整体泄漏换绝对无 UAF。
template <typename Ret>
struct FwdState {
    SemaphoreHandle_t done = nullptr;
    std::function<Ret()> body;
    Ret out{};
    std::atomic<bool> timed_out{false};
    std::atomic<int> finished{0};

    void FinishOne() {
        if (finished.fetch_add(1) == 1) {
            vSemaphoreDelete(done);
            delete this;
        }
    }
};

template <>
struct FwdState<void> {
    SemaphoreHandle_t done = nullptr;
    std::function<void()> body;
    std::atomic<bool> timed_out{false};
    std::atomic<int> finished{0};

    void FinishOne() {
        if (finished.fetch_add(1) == 1) {
            vSemaphoreDelete(done);
            delete this;
        }
    }
};

template <typename Ret>
Ret GuardedNvs(const char* ns, const char* what, Ret default_on_timeout,
               std::function<Ret()> body) {
    if (!CurrentStackInPsram()) {
        return body();
    }
    auto* st = new (std::nothrow) FwdState<Ret>();
    if (st == nullptr) {
        ESP_LOGE(TAG, "转发状态分配失败,跳过 NVS %s(%s)", ns, what);
        return default_on_timeout;
    }
    st->out = default_on_timeout;
    st->body = std::move(body);
    st->done = xSemaphoreCreateBinary();
    if (st->done == nullptr) {
        ESP_LOGE(TAG, "信号量创建失败,跳过 NVS %s(%s)", ns, what);
        delete st;
        return default_on_timeout;
    }
    Application::GetInstance().Schedule([st] {
        st->out = st->body();
        xSemaphoreGive(st->done);
        if (st->timed_out.load() || st->finished.fetch_add(1) == 1) {
            vSemaphoreDelete(st->done);
            delete st;
        }
    });
    if (xSemaphoreTake(st->done, kForwardTimeoutTicks) == pdTRUE) {
        Ret out = st->out;
        st->FinishOne();
        ESP_LOGD(TAG, "PSRAM 栈 NVS %s(%s) 已转发 main 执行", ns, what);
        return out;
    }
    st->timed_out.store(true);
    ESP_LOGE(TAG, "main 任务 3s 未执行 NVS %s(%s),按默认值返回;状态交 main 侧回收", ns,
             what);
    return default_on_timeout;
}

void GuardedNvs(const char* ns, const char* what, std::function<void()> body) {
    if (!CurrentStackInPsram()) {
        body();
        return;
    }
    auto* st = new (std::nothrow) FwdState<void>();
    if (st == nullptr) {
        ESP_LOGE(TAG, "转发状态分配失败,跳过 NVS %s(%s)", ns, what);
        return;
    }
    st->body = std::move(body);
    st->done = xSemaphoreCreateBinary();
    if (st->done == nullptr) {
        ESP_LOGE(TAG, "信号量创建失败,跳过 NVS %s(%s)", ns, what);
        delete st;
        return;
    }
    Application::GetInstance().Schedule([st] {
        st->body();
        xSemaphoreGive(st->done);
        if (st->timed_out.load() || st->finished.fetch_add(1) == 1) {
            vSemaphoreDelete(st->done);
            delete st;
        }
    });
    if (xSemaphoreTake(st->done, kForwardTimeoutTicks) == pdTRUE) {
        st->FinishOne();
        ESP_LOGD(TAG, "PSRAM 栈 NVS %s(%s) 已转发 main 执行", ns, what);
        return;
    }
    st->timed_out.store(true);
    ESP_LOGE(TAG, "main 任务 3s 未执行 NVS %s(%s),操作丢弃;状态交 main 侧回收", ns, what);
}

}  // namespace

Settings::Settings(const std::string& ns, bool read_write) : ns_(ns), read_write_(read_write) {
    GuardedNvs(ns_.c_str(), "open", [&] {
        nvs_open(ns.c_str(), read_write_ ? NVS_READWRITE : NVS_READONLY, &nvs_handle_);
    });
}

Settings::~Settings() {
    if (nvs_handle_ != 0) {
        GuardedNvs(ns_.c_str(), "close", [&] {
            if (read_write_ && dirty_) {
                // NVS 写失败(典型:分区满)绝不能 abort 整机 —— 历史上音量键一次
                // 写失败就把设备打到重启循环。记日志,下次再写。
                esp_err_t err = nvs_commit(nvs_handle_);
                if (err != ESP_OK) {
                    ESP_LOGE(TAG, "nvs_commit failed: %s (ns=%s)", esp_err_to_name(err),
                             ns_.c_str());
                }
            }
            nvs_close(nvs_handle_);
        });
        nvs_handle_ = 0;
    }
}

std::string Settings::GetString(const std::string& key, const std::string& default_value) {
    return GuardedNvs<std::string>(ns_.c_str(), key.c_str(), default_value, [&] {
        if (nvs_handle_ == 0) {
            return default_value;
        }

        size_t length = 0;
        if (nvs_get_str(nvs_handle_, key.c_str(), nullptr, &length) != ESP_OK) {
            return default_value;
        }

        std::string value;
        value.resize(length);
        if (nvs_get_str(nvs_handle_, key.c_str(), value.data(), &length) != ESP_OK) {
            return default_value;
        }
        while (!value.empty() && value.back() == '\0') {
            value.pop_back();
        }
        return value;
    });
}

void Settings::SetString(const std::string& key, const std::string& value) {
    GuardedNvs(ns_.c_str(), key.c_str(), [&] {
        if (read_write_) {
            esp_err_t err = nvs_set_str(nvs_handle_, key.c_str(), value.c_str());
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "nvs_set_str(%s) failed: %s", key.c_str(), esp_err_to_name(err));
                return;
            }
            dirty_ = true;
        } else {
            ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
        }
    });
}

int32_t Settings::GetInt(const std::string& key, int32_t default_value) {
    return GuardedNvs<int32_t>(ns_.c_str(), key.c_str(), default_value, [&] {
        if (nvs_handle_ == 0) {
            return default_value;
        }

        int32_t value;
        if (nvs_get_i32(nvs_handle_, key.c_str(), &value) != ESP_OK) {
            return default_value;
        }
        return value;
    });
}

void Settings::SetInt(const std::string& key, int32_t value) {
    GuardedNvs(ns_.c_str(), key.c_str(), [&] {
        if (read_write_) {
            esp_err_t err = nvs_set_i32(nvs_handle_, key.c_str(), value);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "nvs_set_i32(%s) failed: %s", key.c_str(), esp_err_to_name(err));
                return;
            }
            dirty_ = true;
        } else {
            ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
        }
    });
}

bool Settings::GetBool(const std::string& key, bool default_value) {
    return GuardedNvs<bool>(ns_.c_str(), key.c_str(), default_value, [&] {
        if (nvs_handle_ == 0) {
            return default_value;
        }

        uint8_t value;
        if (nvs_get_u8(nvs_handle_, key.c_str(), &value) != ESP_OK) {
            return default_value;
        }
        return value != 0;
    });
}

void Settings::SetBool(const std::string& key, bool value) {
    GuardedNvs(ns_.c_str(), key.c_str(), [&] {
        if (read_write_) {
            esp_err_t err = nvs_set_u8(nvs_handle_, key.c_str(), value ? 1 : 0);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "nvs_set_u8(%s) failed: %s", key.c_str(), esp_err_to_name(err));
                return;
            }
            dirty_ = true;
        } else {
            ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
        }
    });
}

void Settings::EraseKey(const std::string& key) {
    GuardedNvs(ns_.c_str(), key.c_str(), [&] {
        if (read_write_) {
            auto ret = nvs_erase_key(nvs_handle_, key.c_str());
            if (ret != ESP_OK && ret != ESP_ERR_NVS_NOT_FOUND) {
                ESP_LOGE(TAG, "nvs_erase_key(%s) failed: %s", key.c_str(), esp_err_to_name(ret));
            }
            dirty_ = true;
        } else {
            ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
        }
    });
}

void Settings::EraseAll() {
    GuardedNvs(ns_.c_str(), "erase_all", [&] {
        if (read_write_) {
            esp_err_t err = nvs_erase_all(nvs_handle_);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "nvs_erase_all failed: %s", esp_err_to_name(err));
                return;
            }
            dirty_ = true;
        } else {
            ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
        }
    });
}
