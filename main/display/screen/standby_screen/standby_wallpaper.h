#pragma once

#include "lvgl.h"

namespace standby_wallpaper {

/** 在 root 上构建白底 + 图片控件；不读 NVS */
void Build(lv_obj_t* root);

/**
 * 后台解析待机壁纸路径并解码，经 lv_async 挂到图片。
 * 须在 Build 之后调用；Dismiss/Teardown 后结果按 epoch 丢弃。
 */
void StartLoad();

/** 解码完成（成功或失败） */
bool IsContentReady();

/** 拆掉图片引用并释放像素；可重复调用 */
void Teardown();

}  // namespace standby_wallpaper
