#pragma once
// main/boards/metalio-e-ink-4/board_debug.h —— 调试运行时配置入口(设置→调试页)。
//
// 系统监控(sys_mon 任务)的打印间隔运行时可调:NVS "dbg/mon_iv"(秒,0=关)。
// SetMonitorInterval 含 NVS 写,必须在 main 任务调用(UI 页经 Schedule 投递)。

void BoardDebug_SetMonitorInterval(int sec);
int BoardDebug_GetMonitorInterval();
