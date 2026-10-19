// main/services/calendar/calendar_store.cc —— 见 calendar_store.h(需求一.4)。
//
// 并发模型(2026-10-19 重构):s_mutex 只保护 RAM 缓存与序列化,SD I/O 一律
// 移到锁外由 s_io_mutex 串行化。此前 Persist* 在 s_mutex 内写 SD,日历 UI
// 线程(逐格 has_event)与订阅刷新任务(FetchWorker 整组替换)谁赶上 SD 慢,
// 就把 LVGL worker 一起冻住(实测翻页即死机)。序号(s_*_seq)防旧快照覆盖:
// 锁内每次修改递增,写 SD 前发现序号已变即放弃(更新者会带最新数据重写)。
#include "calendar_store.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <sys/stat.h>

#include <cJSON.h>

#include "SdCardManager.hpp"
#include "sd_paths.h"

namespace {

constexpr const char* TAG = "CalendarStore";
constexpr const char* kEventsFile = SD_PATH_CALENDAR "/events.json";
constexpr const char* kEventsTmp  = SD_PATH_CALENDAR "/events.tmp";
// 订阅日历(ics)分组独立文件:整组替换,与手动日程互不干扰
constexpr const char* kIcsFile = SD_PATH_CALENDAR "/ics_events.json";
constexpr const char* kIcsTmp  = SD_PATH_CALENDAR "/ics_events.tmp";

std::mutex s_mutex;                       // 保护 RAM 缓存与序号(短临界区,无 I/O)
std::mutex s_io_mutex;                    // 串行化 SD 写(tmp+rename 不交错)
std::vector<CalendarEvent> s_events;      // 手动/MCP 日程 RAM 缓存(读全走这里)
std::vector<CalendarEvent> s_ics_events;  // 订阅日历(ics)日程缓存
bool s_loaded = false;
uint64_t s_events_seq = 0;                // 手动日程每次 RAM 修改 +1
uint64_t s_ics_seq = 0;                   // 订阅日程每次 RAM 修改 +1

bool SdReady() { return SdCardManager::GetInstance().IsMounted(); }

// 校验 "YYYY-MM-DD"
bool ValidDate(const std::string& d) {
    if (d.size() != 10 || d[4] != '-' || d[7] != '-') return false;
    for (size_t i = 0; i < d.size(); ++i) {
        if (i == 4 || i == 7) continue;
        if (d[i] < '0' || d[i] > '9') return false;
    }
    return true;
}
// 校验 "HH:MM" 或空(全天)
bool ValidTime(const std::string& t) {
    if (t.empty()) return true;
    if (t.size() != 5 || t[2] != ':') return false;
    for (size_t i = 0; i < t.size(); ++i) {
        if (i == 2) continue;
        if (t[i] < '0' || t[i] > '9') return false;
    }
    return true;
}

std::string Serialize(const std::vector<CalendarEvent>& events) {
    cJSON* root = cJSON_CreateArray();
    for (const auto& e : events) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "date", e.date.c_str());
        cJSON_AddStringToObject(item, "time", e.time.c_str());
        cJSON_AddStringToObject(item, "title", e.title.c_str());
        cJSON_AddItemToArray(root, item);
    }
    char* printed = cJSON_PrintUnformatted(root);
    std::string out = printed != nullptr ? printed : "[]";
    if (printed != nullptr) cJSON_free(printed);
    cJSON_Delete(root);
    return out;
}

void ParseInto(const std::string& json, std::vector<CalendarEvent>& out, bool from_ics) {
    out.clear();
    cJSON* root = cJSON_Parse(json.c_str());
    if (root == nullptr || !cJSON_IsArray(root)) {
        if (root != nullptr) cJSON_Delete(root);
        return;
    }
    const cJSON* item = nullptr;
    cJSON_ArrayForEach(item, root) {
        const cJSON* date = cJSON_GetObjectItem(item, "date");
        const cJSON* time = cJSON_GetObjectItem(item, "time");
        const cJSON* title = cJSON_GetObjectItem(item, "title");
        if (!cJSON_IsString(date) || !cJSON_IsString(title)) continue;
        CalendarEvent e;
        e.date = date->valuestring;
        e.time = cJSON_IsString(time) ? time->valuestring : "";
        e.title = title->valuestring;
        e.from_ics = from_ics;
        if (ValidDate(e.date)) out.push_back(e);
    }
    cJSON_Delete(root);
}

// SD 写(调用方不得持 s_mutex;s_io_mutex 串行化,tmp+rename 原子落盘)。
// expect_seq:调用方锁内修改时的序号快照;不匹配说明已有更新版本,
// 本轮旧快照放弃落盘(防止覆盖后写者的新数据)。
bool WriteBodyTo(uint64_t expect_seq, uint64_t& current_seq,
                 const char* tmp_path, const char* file_path, const std::string& body) {
    std::lock_guard<std::mutex> io_lock(s_io_mutex);
    if (current_seq != expect_seq) {
        return true;   // 已被更新版本取代:跳过旧快照,不算失败
    }
    if (!SdReady()) {
        ESP_LOGW(TAG, "无 SD 卡,日程仅存 RAM(重启不保留)");
        return false;
    }
    if (!SdEnsureAppLayout() || !SdEnsureDir(SD_PATH_CALENDAR)) {
        ESP_LOGW(TAG, "calendar 目录创建失败");
        return false;
    }
    FILE* f = fopen(tmp_path, "wb");
    if (f == nullptr) {
        ESP_LOGW(TAG, "open tmp failed errno=%d", errno);
        return false;
    }
    const size_t w = fwrite(body.data(), 1, body.size(), f);
    fflush(f);
    fclose(f);
    if (w != body.size()) {
        unlink(tmp_path);
        return false;
    }
    unlink(file_path);
    if (rename(tmp_path, file_path) != 0) {
        ESP_LOGW(TAG, "rename failed errno=%d", errno);
        unlink(tmp_path);
        return false;
    }
    return true;
}

void LoadFileLocked(const char* path, std::vector<CalendarEvent>& out, bool from_ics) {
    struct stat st;
    if (stat(path, &st) != 0 || st.st_size <= 0 || st.st_size > 384 * 1024) {
        return;   // 无文件或异常:空
    }
    char* buf = static_cast<char*>(heap_caps_malloc(st.st_size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buf == nullptr) buf = static_cast<char*>(malloc(st.st_size + 1));
    if (buf == nullptr) return;
    FILE* f = fopen(path, "rb");
    if (f != nullptr) {
        const size_t r = fread(buf, 1, st.st_size, f);
        fclose(f);
        buf[r] = '\0';
        ParseInto(buf, out, from_ics);
    }
    free(buf);
}

void LoadLocked() {
    if (s_loaded) return;
    s_loaded = true;
    if (!SdReady()) {
        ESP_LOGW(TAG, "无 SD 卡,日程从空开始(仅 RAM)");
        return;
    }
    LoadFileLocked(kEventsFile, s_events, false);
    LoadFileLocked(kIcsFile, s_ics_events, true);
    ESP_LOGI(TAG, "载入日程:手动 %d 条,订阅 %d 条",
             (int)s_events.size(), (int)s_ics_events.size());
}

}  // namespace

void CalendarStore_EnsureLoaded() {
    std::lock_guard<std::mutex> lock(s_mutex);
    LoadLocked();
}

bool Calendar_AddEvent(const std::string& date, const std::string& time, const std::string& title) {
    if (!ValidDate(date) || !ValidTime(time) || title.empty()) return false;
    std::string body;
    uint64_t seq = 0;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        LoadLocked();
        CalendarEvent e{date, time, title};
        s_events.push_back(e);
        body = Serialize(s_events);
        seq = ++s_events_seq;
    }
    const bool ok = WriteBodyTo(seq, s_events_seq, kEventsTmp, kEventsFile, body);
    ESP_LOGI(TAG, "新增日程 %s %s %s (%s)", date.c_str(), time.c_str(), title.c_str(),
             ok ? "已落盘" : "落盘失败");
    return true;   // RAM 已更新,SD 落盘失败不影响本次会话
}

int Calendar_ClearAll() {
    std::string body;
    uint64_t seq = 0;
    int n = 0;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        LoadLocked();
        n = (int)s_events.size();
        s_events.clear();
        body = Serialize(s_events);
        seq = ++s_events_seq;
    }
    WriteBodyTo(seq, s_events_seq, kEventsTmp, kEventsFile, body);
    return n;
}

int Calendar_RemoveForDate(const std::string& date) {
    if (!ValidDate(date)) return 0;
    std::string body;
    uint64_t seq = 0;
    int removed = 0;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        LoadLocked();
        for (auto it = s_events.begin(); it != s_events.end();) {
            if (it->date == date) { it = s_events.erase(it); ++removed; }
            else ++it;
        }
        if (removed > 0) {
            body = Serialize(s_events);
            seq = ++s_events_seq;
        }
    }
    if (removed > 0) {
        WriteBodyTo(seq, s_events_seq, kEventsTmp, kEventsFile, body);
    }
    return removed;
}

int Calendar_ReplaceIcsEvents(std::vector<CalendarEvent>&& events) {
    std::string body;
    uint64_t seq = 0;
    int n = 0;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        LoadLocked();
        for (auto& e : events) e.from_ics = true;
        s_ics_events = std::move(events);
        n = (int)s_ics_events.size();
        body = Serialize(s_ics_events);
        seq = ++s_ics_seq;
    }
    WriteBodyTo(seq, s_ics_seq, kIcsTmp, kIcsFile, body);
    ESP_LOGI(TAG, "订阅日程整组替换:%d 条", n);
    return n;
}

int Calendar_ClearIcsEvents() {
    std::string body;
    uint64_t seq = 0;
    int n = 0;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        LoadLocked();
        n = (int)s_ics_events.size();
        s_ics_events.clear();
        body = Serialize(s_ics_events);
        seq = ++s_ics_seq;
    }
    WriteBodyTo(seq, s_ics_seq, kIcsTmp, kIcsFile, body);
    return n;
}

int Calendar_IcsCount() {
    std::lock_guard<std::mutex> lock(s_mutex);
    LoadLocked();
    return (int)s_ics_events.size();
}

std::vector<CalendarEvent> Calendar_GetAll() {
    std::lock_guard<std::mutex> lock(s_mutex);
    LoadLocked();
    std::vector<CalendarEvent> out = s_events;
    out.insert(out.end(), s_ics_events.begin(), s_ics_events.end());
    return out;
}

std::vector<CalendarEvent> Calendar_GetForDate(const std::string& date) {
    std::lock_guard<std::mutex> lock(s_mutex);
    LoadLocked();
    std::vector<CalendarEvent> out;
    for (const auto& e : s_events) {
        if (e.date == date) out.push_back(e);
    }
    for (const auto& e : s_ics_events) {
        if (e.date == date) out.push_back(e);
    }
    // 展示有序:全天在前,定时按 HH:MM;同刻手动在前
    std::stable_sort(out.begin(), out.end(), [](const CalendarEvent& a, const CalendarEvent& b) {
        return a.time < b.time;
    });
    return out;
}

bool Calendar_HasEvent(const std::string& date) {
    std::lock_guard<std::mutex> lock(s_mutex);
    LoadLocked();
    for (const auto& e : s_events) {
        if (e.date == date) return true;
    }
    for (const auto& e : s_ics_events) {
        if (e.date == date) return true;
    }
    return false;
}

int Calendar_Count() {
    std::lock_guard<std::mutex> lock(s_mutex);
    LoadLocked();
    return (int)(s_events.size() + s_ics_events.size());
}

std::string Calendar_Today() {
    time_t now = time(nullptr);
    struct tm local = {};
    localtime_r(&now, &local);
    if (local.tm_year + 1900 < 2025) return "";   // 未对时
    char buf[40];   // 年份字段理论 11 字节,-Werror 防截断
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
             local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);
    return buf;
}
