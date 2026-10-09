#pragma once
// main/services/syslog_sender.h —— SYSLOG(RFC3164)日志上报(需求一.2)。
//
// 开启后,把设备 esp_log 日志按 SYSLOG 协议经 UDP 发到指定 IP。仅经 MCP 配置
// (暂不做界面)。复用 esp_debug_cloud 的"vprintf 钩子 + 字节环 + 低优先级 UDP 发送
// 任务 + IP 事件门控"结构,但改为运行时 NVS 配置、可启停。
//
// 线程/安全约束(沿用 esp_debug_cloud 的教训):
//  - vprintf 钩子内禁止 malloc / FreeRTOS 队列;只栈上短缓冲 + 环形缓冲(自旋锁极短临界)。
//  - ISR/重入直接旁路走原 vprintf。
//  - 目标仅支持 IPv4 字面量(inet_pton),主机名需 DNS,不做以免小栈阻塞。
//  - NVS 读写只在 main 任务(MCP 工具回调经 Application::Schedule 在 main 执行)。

#include <string>

// 开机调用一次(main 任务):读 NVS 配置;若已启用且配了 IP 则启动。
void Syslog_Init();

// 注册 MCP 工具(self.syslog.configure / enable / status)。在 main 任务调用一次。
void SyslogRegisterMcpTools();

// MCP 配置(在 main 任务执行)。host 仅 IPv4 字面量(x.x.x.x)。
void Syslog_Configure(const std::string& host, int port);  // port<=0 用默认 514
void Syslog_SetEnabled(bool enable);

// 状态查询
bool Syslog_IsEnabled();
bool Syslog_IsRunning();        // 钩子已装 + 发送任务在跑
std::string Syslog_GetHost();
int Syslog_GetPort();
