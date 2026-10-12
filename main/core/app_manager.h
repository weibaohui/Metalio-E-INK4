#pragma once

#include "app_desc.h"
#include <vector>
#include <cstdint>

/**
 * @brief 系统应用管理器（统一应用注册与生命周期管理）
 *
 * 职责：
 * 1. 统一维护系统中所有已注册应用元信息；
 * 2. 管理各应用的启用/禁用状态，维持内存缓存以保证在 PSRAM 栈（LVGL worker）安全读取；
 * 3. 解除 HomeScreen、SettingsAppsTab 与各具体 Screen 之间的硬编码强耦合；
 * 4. 统一调度开机阶段启用应用的 MCP 工具注册。
 */
class AppManager {
public:
    static AppManager& GetInstance();

    // 开机初始化：注册默认内置应用并从 NVS 加载开关状态到 RAM 缓存
    void Init();

    // 动态注册应用
    void RegisterApp(const AppDesc& desc);

    // 获取所有应用
    const std::vector<AppDesc>& GetAllApps() const { return apps_; }
    int GetAppCount() const { return static_cast<int>(apps_.size()); }

    // 获取当前处于开启状态的应用列表
    std::vector<const AppDesc*> GetEnabledApps() const;
    int GetEnabledAppCount() const;

    // 状态查询
    bool IsAppEnabled(const char* id) const;
    bool IsAppEnabledByIndex(int index) const;
    bool IsAppMandatoryByIndex(int index) const;
    const char* GetAppNameByIndex(int index) const;

    // 切换应用开关（更新 RAM 缓存并调度落盘 NVS）
    void SetAppEnabled(int index, bool enabled);

    // 注册所有已启用应用的 MCP 工具
    void RegisterEnabledMcpTools();

private:
    AppManager();
    ~AppManager() = default;
    AppManager(const AppManager&) = delete;
    AppManager& operator=(const AppManager&) = delete;

    void RegisterDefaultApps();
    void LoadTogglesFromNvs();
    int FindIndexById(const char* id) const;

    std::vector<AppDesc> apps_;
    uint32_t enabled_mask_ = 0;
    bool toggles_loaded_ = false;
};
