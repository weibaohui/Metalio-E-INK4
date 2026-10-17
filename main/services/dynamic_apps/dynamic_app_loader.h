// main/services/dynamic_apps/dynamic_app_loader.h
// 动态应用加载器（惰性模型）：点开才加载，退出即卸载。
// 应用文件放 /sdcard/metalio/e-ink/apps/ 目录（后缀 .so，WiFi 文件管理器 / USB U盘 / 拷卡均可）。
#pragma once

namespace dynamic_apps {

// 注册供动态 .so 解析的固件符号表；开机阶段调用一次（幂等）。
void RegisterElfSymbols();

// 扫描 SD 卡应用目录并注册惰性应用入口（只读元数据，dlopen 后立即 dlclose，
// 不在内存留应用）；SD 未挂载或无目录时为 no-op。
void ScanAndRegister();

}
