#pragma once
// main/services/time_service.h —— 网络对时 + 时区 + 手动校准(需求一.1)。
//
// 模型说明:本机沿用既有"本地时刻作为 epoch"约定(TZ 实为 GMT+0,epoch=本地时间),
// RTC(PCF8563)与状态栏 localtime 都按此工作。本模块不改动 OTA/RTC/状态栏的既有
// 语义,只把"时间来源"从"OTA 偶发同步"升级为"SNTP 定时同步",并把 UTC 按可配置
// 时区偏移换算成本地 epoch。
//
// 线程:NVS 读写、RTC(I2C)写都必须发生在内部 RAM 栈任务上。Init/后台任务/设置页
// 调度都在 main 任务或本服务的内部栈任务里做;设置页(LVGL worker,PSRAM 栈)只调
// 这些公开接口(它们内部已调度/委托,不直接碰 flash)。

#include <string>

// 开机初始化(读 NVS 时区偏移 + 起后台对时任务)。在 main 任务调用一次。
void TimeService_Init();

// 立即触发一次 SNTP 对时(网络未就绪则排队等网络)。异步,立即返回。
void TimeService_SyncNow();

// 时区偏移(分钟,相对 UTC;默认 480 = UTC+8)。
int  TimeService_GetTzOffsetMin();
// 设置时区偏移(分钟)。存 NVS;并触发一次重新对时让显示立刻按新区偏移。
void TimeService_SetTzOffsetMin(int minutes);

// 是否已成功对时过(本次开机以来)。
bool TimeService_IsSynced();
// 最近一次成功对时的本地 epoch;0 = 尚未对时。
long TimeService_LastSyncEpoch();

// 手动校准:直接设定本地日期时间(写系统 + RTC 芯片)。用于无网/纠正漂移。
// 月份 1-12,日 1-31,时 0-23,分 0-59。返回 false=参数非法。
bool TimeService_SetManual(int year, int month, int day, int hour, int minute);

// 当前本地时间的可读字符串 "YYYY-MM-DD HH:MM"(未对时返回 "--")。
std::string TimeService_NowText();
