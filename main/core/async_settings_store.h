// main/core/async_settings_store.h
// 统一异步类型安全配置中心:
// 1. 内存高速缓存 (RAM Cache): 读操作 0 延迟、100% 线程安全、100% Flash-Cache 安全 (无论在 PSRAM 栈还是内部栈调用均永不崩机)。
// 2. 异步自动去抖落盘 (Debounced Async Flush): 写操作先更新内存并标记 dirty, 在内部 RAM 栈后台自动合并延迟落地 NVS, 保护 Flash 寿命且永不阻塞调用方。
// 3. 关机/重启统一 Flush: 保证数据完整落盘。
#pragma once

#include <string>
#include <unordered_map>
#include <mutex>
#include <memory>
#include <atomic>
#include <esp_timer.h>

class AsyncSettingsStore {
public:
    static AsyncSettingsStore& GetInstance();

    // 开机初始化 (由 main 任务调用, 可选预热命名空间)
    void Init();

    // 读接口: 优先读内存缓存, 未缓存时从 NVS 加载并写入缓存 (线程安全、PSRAM 栈安全)
    std::string GetString(const std::string& ns, const std::string& key, const std::string& def_val = "");
    int32_t GetInt(const std::string& ns, const std::string& key, int32_t def_val = 0);
    bool GetBool(const std::string& ns, const std::string& key, bool def_val = false);

    // 写接口: 瞬间更新内存缓存并排队落盘, 非阻塞、PSRAM 栈安全
    void SetString(const std::string& ns, const std::string& key, const std::string& value);
    void SetInt(const std::string& ns, const std::string& key, int32_t value);
    void SetBool(const std::string& ns, const std::string& key, bool value);

    // 立即清空/删除键
    void EraseKey(const std::string& ns, const std::string& key);

    // 立即强制将所有待写入项同步刷盘 (关机/OTA/重启前调用)
    void FlushSync();

private:
    AsyncSettingsStore();
    ~AsyncSettingsStore();
    AsyncSettingsStore(const AsyncSettingsStore&) = delete;
    AsyncSettingsStore& operator=(const AsyncSettingsStore&) = delete;

    struct Entry {
        enum class Type { String, Int, Bool, Erased };
        Type type;
        std::string str_val;
        int32_t int_val = 0;
        bool dirty = false;
    };

    static std::string MakeCompositeKey(const std::string& ns, const std::string& key);
    void ScheduleFlushLocked();
    void DoFlushWork();

    std::mutex mutex_;
    std::unordered_map<std::string, Entry> cache_;
    esp_timer_handle_t flush_timer_ = nullptr;
    std::atomic<bool> flush_scheduled_{false};
    bool initialized_ = false;
};
