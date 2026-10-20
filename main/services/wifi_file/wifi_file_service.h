#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace wifi_file {

/**
 * @brief 文件操作活动通知 (动作, 路径)
 *        动作取值: "上传" / "删除" / "新建目录" / "传书"
 */
using ActivityCallback = std::function<void(const std::string& action, const std::string& path)>;

class WifiFileService {
public:
    static WifiFileService& GetInstance();

    /**
     * @brief 启动 WiFi 传输 Web 服务 (传书 + 文件管理)
     * @param port 监听端口，默认 80；占用时自动回退到 8088
     * @return true 启动成功，false 启动失败
     */
    bool Start(uint16_t port = 80);

    /**
     * @brief 停止 WiFi 传输 Web 服务
     */
    void Stop();

    /**
     * @brief 检查服务是否正在运行
     */
    bool IsRunning() const { return server_ != nullptr; }

    /**
     * @brief 获取当前服务监听端口
     */
    uint16_t GetPort() const { return port_; }

    /**
     * @brief 获取当前设备本机 IP 地址
     */
    std::string GetLocalIp() const;

    /**
     * @brief 获取当前 Web 访问完整 URL (如 http://192.168.1.108/)
     */
    std::string GetServerUrl() const;

    /**
     * @brief 注册文件操作活动通知
     */
    void SetActivityCallback(ActivityCallback cb) {
        std::lock_guard<std::mutex> lock(cb_mutex_);
        activity_cb_ = std::move(cb);
    }

    /**
     * @brief 触发活动通知
     */
    void NotifyActivity(const std::string& action, const std::string& path) {
        ActivityCallback cb_copy;
        {
            std::lock_guard<std::mutex> lock(cb_mutex_);
            cb_copy = activity_cb_;
        }
        if (cb_copy) {
            cb_copy(action, path);
        }
    }

    /**
     * @brief 图书库目录(相对 SD 根),供 Web 端「传书」页签直达
     *        例: /metalio/e-ink/books
     */
    static const char* GetBooksRelDir();

    /**
     * @brief 图书库目录绝对路径 (例: /sdcard/metalio/e-ink/books)
     */
    static const char* GetBooksAbsDir();

private:
    WifiFileService() = default;
    ~WifiFileService() { Stop(); }

    void* server_ = nullptr;
    uint16_t port_ = 80;
    mutable std::mutex cb_mutex_;
    ActivityCallback activity_cb_;
};

}  // namespace wifi_file
