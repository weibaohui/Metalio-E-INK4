// main/core/system_event_bus.cc
#include "system_event_bus.h"
#include <esp_log.h>
#include <algorithm>

SystemEventBus& SystemEventBus::GetInstance() {
    static SystemEventBus s_instance;
    return s_instance;
}

SubscriptionToken SystemEventBus::Subscribe(SystemEventType type, EventCallback cb) {
    if (!cb) return 0;
    std::lock_guard<std::mutex> lock(mutex_);
    SubscriptionToken token = next_token_++;
    if (next_token_ == 0) next_token_ = 1; // 环回防溢出

    subscribers_[token] = SubscriberRecord{token, type, std::move(cb)};
    type_index_[type].push_back(token);
    return token;
}

void SystemEventBus::Unsubscribe(SubscriptionToken token) {
    if (token == 0) return;
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = subscribers_.find(token);
    if (it != subscribers_.end()) {
        SystemEventType type = it->second.type;
        subscribers_.erase(it);

        auto idx_it = type_index_.find(type);
        if (idx_it != type_index_.end()) {
            auto& vec = idx_it->second;
            vec.erase(std::remove(vec.begin(), vec.end(), token), vec.end());
            if (vec.empty()) {
                type_index_.erase(idx_it);
            }
        }
    }
}

void SystemEventBus::Publish(const SystemEvent& event) {
    std::vector<EventCallback> callbacks_to_run;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = type_index_.find(event.type);
        if (it != type_index_.end()) {
            callbacks_to_run.reserve(it->second.size());
            for (SubscriptionToken token : it->second) {
                auto sub_it = subscribers_.find(token);
                if (sub_it != subscribers_.end() && sub_it->second.callback) {
                    callbacks_to_run.push_back(sub_it->second.callback);
                }
            }
        }
    }

    // 锁外执行回调，杜绝重入死锁
    for (const auto& cb : callbacks_to_run) {
        cb(event);
    }
}
