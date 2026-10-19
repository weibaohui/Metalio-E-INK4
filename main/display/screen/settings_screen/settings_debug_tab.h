#pragma once
// 设置 → 调试:调试相关配置统一入口(issue 之外的日常运维需求)。
// - syslog 日志转发:开关 + 单播/广播切换(host/端口修改走 self.syslog.configure)
// - 系统监控打印间隔:关/1s/5s/30s/60s
// - 内存报告:立即打印(每任务占用 + 堆全景,结果在串口/日志)
// 内容较多,墨水屏滚动慢:按节分 3 页,盖板键 vk_prev/vk_next 翻页。
#include "lvgl.h"

void SettingsDebugTab_Build(lv_obj_t* page);
void SettingsDebugTab_Reset();
// vk 翻页入口(在 OnSettingsVkKey 内调用)。feed 任务上下文,内部只改页码,
// 渲染投递 main 任务。返回 true 表示按键已消费。
bool SettingsDebugTab_OnVkKey(const char* key);
