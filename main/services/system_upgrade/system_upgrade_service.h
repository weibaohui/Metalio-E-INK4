#pragma once

#include <string>
#include <vector>
#include <functional>
#include <memory>
#include <cstdint>
#include <ctime>

namespace system_upgrade {

struct FirmwareInfo {
    std::string filename;
    std::string full_path;
    size_t file_size = 0;
    time_t mod_time = 0;
    bool is_valid_esp32_bin = false;
    std::string project_name;
    std::string version;
    std::string build_date;
    std::string build_time;
    std::string idf_ver;
};

struct UpgradeResult {
    bool success = false;
    std::string error_msg;
};

class SystemUpgradeService {
public:
    static SystemUpgradeService& GetInstance();

    // 禁用拷贝/赋值
    SystemUpgradeService(const SystemUpgradeService&) = delete;
    SystemUpgradeService& operator=(const SystemUpgradeService&) = delete;

    // 当前运行环境信息
    std::string GetCurrentVersion() const;
    std::string GetRunningPartition() const;
    std::string GetNextPartition() const;
    std::string GetBuildTime() const;
    std::string GetProjectName() const;

    // SD 卡离线固件识别与烧录
    bool IsSdMounted() const;
    std::vector<FirmwareInfo> ScanSdFirmwares();
    bool ParseFirmwareHeader(const std::string& path, FirmwareInfo& out_info);
    UpgradeResult FlashFromSd(const std::string& path, std::function<void(int progress, size_t speed)> progress_cb = nullptr);
    bool DeleteFirmwareFile(const std::string& path);

    // WiFi 网页端 OTA 烧录服务 (开页按需启动，离页即停释放内存)
    bool StartWebServer(uint16_t port = 80);
    void StopWebServer();
    bool IsWebServerRunning() const;
    std::string GetWebServerUrl() const;

    using WebUploadCallback = std::function<void(const std::string& status, int progress)>;
    void SetWebUploadCallback(WebUploadCallback cb);

private:
    SystemUpgradeService() = default;
    ~SystemUpgradeService();

    void* http_server_handle_ = nullptr;
    WebUploadCallback web_cb_ = nullptr;
};

}  // namespace system_upgrade
