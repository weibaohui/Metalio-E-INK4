#pragma once
// main/services/calendar/calendar_store.h —— 日历日程存储(需求一.4)。
//
// 事件存 SD 卡 JSON(/sdcard/metalio/e-ink/calendar/events.json),原子写(tmp+rename)。
// SD 走 SPI/SDMMC,不关 flash cache —— 与 NVS 不同,可在任意任务(含 MCP 任务、LVGL
// worker)直接读写。无卡时降级为"仅 RAM"(重启不保留),不崩机。
// RAM 缓存:读全走缓存;写(Add/Clear)先更新缓存再原子落盘。互斥保护,多任务可调。

#include <string>
#include <vector>

struct CalendarEvent {
    std::string date;    // "YYYY-MM-DD"
    std::string time;    // "HH:MM" 或空(全天)
    std::string title;   // 日程/会议标题
    bool from_ics = false;  // true=订阅日历(ics)导入;false=手动/MCP 添加
};

// 首次访问时自动 mkdir + 从 SD 载入 RAM 缓存(幂等,任意任务可调)。
void CalendarStore_EnsureLoaded();

// 写操作:更新缓存 + 原子落盘(SD;无卡则仅 RAM)。返回 false=参数非法。
bool Calendar_AddEvent(const std::string& date, const std::string& time, const std::string& title);
int  Calendar_ClearAll();                 // 返回清掉的条数
int  Calendar_RemoveForDate(const std::string& date);  // 返回删掉的条数

// 订阅日历(ics)分组:整体替换/清空,独立 SD 文件(ics_events.json),
// 与手动日程互不干扰;读操作自动合并两个分组。返回替换后的条数。
int  Calendar_ReplaceIcsEvents(std::vector<CalendarEvent>&& events);
int  Calendar_ClearIcsEvents();
int  Calendar_IcsCount();

// 读操作(走 RAM 缓存;手动日程在前,订阅日程在后,各自按 date+time 有序)
std::vector<CalendarEvent> Calendar_GetAll();
std::vector<CalendarEvent> Calendar_GetForDate(const std::string& date);
bool Calendar_HasEvent(const std::string& date);
int  Calendar_Count();

// 今天 "YYYY-MM-DD"(基于本机 civil 时间 localtime)。未对时返回空串。
std::string Calendar_Today();
// 是否已有有效日期(已开机对时/RTC 有效)。
bool Calendar_HasValidDate();
