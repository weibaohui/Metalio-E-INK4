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

void BookSession::ClearTxtIndex() {
    if (txt_fp_ != nullptr) {
        std::fclose(txt_fp_);
        txt_fp_ = nullptr;
    }
    if (txt_lines_ != nullptr) {
        heap_caps_free(txt_lines_);
        txt_lines_ = nullptr;
    }
    txt_lines_len_ = 0;
    txt_lines_cap_ = 0;
    if (txt_page_first_line_ != nullptr) {
        heap_caps_free(txt_page_first_line_);
        txt_page_first_line_ = nullptr;
    }
    txt_page_count_ = 0;
    txt_page_starts_cap_ = 0;
    txt_view_page_ = Page{};
    txt_view_index_ = -1;
    live_preview_active_ = false;
    live_preview_byte_off_ = 0;
    live_preview_at_para_start_ = true;
    live_preview_next_off_ = 0;
    live_preview_next_at_para_start_ = true;
    live_preview_has_next_ = false;
    live_preview_page_ = Page{};
    live_preview_hist_.clear();
    txt_mode_ = false;
    txt_as_gbk_ = false;
    txt_empty_ = false;
    ClearToc();
}

bool BookSession::GrowTxtLines(size_t need) {
    if (need <= txt_lines_cap_) {
        return true;
    }
    // 防止 ncap*=2 溢出成 0 死循环，以及 size 乘法溢出
    constexpr size_t kMaxLines = 2u * 1000u * 1000u;
    if (need > kMaxLines) {
        ESP_LOGE(TAG, "txt lines need=%u over max", static_cast<unsigned>(need));
        return false;
    }
    size_t ncap = txt_lines_cap_ == 0 ? 1024 : txt_lines_cap_;
    while (ncap < need) {
        if (ncap > kMaxLines / 2) {
            ncap = kMaxLines;
            break;
        }
        ncap *= 2;
    }
    if (ncap < need) {
        return false;
    }
    const size_t bytes = ncap * sizeof(TxtLineRef);
    if (bytes / sizeof(TxtLineRef) != ncap) {
        return false;
    }
    auto* np = static_cast<TxtLineRef*>(
        heap_caps_realloc(txt_lines_, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (np == nullptr) {
        np = static_cast<TxtLineRef*>(heap_caps_realloc(txt_lines_, bytes, MALLOC_CAP_8BIT));
    }
    if (np == nullptr) {
        ESP_LOGE(TAG,
                 "txt lines OOM need=%u ncap=%u bytes=%u len=%u int=%u spiram=%u",
                 static_cast<unsigned>(need), static_cast<unsigned>(ncap),
                 static_cast<unsigned>(bytes), static_cast<unsigned>(txt_lines_len_),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        return false;
    }
    txt_lines_ = np;
    txt_lines_cap_ = ncap;
    return true;
}

bool BookSession::GrowTxtPageStarts(size_t need) {
    if (need <= txt_page_starts_cap_) {
        return true;
    }
    constexpr size_t kMaxStarts = 500u * 1000u + 1u;
    if (need > kMaxStarts) {
        ESP_LOGE(TAG, "txt page starts need=%u over max", static_cast<unsigned>(need));
        return false;
    }
    size_t ncap = txt_page_starts_cap_ == 0 ? 64 : txt_page_starts_cap_;
    while (ncap < need) {
        if (ncap > kMaxStarts / 2) {
            ncap = kMaxStarts;
            break;
        }
        ncap *= 2;
    }
    if (ncap < need) {
        return false;
    }
    const size_t bytes = ncap * sizeof(uint32_t);
    if (bytes / sizeof(uint32_t) != ncap) {
        return false;
    }
    auto* np = static_cast<uint32_t*>(
        heap_caps_realloc(txt_page_first_line_, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (np == nullptr) {
        np = static_cast<uint32_t*>(heap_caps_realloc(txt_page_first_line_, bytes, MALLOC_CAP_8BIT));
    }
    if (np == nullptr) {
        ESP_LOGE(TAG,
                 "txt page starts OOM need=%u ncap=%u bytes=%u int=%u spiram=%u",
                 static_cast<unsigned>(need), static_cast<unsigned>(ncap),
                 static_cast<unsigned>(bytes),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        return false;
    }
    txt_page_first_line_ = np;
    txt_page_starts_cap_ = ncap;
    return true;
}

bool BookSession::EnsureTxtFile() const {
    if (txt_fp_ != nullptr) {
        return true;
    }
    if (info_.path.empty()) {
        return false;
    }
    txt_fp_ = std::fopen(info_.path.c_str(), "rb");
    return txt_fp_ != nullptr;
}

std::string BookSession::TxtIndexPath(const char* txt_path) {
    if (txt_path == nullptr) {
        return {};
    }
    return std::string(txt_path) + ".idx";
}

bool BookSession::TryLoadTxtIndex(const char* txt_path, size_t file_size) {
    const std::string idx_path = TxtIndexPath(txt_path);
    FILE* ifp = std::fopen(idx_path.c_str(), "rb");
    if (ifp == nullptr) {
        return false;
    }

    TxtIdxHeader hdr{};
    if (std::fread(&hdr, 1, sizeof(hdr), ifp) != sizeof(hdr)) {
        std::fclose(ifp);
        return false;
    }
    if (std::memcmp(hdr.magic, "TXI1", 4) != 0 || hdr.version != kTxtIdxVersion) {
        std::fclose(ifp);
        ESP_LOGW(TAG, "txt idx bad magic/ver: %s", idx_path.c_str());
        return false;
    }
    if (hdr.file_size != static_cast<uint32_t>(file_size) ||
        hdr.viewport_w != static_cast<uint16_t>(viewport_w_) ||
        hdr.viewport_h != static_cast<uint16_t>(viewport_h_) ||
        hdr.line_gap != static_cast<uint16_t>(line_gap_) ||
        hdr.para_gap != static_cast<uint16_t>(para_gap_) ||
        hdr.font_fp != FontFingerprint() ||
        hdr.as_gbk != static_cast<uint8_t>(txt_as_gbk_ ? 1 : 0) ||
        hdr.reserved[0] != 0 ||
        hdr.line_count == 0 || hdr.page_count == 0) {
        std::fclose(ifp);
        ESP_LOGI(TAG, "txt idx stale, will rebuild: %s", idx_path.c_str());
        return false;
    }
    // 合理上限，防止坏文件拖垮内存
    if (hdr.line_count > 2 * 1000 * 1000 || hdr.page_count > 500 * 1000) {
        std::fclose(ifp);
        ESP_LOGW(TAG, "txt idx absurd counts L=%u P=%u", static_cast<unsigned>(hdr.line_count),
                 static_cast<unsigned>(hdr.page_count));
        return false;
    }

    auto fail_cleanup = [&]() {
        std::fclose(ifp);
        // 半加载状态不可用，清长度避免误用；容量留给后续 StreamPaginate 复用
        txt_lines_len_ = 0;
        txt_page_count_ = 0;
        ClearToc();
    };

    if (!GrowTxtLines(hdr.line_count) || !GrowTxtPageStarts(hdr.page_count + 1)) {
        fail_cleanup();
        ESP_LOGE(TAG, "txt idx OOM L=%u P=%u", static_cast<unsigned>(hdr.line_count),
                 static_cast<unsigned>(hdr.page_count));
        return false;
    }

    for (uint32_t i = 0; i < hdr.line_count; ++i) {
        TxtIdxLineDisk rec{};
        if (std::fread(&rec, 1, sizeof(rec), ifp) != sizeof(rec)) {
            fail_cleanup();
            return false;
        }
        txt_lines_[i].off = rec.off;
        txt_lines_[i].len = rec.len;
    }
    txt_lines_len_ = hdr.line_count;

    if (std::fread(txt_page_first_line_, sizeof(uint32_t), hdr.page_count + 1, ifp) !=
        hdr.page_count + 1) {
        fail_cleanup();
        return false;
    }
    // 校验哨兵
    if (txt_page_first_line_[0] != 0 ||
        txt_page_first_line_[hdr.page_count] != hdr.line_count) {
        fail_cleanup();
        ESP_LOGW(TAG, "txt idx page table mismatch");
        return false;
    }
    txt_page_count_ = hdr.page_count;

    ClearToc();
    uint32_t toc_count = 0;
    if (std::fread(&toc_count, sizeof(toc_count), 1, ifp) != 1) {
        fail_cleanup();
        return false;
    }
    if (toc_count > kMaxTocEntries) {
        fail_cleanup();
        ESP_LOGW(TAG, "txt idx toc absurd %u", static_cast<unsigned>(toc_count));
        return false;
    }
    toc_.reserve(toc_count);
    for (uint32_t i = 0; i < toc_count; ++i) {
        uint32_t first_line = 0;
        uint16_t title_len = 0;
        if (std::fread(&first_line, sizeof(first_line), 1, ifp) != 1 ||
            std::fread(&title_len, sizeof(title_len), 1, ifp) != 1 || title_len > kMaxTocTitleBytes) {
            fail_cleanup();
            return false;
        }
        if (first_line >= hdr.line_count) {
            fail_cleanup();
            return false;
        }
        std::string title(title_len, '\0');
        if (title_len > 0 &&
            std::fread(title.data(), 1, title_len, ifp) != title_len) {
            fail_cleanup();
            return false;
        }
        TocEntry ent;
        ent.title = std::move(title);
        ent.first_line = first_line;
        toc_.push_back(std::move(ent));
    }
    FinalizeTocPages();

    std::fclose(ifp);
    ESP_LOGI(TAG, "txt idx hit %s lines=%u pages=%u toc=%u", idx_path.c_str(),
             static_cast<unsigned>(txt_lines_len_), static_cast<unsigned>(txt_page_count_),
             static_cast<unsigned>(toc_.size()));
    return true;
}

bool BookSession::SaveTxtIndex(const char* txt_path, size_t file_size) const {
    if (txt_path == nullptr || txt_lines_ == nullptr || txt_page_first_line_ == nullptr ||
        txt_page_count_ == 0 || txt_lines_len_ == 0) {
        return false;
    }
    const std::string idx_path = TxtIndexPath(txt_path);
    FILE* ofp = std::fopen(idx_path.c_str(), "wb");
    if (ofp == nullptr) {
        ESP_LOGW(TAG, "txt idx write open fail: %s", idx_path.c_str());
        return false;
    }

    TxtIdxHeader hdr{};
    std::memcpy(hdr.magic, "TXI1", 4);
    hdr.version = kTxtIdxVersion;
    hdr.file_size = static_cast<uint32_t>(file_size);
    hdr.viewport_w = static_cast<uint16_t>(viewport_w_);
    hdr.viewport_h = static_cast<uint16_t>(viewport_h_);
    hdr.line_gap = static_cast<uint16_t>(line_gap_);
    hdr.para_gap = static_cast<uint16_t>(para_gap_);
    hdr.font_fp = FontFingerprint();
    hdr.as_gbk = txt_as_gbk_ ? 1 : 0;
    hdr.reserved[0] = 0;
    hdr.line_count = static_cast<uint32_t>(txt_lines_len_);
    hdr.page_count = static_cast<uint32_t>(txt_page_count_);

    auto fail_write = [&]() {
        std::fclose(ofp);
        std::remove(idx_path.c_str());  // 避免留下半截 .idx 被误加载
    };

    if (std::fwrite(&hdr, 1, sizeof(hdr), ofp) != sizeof(hdr)) {
        fail_write();
        return false;
    }
    for (size_t i = 0; i < txt_lines_len_; ++i) {
        TxtIdxLineDisk rec{txt_lines_[i].off, txt_lines_[i].len};
        if (std::fwrite(&rec, 1, sizeof(rec), ofp) != sizeof(rec)) {
            fail_write();
            return false;
        }
    }
    if (std::fwrite(txt_page_first_line_, sizeof(uint32_t), txt_page_count_ + 1, ofp) !=
        txt_page_count_ + 1) {
        fail_write();
        return false;
    }
    const uint32_t toc_count = static_cast<uint32_t>(toc_.size());
    if (std::fwrite(&toc_count, sizeof(toc_count), 1, ofp) != 1) {
        fail_write();
        return false;
    }
    for (const auto& ent : toc_) {
        uint16_t title_len = static_cast<uint16_t>(
            ent.title.size() > kMaxTocTitleBytes ? kMaxTocTitleBytes : ent.title.size());
        if (std::fwrite(&ent.first_line, sizeof(ent.first_line), 1, ofp) != 1 ||
            std::fwrite(&title_len, sizeof(title_len), 1, ofp) != 1) {
            fail_write();
            return false;
        }
        if (title_len > 0 &&
            std::fwrite(ent.title.data(), 1, title_len, ofp) != title_len) {
            fail_write();
            return false;
        }
    }
    std::fclose(ofp);
    ESP_LOGI(TAG, "txt idx saved %s lines=%u pages=%u toc=%u", idx_path.c_str(),
             static_cast<unsigned>(txt_lines_len_), static_cast<unsigned>(txt_page_count_),
             static_cast<unsigned>(toc_count));
    return true;
}


void BookSession::MaterializeTxtPage(int page) const {
    txt_view_page_.items.clear();
    if (!txt_mode_ || page < 0 || static_cast<size_t>(page) >= txt_page_count_ ||
        txt_page_first_line_ == nullptr || txt_lines_ == nullptr) {
        txt_view_index_ = -1;
        return;
    }
    if (txt_empty_) {
        PageItem item;
        item.kind = ContentKind::kText;
        item.text = UiBlank();
        txt_view_page_.items.push_back(std::move(item));
        txt_view_index_ = page;
        return;
    }
    if (!EnsureTxtFile()) {
        PageItem item;
        item.kind = ContentKind::kText;
        item.text = UiReadFileFail();
        txt_view_page_.items.push_back(std::move(item));
        txt_view_index_ = page;
        return;
    }

    const uint32_t begin = txt_page_first_line_[static_cast<size_t>(page)];
    const uint32_t end = txt_page_first_line_[static_cast<size_t>(page) + 1];
    if (end < begin || end > txt_lines_len_) {
        ESP_LOGW(TAG, "txt page table OOB page=%d begin=%u end=%u lines=%u", page,
                 static_cast<unsigned>(begin), static_cast<unsigned>(end),
                 static_cast<unsigned>(txt_lines_len_));
        txt_view_index_ = -1;
        return;
    }
    txt_view_page_.items.reserve(end > begin ? (end - begin) : 0);

    uint8_t raw[kMaxLineSourceBytes + 4];
    for (uint32_t i = begin; i < end; ++i) {
        const TxtLineRef& lr = txt_lines_[i];
        PageItem item;
        item.kind = ContentKind::kText;
        const uint16_t raw_len = static_cast<uint16_t>(lr.len & kTxtLineLenMask);
        const bool para_start = (lr.len & kTxtLineParaStart) != 0;
        if (raw_len == 0) {
            continue;
        }
        if (std::fseek(txt_fp_, static_cast<long>(lr.off), SEEK_SET) != 0) {
            item.text = "?";
        } else {
            const size_t n = std::fread(raw, 1, raw_len, txt_fp_);
            if (!BytesToUtf8(raw, n, txt_as_gbk_, item.text)) {
                item.text = "?";
            } else if (para_start) {
                // 段首：去掉源文件前导空白；章名顶格不缩进，其余 pad_left 两字
                StripLeadingParaWs(item.text);
                item.para_gap_before = true;
                if (!item.text.empty() && !IsTxtChapterTitleLine(item.text)) {
                    item.para_indent = true;
                }
            }
        }
        txt_view_page_.items.push_back(std::move(item));
    }
    txt_view_index_ = page;
}

bool BookSession::StreamPaginateTxt(FILE* fp, size_t file_size) {
    txt_lines_len_ = 0;
    txt_page_count_ = 0;
    txt_view_index_ = -1;
    txt_view_page_ = Page{};
    // 编排中保留旧 toc_ 供底栏/目录；新目录写入临时表，结束时再替换
    std::vector<TocEntry> built_toc;
    struct TocBuildClear {
        BookSession* self;
        ~TocBuildClear() { self->toc_build_ = nullptr; }
    } toc_build_clear{this};
    toc_build_ = &built_toc;

    if (!GrowTxtPageStarts(2)) {
        ESP_LOGE(TAG, "txt page index OOM int=%u spiram=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        return false;
    }

    const size_t est_lines = file_size / 24 + 256;
    if (!GrowTxtLines(est_lines) || !GrowTxtPageStarts(est_lines / 16 + 32)) {
        ESP_LOGW(TAG, "txt index prealloc partial need_lines=%u int=%u spiram=%u",
                 static_cast<unsigned>(est_lines),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
    }

    txt_page_first_line_[0] = 0;
    size_t page_starts_len = 1;

    const lv_coord_t line_h = FontLineHeight(font_);
    const lv_coord_t indent_w = TxtParaIndentWidth(font_);
    lv_coord_t y = 0;
    uint32_t line_off = 0;
    uint32_t line_len = 0;
    lv_coord_t line_w = 0;
    bool line_started = false;
    bool line_para_start = false;  // 当前行是否为段首行（写入 len 高位）
    bool at_para_start = true;     // 下一非空内容为段首（按 \\n 分段）
    bool para_saw_indent_ws = false;
    bool para_flush_left = false;  // 本段源文顶格 → 可能是章名
    uint32_t para_first_line = 0;
    std::string para_title_acc;

    auto push_page_start = [&](uint32_t line_index) -> bool {
        if (!GrowTxtPageStarts(page_starts_len + 1)) {
            return false;
        }
        txt_page_first_line_[page_starts_len++] = line_index;
        return true;
    };

    const lv_coord_t line_stride = line_h + line_gap_;
    auto flush_line = [&](uint32_t next_off) -> bool {
        if (line_len == 0) {
            line_off = next_off;
            line_w = 0;
            line_started = false;
            line_para_start = false;
            return true;
        }
        if (y + line_stride > viewport_h_ &&
            txt_lines_len_ > txt_page_first_line_[page_starts_len - 1]) {
            if (!push_page_start(static_cast<uint32_t>(txt_lines_len_))) {
                return false;
            }
            y = 0;
        }
        if (!GrowTxtLines(txt_lines_len_ + 1)) {
            return false;
        }
        uint16_t store_len = line_len > kTxtLineLenMask ? kTxtLineLenMask : static_cast<uint16_t>(line_len);
        if (line_para_start && store_len > 0) {
            store_len = static_cast<uint16_t>(store_len | kTxtLineParaStart);
        }
        txt_lines_[txt_lines_len_++] = TxtLineRef{line_off, store_len};
        y += line_stride;
        line_off = next_off;
        line_len = 0;
        line_w = 0;
        line_started = false;
        line_para_start = false;
        return true;
    };

    TxtByteStream stream;
    if (!stream.InitBuf()) {
        ESP_LOGE(TAG, "txt stream buf OOM int=%u spiram=%u",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        return false;
    }
    stream.fp = fp;
    stream.as_gbk = txt_as_gbk_;
    std::rewind(fp);
    stream.SkipBomIfAny();

    const uint32_t paginate_gen = txt_paginate_gen_;
    uint32_t cp = 0;
    uint32_t goff = 0;
    uint16_t glen = 0;
    bool is_nl = false;
    size_t glyph_n = 0;
    while (stream.NextGlyph(&cp, &goff, &glen, &is_nl)) {
        if ((++glyph_n & 0x1FFu) == 0u) {
            if (paginate_gen != txt_paginate_gen_) {
                ESP_LOGI(TAG, "txt paginate aborted");
                return false;
            }
#ifndef EBOOK_EMULATOR
            // 全量扫描让出时间片，避免饿死 LVGL / 设置卡交互
            vTaskDelay(1);
#endif
        }
        if (file_size > 0) {
            const int pct = 10 + static_cast<int>(stream.Tell() * 80 / file_size);
            ReportOpenProgress(pct > 90 ? 90 : pct);
        }
        if (is_nl) {
            if (line_started) {
                const uint32_t chap_line = para_first_line;
                const bool try_chap = para_flush_left && !para_title_acc.empty();

                if (!flush_line(goff + glen)) {
                    ESP_LOGE(TAG, "txt paginate OOM");
                    return false;
                }
                if (try_chap) {
                    (void)TryPushTxtChapter(std::move(para_title_acc), chap_line);
                }
                para_title_acc.clear();
            }
            at_para_start = true;
            para_saw_indent_ws = false;
            para_flush_left = false;
            para_title_acc.clear();
            continue;
        }

        if (at_para_start && !line_started && IsTxtParaIndentWs(cp)) {
            para_saw_indent_ws = true;
            continue;
        }

        const lv_coord_t cw = GlyphWidth(font_, cp);
        if (line_started && (line_w + cw > viewport_w_ || line_len + glen > kMaxLineSourceBytes)) {
            if (!flush_line(goff)) {
                ESP_LOGE(TAG, "txt paginate OOM");
                return false;
            }
        }
        if (!line_started) {
            line_off = goff;
            line_started = true;
            if (at_para_start) {
                // 段间距计入分页高度（与渲染段首 pad_top 一致）；页顶不加
                if (y > 0) {
                    if (y + para_gap_ + line_stride > viewport_h_ &&
                        txt_lines_len_ > txt_page_first_line_[page_starts_len - 1]) {
                        if (!push_page_start(static_cast<uint32_t>(txt_lines_len_))) {
                            return false;
                        }
                        y = 0;
                    } else {
                        y += para_gap_;
                    }
                }
                para_flush_left = !para_saw_indent_ws;
                para_first_line = static_cast<uint32_t>(txt_lines_len_);
                para_title_acc.clear();
                line_w = indent_w;
                line_para_start = true;
                at_para_start = false;
            }
        }
        line_len += glen;
        line_w += cw;
        // 顶格段累加 UTF-8 供章名识别（不回 seek，避免打乱字节流）
        if (para_flush_left && para_title_acc.size() < kMaxTocTitleBytes) {
            char ubuf[8];
            size_t un = 0;
            if (cp < 0x80u) {
                ubuf[0] = static_cast<char>(cp);
                un = 1;
            } else if (cp < 0x800u) {
                ubuf[0] = static_cast<char>(0xC0u | (cp >> 6));
                ubuf[1] = static_cast<char>(0x80u | (cp & 0x3Fu));
                un = 2;
            } else if (cp < 0x10000u) {
                ubuf[0] = static_cast<char>(0xE0u | (cp >> 12));
                ubuf[1] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
                ubuf[2] = static_cast<char>(0x80u | (cp & 0x3Fu));
                un = 3;
            } else {
                ubuf[0] = static_cast<char>(0xF0u | (cp >> 18));
                ubuf[1] = static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
                ubuf[2] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
                ubuf[3] = static_cast<char>(0x80u | (cp & 0x3Fu));
                un = 4;
            }
            if (para_title_acc.size() + un <= kMaxTocTitleBytes) {
                para_title_acc.append(ubuf, un);
            }
        }
    }

    if (line_started) {
        const uint32_t end_off = stream.Tell();
        const uint32_t chap_line = para_first_line;
        const bool try_chap = para_flush_left && !para_title_acc.empty();
        if (!flush_line(end_off)) {
            ESP_LOGE(TAG, "txt paginate OOM");
            return false;
        }
        if (try_chap) {
            (void)TryPushTxtChapter(std::move(para_title_acc), chap_line);
        }
        para_title_acc.clear();
    } else if (txt_lines_len_ == 0) {
        if (!flush_line(stream.Tell())) {
            ESP_LOGE(TAG, "txt paginate OOM");
            return false;
        }
    }
    if (!push_page_start(static_cast<uint32_t>(txt_lines_len_))) {
        return false;
    }
    txt_page_count_ = page_starts_len - 1;
    toc_ = std::move(built_toc);
    FinalizeTocPages();

    ESP_LOGI(TAG, "txt stream index lines=%u pages=%u file=%u toc=%u",
             static_cast<unsigned>(txt_lines_len_), static_cast<unsigned>(txt_page_count_),
             static_cast<unsigned>(file_size),
             static_cast<unsigned>(toc_.size()));
    return true;
}

bool BookSession::OpenTxt(const char* path, size_t known_size) {
    if (path == nullptr) {
        ESP_LOGE(TAG, "txt open: path null");
        return false;
    }
    ESP_LOGI(TAG, "txt open begin int=%u spiram=%u known_size=%u path=%s",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             static_cast<unsigned>(known_size), path);
    ReportOpenProgress(5);

    // FatFS 上部分大文件/长文件名 fseek(END) 会 EIO；优先 stat / 书库已扫到的大小
    size_t sz = 0;
    const char* size_src = "none";
    struct stat st {};
    if (stat(path, &st) == 0 && st.st_size >= 0) {
        sz = static_cast<size_t>(st.st_size);
        size_src = "stat";
    } else if (known_size > 0) {
        sz = known_size;
        size_src = "bookinfo";
        ESP_LOGW(TAG, "txt stat fail errno=%d, use known_size=%u", errno,
                 static_cast<unsigned>(known_size));
    }

    FILE* fp = std::fopen(path, "rb");
    if (fp == nullptr) {
        ESP_LOGE(TAG, "txt fopen fail errno=%d int=%u spiram=%u path=%s", errno,
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)), path);
        return false;
    }

    if (sz == 0) {
        if (std::fseek(fp, 0, SEEK_END) != 0) {
            ESP_LOGE(TAG, "txt size unknown: fseek END fail errno=%d path=%s", errno, path);
            std::fclose(fp);
            return false;
        }
        const long sz_l = std::ftell(fp);
        if (sz_l < 0) {
            ESP_LOGE(TAG, "txt size unknown: ftell fail errno=%d path=%s", errno, path);
            std::fclose(fp);
            return false;
        }
        sz = static_cast<size_t>(sz_l);
        size_src = "fseek";
        std::rewind(fp);
    }
    ESP_LOGI(TAG, "txt size=%u via %s", static_cast<unsigned>(sz), size_src);

    txt_mode_ = true;
    chapter_count_ = 1;
    chapter_index_ = 0;

    if (sz == 0) {
        std::fclose(fp);
        txt_empty_ = true;
        if (!GrowTxtPageStarts(2) || !GrowTxtLines(1)) {
            ESP_LOGE(TAG, "txt empty index OOM");
            return false;
        }
        txt_page_first_line_[0] = 0;
        txt_lines_[0] = TxtLineRef{0, 0};
        txt_lines_len_ = 1;
        txt_page_first_line_[1] = 1;
        txt_page_count_ = 1;
        page_index_ = 0;
        ESP_LOGI(TAG, "txt empty ok: %s", path);
        ReportOpenProgress(90);
        return true;
    }
    if (sz > kMaxTxtBytes) {
        ESP_LOGE(TAG, "txt too large: %u > %u (%s)", static_cast<unsigned>(sz),
                 static_cast<unsigned>(kMaxTxtBytes), path);
        std::fclose(fp);
        return false;
    }

    // fopen 后已在文件头；勿再 SEEK_END（已证实部分 TXT 会 EIO）
    uint8_t peek[4096];
    const size_t npeek = std::fread(peek, 1, sizeof(peek), fp);
    txt_as_gbk_ = !LooksLikeUtf8(peek, npeek);

    const size_t est_lines = sz / 24 + 256;
    ESP_LOGI(TAG,
             "txt %s size=%u encoding=%s est_lines=%u est_idx~%uKB int=%u spiram=%u",
             path, static_cast<unsigned>(sz), txt_as_gbk_ ? "GBK" : "UTF-8",
             static_cast<unsigned>(est_lines),
             static_cast<unsigned>((est_lines * sizeof(TxtLineRef)) / 1024),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

    ReportOpenProgress(10);
    if (TryLoadTxtIndex(path, sz)) {
        txt_fp_ = fp;
        page_index_ = 0;
        ESP_LOGI(TAG, "txt ready pages=%d (from SD cache) lines=%u int=%u spiram=%u",
                 PageCount(), static_cast<unsigned>(txt_lines_len_),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        ReportOpenProgress(90);
        return true;
    }

    ESP_LOGI(TAG, "txt idx miss, stream paginate...");
    if (!StreamPaginateTxt(fp, sz)) {
        ESP_LOGE(TAG, "txt stream paginate fail lines=%u pages=%u int=%u spiram=%u",
                 static_cast<unsigned>(txt_lines_len_), static_cast<unsigned>(txt_page_count_),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        std::fclose(fp);
        ClearTxtIndex();
        return false;
    }

    ReportOpenProgress(92);
    // 索引落盘；失败不影响本次阅读
    if (!SaveTxtIndex(path, sz)) {
        ESP_LOGW(TAG, "txt idx save skipped/failed");
    }

    // 保留文件句柄供翻页物化；失败则 Materialize 时重开
    txt_fp_ = fp;
    page_index_ = 0;
    ESP_LOGI(TAG, "txt ready pages=%d (no full-text in RAM)", PageCount());
    return true;
}


void BookSession::AbortTxtPaginate() {
    txt_paginate_gen_++;
}

int BookSession::PageIndexForLine(uint32_t line) const {
    if (!txt_mode_ || txt_page_first_line_ == nullptr || txt_page_count_ == 0) {
        return 0;
    }
    // 最大的 p 使得 first_line[p] <= line < first_line[p+1]
    size_t lo = 0;
    size_t hi = txt_page_count_;
    while (lo + 1 < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (txt_page_first_line_[mid] <= line) {
            lo = mid;
        } else {
            hi = mid;
        }
    }
    return static_cast<int>(lo);
}

int BookSession::PageIndexForTxtByteOff(uint32_t byte_off) const {
    if (!txt_mode_ || txt_page_first_line_ == nullptr || txt_lines_ == nullptr ||
        txt_page_count_ == 0 || txt_lines_len_ == 0) {
        return 0;
    }
    int ans = 0;
    int lo = 0;
    int hi = static_cast<int>(txt_page_count_) - 1;
    while (lo <= hi) {
        const int mid = lo + (hi - lo) / 2;
        const uint32_t line = txt_page_first_line_[static_cast<size_t>(mid)];
        if (line >= txt_lines_len_) {
            hi = mid - 1;
            continue;
        }
        const uint32_t moff = txt_lines_[line].off;
        if (moff <= byte_off) {
            ans = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return ans;
}

bool BookSession::FillTxtLivePreview(uint32_t byte_off, bool at_para_start) {
    live_preview_page_ = Page{};
    live_preview_byte_off_ = byte_off;
    live_preview_at_para_start_ = at_para_start;
    live_preview_active_ = false;
    live_preview_next_off_ = byte_off;
    live_preview_next_at_para_start_ = at_para_start;
    live_preview_has_next_ = false;

    if (txt_empty_) {
        PageItem item;
        item.kind = ContentKind::kText;
        item.text = UiBlank();
        live_preview_page_.items.push_back(std::move(item));
        live_preview_active_ = true;
        return true;
    }
    if (!EnsureTxtFile() || font_ == nullptr) {
        return false;
    }
    if (std::fseek(txt_fp_, static_cast<long>(byte_off), SEEK_SET) != 0) {
        return false;
    }

    TxtByteStream stream;
    if (!stream.InitBuf()) {
        return false;
    }
    stream.fp = txt_fp_;
    stream.as_gbk = txt_as_gbk_;
    stream.file_base = byte_off;
    stream.pos = 0;
    stream.end = 0;
    stream.eof = false;
    if (byte_off == 0) {
        stream.SkipBomIfAny();
    }

    const lv_coord_t line_h = FontLineHeight(font_);
    const lv_coord_t indent_w = TxtParaIndentWidth(font_);
    const lv_coord_t line_stride = line_h + line_gap_;
    lv_coord_t y = 0;
    std::string line;
    lv_coord_t line_w = 0;
    bool line_started = false;
    bool line_para_start = false;
    bool para_at_start = at_para_start;
    bool para_saw_indent_ws = false;
    uint32_t line_start_off = byte_off;
    bool page_full = false;

    auto append_cp = [&](uint32_t cp) -> bool {
        char ubuf[8];
        size_t un = 0;
        if (cp < 0x80u) {
            ubuf[0] = static_cast<char>(cp);
            un = 1;
        } else if (cp < 0x800u) {
            ubuf[0] = static_cast<char>(0xC0u | (cp >> 6));
            ubuf[1] = static_cast<char>(0x80u | (cp & 0x3Fu));
            un = 2;
        } else if (cp < 0x10000u) {
            ubuf[0] = static_cast<char>(0xE0u | (cp >> 12));
            ubuf[1] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
            ubuf[2] = static_cast<char>(0x80u | (cp & 0x3Fu));
            un = 3;
        } else {
            ubuf[0] = static_cast<char>(0xF0u | (cp >> 18));
            ubuf[1] = static_cast<char>(0x80u | ((cp >> 12) & 0x3Fu));
            ubuf[2] = static_cast<char>(0x80u | ((cp >> 6) & 0x3Fu));
            ubuf[3] = static_cast<char>(0x80u | (cp & 0x3Fu));
            un = 4;
        }
        try {
            line.append(ubuf, un);
        } catch (const std::bad_alloc&) {
            return false;
        }
        return true;
    };

    auto flush_line = [&]() -> int {
        // 1=ok, 0=page full, -1=oom；标志与 MaterializeTxtPage 对齐
        if (line.empty()) {
            line_started = false;
            line_para_start = false;
            line_w = 0;
            return 1;
        }
        if (line_para_start) {
            StripLeadingParaWs(line);
        }
        if (line.empty()) {
            line_started = false;
            line_para_start = false;
            line_w = 0;
            return 1;
        }
        // 与 Materialize 一致：段首非章名 → pad_left 两字；有源文缩进空白的段必缩进
        const bool indent = line_para_start &&
                            (para_saw_indent_ws || !IsTxtChapterTitleLine(line));
        lv_coord_t need_y = y;
        if (line_para_start && y > 0) {
            if (y + para_gap_ + line_stride > viewport_h_ && !live_preview_page_.items.empty()) {
                return 0;
            }
            need_y += para_gap_;
        }
        if (need_y + line_stride > viewport_h_ && !live_preview_page_.items.empty()) {
            return 0;
        }
        if (line_para_start && y > 0) {
            y += para_gap_;
        }
        try {
            PageItem item;
            item.kind = ContentKind::kText;
            item.para_indent = indent;
            item.para_gap_before = line_para_start;
            item.text = std::move(line);
            live_preview_page_.items.push_back(std::move(item));
        } catch (const std::bad_alloc&) {
            line.clear();
            return -1;
        }
        line.clear();
        y += line_stride;
        line_started = false;
        line_para_start = false;
        para_saw_indent_ws = false;
        line_w = 0;
        return 1;
    };

    uint32_t cp = 0;
    uint32_t goff = 0;
    uint16_t glen = 0;
    bool is_nl = false;
    while (stream.NextGlyph(&cp, &goff, &glen, &is_nl)) {
        if (is_nl) {
            if (line_started) {
                const int fr = flush_line();
                if (fr == 0) {
                    live_preview_next_off_ = line_start_off;
                    live_preview_next_at_para_start_ = line_para_start;
                    live_preview_has_next_ = true;
                    page_full = true;
                    break;
                }
                if (fr < 0) {
                    return false;
                }
            }
            para_at_start = true;
            para_saw_indent_ws = false;
            continue;
        }
        if (para_at_start && !line_started && IsTxtParaIndentWs(cp)) {
            para_saw_indent_ws = true;
            continue;
        }
        lv_coord_t cw = GlyphWidth(font_, cp);
        if ((cp == 0x20 || cp == 0xA0) && cw <= 0) {
            cw = std::max<lv_coord_t>(1, FontLineHeight(font_) / 3);
        }
        if (line_started && (line_w + cw > viewport_w_ || line.size() + 4 > kMaxLineSourceBytes)) {
            const int fr = flush_line();
            if (fr == 0) {
                live_preview_next_off_ = line_start_off;
                live_preview_next_at_para_start_ = line_para_start;
                live_preview_has_next_ = true;
                page_full = true;
                break;
            }
            if (fr < 0) {
                return false;
            }
        }
        if (!line_started) {
            line_started = true;
            line_start_off = goff;
            if (para_at_start) {
                line_w = indent_w;
                line_para_start = true;
                para_at_start = false;
            } else {
                line_w = 0;
            }
        }
        if (!append_cp(cp)) {
            return false;
        }
        line_w += cw;
    }
    if (!page_full && line_started) {
        const int fr = flush_line();
        if (fr < 0) {
            return false;
        }
        if (fr == 0) {
            live_preview_next_off_ = line_start_off;
            live_preview_next_at_para_start_ = line_para_start;
            live_preview_has_next_ = true;
            page_full = true;
        }
    }
    if (!page_full) {
        live_preview_next_off_ = stream.Tell();
        live_preview_next_at_para_start_ = para_at_start;
        live_preview_has_next_ = false;
    }
    if (live_preview_page_.items.empty()) {
        PageItem item;
        item.kind = ContentKind::kText;
        item.text = UiBlank();
        live_preview_page_.items.push_back(std::move(item));
    }
    live_preview_active_ = true;
    return true;
}

bool BookSession::FinishTxtRelayout(const LayoutAnchor& anchor) {
    if (!open_ || !txt_mode_) {
        ClearLivePreview();
        return true;
    }
    if (txt_empty_) {
        ClearLivePreview();
        page_index_ = 0;
        SaveProgress();
        return true;
    }
    if (info_.path.empty()) {
        return false;
    }
    FILE* fp = std::fopen(info_.path.c_str(), "rb");
    if (fp == nullptr) {
        return false;
    }
    if (std::fseek(fp, 0, SEEK_END) != 0) {
        std::fclose(fp);
        return false;
    }
    const long sz_l = std::ftell(fp);
    if (sz_l < 0) {
        std::fclose(fp);
        return false;
    }
    const size_t sz = static_cast<size_t>(sz_l);
    OpenProgressFn saved_fn = progress_fn_;
    void* saved_user = progress_user_;
    progress_fn_ = nullptr;
    progress_user_ = nullptr;
    const bool ok = StreamPaginateTxt(fp, sz);
    progress_fn_ = saved_fn;
    progress_user_ = saved_user;
    std::fclose(fp);
    if (!ok) {
        return false;
    }
    if (!SaveTxtIndex(info_.path.c_str(), sz)) {
        ESP_LOGW(TAG, "txt idx save after relayout skipped");
    }
    // 优先用当前预览锚点（编排中可能已翻页/换参）
    const uint32_t off = live_preview_active_
                             ? live_preview_byte_off_
                             : (anchor.valid ? anchor.txt_byte_off : 0);
    page_index_ = PageIndexForTxtByteOff(off);
    if (page_index_ < 0) {
        page_index_ = 0;
    }
    if (page_index_ >= PageCount()) {
        page_index_ = std::max(0, PageCount() - 1);
    }
    txt_view_index_ = -1;
    ClearLivePreview();
    SaveProgress();
    ESP_LOGI(TAG, "txt relayout done pages=%d at=%d off=%u", PageCount(), page_index_ + 1,
             static_cast<unsigned>(off));
    return true;
}

void BookSession::FinalizeTocPages() {
    for (auto& ent : toc_) {
        ent.first_page = PageIndexForLine(ent.first_line);
        if (ent.first_line < txt_lines_len_ && txt_lines_ != nullptr) {
            ent.first_byte_off = txt_lines_[ent.first_line].off;
        }
    }
}

bool BookSession::TryPushTxtChapter(std::string title, uint32_t first_line) {
    auto& dest = toc_build_ != nullptr ? *toc_build_ : toc_;
    if (dest.size() >= kMaxTocEntries) {
        return false;
    }
    while (!title.empty() && (title.back() == '\r' || title.back() == '\n' || title.back() == ' ' ||
                              title.back() == '\t')) {
        title.pop_back();
    }
    StripLeadingParaWs(title);
    if (!IsTxtChapterTitleLine(title)) {
        return false;
    }
    if (title.size() > kMaxTocTitleBytes) {
        title.resize(kMaxTocTitleBytes);
    }
    if (!dest.empty() && dest.back().first_line == first_line) {
        return false;
    }
    TocEntry ent;
    ent.title = std::move(title);
    ent.first_line = first_line;
    if (first_line < txt_lines_len_ && txt_lines_ != nullptr) {
        ent.first_byte_off = txt_lines_[first_line].off;
    }
    dest.push_back(std::move(ent));
    return true;
}


}  // namespace reader
