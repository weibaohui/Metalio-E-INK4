// main/services/dynamic_apps/dynamic_app_loader.h
// 动态应用加载器（惰性模型）：点开才加载，退出即卸载。
// 应用包 = SD 卡 apps/<dir>/（manifest.json + app.so + 可选 icon.png）。
// 安装态持久化于 NVS（dynapp/in_<id>，缺省=已安装）：推送的新包自动上桌面；
// 卸载只注销桌面入口并保留文件，可在应用商店（app_store_screen）重新安装。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dynamic_apps {

// SD 卡上的一个应用包（manifest 有效）
struct DynPackage {
    std::string dir;      // 完整目录路径（/sdcard/metalio/e-ink/apps/<dir>）
    std::string id;       // manifest id
    std::string name;     // 显示名
    std::string version;  // 版本（可空）
    bool installed = false; // 当前是否已注册进桌面
};

struct AppInstallResult {
    bool success = false;
    std::string app_id;
    std::string app_name;
    std::string version;
    std::string bundle_dir;
    std::string error_msg;
};

struct AppDetails {
    std::string id;
    std::string name;
    std::string version;
    std::string description;
    std::string author;
    std::string bundle_dir;
    bool installed = false;
    bool enabled = false;
    bool has_icon = false;
    bool has_readme = false;
    std::string readme_text;
    uint32_t total_size_bytes = 0;
    std::vector<std::string> file_list;
};

// 注册供动态 .so 解析的固件符号表；开机阶段调用一次（幂等）。
void RegisterElfSymbols();

// 扫描 SD 卡应用目录：有效包按安装标志注册（未安装的跳过，留在商店）。
// SD 未挂载或无目录时为 no-op。
void ScanAndRegister();

// 列出 SD 卡上所有有效应用包及其安装状态（供应用商店展示）。
// SD 未挂载时返回空表。
std::vector<DynPackage> ListPackages();

// 安装：注册进桌面并置安装标志。找不到该 id 的包返回 false。
bool InstallPackage(const std::string& id);

// 卸载（应用商店停用模式）：注销桌面入口并清安装标志（保留 SD 文件，可在商店重新点装）。
bool UninstallPackage(const std::string& id);

// 从本地 ZIP 包解压安装/更新应用
AppInstallResult InstallFromZip(const std::string& zip_path, bool overwrite = true);

// 从远程 HTTP/HTTPS URL 下载并解压安装应用
AppInstallResult InstallFromUrl(const std::string& url);

// 彻底卸载应用（从桌面注销并清理 SD 卡应用包目录；keep_data=true 可保留 data/ 私有数据）
bool UninstallApp(const std::string& app_id, bool keep_data, std::string& out_error);

// 获取已安装应用的详细信息（包含 manifest、README、文件清单等）
bool GetAppDetails(const std::string& app_id, AppDetails& out_details);

// 设置应用启用/禁用
bool SetAppEnabled(const std::string& app_id, bool enabled);

// 注册应用管理 MCP 工具 (self.app.list, self.app.get_info, self.app.install, self.app.uninstall, self.app.enable, self.app.disable)
void RegisterMcpTools();

}  // namespace dynamic_apps
