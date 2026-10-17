// main/services/dynamic_apps/dynamic_app_abi.h
// 动态应用 (.so) 与固件之间的稳定 C ABI（本头文件同时被固件与 .so 应用工程引用）。
//
// 应用侧约定：
//   extern "C" void metalio_app_entry(metalio_app_desc_t* out);
//   填写 desc 后返回；固件 dlopen + dlsym("metalio_app_entry") 取出。
//
// 约束：
//   - desc 全 POD / C 字符串字面量，不传 C++ 对象、不传需释放的指针；
//   - entry 必须无副作用：开机扫描（读元数据）与每次点开都会调用，仅填 desc；
//   - create 仅在点开时调用，返回 lv_obj_create(NULL) 新建的屏幕；
//   - 惰性生命周期：退出应用（屏幕销毁）后固件 dlclose 释放 .so 代码段——
//     应用的定时器/任务必须在屏幕 LV_EVENT_DELETE 里自行清理，跨屏幕的
//     后台定时器/任务存活即悬空（需要后台常驻能力的应用不适用本模型）；
//   - .so 与固件构建同版本强耦合（符号表随固件地址变化，升级后 .so 需重编）。
#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define METALIO_DYNAPP_ABI_VERSION 1

typedef struct {
    uint32_t abi_version;           ///< 必须 = METALIO_DYNAPP_ABI_VERSION，固件校验不匹配则拒绝
    const char* id;                 ///< 应用短标识 (<=15 字符，进 NVS 开关键 en_<id>)
    const char* name;               ///< 桌面显示名
    const char* icon;               ///< 图标资产路径（可复用内置，如 "A:ic_app_home_clock.spng"）
    void* (*create)(void);          ///< 创建 LVGL 屏幕，返回根对象 (lv_obj_t*)；仅用固件符号表内的 API
} metalio_app_desc_t;

#ifdef __cplusplus
}
#endif
