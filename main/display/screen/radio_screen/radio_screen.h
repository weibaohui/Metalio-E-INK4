#pragma once
// main/display/screen/radio_screen/radio_screen.h
#include <lvgl.h>

class RadioScreen {
public:
    static lv_obj_t* Create();
    // 收音机屏是否活跃(供主板判断:活跃时实体音量键改切上一台/下一台,不再调音量)。
    static bool IsActive();
};
