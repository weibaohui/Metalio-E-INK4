#pragma once

#include "lvgl.h"

// 设置 → 应用:首页 APP 显示开关(需求一.3)。关闭的 APP 不在首页显示。
void SettingsAppsTab_Build(lv_obj_t* page);
void SettingsAppsTab_Reset();
