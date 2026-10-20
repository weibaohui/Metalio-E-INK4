#pragma once

#include "app_module.h"
#include <vector>
#include <memory>
#include <string>
#include <unordered_map>
#include <cstdint>

/**
 * @brief 系统应用管理器（统一应用注册与生命周期管理）
 *
 * 职责：
 * 1. 统一维护系统中所有已注册应用模块 (AppModule)；
 * 2. 基于 AsyncSettingsStore 进行开关状态的 RAM 缓存与防抖异步持久化；
 * 3. 解除 HomeScreen、SettingsAppsTab 与各具体 Screen 之间的硬编码强耦合；
 * 4. 统一调度开机阶段启用应用的 MCP 工具注册；
 * 5. 全面采用面向对象的 AppModule 架构。
 */
class AppManager {
public:
    static AppManager& GetInstance();

    // 开机初始化：注册默认内置应用并从异步配置中心加载开关状态到 RAM 缓存
    void Init();

    // 动态注册应用模块
    void RegisterApp(std::shared_ptr<AppModule> module);

    // 注销动态应用（按 id）；存在返回 true。仅用于动态应用（位于 modules_ 尾部，
    // 注销不影响内置应用的开关位映射）。
    bool UnregisterApp(const char* id);

    // 获取所有应用模块
    const std::vector<std::shared_ptr<AppModule>>& GetAllApps() const { return modules_; }
    int GetAppCount() const { return static_cast<int>(modules_.size()); }

    // 获取当前处于开启状态的应用列表
    std::vector<std::shared_ptr<AppModule>> GetEnabledApps() const;
    int GetEnabledAppCount() const;

    // 状态与信息查询
    std::shared_ptr<AppModule> GetAppById(const char* id) const;
    std::shared_ptr<AppModule> GetAppByIndex(int index) const;
    bool IsAppEnabled(const char* id) const;
    bool IsAppEnabledByIndex(int index) const;
    bool IsAppMandatoryByIndex(int index) const;
    const char* GetAppNameByIndex(int index) const;

    // 切换应用开关（更新 RAM 缓存并经 AsyncSettingsStore 防抖落盘）
    void SetAppEnabled(int index, bool enabled);
    void SetAppEnabled(const char* id, bool enabled);

    // 注册所有已启用应用的 MCP 工具
    void RegisterEnabledMcpTools();

    // 统一打开应用
    void LaunchApp(std::shared_ptr<AppModule> app);
    void LaunchApp(const char* id);

    // 页面生命周期分发调度 (由 VkKeyHandler 在屏幕加载/卸载时驱动 AppModule 生命周期)
    // screen 为该事件对应的屏幕对象，用于区分同 app 重开时的"当前屏"与"旧屏"
    void DispatchScreenLifecycle(const char* screen_id, bool loaded, void* screen);
    std::shared_ptr<AppModule> GetActiveApp() const { return active_app_; }

private:
    AppManager();
    ~AppManager() = default;
    AppManager(const AppManager&) = delete;
    AppManager& operator=(const AppManager&) = delete;

    void RegisterDefaultApps();
    void LoadToggles();
    int FindIndexById(const char* id) const;

    std::vector<std::shared_ptr<AppModule>> modules_;
    std::shared_ptr<AppModule> active_app_ = nullptr;
    // 每个 app 当前在屏的屏幕对象：同 app 重开时旧屏卸载不得销毁该 app
    std::unordered_map<std::string, void*> app_screens_;
    uint32_t enabled_mask_ = 0xFFFFFFFF;
    bool toggles_loaded_ = false;
};
