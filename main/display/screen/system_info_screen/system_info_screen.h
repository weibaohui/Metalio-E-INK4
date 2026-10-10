#pragma once
// main/display/screen/system_info_screen/system_info_screen.h —— 系统信息 APP。
// 展示设备运行状态:内部 RAM / PSRAM(空闲/总量/最大连续块/历史最低)、Flash 与应用
// 分区、SD 卡容量、NVS 用量与全部键名(按命名空间)、芯片/IDF/运行时长等。
// 内容超屏,整页可上下滑动;"刷新"按钮重新采样。纯 UI,无后台常驻,返回即销毁。
#include <lvgl.h>

class SystemInfoScreen {
public:
    static lv_obj_t* Create();
};
