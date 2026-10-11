#pragma once
// main/services/mem_report/mem_report.h —— 每任务内存占用报告。
//
// 内部 RAM 是全系统瓶颈(空闲常年 ~40KB,TLS 握手要看"最大连续块"),但
// heap_caps_print_heap_info 只给区域汇总,回答不了"这 400KB 都谁用了"。
// 本服务基于 CONFIG_HEAP_TASK_TRACKING 的 heap_caps_get_all_task_stat,
// 按任务聚合内部/PSRAM 占用,排序后输出,并注册 self.mem.tasks MCP 工具
// 供随时查询(本地 HTTP MCP :8080/mcp 或助手)。
//
// 注意:开销是每个堆块头部多一个任务指针(数百块约 2KB),换取永久可观测。

#include <string>

// 生成每任务内存占用报告:按内部 RAM 当前占用降序,含峰值/块数/PSRAM。
// to_serial=true 时同步逐行 ESP_LOGI(日志里也留档)。
std::string MemReport_PerTask(bool to_serial);

// 注册 self.mem.tasks MCP 工具(无参数,返回报告文本)。
void MemReport_RegisterMcpTools();
