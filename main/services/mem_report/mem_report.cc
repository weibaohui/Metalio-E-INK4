#include "mem_report.h"

#if CONFIG_HEAP_TASK_TRACKING

#include <algorithm>
#include <cstdio>
#include <vector>

#include <esp_heap_caps.h>
#include <esp_heap_task_info.h>
#include <esp_log.h>

#include "mcp_server.h"

static const char* TAG = "MemReport";

namespace {

struct TaskRow {
    char name[configMAX_TASK_NAME_LEN + 1];
    size_t internal;        // 当前内部 RAM 占用(多堆求和)
    size_t internal_peak;   // 内部 RAM 峰值
    size_t blocks;          // 内部分配块数
    size_t psram;           // 当前 PSRAM 占用
};

bool HeapIsPsram(uint32_t caps) {
    return (caps & MALLOC_CAP_SPIRAM) != 0;
}

}  // namespace

std::string MemReport_PerTask(bool to_serial) {
    heap_all_tasks_stat_t st = {};
    if (heap_caps_alloc_all_task_stat_arrays(&st) != ESP_OK) {
        return "统计数组分配失败(内存不足)";
    }
    std::string out;
    do {
        if (heap_caps_get_all_task_stat(&st) != ESP_OK) {
            out = "heap_caps_get_all_task_stat 失败";
            break;
        }
        std::vector<TaskRow> rows;
        rows.reserve(st.task_count);
        for (size_t i = 0; i < st.task_count; ++i) {
            const task_stat_t& ts = st.stat_arr[i];
            TaskRow row = {};
            snprintf(row.name, sizeof(row.name), "%s", ts.name);
            const size_t heap_n =
                ts.heap_count < st.heap_count ? ts.heap_count : st.heap_count;
            for (size_t h = 0; h < heap_n; ++h) {
                const heap_stat_t& hs = st.heap_stat_start[h];
                if (HeapIsPsram(hs.caps)) {
                    row.psram += hs.current_usage;
                } else if (hs.caps & MALLOC_CAP_INTERNAL) {
                    row.internal += hs.current_usage;
                    row.internal_peak += hs.peak_usage;
                    row.blocks += hs.alloc_count;
                }
            }
            rows.push_back(row);
        }
        std::sort(rows.begin(), rows.end(),
                  [](const TaskRow& a, const TaskRow& b) { return a.internal > b.internal; });

        char line[128];
        size_t sum_internal = 0;
        size_t sum_psram = 0;
        out += "每任务内存占用(内部 RAM 降序,前 20):\n";
        const size_t shown = rows.size() < 20 ? rows.size() : 20;
        for (size_t i = 0; i < shown; ++i) {
            const TaskRow& r = rows[i];
            snprintf(line, sizeof(line),
                     "%2d. %-16s 内部=%3uKB(峰%3uKB 块%3u) PSRAM=%4uKB",
                     static_cast<int>(i + 1), r.name,
                     static_cast<unsigned>(r.internal / 1024),
                     static_cast<unsigned>(r.internal_peak / 1024),
                     static_cast<unsigned>(r.blocks),
                     static_cast<unsigned>(r.psram / 1024));
            out += line;
            out += '\n';
            if (to_serial) {
                ESP_LOGI(TAG, "%s", line);
            }
        }
        for (const TaskRow& r : rows) {
            sum_internal += r.internal;
            sum_psram += r.psram;
        }
        // 堆实际已用 = 总量 - 空闲;与任务合计的差额 = 中断上下文分配 +
        // 已删除任务的残留 + 追踪启用前的早期分配
        const size_t internal_heap_used = heap_caps_get_total_size(MALLOC_CAP_INTERNAL) -
                                          heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const size_t psram_heap_used = heap_caps_get_total_size(MALLOC_CAP_SPIRAM) -
                                       heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        snprintf(line, sizeof(line),
                 "任务合计: 内部=%uKB PSRAM=%uKB | 堆实际: 内部已用=%uKB PSRAM已用=%uKB",
                 static_cast<unsigned>(sum_internal / 1024),
                 static_cast<unsigned>(sum_psram / 1024),
                 static_cast<unsigned>(internal_heap_used / 1024),
                 static_cast<unsigned>(psram_heap_used / 1024));
        out += line;
        if (to_serial) {
            ESP_LOGI(TAG, "%s", line);
        }
    } while (false);
    heap_caps_free_all_task_stat_arrays(&st);
    return out;
}

void MemReport_RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();
    mcp.AddTool(
        "self.mem.tasks",
        "Per-task memory usage report: internal RAM and PSRAM usage of every task, "
        "sorted by internal usage descending, with peaks and block counts.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            return MemReport_PerTask(false);
        });
    ESP_LOGI(TAG, "mem report MCP tools registered");
}

#else  // !CONFIG_HEAP_TASK_TRACKING

std::string MemReport_PerTask(bool) {
    return "未启用 CONFIG_HEAP_TASK_TRACKING,无法按任务统计(sdkconfig 打开后重新编译)";
}

void MemReport_RegisterMcpTools() {}

#endif  // CONFIG_HEAP_TASK_TRACKING
