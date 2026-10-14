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

    using ScreenFactory = lv_obj_t* (*)();

    /** @brief 页面导航工厂回调 (若基于工厂函数则返回之, 方便 ScreenNavigateTo 压栈) */
    virtual ScreenFactory GetScreenFactory() const { return nullptr; }

    /** @brief 应用进入前台 (用户打开该界面) */
    virtual void OnResume() {}

    /** @brief 应用退至后台 (被切换走或弹窗遮挡) */
    virtual void OnPause() {}

    /** @brief 应用视图销毁 (返回桌面或完全退出) */
    virtual void OnDestroy() {}

    /** @brief 注册该应用对大模型开放的 MCP 工具 */
    virtual void RegisterMcpTools() {}
};

/**
 * @brief 标准应用模块实现类 (StandardAppModule)
 * 规范化封装函数指针, 方便各业务屏零阻碍升级为面向对象 AppModule
 */
class StandardAppModule : public AppModule {
public:
    using NameFunc = const char* (*)();
    using CreateFunc = lv_obj_t* (*)();
    using McpFunc = void (*)();

    StandardAppModule(const char* id, const char* icon_path, NameFunc name_fn, CreateFunc create_fn, McpFunc mcp_fn = nullptr, bool mandatory = false)
        : id_(id), icon_path_(icon_path), name_fn_(name_fn), create_fn_(create_fn), mcp_fn_(mcp_fn), mandatory_(mandatory) {}

    const char* GetId() const override { return id_; }
    const char* GetName() const override { return name_fn_ ? name_fn_() : id_; }
    const char* GetIconPath() const override { return icon_path_; }
    bool IsMandatory() const override { return mandatory_; }
    lv_obj_t* CreateUI(lv_obj_t* /*parent*/ = nullptr) override { return create_fn_ ? create_fn_() : nullptr; }
    ScreenFactory GetScreenFactory() const override { return create_fn_; }
    void RegisterMcpTools() override { if (mcp_fn_) mcp_fn_(); }

private:
    const char* id_;
    const char* icon_path_;
    NameFunc name_fn_;
    CreateFunc create_fn_;
    McpFunc mcp_fn_;
    bool mandatory_;
};
