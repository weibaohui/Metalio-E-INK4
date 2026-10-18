#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <ctime>

namespace backup_restore {

// 备份类型枚举
enum BackupType {
    BACKUP_TYPE_FULL = 0,       // 全量快照：系统配置 + 应用数据 + 已安装应用包
    BACKUP_TYPE_SYSTEM = 1,     // 仅系统配置：NVS 系统核心命名空间 (Wi-Fi, settings, power 等)
    BACKUP_TYPE_APP_DATA = 2,   // 仅应用数据：NVS 应用开关 + SD 卡各应用 data/ 目录
};

// 恢复选择掩码（bitmask）
enum RestoreMask {
    RESTORE_MASK_SYSTEM = 1 << 0,      // 恢复系统配置
    RESTORE_MASK_APP_DATA = 1 << 1,    // 恢复应用数据与状态
    RESTORE_MASK_APP_BUNDLES = 1 << 2, // 恢复应用安装包本体
    RESTORE_MASK_ALL = (1 << 0) | (1 << 1) | (1 << 2),
};

struct BackupFileInfo {
    std::string filename;      // 备份目录名 (如 20261018_121530_full)
    std::string full_path;     // 完整路径
    std::string created_at;    // 格式化时间 "2026-10-18 12:15:30"
    int backup_type = 0;       // 0=全量, 1=系统配置, 2=应用数据
    bool has_system = false;
    bool has_app_data = false;
    bool has_app_bundles = false;
    int system_nvs_count = 0;
    int app_nvs_count = 0;
    int app_data_count = 0;
    int app_bundle_count = 0;
    uint32_t size_bytes = 0;
    time_t mod_time = 0;
};

struct ExportResult {
    bool success = false;
    std::string filename;
    std::string full_path;
    std::string error_msg;
    int system_count = 0;
    int app_count = 0;
};

struct ImportResult {
    bool success = false;
    std::string filename;
    std::string error_msg;
    int restored_items = 0;
};

class NvsBackupService {
public:
    static NvsBackupService& GetInstance();

    // 检查 SD 卡是否已挂载
    bool IsSdMounted();

    // 获取 SD 卡可用容量与总容量文本
    bool GetSdCapacityText(std::string& free_str, std::string& total_str);

    // 获取统计信息：系统 NVS 项数、应用 NVS 项数、已安装应用数
    bool GetStats(int& out_system_nvs, int& out_app_nvs, int& out_installed_apps);

    // 列出 SD 卡 backup 目录下的所有结构化备份快照（按修改时间倒序）
    std::vector<BackupFileInfo> ListBackups();

    // 导出备份到 SD 卡 (backup_type: 0=全量, 1=系统配置, 2=应用数据)
    ExportResult ExportToSd(int backup_type = BACKUP_TYPE_FULL);

    // 从指定备份快照目录恢复 (restore_mask: 组合 RestoreMask 标志)
    ImportResult ImportFromSd(const std::string& snapshot_path, int restore_mask = RESTORE_MASK_ALL);

    // 删除指定的备份快照目录
    bool DeleteBackup(const std::string& snapshot_path);

    // 导入后重载关键运行时缓存
    void ReloadRuntimeCaches();

private:
    NvsBackupService() = default;
    ~NvsBackupService() = default;
    NvsBackupService(const NvsBackupService&) = delete;
    NvsBackupService& operator=(const NvsBackupService&) = delete;
};

// 注册备份与恢复相关的 MCP 工具 (self.backup.create / list / restore / delete)
void Backup_RegisterMcpTools();

}  // namespace backup_restore

