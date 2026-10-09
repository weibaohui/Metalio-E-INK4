// main/services/calendar/calendar_mcp.cc —— 日历 MCP 工具(需求一.4)。
// 远端 MCP 直接推送/同步日程到设备(SD 卡存储;SD 走 SDMMC 不关 flash cache,
// 故 MCP 任务上下文里写 SD 安全 —— 与 NVS 不同,无需改栈)。
#include "calendar_mcp.h"

#include <cstdio>
#include <ctime>
#include <string>

#include <esp_log.h>

#include "calendar_store.h"
#include "calendar_ics.h"
#include "mcp_server.h"

namespace {
constexpr const char* TAG = "CalendarMcp";

std::string FormatEvent(const CalendarEvent& e) {
    return e.date + (e.time.empty() ? std::string(" ") : " " + e.time + " ") + e.title;
}
}  // namespace

void CalendarRegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool("self.calendar.add_event",
        "Add a calendar/schedule event to the device. Args: date(YYYY-MM-DD), title, optional time(HH:MM, omit for all-day).",
        PropertyList({Property("date", kPropertyTypeString),
                      Property("title", kPropertyTypeString),
                      Property("time", kPropertyTypeString, std::string(""))}),
        [](const PropertyList& p) -> ReturnValue {
            const std::string date = p["date"].value<std::string>();
            const std::string title = p["title"].value<std::string>();
            const std::string time = p["time"].value<std::string>();
            if (!Calendar_AddEvent(date, time, title)) {
                return std::string("参数非法:date 需 YYYY-MM-DD,time 需 HH:MM 或留空,title 非空");
            }
            return std::string("已添加日程:") + date + " " + (time.empty() ? "" : time + " ") + title;
        });

    mcp.AddTool("self.calendar.add_event_today",
        "Add a calendar event for today. Args: title, optional time(HH:MM).",
        PropertyList({Property("title", kPropertyTypeString),
                      Property("time", kPropertyTypeString, std::string(""))}),
        [](const PropertyList& p) -> ReturnValue {
            const std::string today = Calendar_Today();
            if (today.empty()) return std::string("设备时间未对时,无法确定今天;请先联网对时");
            const std::string title = p["title"].value<std::string>();
            const std::string time = p["time"].value<std::string>();
            if (!Calendar_AddEvent(today, time, title)) return std::string("参数非法");
            return std::string("已添加今日日程:") + today + " " +
                   (time.empty() ? "" : time + " ") + title;
        });

    mcp.AddTool("self.calendar.list_events",
        "List calendar events. Optional arg: date(YYYY-MM-DD) to list one day; omit for all.",
        PropertyList({Property("date", kPropertyTypeString, std::string(""))}),
        [](const PropertyList& p) -> ReturnValue {
            const std::string date = p["date"].value<std::string>();
            const auto events = date.empty() ? Calendar_GetAll() : Calendar_GetForDate(date);
            if (events.empty()) return std::string(date.empty() ? "暂无日程" : date + " 无日程");
            std::string out = std::string("共 ") + std::to_string(events.size()) + " 条:";
            for (const auto& e : events) out += "\n" + FormatEvent(e);
            return out;
        });

    mcp.AddTool("self.calendar.today",
        "Get today's date and its schedule.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            const std::string today = Calendar_Today();
            if (today.empty()) return std::string("设备时间未对时");
            const auto events = Calendar_GetForDate(today);
            std::string out = "今天 " + today + ",共 " + std::to_string(events.size()) + " 条日程";
            for (const auto& e : events) out += "\n" + FormatEvent(e);
            return out;
        });

    mcp.AddTool("self.calendar.remove_date",
        "Remove all events on a date. Arg: date(YYYY-MM-DD).",
        PropertyList({Property("date", kPropertyTypeString)}),
        [](const PropertyList& p) -> ReturnValue {
            const int n = Calendar_RemoveForDate(p["date"].value<std::string>());
            return std::string("已删除 ") + std::to_string(n) + " 条";
        });

    mcp.AddTool("self.calendar.clear",
        "Clear all calendar events on the device.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            const int n = Calendar_ClearAll();
            return std::string("已清空全部日程(") + std::to_string(n) + " 条)";
        });

    // ---- ICS 日历订阅(如苹果法定节假日 https://calendars.icloud.com/holidays/cn_zh.ics) ----

    mcp.AddTool("self.calendar.subscribe_ics",
        "Subscribe the device calendar to an iCalendar(.ics) URL, e.g. Apple holidays "
        "https://calendars.icloud.com/holidays/cn_zh.ics. Events are fetched in background, "
        "saved to SD card and shown in the calendar app. Arg: url.",
        PropertyList({Property("url", kPropertyTypeString)}),
        [](const PropertyList& p) -> ReturnValue {
            return CalendarIcs_Subscribe(p["url"].value<std::string>());
        });

    mcp.AddTool("self.calendar.unsubscribe_ics",
        "Unsubscribe the ICS calendar and remove its events (manually added events are kept).",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            return CalendarIcs_Unsubscribe();
        });

    mcp.AddTool("self.calendar.refresh_ics",
        "Re-fetch the subscribed ICS calendar now (background).",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            std::string msg;
            CalendarIcs_Refresh(&msg);
            return msg;
        });

    mcp.AddTool("self.calendar.ics_status",
        "Get ICS subscription status: url, syncing state, event count, last sync time, last error.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            const CalendarIcsStatus st = CalendarIcs_GetStatus();
            if (st.url.empty()) return std::string("未订阅日历;用 self.calendar.subscribe_ics 设置 URL");
            std::string out = "订阅:" + st.url;
            out += std::string("\n状态:") + (st.fetching ? "同步中" : "空闲");
            out += "\n订阅日程:" + std::to_string(st.event_count) + " 条";
            if (st.last_fetched > 0) {
                struct tm tmv = {};
                localtime_r(&st.last_fetched, &tmv);
                char b[40];
                snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d",
                         tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min);
                out += std::string("\n最近同步:") + b;
            } else {
                out += "\n最近同步:从未";
            }
            if (!st.last_error.empty()) out += "\n最近错误:" + st.last_error;
            return out;
        });

    ESP_LOGI(TAG, "calendar MCP tools registered");
}
