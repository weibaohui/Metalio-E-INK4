#include "book_session.h"
#include "book_session_internal.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <ctime>
#include <new>

#include <unistd.h>
#include <sys/stat.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#ifndef EBOOK_EMULATOR
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#endif

#include "book_library.h"
#ifndef EBOOK_EMULATOR
#include "book_home_snapshot.h"
#include "book_progress_cache.h"
#endif
#include "image_util.h"
#include "reader_core/text_encoding.h"
#include "reader_core/txt_chapter.h"
#ifndef EBOOK_EMULATOR
#include "assets/lang_config.h"
#include "fontpack_lvgl.h"
#endif

using namespace reader::book_session_detail;

namespace reader {

std::string BookSession::ProgressPath(const char* book_path) {
    if (book_path == nullptr) {
        return {};
    }
    return std::string(book_path) + ".pos";
}

BookSession::PosLoadStatus BookSession::LoadPosFileOnce(const char* pos_path, PosFileData& out) {
    out = {};
    if (pos_path == nullptr || pos_path[0] == '\0') {
        return PosLoadStatus::kMissing;
    }
    FILE* fp = std::fopen(pos_path, "rb");
    if (fp == nullptr) {
        return PosLoadStatus::kMissing;
    }
    const bool head_ok =
        std::fread(out.magic, 1, 4, fp) == 4 &&
        std::fread(&out.chapter, sizeof(out.chapter), 1, fp) == 1 &&
        std::fread(&out.page, sizeof(out.page), 1, fp) == 1;
    if (!head_ok || !IsProgressMagic(out.magic)) {
        std::fclose(fp);
        return PosLoadStatus::kInvalid;
    }
    // 可选尾部：缺字段不视为整文件坏（兼容 POS1 短文件）
    out.has_pct = std::fread(&out.pct_x10, sizeof(out.pct_x10), 1, fp) == 1;
    if (out.has_pct) {
        out.has_seconds = std::fread(&out.reading_seconds, sizeof(out.reading_seconds), 1, fp) == 1;
    }
    if (out.has_seconds) {
        out.has_daily = std::fread(&out.day_id, sizeof(out.day_id), 1, fp) == 1 &&
                        std::fread(&out.daily_seconds, sizeof(out.daily_seconds), 1, fp) == 1;
    }
    std::fclose(fp);

    // 按 magic 语义收窄：旧版即使误读到多字节也不采用
    if (!MagicHasLifetime(out.magic)) {
        out.has_seconds = false;
        out.reading_seconds = 0;
        out.has_daily = false;
        out.day_id = 0;
        out.daily_seconds = 0;
    } else if (!MagicHasDaily(out.magic)) {
        out.has_daily = false;
        out.day_id = 0;
        out.daily_seconds = 0;
    }
    return PosLoadStatus::kOk;
}

BookSession::PosLoadStatus BookSession::LoadPosFile(const char* pos_path, PosFileData& out) {
    const PosLoadStatus st = LoadPosFileOnce(pos_path, out);
    if (st != PosLoadStatus::kMissing) {
        return st;
    }
    // 原子写失败窗口：.pos 已 unlink、.pos.tmp 尚未 rename → 从 .tmp 恢复
    if (pos_path == nullptr || pos_path[0] == '\0') {
        return PosLoadStatus::kMissing;
    }
    const std::string tmp = std::string(pos_path) + ".tmp";
    return LoadPosFileOnce(tmp.c_str(), out);
}

uint32_t BookSession::LocalCalendarDayId() {
    // 与首页时钟门禁一致：未对时（年 < 2025）不可信，返回 0
    const time_t now = time(nullptr);
    if (now == static_cast<time_t>(-1)) {
        return 0;
    }
    struct tm tm_info = {};
    if (localtime_r(&now, &tm_info) == nullptr) {
        return 0;
    }
    if (tm_info.tm_year < (2025 - 1900)) {
        return 0;
    }
    const int year = tm_info.tm_year + 1900;
    const int mon = tm_info.tm_mon + 1;
    const int day = tm_info.tm_mday;
    if (year < 2025 || year > 2099 || mon < 1 || mon > 12 || day < 1 || day > 31) {
        return 0;
    }
    return static_cast<uint32_t>(year * 10000 + mon * 100 + day);
}

uint32_t BookSession::DailySecondsIfToday(const PosFileData& data) {
    if (!data.has_daily) {
        return 0;
    }
    const uint32_t today = LocalCalendarDayId();
    if (today == 0 || data.day_id != today) {
        return 0;
    }
    return data.daily_seconds;
}

void BookSession::AddReadingSecondsSaturated(uint32_t delta_sec) {
    if (delta_sec == 0) {
        return;
    }
    if (reading_seconds_ > UINT32_MAX - delta_sec) {
        reading_seconds_ = UINT32_MAX;
    } else {
        reading_seconds_ += delta_sec;
    }
}

void BookSession::AddDailySecondsSaturated(uint32_t delta_sec) {
    if (delta_sec == 0) {
        return;
    }
    if (daily_seconds_ > UINT32_MAX - delta_sec) {
        daily_seconds_ = UINT32_MAX;
    } else {
        daily_seconds_ += delta_sec;
    }
}

void BookSession::EnsureDailyBucket() {
    const uint32_t today = LocalCalendarDayId();
    if (today == 0) {
        // 时钟无效：不换日、不改 day_id，避免把有效日戳冲成 0
        return;
    }
    if (reading_day_id_ != today) {
        reading_day_id_ = today;
        daily_seconds_ = 0;
    }
}

void BookSession::FoldReadingClock() {
    if (!reading_clock_active_) {
        return;
    }
    const int64_t now = esp_timer_get_time();
    int64_t delta_us = now - reading_clock_since_us_;
    if (delta_us < 0) {
        // 时钟回拨：丢弃异常段，从 now 重新计
        reading_clock_since_us_ = now;
        return;
    }
    if (delta_us > kMaxFoldDeltaUs) {
        ESP_LOGW(TAG, "reading clock fold clamp %lld us (max %lld)",
                 static_cast<long long>(delta_us), static_cast<long long>(kMaxFoldDeltaUs));
        delta_us = kMaxFoldDeltaUs;
    }
    const uint32_t sec = static_cast<uint32_t>(delta_us / 1000000LL);
    if (sec > 0) {
        // 先换日再累加：跨午夜的一小段（≤ checkpoint）记入新日，可接受
        const uint32_t today = LocalCalendarDayId();
        if (today != 0) {
            if (reading_day_id_ != today) {
                reading_day_id_ = today;
                daily_seconds_ = 0;
            }
            AddReadingSecondsSaturated(sec);
            AddDailySecondsSaturated(sec);
        } else {
            // 未对时：只累终身，不动当日桶
            AddReadingSecondsSaturated(sec);
        }
        // 保留不足 1 秒的余量，避免多次 fold 丢精度
        reading_clock_since_us_ = now - (delta_us % 1000000LL);
    }
}

void BookSession::StartReadingClock() {
    if (!open_) {
        return;
    }
    if (reading_clock_active_) {
        return;
    }
    // 开钟前对齐当日桶，避免昨日内存日戳带到今日首段
    EnsureDailyBucket();
    reading_clock_active_ = true;
    reading_clock_since_us_ = esp_timer_get_time();
}

void BookSession::StopReadingClock() {
    FoldReadingClock();
    reading_clock_active_ = false;
    reading_clock_since_us_ = 0;
}

bool BookSession::SaveProgress() {
    if (!open_ || info_.path.empty()) {
        return false;
    }
    FoldReadingClock();
    EnsureDailyBucket();
    const std::string path = ProgressPath(info_.path.c_str());
    if (path.empty()) {
        return false;
    }

    // 整块写出 + WriteFileAtomic：避免 fopen("wb") 先截断再写失败导致进度/时长归零
    Pos3Blob blob{};
    blob.magic[0] = 'P';
    blob.magic[1] = 'O';
    blob.magic[2] = 'S';
    blob.magic[3] = '3';
    blob.chapter = static_cast<uint32_t>(std::max(0, chapter_index_));
    blob.page = static_cast<uint32_t>(std::max(0, page_index_));
    blob.pct_x10 = static_cast<uint32_t>(std::max(0, ReadingProgressX10()));
    blob.reading_seconds = reading_seconds_;
    blob.day_id = reading_day_id_;
    blob.daily_seconds = daily_seconds_;

    const bool ok =
        WriteFileAtomic(path.c_str(), reinterpret_cast<const uint8_t*>(&blob), sizeof(blob));
    if (!ok) {
        ESP_LOGW(TAG, "progress save fail: %s", path.c_str());
        return false;
    }
#ifndef EBOOK_EMULATOR
    // 首页「最近阅读」MRU：仅进度真正落盘后轮换；已在队首则内部跳过写 NVS
    book_home_snapshot::NoteOpenedBook(info_.path.c_str());
    {
        BookSession::ProgressPeek snap;
        snap.progress_x10 = static_cast<int>(blob.pct_x10);
        snap.reading_seconds = blob.reading_seconds;
        snap.daily_seconds = blob.daily_seconds;
        book_progress_cache::Upsert(info_.path.c_str(), snap);
    }
#endif
    ESP_LOGD(TAG, "progress saved %s ch=%u pg=%u pct=%u.%u sec=%u day=%u daily=%u", path.c_str(),
             static_cast<unsigned>(blob.chapter), static_cast<unsigned>(blob.page),
             static_cast<unsigned>(blob.pct_x10 / 10), static_cast<unsigned>(blob.pct_x10 % 10),
             static_cast<unsigned>(blob.reading_seconds), static_cast<unsigned>(blob.day_id),
             static_cast<unsigned>(blob.daily_seconds));
    return true;
}

int BookSession::PeekReadingProgressX10(const char* book_path) {
    if (book_path == nullptr || book_path[0] == '\0') {
        return -1;
    }
    PosFileData data;
    if (LoadPosFile(ProgressPath(book_path).c_str(), data) != PosLoadStatus::kOk) {
        return -1;
    }
    // 旧版 .pos 无百分比字段：视为尚无可展示进度
    if (!data.has_pct) {
        return -1;
    }
    uint32_t pct_x10 = data.pct_x10;
    if (pct_x10 > 1000) {
        pct_x10 = 1000;
    }
    return static_cast<int>(pct_x10);
}

uint32_t BookSession::PeekReadingSeconds(const char* book_path) {
    if (book_path == nullptr || book_path[0] == '\0') {
        return 0;
    }
    PosFileData data;
    if (LoadPosFile(ProgressPath(book_path).c_str(), data) != PosLoadStatus::kOk) {
        return 0;
    }
    if (!data.has_seconds) {
        return 0;
    }
    return data.reading_seconds;
}

uint32_t BookSession::PeekReadingDailySeconds(const char* book_path) {
    if (book_path == nullptr || book_path[0] == '\0') {
        return 0;
    }
    PosFileData data;
    if (LoadPosFile(ProgressPath(book_path).c_str(), data) != PosLoadStatus::kOk) {
        return 0;
    }
    return DailySecondsIfToday(data);
}

BookSession::ProgressPeek BookSession::PeekProgress(const char* book_path) {
    ProgressPeek out;
    if (book_path == nullptr || book_path[0] == '\0') {
        return out;
    }
    PosFileData data;
    if (LoadPosFile(ProgressPath(book_path).c_str(), data) != PosLoadStatus::kOk) {
        return out;
    }
    if (data.has_pct) {
        uint32_t pct_x10 = data.pct_x10;
        if (pct_x10 > 1000) {
            pct_x10 = 1000;
        }
        out.progress_x10 = static_cast<int>(pct_x10);
    }
    if (data.has_seconds) {
        out.reading_seconds = data.reading_seconds;
    }
    out.daily_seconds = DailySecondsIfToday(data);
    return out;
}

bool BookSession::ReadProgressPos(uint32_t& chapter, uint32_t& page, uint32_t* seconds_out,
                                  uint32_t* day_id_out, uint32_t* daily_out) {
    chapter = 0;
    page = 0;
    if (seconds_out != nullptr) {
        *seconds_out = 0;
    }
    if (day_id_out != nullptr) {
        *day_id_out = 0;
    }
    if (daily_out != nullptr) {
        *daily_out = 0;
    }
    if (info_.path.empty()) {
        return false;
    }
    const std::string path = ProgressPath(info_.path.c_str());
    PosFileData data;
    const PosLoadStatus st = LoadPosFile(path.c_str(), data);
    if (st == PosLoadStatus::kMissing) {
        return false;
    }
    if (st != PosLoadStatus::kOk) {
        ESP_LOGW(TAG, "progress bad file: %s", path.c_str());
        return false;
    }
    chapter = data.chapter;
    page = data.page;
    if (seconds_out != nullptr && data.has_seconds) {
        *seconds_out = data.reading_seconds;
    }
    if (day_id_out != nullptr && data.has_daily) {
        *day_id_out = data.day_id;
    }
    if (daily_out != nullptr && data.has_daily) {
        *daily_out = data.daily_seconds;
    }
    return true;
}

}  // namespace reader
