#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <ctime>

namespace backup_restore {

struct BackupFileInfo {
    std::string filename;
    std::string full_path;
    uint32_t size_bytes = 0;
    time_t mod_time = 0;
    int entry_count = -1;
};

struct ExportResult {
    bool success = false;
    std::string filename;
    std::string full_path;
    std::string error_msg;
    int count = 0;
};

struct ImportResult {
    bool success = false;
    std::string filename;
    std::string error_msg;
    int count = 0;
    int failed_count = 0;
};

class NvsBackupService {
public:
    static NvsBackupService& GetInstance();

    // 检查 SD 卡是否已挂载
    bool IsSdMounted();

    // 获取 SD 卡可用容量与总容量文本
    bool GetSdCapacityText(std::string& free_str, std::string& total_str);

    // 获取本机 NVS 当前已用条目数
    int GetLocalNvsEntryCount();

    // 列出 SD 卡 backup 目录下的所有备份文件（按修改时间倒序）
    std::vector<BackupFileInfo> ListBackups();

    // 导出本机所有 NVS 配置（排除 phy 等硬件校准命名空间）到 SD 卡
    ExportResult ExportToSd();

    // 从指定备份文件导入并覆盖 NVS 配置
    ImportResult ImportFromSd(const std::string& filepath);

    // 删除指定的备份文件
    bool DeleteBackup(const std::string& filepath);

    // 导入后直接生效时，重载关键运行时内存缓存（如 APP 隐藏/显示开关等）
    void ReloadRuntimeCaches();

private:
    NvsBackupService() = default;
    ~NvsBackupService() = default;
    NvsBackupService(const NvsBackupService&) = delete;
    NvsBackupService& operator=(const NvsBackupService&) = delete;
};

}  // namespace backup_restore
