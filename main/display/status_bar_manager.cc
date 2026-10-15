// main/display/status_bar_manager.cc
#include "status_bar_manager.h"
#include "screen_common.h"
#include "core/system_event_bus.h"
#include <cstring>
#include <esp_log.h>
#include <esp_timer.h>

#define TAG "StatusBarMgr"

StatusBarManager& StatusBarManager::GetInstance() {
    static StatusBarManager instance;
    return instance;
}

StatusBarManager::StatusBarManager() {
    esp_timer_create_args_t timer_args = {};
    timer_args.callback = [](void* arg) {
        auto* self = static_cast<StatusBarManager*>(arg);
        if (self) {
            self->Refresh();
        }
    };
    timer_args.arg = this;
    timer_args.name = "sb_transient";
    esp_timer_create(&timer_args, reinterpret_cast<esp_timer_handle_t*>(&transient_timer_));

    SystemEventBus::GetInstance().Subscribe(SystemEventType::kTimeTick, [this](const SystemEvent& ev) {
        UpdateClock(ev.extra_str);
    });
}

StatusBarManager::~StatusBarManager() {
    if (transient_timer_) {
        esp_timer_stop(static_cast<esp_timer_handle_t>(transient_timer_));
        esp_timer_delete(static_cast<esp_timer_handle_t>(transient_timer_));
        transient_timer_ = nullptr;
    }
}

void StatusBarManager::SetCriticalAlert(const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    critical_msg_ = message;
    ESP_LOGI(TAG, "设置关键警告: %s", message.c_str());
    std::string text;
    StatusPriority prio;
    ArbitrateLocked(text, prio);
    ApplyToUiLocked(text);
}

void StatusBarManager::ClearCriticalAlert() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!critical_msg_.empty()) {
        critical_msg_.clear();
        ESP_LOGI(TAG, "清除关键警告");
        std::string text;
        StatusPriority prio;
        ArbitrateLocked(text, prio);
        ApplyToUiLocked(text);
    }
}

void StatusBarManager::ShowToast(const std::string& message, uint32_t duration_ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    transient_msg_ = message;
    transient_expire_us_ = esp_timer_get_time() + static_cast<uint64_t>(duration_ms) * 1000ULL;
    ESP_LOGI(TAG, "临时状态通知 (%u ms): %s", (unsigned)duration_ms, message.c_str());

    if (transient_timer_) {
        esp_timer_stop(static_cast<esp_timer_handle_t>(transient_timer_));
        esp_timer_start_once(static_cast<esp_timer_handle_t>(transient_timer_),
                             static_cast<uint64_t>(duration_ms) * 1000ULL);
    }

    std::string text;
    StatusPriority prio;
    ArbitrateLocked(text, prio);
    ApplyToUiLocked(text);
}

void StatusBarManager::SetOngoing(const std::string& owner_id, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool found = false;
    for (auto& pair : ongoing_statuses_) {
        if (pair.first == owner_id) {
            pair.second = message;
            found = true;
            break;
        }
    }
    if (!found) {
        ongoing_statuses_.emplace_back(owner_id, message);
    }
    ESP_LOGI(TAG, "业务 [%s] 登记持续状态: %s", owner_id.c_str(), message.c_str());
    std::string text;
    StatusPriority prio;
    ArbitrateLocked(text, prio);
    ApplyToUiLocked(text);
}

void StatusBarManager::ClearOngoing(const std::string& owner_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = ongoing_statuses_.begin(); it != ongoing_statuses_.end(); ++it) {
        if (it->first == owner_id) {
            ongoing_statuses_.erase(it);
            ESP_LOGI(TAG, "业务 [%s] 清除持续状态", owner_id.c_str());
            std::string text;
            StatusPriority prio;
            ArbitrateLocked(text, prio);
            ApplyToUiLocked(text);
            break;
        }
    }
}

void StatusBarManager::UpdateClock(const std::string& clock_str) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (clock_str_ == clock_str) return;
    clock_str_ = clock_str;

    std::string text;
    StatusPriority prio;
    ArbitrateLocked(text, prio);
    if (prio == StatusPriority::Default) {
        ApplyToUiLocked(text);
    }
}

void StatusBarManager::BindStatusWidget(lv_obj_t* status_label) {
    std::lock_guard<std::mutex> lock(mutex_);
    bound_label_ = status_label;
    current_rendered_text_.clear(); // 强制刷进新绑定的页面

    if (clock_str_.empty()) {
        time_t now = time(nullptr);
        struct tm tm_info = {};
        if (localtime_r(&now, &tm_info) != nullptr && tm_info.tm_year >= (2025 - 1900)) {
            char buf[16];
            strftime(buf, sizeof(buf), "%H:%M", &tm_info);
            clock_str_ = buf;
        }
    }

    std::string text;
    StatusPriority prio;
    ArbitrateLocked(text, prio);
    ApplyToUiLocked(text);
}

void StatusBarManager::UnbindStatusWidget(lv_obj_t* status_label) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (bound_label_ == status_label) {
        bound_label_ = nullptr;
    }
}

void StatusBarManager::Refresh() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string text;
    StatusPriority prio;
    ArbitrateLocked(text, prio);
    ApplyToUiLocked(text);
}

std::string StatusBarManager::GetCurrentDisplayMessage() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::string text;
    StatusPriority prio;
    ArbitrateLocked(text, prio);
    return text;
}

void StatusBarManager::ArbitrateLocked(std::string& out_msg, StatusPriority& out_prio) const {
    // 1. 最高优先级: 关键致命警告 (Sticky Critical)
    if (!critical_msg_.empty()) {
        out_msg = critical_msg_;
        out_prio = StatusPriority::Critical;
        return;
    }

    // 2. 次高优先级: 临时提示通知 (Transient Toast)
    if (!transient_msg_.empty()) {
        if (esp_timer_get_time() < transient_expire_us_) {
            out_msg = transient_msg_;
            out_prio = StatusPriority::Transient;
            return;
        }
    }

    // 3. 第三优先级: 活跃业务持续状态 (Ongoing Status, 取最新登记的非空条目)
    if (!ongoing_statuses_.empty()) {
        for (auto it = ongoing_statuses_.rbegin(); it != ongoing_statuses_.rend(); ++it) {
            if (!it->second.empty()) {
                out_msg = it->second;
                out_prio = StatusPriority::Ongoing;
                return;
            }
        }
    }

    // 4. 默认基底状态: 系统时钟
    out_msg = clock_str_;
    out_prio = StatusPriority::Default;
}

void StatusBarManager::ApplyToUiLocked(const std::string& text) {
    if (bound_label_ == nullptr) return;
    if (current_rendered_text_ == text) return;
    current_rendered_text_ = text;

    struct UiUpdateArg {
        lv_obj_t* label;
        char text[64];
    };
    auto* arg = new UiUpdateArg();
    arg->label = bound_label_;
    size_t copy_len = text.size() < sizeof(arg->text) - 1 ? text.size() : sizeof(arg->text) - 1;
    std::memcpy(arg->text, text.data(), copy_len);
    arg->text[copy_len] = '\0';

    ScreenLvAsync([](void* p) {
        auto* a = static_cast<UiUpdateArg*>(p);
        if (a && a->label && lv_obj_is_valid(a->label)) {
            lv_label_set_text(a->label, a->text);
            if (a->text[0] == '\0') {
                lv_obj_add_flag(a->label, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_remove_flag(a->label, LV_OBJ_FLAG_HIDDEN);
            }
        }
        delete a;
    }, arg);
}
