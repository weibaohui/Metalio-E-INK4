// main/services/calendar/calendar_store.cc —— 见 calendar_store.h(需求一.4)。
#include "calendar_store.h"

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

std::mutex s_mutex;
std::vector<CalendarEvent> s_events;   // RAM 缓存(读全走这里)
bool s_loaded = false;

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

std::string Serialize() {
    cJSON* root = cJSON_CreateArray();
    for (const auto& e : s_events) {
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

void ParseInto(const std::string& json) {
    s_events.clear();
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
        if (ValidDate(e.date)) s_events.push_back(e);
    }
    cJSON_Delete(root);
}

// 原子写:tmp + rename(SD;无卡返回 false 但缓存已更新)。调用方须持 s_mutex。
bool PersistLocked() {
    if (!SdReady()) {
        ESP_LOGW(TAG, "无 SD 卡,日程仅存 RAM(重启不保留)");
        return false;
    }
    if (!SdEnsureAppLayout() || !SdEnsureDir(SD_PATH_CALENDAR)) {
        ESP_LOGW(TAG, "calendar 目录创建失败");
        return false;
    }
    const std::string body = Serialize();
    FILE* f = fopen(kEventsTmp, "wb");
    if (f == nullptr) {
        ESP_LOGW(TAG, "open tmp failed errno=%d", errno);
        return false;
    }
    const size_t w = fwrite(body.data(), 1, body.size(), f);
    fflush(f);
    fclose(f);
    if (w != body.size()) {
        unlink(kEventsTmp);
        return false;
    }
    unlink(kEventsFile);
    if (rename(kEventsTmp, kEventsFile) != 0) {
        ESP_LOGW(TAG, "rename failed errno=%d", errno);
        unlink(kEventsTmp);
        return false;
    }
    return true;
}

void LoadLocked() {
    if (s_loaded) return;
    s_loaded = true;
    if (!SdReady()) {
        ESP_LOGW(TAG, "无 SD 卡,日程从空开始(仅 RAM)");
        return;
    }
    struct stat st;
    if (stat(kEventsFile, &st) != 0 || st.st_size <= 0 || st.st_size > 256 * 1024) {
        return;   // 无文件或异常:空
    }
    char* buf = static_cast<char*>(heap_caps_malloc(st.st_size + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buf == nullptr) buf = static_cast<char*>(malloc(st.st_size + 1));
    if (buf == nullptr) return;
    FILE* f = fopen(kEventsFile, "rb");
    if (f != nullptr) {
        const size_t r = fread(buf, 1, st.st_size, f);
        fclose(f);
        buf[r] = '\0';
        ParseInto(buf);
    }
    free(buf);
    ESP_LOGI(TAG, "载入日程 %d 条", (int)s_events.size());
}

}  // namespace

void CalendarStore_EnsureLoaded() {
    std::lock_guard<std::mutex> lock(s_mutex);
    LoadLocked();
}

bool Calendar_AddEvent(const std::string& date, const std::string& time, const std::string& title) {
    if (!ValidDate(date) || !ValidTime(time) || title.empty()) return false;
    std::lock_guard<std::mutex> lock(s_mutex);
    LoadLocked();
    CalendarEvent e{date, time, title};
    s_events.push_back(e);
    PersistLocked();
    ESP_LOGI(TAG, "新增日程 %s %s %s", date.c_str(), time.c_str(), title.c_str());
    return true;
}

int Calendar_ClearAll() {
    std::lock_guard<std::mutex> lock(s_mutex);
    LoadLocked();
    const int n = (int)s_events.size();
    s_events.clear();
    PersistLocked();
    return n;
}

int Calendar_RemoveForDate(const std::string& date) {
    if (!ValidDate(date)) return 0;
    std::lock_guard<std::mutex> lock(s_mutex);
    LoadLocked();
    int removed = 0;
    for (auto it = s_events.begin(); it != s_events.end();) {
        if (it->date == date) { it = s_events.erase(it); ++removed; }
        else ++it;
    }
    if (removed > 0) PersistLocked();
    return removed;
}

std::vector<CalendarEvent> Calendar_GetAll() {
    std::lock_guard<std::mutex> lock(s_mutex);
    LoadLocked();
    return s_events;
}

std::vector<CalendarEvent> Calendar_GetForDate(const std::string& date) {
    std::lock_guard<std::mutex> lock(s_mutex);
    LoadLocked();
    std::vector<CalendarEvent> out;
    for (const auto& e : s_events) {
        if (e.date == date) out.push_back(e);
    }
    return out;
}

bool Calendar_HasEvent(const std::string& date) {
    std::lock_guard<std::mutex> lock(s_mutex);
    LoadLocked();
    for (const auto& e : s_events) {
        if (e.date == date) return true;
    }
    return false;
}

int Calendar_Count() {
    std::lock_guard<std::mutex> lock(s_mutex);
    LoadLocked();
    return (int)s_events.size();
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

bool Calendar_HasValidDate() { return !Calendar_Today().empty(); }
