// main/audio/audio_focus_manager.cc
#include "audio_focus_manager.h"
#include <esp_log.h>
#include <algorithm>

#define TAG "AudioFocus"

AudioFocusManager& AudioFocusManager::GetInstance() {
    static AudioFocusManager instance;
    return instance;
}

bool AudioFocusManager::RequestFocus(const std::string& client_id,
                                     AudioClientType type,
                                     AudioFocusCallback callback) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    ESP_LOGI(TAG, "业务 [%s] 请求音频焦点 (type=%d)", client_id.c_str(), static_cast<int>(type));

    // 1. 如果已有业务持有焦点, 根据优先级判定是否打断
    if (!focus_stack_.empty()) {
        auto& current = focus_stack_.back();
        if (current.client_id == client_id) {
            // 同一业务重入更新
            current.type = type;
            current.callback = callback;
            if (callback) callback(AudioFocusState::Gained);
            return true;
        }

        // 抢占策略:
        // Exclusive 或 Transient 可抢占 Media
        if (type == AudioClientType::Exclusive || type == AudioClientType::Transient) {
            ESP_LOGI(TAG, "业务 [%s] 抢占当前焦点 [%s]", client_id.c_str(), current.client_id.c_str());
            if (current.callback) {
                current.callback(AudioFocusState::LossTransient);
            }
        } else if (type == AudioClientType::Media && current.type == AudioClientType::Exclusive) {
            // 当前正有独占业务 (如 AI 语音问答), Media 请求先排入栈中暂缓播放
            ESP_LOGI(TAG, "当前独占业务 [%s] 活跃, [%s] 挂起排队", current.client_id.c_str(), client_id.c_str());
            // 先清理同名记录
            focus_stack_.erase(
                std::remove_if(focus_stack_.begin(), focus_stack_.end(),
                               [&](const FocusRecord& r) { return r.client_id == client_id; }),
                focus_stack_.end());
            // 插入到 Exclusive 之下
            focus_stack_.insert(focus_stack_.begin(), {client_id, type, callback});
            if (callback) {
                callback(AudioFocusState::LossTransient);
            }
            return false;
        }
    }

    // 2. 清理历史同名记录并将本业务置于栈顶
    focus_stack_.erase(
        std::remove_if(focus_stack_.begin(), focus_stack_.end(),
                       [&](const FocusRecord& r) { return r.client_id == client_id; }),
        focus_stack_.end());

    focus_stack_.push_back({client_id, type, callback});
    if (callback) {
        callback(AudioFocusState::Gained);
    }
    return true;
}

void AudioFocusManager::AbandonFocus(const std::string& client_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (focus_stack_.empty()) return;

    ESP_LOGI(TAG, "业务 [%s] 释放音频焦点", client_id.c_str());
    bool was_top = (focus_stack_.back().client_id == client_id);

    focus_stack_.erase(
        std::remove_if(focus_stack_.begin(), focus_stack_.end(),
                       [&](const FocusRecord& r) { return r.client_id == client_id; }),
        focus_stack_.end());

    // 如果释放的是栈顶业务, 且栈内还有挂起的业务 (如被暂停的收音机), 唤醒它恢复播放
    if (was_top && !focus_stack_.empty()) {
        auto& next = focus_stack_.back();
        ESP_LOGI(TAG, "恢复被挂起的音频业务 [%s]", next.client_id.c_str());
        if (next.callback) {
            next.callback(AudioFocusState::Gained);
        }
    }
}

std::string AudioFocusManager::GetCurrentHolder() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    if (focus_stack_.empty()) return "";
    return focus_stack_.back().client_id;
}
