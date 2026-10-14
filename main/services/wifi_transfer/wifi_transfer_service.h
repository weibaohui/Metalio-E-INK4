#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace wifi_transfer {

struct UploadEvent {
    std::string filename;
    size_t size_bytes = 0;
    bool success = false;
    std::string message;
};

using UploadCallback = std::function<void(const UploadEvent& event)>;

class WifiTransferService {
public:
    static WifiTransferService& GetInstance();

    /**
     * @brief 启动 WiFi 传书 Web 服务
     * @param port 监听端口，默认 8080
     * @return true 启动成功，false 启动失败（如端口被占或未联网）
     */
    bool Start(uint16_t port = 8080);

    /**
     * @brief 停止 WiFi 传书 Web 服务，释放所有内存和 Socket
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
     * @brief 获取当前设备本机 IP 地址（如 192.168.1.108）
     */
    std::string GetLocalIp() const;

    /**
     * @brief 获取当前 Web 访问完整 URL（如 http://192.168.1.108:8080/）
     */
    std::string GetServerUrl() const;

    /**
     * @brief 注册文件上传完成回调
     */
    void SetUploadCallback(UploadCallback cb) { upload_cb_ = cb; }

    /**
     * @brief 触发上传完成通知
     */
    void NotifyUpload(const UploadEvent& ev) {
        if (upload_cb_) {
            upload_cb_(ev);
        }
    }

    /**
     * @brief 获取图书存储根目录
     */
    static const char* GetBooksDir();

private:
    WifiTransferService() = default;
    ~WifiTransferService() { Stop(); }

    void* server_ = nullptr;
    uint16_t port_ = 8080;
    UploadCallback upload_cb_;
};

}  // namespace wifi_transfer
