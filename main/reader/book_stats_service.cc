// main/reader/book_stats_service.cc
#include "reader/book_stats_service.h"
#include "reader/book_home_snapshot.h"
#include "reader/book_progress_cache.h"
#include "reader/book_session.h"
#include <cstdio>

namespace reader {

void BookStatsService::FormatDurationShort(char* out, size_t out_sz, uint32_t seconds) {
    if (out == nullptr || out_sz == 0) {
        return;
    }
    if (seconds < 60u) {
        std::snprintf(out, out_sz, "0m");
        return;
    }
    const uint32_t total_min = seconds / 60u;
    if (total_min < 60u) {
        std::snprintf(out, out_sz, "%um", static_cast<unsigned>(total_min));
        return;
    }
    const double hours = static_cast<double>(seconds) / 3600.0;
    std::snprintf(out, out_sz, "%.1fh", hours);
}

HomeStats BookStatsService::CollectHomeStats(const std::vector<BookInfo>& books) {
    HomeStats out;
    const auto agg = reader::book_home_snapshot::LoadAggregateStats();
    if (agg.valid) {
        out.total_seconds = agg.total_seconds;
        out.finished_count = agg.finished_count;
    }

    if (books.empty()) {
        return out;
    }

    std::vector<std::string> recent_abs;
    reader::book_home_snapshot::LoadRecentAbsPaths(recent_abs);
    if (recent_abs.empty()) {
        return out;
    }

    int recent_n = 0;
    auto push_recent = [&](int idx) {
        if (idx < 0 || recent_n >= 2) {
            return;
        }
        for (int k = 0; k < recent_n; ++k) {
            if (out.recent_indices[k] == idx) {
                return;
            }
        }
        out.recent_indices[recent_n++] = idx;
    };

    for (const std::string& path : recent_abs) {
        int found = -1;
        for (int i = 0; i < static_cast<int>(books.size()); ++i) {
            if (books[static_cast<size_t>(i)].path == path) {
                found = i;
                break;
            }
        }
        if (found < 0) {
            continue;
        }
        push_recent(found);
        if (recent_n >= 2) {
            break;
        }
    }

    if (recent_n <= 0) {
        return out;
    }

    out.has_recent = true;
    out.continue_index = out.recent_indices[0];
    {
        reader::BookSession::ProgressPeek peek;
        const char* path = books[static_cast<size_t>(out.continue_index)].path.c_str();
        if (reader::book_progress_cache::TryGet(path, peek) && peek.progress_x10 >= 0) {
            out.continue_progress_x10 = peek.progress_x10;
        } else {
            out.continue_progress_x10 = reader::BookSession::PeekReadingProgressX10(path);
        }
    }
    return out;
}

} // namespace reader
