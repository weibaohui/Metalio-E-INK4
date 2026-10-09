#pragma once
// main/display/screen/power_screen/power_screen.h —— 电源管理 APP(新需求)。
// 三个按钮:息屏 / 重启 / 关机。重启与关机需二次确认(防误触);息屏即时。
// 纯 UI,无后台常驻;返回即退出销毁。
#include <lvgl.h>

class PowerScreen {
public:
    static lv_obj_t* Create();
};
