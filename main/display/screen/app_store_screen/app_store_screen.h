// main/display/screen/app_store_screen/app_store_screen.h
// 应用商店：扫描 SD 卡 apps/ 下的应用包，安装（上桌面）/卸载（下桌面）。
#pragma once

#include "lvgl.h"

class AppStoreScreen {
public:
    static lv_obj_t* Create();
};
