#pragma once

#include <atomic>

class LocalMcpServer {
public:
    static LocalMcpServer& GetInstance() {
        static LocalMcpServer instance;
        return instance;
    }

    void Start();
    void Stop();
    bool IsRunning() const { return running_.load(); }

private:
    LocalMcpServer() = default;
    ~LocalMcpServer() = default;
    LocalMcpServer(const LocalMcpServer&) = delete;
    LocalMcpServer& operator=(const LocalMcpServer&) = delete;

    std::atomic<bool> running_{false};
    void* server_{nullptr};
};
