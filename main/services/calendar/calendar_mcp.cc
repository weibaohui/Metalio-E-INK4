// main/services/calendar/calendar_mcp.cc —— 日历 MCP 工具(需求一.4)。
// 远端 MCP 直接推送/同步日程到设备(SD 卡存储;SD 走 SDMMC 不关 flash cache,
// 故 MCP 任务上下文里写 SD 安全 —— 与 NVS 不同,无需改栈)。
#include "calendar_mcp.h"

#include <cstdio>
#include <string>

#include <esp_log.h>

#include "calendar_store.h"
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

    ESP_LOGI(TAG, "calendar MCP tools registered");
}
