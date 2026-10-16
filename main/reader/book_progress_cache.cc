#include "book_progress_cache.h"

#include "reader_core/reader_types.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <vector>

#include <esp_log.h>

namespace reader {
namespace book_progress_cache {
namespace {

constexpr const char* TAG = "BookProgCache";

// 扁平连续内存缓存项，消除 std::unordered_map 的独立哈希节点堆碎片
struct CacheEntry {
    std::string path;
    BookSession::ProgressPeek peek;
};

std::mutex s_mu;
std::vector<CacheEntry> s_entries;
bool s_ready = false;
uint32_t s_generation = 0;

auto FindEntry(const char* book_abs_path) {
    return std::lower_bound(
        s_entries.begin(), s_entries.end(), book_abs_path,
        [](const CacheEntry& entry, const char* p) {
            return entry.path < p;
        });
}

}  // namespace

bool IsReady() {
    std::lock_guard<std::mutex> lock(s_mu);
    return s_ready;
}

uint32_t Generation() {
    std::lock_guard<std::mutex> lock(s_mu);
    return s_generation;
}

void Invalidate() {
    std::lock_guard<std::mutex> lock(s_mu);
    s_entries.clear();
    s_ready = false;
    ++s_generation;
    ESP_LOGI(TAG, "invalidated");
}

void ReplaceAll(std::vector<std::pair<std::string, BookSession::ProgressPeek>> items) {
    std::lock_guard<std::mutex> lock(s_mu);
    s_entries.clear();
    s_entries.reserve(items.size());
    for (auto& it : items) {
        if (it.first.empty()) {
            continue;
        }
        s_entries.push_back({std::move(it.first), it.second});
    }
    std::sort(s_entries.begin(), s_entries.end(),
              [](const CacheEntry& a, const CacheEntry& b) {
                  return a.path < b.path;
              });
    s_ready = true;
    ++s_generation;
    ESP_LOGI(TAG, "replaced n=%u gen=%u (flat sorted vector)", static_cast<unsigned>(s_entries.size()),
             static_cast<unsigned>(s_generation));
}

bool TryGet(const char* book_abs_path, BookSession::ProgressPeek& out) {
    out = {};
    if (book_abs_path == nullptr || book_abs_path[0] == '\0') {
        return false;
    }
    std::lock_guard<std::mutex> lock(s_mu);
    const auto it = FindEntry(book_abs_path);
    if (it == s_entries.end() || it->path != book_abs_path) {
        return false;
    }
    out = it->peek;
    return true;
}

void Upsert(const char* book_abs_path, const BookSession::ProgressPeek& peek) {
    if (book_abs_path == nullptr || book_abs_path[0] == '\0') {
        return;
    }
    std::lock_guard<std::mutex> lock(s_mu);
    const auto it = FindEntry(book_abs_path);
    if (it != s_entries.end() && it->path == book_abs_path) {
        it->peek = peek;
    } else {
        s_entries.insert(it, {std::string(book_abs_path), peek});
    }
    // 单本更新不 Cleared ready；若尚未 Replace 过，仍允许局部写入供后续合并
    if (!s_ready) {
        s_ready = true;
    }
    ++s_generation;
}

void Erase(const char* book_abs_path) {
    if (book_abs_path == nullptr || book_abs_path[0] == '\0') {
        return;
    }
    std::lock_guard<std::mutex> lock(s_mu);
    const auto it = FindEntry(book_abs_path);
    if (it != s_entries.end() && it->path == book_abs_path) {
        s_entries.erase(it);
        ++s_generation;
        ESP_LOGD(TAG, "erased %s", book_abs_path);
    }
}

size_t Size() {
    std::lock_guard<std::mutex> lock(s_mu);
    return s_entries.size();
}

size_t FillPeeksForBooks(const std::vector<BookInfo>& books,
                         std::vector<BookSession::ProgressPeek>& out) {
    out.clear();
    out.resize(books.size());
    std::lock_guard<std::mutex> lock(s_mu);
    size_t hit = 0;
    for (size_t i = 0; i < books.size(); ++i) {
        const auto it = FindEntry(books[i].path.c_str());
        if (it != s_entries.end() && it->path == books[i].path) {
            out[i] = it->peek;
            ++hit;
        } else {
            out[i] = {};
        }
    }
    return hit;
}

}  // namespace book_progress_cache
}  // namespace reader
