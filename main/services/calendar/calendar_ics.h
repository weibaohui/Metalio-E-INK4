#pragma once
// main/services/calendar/calendar_ics.h —— ICS 日历订阅(通过 MCP 设置 URL)。
//
// 订阅一个 iCalendar(.ics)URL(如 https://calendars.icloud.com/holidays/cn_zh.ics):
// 后台任务下载并解析 VEVENT(全天/定时/跨天展开),结果写入日历存储的"订阅分组"
// (独立 SD 文件,与手动日程互不干扰),日历屏照常显示。
//
// 内存/栈约束(全仓库统一):
// - NVS 只允许在 main 任务/短内部栈任务读写 → Init 在 boot(main 任务)调用;
//   订阅 URL / 最近同步时间的落盘一律经 Application::Schedule 转 main 任务。
// - 下载解析工作线程用 PSRAM 栈(与 radio/GLM 同例),只碰 SD 与 RAM,不碰 NVS/flash。
// - 不向订阅 URL 携带任何云端鉴权头(第三方服务器,防 token 外泄)。

#include <ctime>
#include <string>

struct CalendarIcsStatus {
    std::string url;        // 已订阅 URL(空=未订阅)
    bool fetching = false;  // 后台同步进行中
    int event_count = 0;    // 当前订阅分组日程条数(跨天展开后)
    time_t last_fetched = 0;// 最近成功同步时刻(0=从未)
    std::string last_error; // 最近一次失败原因(成功即清空)
};

// boot 期(main 任务)调用:载入 NVS 中的订阅 URL / 最近同步时间。幂等。
void CalendarIcs_Init();

// 订阅/退订。Subscribe 校验 URL 后立刻后台同步;返回给人看的提示串。
// 可在 MCP 任务/任意任务调用(内部不做 NVS,落盘走 Schedule)。
std::string CalendarIcs_Subscribe(const std::string& url);
std::string CalendarIcs_Unsubscribe();

// 手动触发同步。已在做/未订阅/无网络时返回 false 并在 out_msg 说明。
bool CalendarIcs_Refresh(std::string* out_msg = nullptr);

// 订阅且数据过期(>12h 或从未成功)时触发后台同步;否则 no-op。日历屏进入时调。
void CalendarIcs_MaybeAutoRefresh();

CalendarIcsStatus CalendarIcs_GetStatus();

// 订阅分组变化回调(在后台工作线程上触发;UI 侧须自行 ScreenLvAsync 转 LVGL 线程)。
// 同时只保留一个回调:日历屏 Create 注册、Unload 清掉。
void CalendarIcs_SetChangedCallback(void (*cb)());
