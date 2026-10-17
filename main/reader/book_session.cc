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

uint32_t BookSession::FontFingerprint() const {
    // 行高 + 若干样例字宽：换字体/字号后自动失效缓存
    uint32_t h = static_cast<uint32_t>(FontLineHeight(font_));
    static const uint32_t kSamples[] = {
        0x4E2Du,  // 中
        0x6587u,  // 文
        static_cast<uint32_t>('a'),
        static_cast<uint32_t>('0'),
        0xFF0Cu,  // ，
        0x3002u,  // 。
        0x201Cu,  // “
    };
    for (uint32_t cp : kSamples) {
        h = h * 131u + static_cast<uint32_t>(GlyphWidth(font_, cp));
    }
    h = h * 131u + static_cast<uint32_t>(viewport_w_);
    h = h * 131u + static_cast<uint32_t>(viewport_h_);
    return h;
}

void BookSession::ReportOpenProgress(int percent) {
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    // 节流：至少 +10% 或到 100，避免墨水屏刷新过密
    if (last_progress_pct_ >= 0 && percent < 100 && percent < last_progress_pct_ + 10) {
        return;
    }
    last_progress_pct_ = percent;
    if (progress_fn_ != nullptr) {
        progress_fn_(percent, progress_user_);
    }
}

void BookSession::Close() {
    if (open_) {
        SaveProgress();  // fold 活跃时钟并写 POS3
    }
    StopReadingClock();
    reading_seconds_ = 0;
    daily_seconds_ = 0;
    reading_day_id_ = 0;
    if (epub_) {
        epub_->Close();
        epub_.reset();
    }
    if (ebook_) {
        ebook_->Close();
        ebook_.reset();
    }
    ebook_spine_.clear();
    page_images_mode_ = false;
    ClearTxtIndex();
    ClearToc();
    pages_.clear();
    pages_.shrink_to_fit();
    page_index_ = 0;
    chapter_index_ = 0;
    chapter_count_ = 1;
    chapter_page_counts_.clear();
    chapter_page_counts_.shrink_to_fit();
    chapter_pages_ready_ = false;
    chapter_pages_gen_ = 0;
    cover_.Reset();
    cover_tried_ = false;
    cached_href_.clear();
    cached_image_.Reset();
    live_preview_active_ = false;
    live_preview_byte_off_ = 0;
    live_preview_at_para_start_ = true;
    live_preview_next_off_ = 0;
    live_preview_next_at_para_start_ = true;
    live_preview_has_next_ = false;
    live_preview_page_ = Page{};
    live_preview_hist_.clear();
    open_ = false;
    info_ = {};
    display_title_.clear();
    display_author_.clear();
}

bool BookSession::Open(const BookInfo& info) {
    Close();
    last_progress_pct_ = -1;
    ReportOpenProgress(0);
    if (font_ == nullptr) {
        font_ = DefaultFont();
    }
    info_ = info;
    display_title_ = info.title;
    bool ok = false;
    if (info.format == BookFormat::kTxt) {
        ok = OpenTxt(info.path.c_str(), info.file_size);
    } else if (info.format == BookFormat::kEpub) {
        ok = OpenEpub(info.path.c_str());
    } else if (info.format == BookFormat::kEbook) {
        ok = OpenEbook(info.path.c_str());
    } else {
        ESP_LOGE(TAG, "unknown format");
    }
    open_ = ok;
    if (!ok) {
        Close();
        return false;
    }
    ReportOpenProgress(95);
    uint32_t prog_ch = 0;
    uint32_t prog_page = 0;
    uint32_t prog_seconds = 0;
    uint32_t prog_day_id = 0;
    uint32_t prog_daily = 0;
    const bool has_prog =
        ReadProgressPos(prog_ch, prog_page, &prog_seconds, &prog_day_id, &prog_daily);
    // 尽早恢复时长，避免后续章加载失败走 Close 时把秒数写成 0
    reading_seconds_ = prog_seconds;
    reading_day_id_ = prog_day_id;
    daily_seconds_ = prog_daily;
    // 懒换日：跨日打开只清内存当日；下次 Save 落盘。终身累计不动。
    EnsureDailyBucket();
    reading_clock_active_ = false;
    reading_clock_since_us_ = 0;
    if (!txt_mode_) {
        int ch = has_prog ? static_cast<int>(prog_ch) : 0;
        if (ch < 0 || ch >= chapter_count_) {
            ch = 0;
        }
        bool loaded = false;
        if (epub_ != nullptr) {
            loaded = LoadEpubChapter(ch);
        } else if (ebook_ != nullptr) {
            loaded = LoadEbookChapter(ch);
        }
        if (!loaded) {
            Close();
            return false;
        }
        // 全书页表：打开只读 .cpg；未命中则估算，后台 FinishChapterPageTable 补齐
        WarmChapterPageTableFromCache();
    }
    if (has_prog) {
        const int pc = PageCount();
        if (pc > 0) {
            if (prog_page >= static_cast<uint32_t>(pc)) {
                prog_page = static_cast<uint32_t>(pc - 1);
            }
            page_index_ = static_cast<int>(prog_page);
            ESP_LOGI(TAG, "progress restored chapter=%d page=%d/%d book=%d/%d", chapter_index_ + 1,
                     page_index_ + 1, PageCount(), BookCurrentPage() + 1, BookPageCount());
        }
    }
    ReportOpenProgress(100);
    return true;
}

int BookSession::PageCount() const {
    if (txt_mode_) {
        return static_cast<int>(txt_page_count_);
    }
    return static_cast<int>(pages_.size());
}

int BookSession::BookPageCount() const {
    if (!IsChapteredBook() || chapter_count_ <= 1) {
        return std::max(1, PageCount());
    }
    if (chapter_page_counts_.size() == static_cast<size_t>(chapter_count_)) {
        int64_t sum = 0;
        for (uint16_t n : chapter_page_counts_) {
            sum += std::max<uint16_t>(1, n);
        }
        if (sum > 0) {
            return static_cast<int>(sum);
        }
    }
    // 未就绪：沿用「本章页数 × 章数」估算
    return std::max(1, ChapterCount()) * std::max(1, PageCount());
}

int BookSession::BookCurrentPage() const {
    if (!IsChapteredBook() || chapter_count_ <= 1) {
        return CurrentPage();
    }
    if (chapter_page_counts_.size() != static_cast<size_t>(chapter_count_)) {
        // 估算：假定每章页数=本章
        const int pc = std::max(1, PageCount());
        return chapter_index_ * pc + CurrentPage();
    }
    int64_t before = 0;
    const int ch = std::clamp(chapter_index_, 0, chapter_count_ - 1);
    for (int i = 0; i < ch; ++i) {
        before += std::max<uint16_t>(1, chapter_page_counts_[static_cast<size_t>(i)]);
    }
    return static_cast<int>(before + CurrentPage());
}

bool BookSession::PaginateTextBlock(const std::string& text, std::vector<Page>& pages, Page& cur,
                                    lv_coord_t& y) {
    // .ebook / EPUB：每 ContentBlock 一段；段首 strip 源文空白 + pad_left 两字；段内 \\n 软换行顶格
    const lv_coord_t line_h = FontLineHeight(font_);
    const lv_coord_t indent_w = TxtParaIndentWidth(font_);
    std::string line;
    lv_coord_t line_w = 0;
    bool first_line_of_para = true;
    const uint8_t* data = reinterpret_cast<const uint8_t*>(text.data());
    size_t i = 0;
    while (i < text.size() && text[i] == '\n') {
        ++i;
    }

    auto flush_line = [&](bool indent_first) -> bool {
        if (line.empty()) {
            return true;
        }
        if (indent_first) {
            StripLeadingParaWs(line);
        }
        const bool indent = indent_first && !line.empty();
        if (!FlushLineToPage(line, pages, cur, y, line_h, line_gap_, viewport_h_, indent)) {
            return false;
        }
        first_line_of_para = false;
        return true;
    };

    while (i < text.size()) {
        if (text[i] == '\n') {
            if (!flush_line(first_line_of_para)) {
                return false;
            }
            line_w = 0;
            ++i;
            continue;
        }
        uint32_t cp = 0;
        const size_t n = Utf8Next(data + i, text.size() - i, &cp);
        if (n == 0) {
            break;
        }
        // 段首：吞掉源文前导空白，不参与折行/渲染；缩进仅由 pad_left 两字承担
        if (first_line_of_para && line.empty() && IsTxtParaIndentWs(cp)) {
            i += n;
            continue;
        }
        lv_coord_t cw = GlyphWidth(font_, cp);
        if ((cp == 0x20 || cp == 0xA0) && cw <= 0) {
            cw = std::max<lv_coord_t>(1, FontLineHeight(font_) / 3);
        }
        if (!line.empty() && line_w + cw > viewport_w_) {
            if (!flush_line(first_line_of_para)) {
                return false;
            }
            line_w = 0;
        }
        if (line.empty()) {
            line_w = first_line_of_para ? indent_w : 0;
        }
        try {
            line.append(text.data() + i, n);
        } catch (const std::bad_alloc&) {
            return false;
        }
        line_w += cw;
        i += n;
    }
    if (!line.empty()) {
        if (!flush_line(first_line_of_para)) {
            return false;
        }
    }
    return true;
}

bool BookSession::BuildPagesFromBlocks(const std::vector<ContentBlock>& blocks) {
    // 写入临时 built，成功后再 swap 进 pages_：
    // - 失败：pages_/page_index_ 不变（跨章翻页可回退到旧章）
    // - 峰值：切换章瞬间 ≈ 旧 pages_ + 新 built + blocks（靠转换拆章把单章压在预算内）
    std::vector<Page> built;
    Page cur;
    lv_coord_t y = 0;
    const lv_coord_t line_h = FontLineHeight(font_);

    auto fail_oom = []() -> bool {
        ESP_LOGE(TAG, "ebook/epub paginate OOM");
        return false;
    };

    try {
        for (size_t bi = 0; bi < blocks.size(); ++bi) {
            const auto& block = blocks[bi];
            if (block.kind == ContentKind::kText) {
                if (!PaginateTextBlock(block.text, built, cur, y)) {
                    return fail_oom();
                }
                if (!EnsurePageVerticalSpace(y, cur, built, para_gap_, viewport_h_)) {
                    return fail_oom();
                }
                y += para_gap_;
                continue;
            }
            lv_coord_t img_w = viewport_w_;
            lv_coord_t img_h = viewport_w_ * 3 / 4;
            if (page_images_mode_) {
                // 整页转图：每图独占一页，占满视口
                if (!cur.items.empty()) {
                    built.push_back(std::move(cur));
                    cur = Page{};
                    y = 0;
                }
                img_h = viewport_h_;
                PageItem item;
                item.kind = ContentKind::kImage;
                item.image_href = block.image_href;
                item.image_w = img_w;
                item.image_h = img_h;
                cur.items.push_back(std::move(item));
                built.push_back(std::move(cur));
                cur = Page{};
                y = 0;
                continue;
            }
            if (img_h > viewport_h_ - line_h) {
                img_h = viewport_h_ - line_h;
            }
            if (img_h < 40) {
                img_h = 40;
            }
            if (y + img_h + kImageGap > viewport_h_ && !cur.items.empty()) {
                built.push_back(std::move(cur));
                cur = Page{};
                y = 0;
            }
            if (img_h + kImageGap > viewport_h_ / 2 && !cur.items.empty()) {
                built.push_back(std::move(cur));
                cur = Page{};
                y = 0;
            }
            PageItem item;
            item.kind = ContentKind::kImage;
            item.image_href = block.image_href;
            item.image_w = img_w;
            item.image_h = img_h;
            cur.items.push_back(std::move(item));
            y += img_h + kImageGap;
        }

        if (!cur.items.empty()) {
            built.push_back(std::move(cur));
        }
        if (built.empty()) {
            Page empty;
            PageItem item;
            item.kind = ContentKind::kText;
            item.text = UiBlank();
            empty.items.push_back(std::move(item));
            built.push_back(std::move(empty));
        }
    } catch (const std::bad_alloc&) {
        return fail_oom();
    }

    pages_.swap(built);
    page_index_ = 0;
    ESP_LOGI(TAG, "paginated %u pages", static_cast<unsigned>(pages_.size()));
    return true;
}

bool BookSession::CountPagesFromBlocks(const std::vector<ContentBlock>& blocks, int& out_pages) const {
    // 与 BuildPagesFromBlocks 同口径，只计页、不存正文（扫全书页表用）
    out_pages = 0;
    int pages = 0;
    bool cur_has = false;
    lv_coord_t y = 0;
    const lv_coord_t line_h = FontLineHeight(font_);
    const lv_coord_t indent_w = TxtParaIndentWidth(font_);
    const lv_coord_t stride = line_h + line_gap_;

    auto break_page = [&]() {
        if (cur_has) {
            ++pages;
            cur_has = false;
            y = 0;
        }
    };

    auto ensure_space = [&](lv_coord_t need_h) {
        if (need_h > 0 && y + need_h > viewport_h_ && cur_has) {
            break_page();
        }
    };

    auto flush_line = [&](std::string& line, bool indent_first) {
        if (line.empty()) {
            return;
        }
        if (indent_first) {
            StripLeadingParaWs(line);
        }
        if (line.empty()) {
            return;
        }
        ensure_space(stride);
        cur_has = true;
        y += stride;
        line.clear();
    };

    auto paginate_text = [&](const std::string& text) {
        std::string line;
        lv_coord_t line_w = 0;
        bool first_line_of_para = true;
        const uint8_t* data = reinterpret_cast<const uint8_t*>(text.data());
        size_t i = 0;
        while (i < text.size() && text[i] == '\n') {
            ++i;
        }
        while (i < text.size()) {
            if (text[i] == '\n') {
                flush_line(line, first_line_of_para);
                first_line_of_para = false;
                line_w = 0;
                ++i;
                continue;
            }
            uint32_t cp = 0;
            const size_t n = Utf8Next(data + i, text.size() - i, &cp);
            if (n == 0) {
                break;
            }
            if (first_line_of_para && line.empty() && IsTxtParaIndentWs(cp)) {
                i += n;
                continue;
            }
            lv_coord_t cw = GlyphWidth(font_, cp);
            if ((cp == 0x20 || cp == 0xA0) && cw <= 0) {
                cw = std::max<lv_coord_t>(1, FontLineHeight(font_) / 3);
            }
            if (!line.empty() && line_w + cw > viewport_w_) {
                flush_line(line, first_line_of_para);
                first_line_of_para = false;
                line_w = 0;
            }
            if (line.empty()) {
                line_w = first_line_of_para ? indent_w : 0;
            }
            line.append(text.data() + i, n);
            line_w += cw;
            i += n;
        }
        if (!line.empty()) {
            flush_line(line, first_line_of_para);
        }
    };

    for (const auto& block : blocks) {
        if (block.kind == ContentKind::kText) {
            paginate_text(block.text);
            ensure_space(para_gap_);
            y += para_gap_;
            continue;
        }
        lv_coord_t img_w = viewport_w_;
        lv_coord_t img_h = viewport_w_ * 3 / 4;
        (void)img_w;
        if (page_images_mode_) {
            break_page();
            ++pages;
            cur_has = false;
            y = 0;
            continue;
        }
        if (img_h > viewport_h_ - line_h) {
            img_h = viewport_h_ - line_h;
        }
        if (img_h < 40) {
            img_h = 40;
        }
        if (y + img_h + kImageGap > viewport_h_ && cur_has) {
            break_page();
        }
        if (img_h + kImageGap > viewport_h_ / 2 && cur_has) {
            break_page();
        }
        cur_has = true;
        y += img_h + kImageGap;
    }

    if (cur_has) {
        ++pages;
    }
    if (pages <= 0) {
        pages = 1;
    }
    out_pages = pages;
    return true;
}

bool BookSession::EnsureCover(int max_w, int max_h) {
    if (cover_tried_) {
        return !cover_.empty();
    }
    cover_tried_ = true;
    cover_.Reset();

    if (info_.format == BookFormat::kEpub && epub_) {
        if (epub_->DecodeCoverImageToL8(max_w, max_h, cover_)) {
            return true;
        }
    }
    if (info_.format == BookFormat::kEbook && ebook_) {
        std::vector<uint8_t> bytes;
        if (ebook_->LoadCoverBytes(bytes) &&
            DecodeImageToL8(bytes.data(), bytes.size(), max_w, max_h, cover_)) {
            return true;
        }
    }
    return false;
}

bool BookSession::TryCopyCachedPageImage(const std::string& href, RasterImage& out) const {
    out.Reset();
    if (href.empty() || cached_href_ != href || cached_image_.empty()) {
        return false;
    }
    out.pixels = cached_image_.pixels;
    out.width = cached_image_.width;
    out.height = cached_image_.height;
    out.BindDsc();
    return true;
}

void BookSession::SetCachedPageImage(const std::string& href, RasterImage&& img) {
    if (href.empty() || img.empty()) {
        return;
    }
    cached_href_ = href;
    cached_image_ = std::move(img);
    cached_image_.BindDsc();
}

bool BookSession::LoadPageImage(const std::string& href, int max_w, int max_h, RasterImage& out) {
    out.Reset();
    if (href.empty()) {
        return false;
    }
    if (cached_href_ == href && !cached_image_.empty()) {
        out.pixels = cached_image_.pixels;
        out.width = cached_image_.width;
        out.height = cached_image_.height;
        out.BindDsc();
        return true;
    }
    if (info_.format == BookFormat::kEpub && epub_) {
        if (!epub_->DecodeItemImageToL8(href.c_str(), max_w, max_h, out)) {
            return false;
        }
    } else if (info_.format == BookFormat::kEbook && ebook_) {
        uint32_t off = 0;
        if (!EbookDocument::ParseImageHref(href, off)) {
            return false;
        }
        std::vector<uint8_t> bytes;
        if (!ebook_->LoadImageBlockBytes(off, bytes)) {
            return false;
        }
        if (!DecodeImageToL8(bytes.data(), bytes.size(), max_w, max_h, out)) {
            return false;
        }
    } else {
        if (!DecodeImageFileToL8(href.c_str(), max_w, max_h, out)) {
            return false;
        }
    }
    cached_href_ = href;
    cached_image_.pixels = out.pixels;
    cached_image_.width = out.width;
    cached_image_.height = out.height;
    cached_image_.BindDsc();
    return true;
}

const Page* BookSession::CurrentPageData() const {
    if (live_preview_active_) {
        return &live_preview_page_;
    }
    if (txt_mode_) {
        if (page_index_ < 0 || page_index_ >= PageCount()) {
            return nullptr;
        }
        if (txt_view_index_ != page_index_) {
            MaterializeTxtPage(page_index_);
        }
        return &txt_view_page_;
    }
    if (page_index_ < 0 || page_index_ >= PageCount()) {
        return nullptr;
    }
    return &pages_[static_cast<size_t>(page_index_)];
}

bool BookSession::GoToPage(int page) {
    if (live_preview_active_) {
        return false;
    }
    if (page < 0 || page >= PageCount()) {
        return false;
    }
    page_index_ = page;
    SaveProgress();
    return true;
}

bool BookSession::HasNextPage() const {
    if (live_preview_active_) {
        return live_preview_has_next_;
    }
    if (page_index_ + 1 < PageCount()) {
        return true;
    }
    return IsChapteredBook() && chapter_index_ + 1 < chapter_count_;
}

bool BookSession::HasPrevPage() const {
    if (live_preview_active_) {
        return !live_preview_hist_.empty();
    }
    if (page_index_ > 0) {
        return true;
    }
    return IsChapteredBook() && chapter_index_ > 0;
}

bool BookSession::NextPage() {
    if (live_preview_active_) {
        if (!live_preview_has_next_) {
            return false;
        }
        if (live_preview_hist_.size() < kLivePreviewHistMax) {
            try {
                live_preview_hist_.push_back(
                    LivePreviewHistEnt{live_preview_byte_off_, live_preview_at_para_start_});
            } catch (const std::bad_alloc&) {
                // 栈满则仍翻下一页，只是更早无法回退
            }
        } else if (!live_preview_hist_.empty()) {
            // ponytail: 满则丢最旧，保近期回退
            live_preview_hist_.erase(live_preview_hist_.begin());
            live_preview_hist_.push_back(
                LivePreviewHistEnt{live_preview_byte_off_, live_preview_at_para_start_});
        }
        const uint32_t off = live_preview_next_off_;
        const bool para = live_preview_next_at_para_start_;
        if (!FillTxtLivePreview(off, para)) {
            if (!live_preview_hist_.empty()) {
                live_preview_hist_.pop_back();
            }
            return false;
        }
        return true;
    }
    if (page_index_ + 1 < PageCount()) {
        ++page_index_;
        SaveProgress();
        return true;
    }
    if (IsChapteredBook() && chapter_index_ + 1 < chapter_count_) {
        const bool ok = (ebook_ != nullptr) ? LoadEbookChapter(chapter_index_ + 1)
                                            : LoadEpubChapter(chapter_index_ + 1);
        if (ok) {
            SaveProgress();
            return true;
        }
    }
    return false;
}

bool BookSession::PrevPage() {
    if (live_preview_active_) {
        if (live_preview_hist_.empty()) {
            return false;
        }
        const LivePreviewHistEnt ent = live_preview_hist_.back();
        live_preview_hist_.pop_back();
        if (!FillTxtLivePreview(ent.byte_off, ent.at_para_start)) {
            try {
                live_preview_hist_.push_back(ent);
            } catch (const std::bad_alloc&) {
            }
            return false;
        }
        return true;
    }
    if (page_index_ > 0) {
        --page_index_;
        SaveProgress();
        return true;
    }
    if (IsChapteredBook() && chapter_index_ > 0) {
        const bool ok = (ebook_ != nullptr) ? LoadEbookChapter(chapter_index_ - 1)
                                            : LoadEpubChapter(chapter_index_ - 1);
        if (ok) {
            page_index_ = std::max(0, PageCount() - 1);
            SaveProgress();
            return true;
        }
    }
    return false;
}


void BookSession::ClearToc() {
    toc_.clear();
}

void BookSession::ClearLivePreview() {
    live_preview_active_ = false;
    live_preview_byte_off_ = 0;
    live_preview_at_para_start_ = true;
    live_preview_next_off_ = 0;
    live_preview_next_at_para_start_ = true;
    live_preview_has_next_ = false;
    live_preview_page_ = Page{};
    live_preview_hist_.clear();
}

BookSession::LayoutAnchor BookSession::CaptureLayoutAnchor() const {
    LayoutAnchor a;
    if (!open_) {
        return a;
    }
    a.valid = true;
    a.txt = txt_mode_;
    a.chapter = chapter_index_;
    a.page = std::max(0, page_index_);
    a.page_count = std::max(1, PageCount());
    if (txt_mode_) {
        if (live_preview_active_) {
            a.txt_byte_off = live_preview_byte_off_;
            a.txt_at_para_start = live_preview_at_para_start_;
        } else if (!txt_empty_ && txt_lines_ != nullptr && txt_page_first_line_ != nullptr &&
                   page_index_ >= 0 && static_cast<size_t>(page_index_) < txt_page_count_) {
            const uint32_t line = txt_page_first_line_[static_cast<size_t>(page_index_)];
            if (line < txt_lines_len_) {
                a.txt_byte_off = txt_lines_[line].off;
                a.txt_at_para_start = (txt_lines_[line].len & kTxtLineParaStart) != 0;
            }
        }
    }
    return a;
}

bool BookSession::BeginLiveRelayout(LayoutAnchor* anchor_inout) {
    if (!open_ || font_ == nullptr) {
        return false;
    }
    LayoutAnchor anchor = CaptureLayoutAnchor();
    if (anchor_inout != nullptr && anchor_inout->valid) {
        anchor = *anchor_inout;
    } else if (anchor_inout != nullptr) {
        *anchor_inout = anchor;
    }
    if (!anchor.valid) {
        return false;
    }

    if (txt_mode_) {
        // 全量重建前把章首行映成字节锚点，编排中底栏/目录仍可用旧 toc_
        for (auto& ent : toc_) {
            if (ent.first_line < txt_lines_len_ && txt_lines_ != nullptr) {
                ent.first_byte_off = txt_lines_[ent.first_line].off;
            }
        }
        live_preview_hist_.clear();
        if (!FillTxtLivePreview(anchor.txt_byte_off, anchor.txt_at_para_start)) {
            ClearLivePreview();
            return false;
        }
        if (anchor_inout != nullptr) {
            *anchor_inout = anchor;
            anchor_inout->txt_byte_off = live_preview_byte_off_;
            anchor_inout->txt_at_para_start = live_preview_at_para_start_;
        }
        return true;
    }
    if (IsChapteredBook()) {
        ClearLivePreview();
        return RelayoutCurrentChapter(anchor);
    }
    return false;
}

int BookSession::TocCount() const {
    if (txt_mode_) {
        return static_cast<int>(toc_.size());
    }
    if (ebook_) {
        // 无原生目录标记时不回退成「按章数造目录」
        return ebook_->HasToc() ? static_cast<int>(toc_.size()) : 0;
    }
    if (IsChapteredBook()) {
        return !toc_.empty() ? static_cast<int>(toc_.size()) : chapter_count_;
    }
    return 0;
}

const TocEntry* BookSession::TocAt(int index) const {
    if (index < 0) {
        return nullptr;
    }
    if (static_cast<size_t>(index) >= toc_.size()) {
        return nullptr;
    }
    // TXT / EPUB / EBOOK 均可能填 toc_
    if (txt_mode_ || IsChapteredBook()) {
        return &toc_[static_cast<size_t>(index)];
    }
    return nullptr;
}

int BookSession::CurrentTocIndex() const {
    if (txt_mode_) {
        if (toc_.empty()) {
            return -1;
        }
        // 编排预览：按字节锚点；页码在全量重建中不可靠
        if (live_preview_active_) {
            const uint32_t off = live_preview_byte_off_;
            int best = 0;
            for (size_t i = 0; i < toc_.size(); ++i) {
                if (toc_[i].first_byte_off <= off) {
                    best = static_cast<int>(i);
                } else {
                    break;
                }
            }
            return best;
        }
        const int page = page_index_;
        int best = 0;
        for (size_t i = 0; i < toc_.size(); ++i) {
            if (toc_[i].first_page <= page) {
                best = static_cast<int>(i);
            } else {
                break;
            }
        }
        return best;
    }
    if (IsChapteredBook() && chapter_count_ > 0) {
        return chapter_index_;
    }
    return -1;
}

bool BookSession::GoToToc(int index) {
    if (txt_mode_) {
        if (index < 0 || static_cast<size_t>(index) >= toc_.size()) {
            return false;
        }
        if (live_preview_active_) {
            live_preview_hist_.clear();
            const uint32_t off = toc_[static_cast<size_t>(index)].first_byte_off;
            return FillTxtLivePreview(off, true);
        }
        return GoToPage(toc_[static_cast<size_t>(index)].first_page);
    }
    if (live_preview_active_) {
        return false;
    }
    if (ebook_ && !ebook_->HasToc()) {
        return false;
    }
    if (IsChapteredBook()) {
        if (index < 0 || index >= chapter_count_) {
            return false;
        }
        const bool ok = (ebook_ != nullptr) ? LoadEbookChapter(index) : LoadEpubChapter(index);
        if (!ok) {
            return false;
        }
        SaveProgress();
        return true;
    }
    return false;
}

std::string BookSession::CurrentTocTitle() const {
    if (txt_mode_) {
        const int i = CurrentTocIndex();
        if (i < 0 || static_cast<size_t>(i) >= toc_.size()) {
            return {};
        }
        return toc_[static_cast<size_t>(i)].title;
    }
    if (IsChapteredBook() && chapter_count_ > 0) {
        if (static_cast<size_t>(chapter_index_) < toc_.size()) {
            return toc_[static_cast<size_t>(chapter_index_)].title;
        }
        char buf[48];
        std::snprintf(buf, sizeof(buf), UiChapterFmt(), chapter_index_ + 1);
        return std::string(buf);
    }
    return {};
}

int BookSession::ReadingProgressX10() const {
    if (IsChapteredBook() && ChapterCount() > 1) {
        const int page = BookCurrentPage() + 1;
        const int pages = BookPageCount();
        if (pages <= 0) {
            return 0;
        }
        int pct_x10 = page * 1000 / pages;
        if (pct_x10 < 0) {
            pct_x10 = 0;
        }
        if (pct_x10 > 1000) {
            pct_x10 = 1000;
        }
        return pct_x10;
    }
    const int page = CurrentPage() + 1;
    const int pages = PageCount();
    if (pages <= 0) {
        return 0;
    }
    int pct_x10 = page * 1000 / pages;
    if (pct_x10 < 0) {
        pct_x10 = 0;
    }
    if (pct_x10 > 1000) {
        pct_x10 = 1000;
    }
    return pct_x10;
}


}  // namespace reader
