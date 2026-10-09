#pragma once
// main/display/screen/calendar_screen/calendar_screen.h —— 日历 APP(需求一.4)。
// 月视图/周视图切换,显示每天日程;日程经远端 MCP 推送同步(calendar_store)。
#include <lvgl.h>

class CalendarScreen {
public:
    static lv_obj_t* Create();
};
