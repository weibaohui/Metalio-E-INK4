// main/reader/book_stats_service.h
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "reader/book_library.h"

namespace reader {

struct HomeStats {
    int continue_index = -1;     // 继续阅读书目下标；-1 无
    int continue_progress_x10 = -1;
    uint32_t total_seconds = 0;
    int finished_count = 0;
    int recent_indices[2] = {-1, -1};
    bool has_recent = false;     // NVS MRU 非空且能对上当前书库
};

class BookStatsService {
public:
    /**
     * @brief 格式化阅读总时长短文案（如 50.8h / 32m / 0m）
     */
    static void FormatDurationShort(char* out, size_t out_sz, uint32_t seconds);

    /**
     * @brief 搜集首页阅读聚合统计与最近阅读项
     * - 最近/继续：book_home_snapshot MRU（写进度时维护）；空则不展示
     * - 总时长/读完：开机 sync Peek 累加快照；未同步过则显示 0
     * - 仅对「继续阅读」那一本 Peek 进度百分比（实时）
     */
    static HomeStats CollectHomeStats(const std::vector<BookInfo>& books);
};

} // namespace reader
