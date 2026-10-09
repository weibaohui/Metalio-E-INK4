#pragma once
// main/display/screen/time_sync_screen/time_sync_screen.h —— 独立"校时"应用(需求一.1)。
// 内存精贵:做成按需加载的独立 APP —— 进屏即触发 SNTP 对时、大字显示当前时间/状态,
// 返回即退出销毁,不驻留内存。时区/手动校准也收在这里(取代设置里的"时间"标签)。
#include <lvgl.h>

class TimeSyncScreen {
public:
    static lv_obj_t* Create();
};
