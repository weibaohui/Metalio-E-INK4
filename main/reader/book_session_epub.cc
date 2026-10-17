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

std::string BookSession::ChapterPageCachePath(const char* book_path) {
    if (book_path == nullptr) {
        return {};
    }
    return std::string(book_path) + ".cpg";
}

bool BookSession::OpenEpub(const char* path) {
    ReportOpenProgress(10);
    epub_ = std::make_unique<EpubDocument>();
    if (!epub_->Open(path)) {
        epub_.reset();
        return false;
    }
    ReportOpenProgress(40);
    if (!epub_->Title().empty()) {
        display_title_ = epub_->Title();
    }
    display_author_ = epub_->Author();
    chapter_count_ = std::max(1, epub_->SpineCount());
    chapter_index_ = 0;
    ClearToc();
    for (int i = 0; i < chapter_count_; ++i) {
        TocEntry ent;
        char buf[48];
        std::snprintf(buf, sizeof(buf), UiChapterFmt(), i + 1);
        ent.title = buf;
        toc_.push_back(std::move(ent));
    }
    return true;
}

bool BookSession::OpenEbook(const char* path) {
    ReportOpenProgress(10);
    ebook_ = std::make_unique<EbookDocument>();
    if (!ebook_->Open(path)) {
        ebook_.reset();
        return false;
    }
    ReportOpenProgress(40);
    if (!ebook_->Title().empty()) {
        display_title_ = ebook_->Title();
    }
    display_author_ = ebook_->Author();
    page_images_mode_ = ebook_->HasPageImages();
    ebook_spine_.clear();
    const int raw_n = ebook_->ChapterCount();
    for (int i = 0; i < raw_n; ++i) {
        if (ebook_->ChapterLikelyHasContent(i)) {
            ebook_spine_.push_back(i);
        }
        if (raw_n > 0 && (i % 8 == 0 || i + 1 == raw_n)) {
            ReportOpenProgress(40 + (20 * (i + 1)) / raw_n);
        }
    }
    if (ebook_spine_.empty()) {
        ESP_LOGW(TAG, "ebook has no non-empty chapters");
        ebook_.reset();
        ebook_spine_.clear();
        return false;
    }
    chapter_count_ = static_cast<int>(ebook_spine_.size());
    chapter_index_ = 0;
    ClearToc();
    // 仅当 FLAG_HAS_TOC 时填目录；无原生目录 → TocCount()==0，UI 显示「暂无目录」
    if (ebook_->HasToc()) {
        for (int file_ch : ebook_spine_) {
            TocEntry ent;
            ent.title = ebook_->ChapterTitle(file_ch);
            if (ent.title.empty()) {
                char buf[48];
                std::snprintf(buf, sizeof(buf), UiChapterFmt(), file_ch + 1);
                ent.title = buf;
            }
            toc_.push_back(std::move(ent));
        }
    }
    return true;
}

bool BookSession::LoadEpubChapter(int chapter) {
    std::lock_guard<std::mutex> lock(chapter_io_mu_);
    if (!epub_ || chapter < 0 || chapter >= epub_->SpineCount()) {
        return false;
    }
    ReportOpenProgress(55);
    std::vector<ContentBlock> blocks;
    try {
        if (!epub_->LoadChapterBlocks(chapter, blocks)) {
            ContentBlock b;
            b.kind = ContentKind::kText;
            b.text = UiChapterNoText();
            blocks.push_back(std::move(b));
        }
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "epub LoadChapterBlocks OOM chapter=%d", chapter);
        return false;
    }
    ReportOpenProgress(75);
    // 先分页到临时缓冲；失败则不改 chapter_index_/pages_，避免跨章翻页半更新
    if (!BuildPagesFromBlocks(blocks)) {
        return false;
    }
    chapter_index_ = chapter;
    NoteChapterPageCount(chapter, static_cast<int>(pages_.size()));
    ReportOpenProgress(90);
    return true;
}

bool BookSession::LoadEbookChapter(int chapter) {
    std::lock_guard<std::mutex> lock(chapter_io_mu_);
    if (!ebook_ || chapter < 0 || static_cast<size_t>(chapter) >= ebook_spine_.size()) {
        return false;
    }
    const int file_ch = ebook_spine_[static_cast<size_t>(chapter)];
    const uint32_t uncomp = ebook_->ChapterUncompressedTotal(file_ch);
    if (uncomp > kMaxChapterUncompForPaginate) {
        ESP_LOGE(TAG, "ebook chapter too large file_idx=%d uncomp=%u max=%u (re-convert with size split)",
                 file_ch, static_cast<unsigned>(uncomp),
                 static_cast<unsigned>(kMaxChapterUncompForPaginate));
        return false;
    }
    ReportOpenProgress(55);
    std::vector<ContentBlock> blocks;
    try {
        if (!ebook_->LoadChapterBlocks(file_ch, blocks) || blocks.empty()) {
            ESP_LOGW(TAG, "empty ebook chapter file_idx=%d", file_ch);
            return false;
        }
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "ebook LoadChapterBlocks OOM file_idx=%d", file_ch);
        return false;
    }
    ReportOpenProgress(75);
    if (!BuildPagesFromBlocks(blocks)) {
        return false;
    }
    chapter_index_ = chapter;
    NoteChapterPageCount(chapter, static_cast<int>(pages_.size()));
    ESP_LOGI(TAG, "ebook chapter ready %d/%d pages=%u", chapter_index_ + 1, chapter_count_,
             static_cast<unsigned>(pages_.size()));
    ReportOpenProgress(90);
    return true;
}

void BookSession::NoteChapterPageCount(int chapter, int pages) {
    if (!IsChapteredBook() || chapter < 0 || chapter >= chapter_count_) {
        return;
    }
    if (chapter_page_counts_.size() != static_cast<size_t>(chapter_count_)) {
        try {
            chapter_page_counts_.assign(static_cast<size_t>(chapter_count_), 0);
        } catch (const std::bad_alloc&) {
            return;
        }
        chapter_pages_ready_ = false;
    }
    if (pages < 1) {
        pages = 1;
    }
    if (pages > 65535) {
        pages = 65535;
    }
    chapter_page_counts_[static_cast<size_t>(chapter)] = static_cast<uint16_t>(pages);
}

bool BookSession::CountChapterPages(int chapter, int& out_pages) const {
    out_pages = 1;
    if (!IsChapteredBook() || chapter < 0 || chapter >= chapter_count_) {
        return false;
    }
    std::lock_guard<std::mutex> lock(chapter_io_mu_);
    // 当前章已物化：直接用 pages_，避免重复解压
    if (chapter == chapter_index_ && !pages_.empty()) {
        out_pages = static_cast<int>(pages_.size());
        return true;
    }

    std::vector<ContentBlock> blocks;
    try {
        if (ebook_ != nullptr) {
            if (static_cast<size_t>(chapter) >= ebook_spine_.size()) {
                return false;
            }
            const int file_ch = ebook_spine_[static_cast<size_t>(chapter)];
            const uint32_t uncomp = ebook_->ChapterUncompressedTotal(file_ch);
            if (uncomp > kMaxChapterUncompForPaginate) {
                out_pages = 1;
                return true;
            }
            if (!ebook_->LoadChapterBlocks(file_ch, blocks) || blocks.empty()) {
                out_pages = 1;
                return true;
            }
        } else if (epub_ != nullptr) {
            if (!epub_->LoadChapterBlocks(chapter, blocks)) {
                ContentBlock b;
                b.kind = ContentKind::kText;
                b.text = UiChapterNoText();
                blocks.push_back(std::move(b));
            }
        } else {
            return false;
        }
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "CountChapterPages OOM chapter=%d", chapter);
        return false;
    }
    return CountPagesFromBlocks(blocks, out_pages);
}

bool BookSession::TryLoadChapterPageCache() {
    if (!IsChapteredBook() || info_.path.empty() || chapter_count_ <= 0) {
        return false;
    }
    const std::string path = ChapterPageCachePath(info_.path.c_str());
    FILE* ifp = std::fopen(path.c_str(), "rb");
    if (ifp == nullptr) {
        return false;
    }
    ChapterPageHdr hdr{};
    if (std::fread(&hdr, 1, sizeof(hdr), ifp) != sizeof(hdr)) {
        std::fclose(ifp);
        return false;
    }
    if (std::memcmp(hdr.magic, "CPG1", 4) != 0 || hdr.version != kChapterPageCacheVersion ||
        hdr.chapter_count != static_cast<uint32_t>(chapter_count_) ||
        hdr.viewport_w != static_cast<uint16_t>(viewport_w_) ||
        hdr.viewport_h != static_cast<uint16_t>(viewport_h_) ||
        hdr.line_gap != static_cast<uint16_t>(line_gap_) ||
        hdr.para_gap != static_cast<uint16_t>(para_gap_) || hdr.font_fp != FontFingerprint() ||
        hdr.page_images_mode != static_cast<uint8_t>(page_images_mode_ ? 1 : 0)) {
        std::fclose(ifp);
        return false;
    }
    std::vector<uint16_t> counts;
    try {
        counts.resize(static_cast<size_t>(chapter_count_));
    } catch (const std::bad_alloc&) {
        std::fclose(ifp);
        return false;
    }
    if (std::fread(counts.data(), sizeof(uint16_t), counts.size(), ifp) != counts.size()) {
        std::fclose(ifp);
        return false;
    }
    std::fclose(ifp);
    for (uint16_t n : counts) {
        if (n == 0) {
            return false;
        }
    }
    chapter_page_counts_.swap(counts);
    chapter_pages_ready_ = true;
    // 与当前已加载章对齐（缓存可能略旧于二次分页）
    if (!pages_.empty() && chapter_index_ >= 0 &&
        static_cast<size_t>(chapter_index_) < chapter_page_counts_.size()) {
        NoteChapterPageCount(chapter_index_, static_cast<int>(pages_.size()));
    }
    ESP_LOGI(TAG, "chapter page cache hit %s chapters=%d book_pages=%d", path.c_str(), chapter_count_,
             BookPageCount());
    return true;
}

bool BookSession::SaveChapterPageCache() const {
    if (!IsChapteredBook() || !chapter_pages_ready_ || info_.path.empty() ||
        chapter_page_counts_.size() != static_cast<size_t>(chapter_count_)) {
        return false;
    }
    const std::string path = ChapterPageCachePath(info_.path.c_str());
    FILE* ofp = std::fopen(path.c_str(), "wb");
    if (ofp == nullptr) {
        return false;
    }
    ChapterPageHdr hdr{};
    std::memcpy(hdr.magic, "CPG1", 4);
    hdr.version = kChapterPageCacheVersion;
    hdr.chapter_count = static_cast<uint32_t>(chapter_count_);
    hdr.viewport_w = static_cast<uint16_t>(viewport_w_);
    hdr.viewport_h = static_cast<uint16_t>(viewport_h_);
    hdr.line_gap = static_cast<uint16_t>(line_gap_);
    hdr.para_gap = static_cast<uint16_t>(para_gap_);
    hdr.font_fp = FontFingerprint();
    hdr.page_images_mode = static_cast<uint8_t>(page_images_mode_ ? 1 : 0);
    if (std::fwrite(&hdr, 1, sizeof(hdr), ofp) != sizeof(hdr) ||
        std::fwrite(chapter_page_counts_.data(), sizeof(uint16_t), chapter_page_counts_.size(),
                    ofp) != chapter_page_counts_.size()) {
        std::fclose(ofp);
        return false;
    }
    std::fclose(ofp);
    ESP_LOGI(TAG, "chapter page cache saved %s chapters=%d", path.c_str(), chapter_count_);
    return true;
}

bool BookSession::RebuildChapterPageTable() {
    if (!IsChapteredBook() || chapter_count_ <= 0) {
        chapter_pages_ready_ = true;
        return true;
    }
    const uint32_t gen = chapter_pages_gen_;
    std::vector<uint16_t> built;
    try {
        built.resize(static_cast<size_t>(chapter_count_), 0);
    } catch (const std::bad_alloc&) {
        ESP_LOGE(TAG, "RebuildChapterPageTable OOM");
        return false;
    }
    for (int ch = 0; ch < chapter_count_; ++ch) {
        if (gen != chapter_pages_gen_) {
            chapter_pages_ready_ = false;
            ESP_LOGI(TAG, "chapter page table abort at %d/%d gen=%u", ch + 1, chapter_count_,
                     static_cast<unsigned>(chapter_pages_gen_));
            return true;
        }
        int n = 1;
        if (!CountChapterPages(ch, n)) {
            n = 1;
        }
        if (n < 1) {
            n = 1;
        }
        if (n > 65535) {
            n = 65535;
        }
        built[static_cast<size_t>(ch)] = static_cast<uint16_t>(n);
    }
    chapter_page_counts_.swap(built);
    if (gen != chapter_pages_gen_) {
        // 扫表期间又换了排版：作废本次结果，留给 again 再跑
        chapter_pages_ready_ = false;
        ESP_LOGI(TAG, "chapter page table stale gen=%u now=%u", static_cast<unsigned>(gen),
                 static_cast<unsigned>(chapter_pages_gen_));
        return true;
    }
    chapter_pages_ready_ = true;
    SaveChapterPageCache();
    ESP_LOGI(TAG, "chapter page table ready chapters=%d book_pages=%d", chapter_count_,
             BookPageCount());
    return true;
}

bool BookSession::WarmChapterPageTableFromCache() {
    if (!IsChapteredBook()) {
        chapter_pages_ready_ = true;
        return true;
    }
    if (TryLoadChapterPageCache()) {
        return true;
    }
    NoteChapterPageCount(chapter_index_, std::max(1, PageCount()));
    chapter_pages_ready_ = false;
    return false;
}

bool BookSession::FinishChapterPageTable() {
    if (!IsChapteredBook()) {
        chapter_pages_ready_ = true;
        return true;
    }
    OpenProgressFn saved_fn = progress_fn_;
    void* saved_user = progress_user_;
    progress_fn_ = nullptr;
    progress_user_ = nullptr;
    const bool ok = RebuildChapterPageTable();
    progress_fn_ = saved_fn;
    progress_user_ = saved_user;
    return ok;
}

bool BookSession::IsChapteredBook() const {
    return (info_.format == BookFormat::kEpub && epub_) ||
           (info_.format == BookFormat::kEbook && ebook_);
}

bool BookSession::NeedsChapterPageRebuild() const {
    return IsChapteredBook() && !chapter_pages_ready_;
}

void BookSession::InvalidateChapterPageTable() {
    if (!IsChapteredBook()) {
        return;
    }
    chapter_pages_gen_++;
    chapter_pages_ready_ = false;
}

bool BookSession::RelayoutCurrentChapter(const LayoutAnchor& anchor) {
    if (!IsChapteredBook()) {
        return false;
    }
    const int ch = anchor.valid ? anchor.chapter : chapter_index_;
    const int old_page = anchor.valid ? anchor.page : page_index_;
    const int old_pc = anchor.valid ? std::max(1, anchor.page_count) : std::max(1, PageCount());
    OpenProgressFn saved_fn = progress_fn_;
    void* saved_user = progress_user_;
    progress_fn_ = nullptr;
    progress_user_ = nullptr;
    const bool ok =
        (ebook_ != nullptr) ? LoadEbookChapter(ch) : LoadEpubChapter(ch);
    progress_fn_ = saved_fn;
    progress_user_ = saved_user;
    if (!ok) {
        return false;
    }
    const int new_pc = std::max(1, PageCount());
    int page = (old_page * new_pc) / old_pc;
    if (page >= new_pc) {
        page = new_pc - 1;
    }
    if (page < 0) {
        page = 0;
    }
    page_index_ = page;
    // Load*Chapter 已 Note 当前章；其它章随排版失效，后台 FinishChapterPageTable 重算
    InvalidateChapterPageTable();
    SaveProgress();
    return true;
}

}  // namespace reader
