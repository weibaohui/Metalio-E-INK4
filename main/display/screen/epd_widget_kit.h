// main/display/screen/epd_widget_kit.h
// 墨水屏标准 UI 控件库 (EpdWidgetKit):
// 专为 480x800 墨水屏 (E-Ink) 设计的高对比度、零动效消耗、抗残影组件库。
// 帮助后续新 App 快速以声明式搭积木的方式完成界面开发, 统一整机视觉与交互规范。
#pragma once

#include <lvgl.h>
#include <functional>
#include <string>

namespace EpdWidgetKit {

/**
 * @brief 墨水屏标准顶栏组件 (包含返回键、主标题与右侧可选操作区)
 */
class EpdHeader {
public:
    static lv_obj_t* Create(lv_obj_t* parent, const char* title, lv_event_cb_t on_back = nullptr);
};

/**
 * @brief 墨水屏标准列表条目组件 (Cell)
 */
class EpdCell {
public:
    /** @brief 文本展示/导航条目 (左侧标题, 右侧数值或箭头) */
    static lv_obj_t* CreateText(lv_obj_t* parent, const char* title, const char* value_text, lv_event_cb_t on_click = nullptr);

    /** @brief 开关配置条目 (左侧标题, 右侧 Switch 开关) */
    static lv_obj_t* CreateSwitch(lv_obj_t* parent, const char* title, bool initial_state, lv_event_cb_t on_toggle = nullptr);
};

/**
 * @brief 墨水屏轻量提示小浮窗 (Toast)
 */
class EpdToast {
public:
    static void Show(const char* message, uint32_t duration_ms = 2000);
};

/**
 * @brief 墨水屏标准二选一确认对话框 (Dialog)
 */
class EpdDialog {
public:
    using ResultCallback = std::function<void(bool confirmed)>;
    static void Show(const char* title, const char* message, const char* confirm_text, ResultCallback on_result);
};

} // namespace EpdWidgetKit
