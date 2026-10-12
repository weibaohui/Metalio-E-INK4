#pragma once

#include <lvgl.h>

/**
 * @brief 应用元数据描述符
 *
 * 规范化定义各独立应用的信息与生命周期入口，消除首页和设置页对各 Screen 的直接硬编码依赖。
 */
struct AppDesc {
    const char *id = nullptr;             ///< 稳定短 ID（NVS 键 en_<id>，长度建议 ≤ 15 字符）
    const char *icon_path = nullptr;      ///< 桌面图标资源路径（如 "A:ic_app_home_book.spng"）
    const char *(*name)() = nullptr;      ///< 应用多语言名称回调
    lv_obj_t *(*create)() = nullptr;      ///< UI 界面创建入口
    void (*register_mcp)() = nullptr;     ///< MCP 工具注册回调（开机阶段如果该应用启用则调用）
    bool is_mandatory = false;            ///< 是否为常驻不可关闭应用（如“设置”）
};
