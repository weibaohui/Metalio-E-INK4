// main/services/dynamic_apps/dynamic_app_loader.h
// 动态应用加载器（惰性模型）：点开才加载，退出即卸载。
// 应用包 = SD 卡 apps/<dir>/（manifest.json + app.so + 可选 icon.png）。
// 安装态持久化于 NVS（dynapp/in_<id>，缺省=已安装）：推送的新包自动上桌面；
// 卸载只注销桌面入口并保留文件，可在应用商店（app_store_screen）重新安装。
#pragma once

#include <string>
#include <vector>

namespace dynamic_apps {

// SD 卡上的一个应用包（manifest 有效）
struct DynPackage {
    std::string dir;      // 完整目录路径（/sdcard/metalio/e-ink/apps/<dir>）
    std::string id;       // manifest id
    std::string name;     // 显示名
    std::string version;  // 版本（可空）
    bool installed;       // 当前是否已注册进桌面
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

// 卸载：注销桌面入口并清安装标志（保留 SD 文件，可重装）。未注册也视为成功。
bool UninstallPackage(const std::string& id);

}
