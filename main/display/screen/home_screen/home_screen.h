#pragma once

#include "lvgl.h"

// 首页：状态栏 + Hero + 应用宫格；支持多种卡片样式。
class HomeScreen {
public:
    static constexpr int kCardStyleWhite = 0;
    static constexpr int kCardStyleGray = 1;
    static constexpr int kCardStyleBorder = 2;
    static constexpr int kCardStyleSlash = 3;

    static lv_obj_t* Create();

    // 读写首页卡片样式，非法值回落到默认样式。
    static int LoadCardStyle();
    static void SaveCardStyle(int style);

    // ---- APP 开关(需求一.3):只显示开启的 APP;未开启不渲染(本就按需 Create,不常驻) ----
    // boot 期在 main 任务调用一次,把 NVS 开关载入 RAM 缓存(LVGL worker 禁直接读 flash)。
    static void InitAppToggles();
    // 设置页枚举/开关用:
    static int GetAppToggleCount();                    // 可开关的 APP 数(不含永驻的"设置")
    static const char* GetAppToggleName(int index);    // 第 index 个可开关 APP 的显示名
    static bool IsAppEnabledByIndex(int index);        // 是否开启
    static bool IsAppMandatoryByIndex(int index);      // 是否永驻("设置")
    static void SetAppEnabled(int index, bool enabled);// 更新缓存+落盘(main)+刷新首页
};
