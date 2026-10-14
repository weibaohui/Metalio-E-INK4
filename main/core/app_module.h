// main/core/app_module.h
// 标准化应用模块生命周期接口 (AppModule):
// 提供类似现代化移动端 OS 的组件式生命周期管理:
// 1. OnCreate(parent): 界面组件树装配;
// 2. OnResume(): 前台激活 (恢复定时刷新、按键监听与高能耗外设);
// 3. OnPause(): 切入后台 (暂停无用轮询、降低功耗);
// 4. OnDestroy(): 页面销毁 (回收栈内存、解绑监听、释放任务);
// 5. RegisterMcpTools(): 开机统一工具注册。
#pragma once

#include <lvgl.h>
#include <string>
#include <memory>

class AppModule {
public:
    virtual ~AppModule() = default;

    /** @brief 唯一应用短标识 (建议 <= 15 字符, 用于 NVS 开关与配置分区) */
    virtual const char* GetId() const = 0;

    /** @brief 应用多语言名称 */
    virtual const char* GetName() const = 0;

    /** @brief 桌面图标资产路径 (如 "A:ic_app_home_book.spng") */
    virtual const char* GetIconPath() const = 0;

    /** @brief 是否为系统级强制常驻应用 (不可禁用, 如设置页) */
    virtual bool IsMandatory() const { return false; }

    /** @brief 界面装配入口 (创建 LVGL 视图树并返回根容器) */
    virtual lv_obj_t* CreateUI(lv_obj_t* parent) = 0;

    /** @brief 应用进入前台 (用户打开该界面) */
    virtual void OnResume() {}

    /** @brief 应用退至后台 (被切换走或弹窗遮挡) */
    virtual void OnPause() {}

    /** @brief 应用视图销毁 (返回桌面或完全退出) */
    virtual void OnDestroy() {}

    /** @brief 注册该应用对大模型开放的 MCP 工具 */
    virtual void RegisterMcpTools() {}
};
