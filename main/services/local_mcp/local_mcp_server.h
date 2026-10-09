#pragma once

#include <atomic>
#include <mutex>
#include <string>

class LocalMcpServer {
public:
    static LocalMcpServer& GetInstance() {
        static LocalMcpServer instance;
        return instance;
    }

    void Start();
    void Stop();
    bool IsRunning() const { return running_.load(); }

    // ---- 6 位数字 PIN 认证(需求一.5) ----
    // PIN 存 NVS "local_mcp/token"(复用已有 Bearer 校验)。空 = 未启用认证(放开)。
    // pin_ 是 RAM 缓存:Start() 在 main 任务从 NVS 载入;设置/重生成会先更新缓存
    // 再把落盘 Schedule 到 main 任务(设置页跑在 LVGL worker PSRAM 栈,禁直接写 flash)。
    bool AuthEnabled();          // pin 非空
    std::string GetPin();        // 当前 PIN(空 = 未启用)
    void SetAuthEnabled(bool enable);  // 开:无 PIN 则随机生成;关:清空 PIN
    void RegeneratePin();        // 随机生成新 6 位 PIN(无论是否启用都生成并启用)

private:
    LocalMcpServer() = default;
    ~LocalMcpServer() = default;
    LocalMcpServer(const LocalMcpServer&) = delete;
    LocalMcpServer& operator=(const LocalMcpServer&) = delete;

    void SetPin(const std::string& pin);  // 更新缓存 + 调度落盘

    std::atomic<bool> running_{false};
    void* server_{nullptr};
    std::mutex pin_mutex_;
    std::string pin_;
};
