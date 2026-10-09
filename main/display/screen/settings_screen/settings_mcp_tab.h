#pragma once

#include "lvgl.h"

// 设置 → MCP:局域网 MCP 6 位数字 PIN 认证(需求一.5)。
// 开关 + 当前 PIN 显示 + 重新生成。
void SettingsMcpTab_Build(lv_obj_t* page);
void SettingsMcpTab_Reset();
