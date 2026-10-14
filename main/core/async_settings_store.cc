// main/core/async_settings_store.cc
#include "async_settings_store.h"
#include "settings.h"
#include "application.h"

#include <esp_log.h>
#include <vector>

#define TAG "AsyncSettings"

AsyncSettingsStore& AsyncSettingsStore::GetInstance() {
    static AsyncSettingsStore instance;
    return instance;
}

AsyncSettingsStore::AsyncSettingsStore() {
}

AsyncSettingsStore::~AsyncSettingsStore() {
    if (flush_timer_ != nullptr) {
        esp_timer_stop(flush_timer_);
        esp_timer_delete(flush_timer_);
        flush_timer_ = nullptr;
    }
}

void AsyncSettingsStore::Init() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) return;

    esp_timer_create_args_t timer_args = {
        .callback = [](void* arg) {
            auto* store = static_cast<AsyncSettingsStore*>(arg);
            // 收到去抖定时器超时, 投递到 main 任务执行 NVS 落地 (内部栈, Flash 绝对安全)
            Application::GetInstance().Schedule([store]() {
                store->DoFlushWork();
            });
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "settings_flush",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&timer_args, &flush_timer_) == ESP_OK) {
        initialized_ = true;
        ESP_LOGI(TAG, "AsyncSettingsStore ready");
    } else {
        ESP_LOGE(TAG, "Failed to create flush timer");
    }
}

std::string AsyncSettingsStore::MakeCompositeKey(const std::string& ns, const std::string& key) {
    return ns + "/" + key;
}

std::string AsyncSettingsStore::GetString(const std::string& ns, const std::string& key, const std::string& def_val) {
    const std::string comp = MakeCompositeKey(ns, key);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(comp);
        if (it != cache_.end()) {
            if (it->second.type == Entry::Type::Erased) return def_val;
            return it->second.str_val;
        }
    }
    // 未缓存: 从底层 Settings 读取并填充缓存
    Settings s(ns, false);
    std::string val = s.GetString(key, def_val);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Entry e;
        e.type = Entry::Type::String;
        e.str_val = val;
        e.dirty = false;
        cache_[comp] = std::move(e);
    }
    return val;
}

int32_t AsyncSettingsStore::GetInt(const std::string& ns, const std::string& key, int32_t def_val) {
    const std::string comp = MakeCompositeKey(ns, key);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = cache_.find(comp);
        if (it != cache_.end()) {
            if (it->second.type == Entry::Type::Erased) return def_val;
            return it->second.int_val;
        }
    }
    Settings s(ns, false);
    int32_t val = s.GetInt(key, def_val);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        Entry e;
        e.type = Entry::Type::Int;
        e.int_val = val;
        e.dirty = false;
        cache_[comp] = e;
    }
    return val;
}

bool AsyncSettingsStore::GetBool(const std::string& ns, const std::string& key, bool def_val) {
    return GetInt(ns, key, def_val ? 1 : 0) != 0;
}

void AsyncSettingsStore::SetString(const std::string& ns, const std::string& key, const std::string& value) {
    const std::string comp = MakeCompositeKey(ns, key);
    std::lock_guard<std::mutex> lock(mutex_);
    Entry& e = cache_[comp];
    if (e.type == Entry::Type::String && e.str_val == value && !e.dirty) {
        return; // 值无变化且已落盘
    }
    e.type = Entry::Type::String;
    e.str_val = value;
    e.dirty = true;
    ScheduleFlushLocked();
}

void AsyncSettingsStore::SetInt(const std::string& ns, const std::string& key, int32_t value) {
    const std::string comp = MakeCompositeKey(ns, key);
    std::lock_guard<std::mutex> lock(mutex_);
    Entry& e = cache_[comp];
    if (e.type == Entry::Type::Int && e.int_val == value && !e.dirty) {
        return;
    }
    e.type = Entry::Type::Int;
    e.int_val = value;
    e.dirty = true;
    ScheduleFlushLocked();
}

void AsyncSettingsStore::SetBool(const std::string& ns, const std::string& key, bool value) {
    SetInt(ns, key, value ? 1 : 0);
}

void AsyncSettingsStore::EraseKey(const std::string& ns, const std::string& key) {
    const std::string comp = MakeCompositeKey(ns, key);
    std::lock_guard<std::mutex> lock(mutex_);
    Entry& e = cache_[comp];
    e.type = Entry::Type::Erased;
    e.dirty = true;
    ScheduleFlushLocked();
}

void AsyncSettingsStore::ScheduleFlushLocked() {
    if (!initialized_ || flush_timer_ == nullptr) return;
    if (!flush_scheduled_.exchange(true)) {
        // 300ms 去抖窗口: 快速连续修改合并为一次 NVS 写入
        esp_timer_start_once(flush_timer_, 300000);
    }
}

void AsyncSettingsStore::DoFlushWork() {
    flush_scheduled_.store(false);

    struct WriteItem {
        std::string ns;
        std::string key;
        Entry entry;
    };
    std::vector<WriteItem> to_write;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& pair : cache_) {
            if (pair.second.dirty) {
                size_t slash = pair.first.find('/');
                if (slash != std::string::npos) {
                    WriteItem item;
                    item.ns = pair.first.substr(0, slash);
                    item.key = pair.first.substr(slash + 1);
                    item.entry = pair.second;
                    to_write.push_back(std::move(item));
                    pair.second.dirty = false;
                }
            }
        }
    }

    if (to_write.empty()) return;

    ESP_LOGI(TAG, "落地 NVS: %u 项待持久化", (unsigned)to_write.size());
    // 按命名空间打开并批量写入
    std::string cur_ns;
    std::unique_ptr<Settings> s;

    for (const auto& item : to_write) {
        if (item.ns != cur_ns || s == nullptr) {
            s = std::make_unique<Settings>(item.ns, true);
            cur_ns = item.ns;
        }
        if (item.entry.type == Entry::Type::String) {
            s->SetString(item.key, item.entry.str_val);
        } else if (item.entry.type == Entry::Type::Int) {
            s->SetInt(item.key, item.entry.int_val);
        } else if (item.entry.type == Entry::Type::Erased) {
            s->EraseKey(item.key);
        }
    }
}

void AsyncSettingsStore::FlushSync() {
    DoFlushWork();
}
