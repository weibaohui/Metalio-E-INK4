#pragma once

// BookSession 实现共享：原 book_session.cc 匿名命名空间，供按格式分文件复用。

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <new>

#include <esp_heap_caps.h>

#include "lvgl.h"
#include "reader_core/reader_types.h"
#include "reader_core/text_encoding.h"

#ifndef EBOOK_EMULATOR
#include "assets/lang_config.h"
#include "fontpack_lvgl.h"
#endif

namespace reader {
namespace book_session_detail {

extern const char* TAG;

const char* UiBlank();
const char* UiReadFileFail();
const char* UiChapterNoText();
const char* UiChapterFmt();

constexpr lv_coord_t kImageGap = 8;
constexpr size_t kMaxTxtBytes = 16 * 1024 * 1024;
constexpr size_t kTxtReadChunk = 1024;
constexpr uint32_t kMaxLineSourceBytes = 512;
constexpr uint32_t kTxtIdxVersion = 6;
constexpr uint16_t kTxtLineParaStart = 0x8000u;
constexpr uint16_t kTxtLineLenMask = 0x7FFFu;
constexpr size_t kMaxTocEntries = 8000;
constexpr size_t kMaxTocTitleBytes = 96;
constexpr uint32_t kMaxChapterUncompForPaginate = 384 * 1024;

#pragma pack(push, 1)
struct TxtIdxHeader {
    char magic[4];
    uint32_t version;
    uint32_t file_size;
    uint16_t viewport_w;
    uint16_t viewport_h;
    uint16_t line_gap;
    uint16_t para_gap;
    uint32_t font_fp;
    uint8_t as_gbk;
    uint8_t reserved[3];
    uint32_t line_count;
    uint32_t page_count;
};

struct TxtIdxLineDisk {
    uint32_t off;
    uint16_t len;
};
#pragma pack(pop)

const lv_font_t* DefaultFont();
lv_coord_t FontLineHeight(const lv_font_t* font);
lv_coord_t GlyphWidth(const lv_font_t* font, uint32_t cp);
bool IsTxtParaIndentWs(uint32_t cp);
lv_coord_t TxtParaIndentWidth(const lv_font_t* font);
void StripLeadingParaWs(std::string& s);
bool EnsurePageVerticalSpace(lv_coord_t& y, Page& cur, std::vector<Page>& pages, lv_coord_t need_h,
                             lv_coord_t viewport_h);
bool FlushLineToPage(std::string& line, std::vector<Page>& pages, Page& cur, lv_coord_t& y,
                     lv_coord_t line_h, lv_coord_t line_gap, lv_coord_t viewport_h,
                     bool para_indent = false);

struct TxtByteStream {
    FILE* fp = nullptr;
    bool as_gbk = false;
    uint8_t* buf = nullptr;
    size_t pos = 0;
    size_t end = 0;
    uint32_t file_base = 0;  // buf[0] 对应的文件偏移
    bool eof = false;

    TxtByteStream() = default;
    ~TxtByteStream() { FreeBuf(); }

    TxtByteStream(const TxtByteStream&) = delete;
    TxtByteStream& operator=(const TxtByteStream&) = delete;

    void FreeBuf() {
        if (buf != nullptr) {
            heap_caps_free(buf);
            buf = nullptr;
        }
    }

    bool InitBuf() {
        FreeBuf();
        buf = static_cast<uint8_t*>(
            heap_caps_malloc(kTxtReadChunk, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (buf == nullptr) {
            buf = static_cast<uint8_t*>(std::malloc(kTxtReadChunk));
        }
        return buf != nullptr;
    }

    uint32_t Tell() const { return file_base + static_cast<uint32_t>(pos); }

    bool Fill() {
        if (eof || buf == nullptr) {
            return false;
        }
        if (pos > 0 && pos < end) {
            const size_t keep = end - pos;
            std::memmove(buf, buf + pos, keep);
            file_base += static_cast<uint32_t>(pos);
            pos = 0;
            end = keep;
        } else if (pos >= end) {
            file_base += static_cast<uint32_t>(end);
            pos = 0;
            end = 0;
        }
        const size_t n = std::fread(buf + end, 1, kTxtReadChunk - end, fp);
        end += n;
        if (n == 0) {
            eof = true;
            return end > pos;
        }
        return true;
    }

    bool Ensure(size_t need) {
        while (end - pos < need) {
            if (eof) {
                return end > pos;
            }
            if (!Fill() && end <= pos) {
                return false;
            }
            if (eof && end - pos < need) {
                return end > pos;
            }
        }
        return true;
    }

    // 跳过 UTF-8 BOM（仅文件开头）
    void SkipBomIfAny() {
        if (file_base != 0 || pos != 0) {
            return;
        }
        if (!Ensure(3)) {
            return;
        }
        if (end - pos >= 3 && buf[pos] == 0xEF && buf[pos + 1] == 0xBB && buf[pos + 2] == 0xBF) {
            pos += 3;
        }
    }

    // 取下一显示字符。skip_cr：吃掉 \r 不产出。返回 false=EOF。
    // out_off/out_len：该字符在源文件中的字节区间（不含被跳过的 \r）。
    bool NextGlyph(uint32_t* cp, uint32_t* out_off, uint16_t* out_len, bool* is_newline) {
        *is_newline = false;
        for (;;) {
            if (!Ensure(1)) {
                return false;
            }
            const uint8_t b = buf[pos];
            if (b == '\r') {
                ++pos;
                continue;
            }
            *out_off = Tell();
            if (b == '\n') {
                ++pos;
                *cp = '\n';
                *out_len = 1;
                *is_newline = true;
                return true;
            }
            if (as_gbk) {
                if (b < 0x80) {
                    ++pos;
                    *cp = b;
                    *out_len = 1;
                    return true;
                }
                if (!Ensure(2)) {
                    // 残缺双字节
                    ++pos;
                    *cp = static_cast<uint32_t>('?');
                    *out_len = 1;
                    return true;
                }
                uint32_t uni = 0;
                const size_t n = GbkNext(buf + pos, end - pos, &uni);
                pos += n;
                *cp = uni;
                *out_len = static_cast<uint16_t>(n);
                return true;
            }
            uint32_t uni = 0;
            // UTF-8 最多 4 字节
            if (!Ensure(4) && end - pos < 1) {
                return false;
            }
            const size_t n = Utf8Next(buf + pos, end - pos, &uni);
            pos += n;
            *cp = uni;
            *out_len = static_cast<uint16_t>(n);
            return true;
        }
    }
};


bool IsProgressMagic(const char magic[4]);
bool MagicHasLifetime(const char magic[4]);
bool MagicHasSeconds(const char magic[4]);
bool MagicHasDaily(const char magic[4]);
constexpr int64_t kMaxFoldDeltaUs = 6LL * 3600LL * 1000000LL;

#pragma pack(push, 1)
struct Pos3Blob {
    char magic[4];
    uint32_t chapter;
    uint32_t page;
    uint32_t pct_x10;
    uint32_t reading_seconds;
    uint32_t day_id;
    uint32_t daily_seconds;
};
#pragma pack(pop)
static_assert(sizeof(Pos3Blob) == 28, "POS3 on-disk size");

constexpr uint32_t kChapterPageCacheVersion = 1;

#pragma pack(push, 1)
struct ChapterPageHdr {
    char magic[4];
    uint32_t version;
    uint32_t chapter_count;
    uint16_t viewport_w;
    uint16_t viewport_h;
    uint16_t line_gap;
    uint16_t para_gap;
    uint32_t font_fp;
    uint8_t page_images_mode;
    uint8_t reserved[3];
};
#pragma pack(pop)

}  // namespace book_session_detail
}  // namespace reader
