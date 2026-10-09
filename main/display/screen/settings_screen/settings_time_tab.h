#pragma once

#include "lvgl.h"

// 设置 → 时间:网络对时(SNTP)+ 时区选择 + 手动校准(需求一.1)。
void SettingsTimeTab_Build(lv_obj_t* page);
void SettingsTimeTab_Reset();
