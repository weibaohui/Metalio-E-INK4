#include "book_session_internal.h"

#include <new>

namespace reader {
namespace book_session_detail {

const char* TAG = "BookSession";

#ifndef EBOOK_EMULATOR
const char* UiBlank() { return Lang::Strings::BOOK_BLANK; }
const char* UiReadFileFail() { return Lang::Strings::BOOK_READ_FILE_FAIL; }
const char* UiChapterNoText() { return Lang::Strings::BOOK_CHAPTER_NO_TEXT; }
const char* UiChapterFmt() { return Lang::Strings::BOOK_CHAPTER_FMT; }
#else
const char* UiBlank() { return "(Blank)"; }
const char* UiReadFileFail() { return "(Read failed)"; }
const char* UiChapterNoText() { return "(No text in chapter)"; }
const char* UiChapterFmt() { return "Ch.%d"; }
#endif

const lv_font_t* DefaultFont() {
#ifdef EBOOK_EMULATOR
    return nullptr;
#else
    return fontpack_lv_font_ui();
#endif
}

lv_coord_t FontLineHeight(const lv_font_t* font) {
    return font != nullptr ? font->line_height : 28;
}

lv_coord_t GlyphWidth(const lv_font_t* font, uint32_t cp) {
    if (font == nullptr) {
        return 12;
    }
    const lv_coord_t w = static_cast<lv_coord_t>(lv_font_get_glyph_width(font, cp, 0));
    return w > 0 ? w : 1;
}

bool IsTxtParaIndentWs(uint32_t cp) {
    return cp == ' ' || cp == '\t' || cp == 0xA0u || cp == 0x3000u;
}

lv_coord_t TxtParaIndentWidth(const lv_font_t* font) {
    // 与渲染侧一致：优先用「一」字宽（字库必有），勿依赖 U+3000
    lv_coord_t w = GlyphWidth(font, 0x4E00u);
    if (w <= 1) {
        w = GlyphWidth(font, 0x3000u);
    }
    if (w <= 1) {
        w = FontLineHeight(font) / 2;
        if (w < 1) {
            w = 12;
        }
    }
    return w * 2;
}

void StripLeadingParaWs(std::string& s) {
    size_t i = 0;
    const uint8_t* data = reinterpret_cast<const uint8_t*>(s.data());
    while (i < s.size()) {
        uint32_t cp = 0;
        const size_t n = Utf8Next(data + i, s.size() - i, &cp);
        if (n == 0 || !IsTxtParaIndentWs(cp)) {
            break;
        }
        i += n;
    }
    if (i > 0) {
        s.erase(0, i);
    }
}

bool EnsurePageVerticalSpace(lv_coord_t& y, Page& cur, std::vector<Page>& pages, lv_coord_t need_h,
                             lv_coord_t viewport_h) {
    if (need_h <= 0) {
        return true;
    }
    if (y + need_h > viewport_h && !cur.items.empty()) {
        try {
            pages.push_back(std::move(cur));
        } catch (const std::bad_alloc&) {
            return false;
        }
        cur = Page{};
        y = 0;
    }
    return true;
}

bool FlushLineToPage(std::string& line, std::vector<Page>& pages, Page& cur, lv_coord_t& y,
                     lv_coord_t line_h, lv_coord_t line_gap, lv_coord_t viewport_h,
                     bool para_indent) {
    if (line.empty()) {
        return true;
    }
    const lv_coord_t stride = line_h + line_gap;
    if (!EnsurePageVerticalSpace(y, cur, pages, stride, viewport_h)) {
        return false;
    }
    try {
        PageItem item;
        item.kind = ContentKind::kText;
        item.para_indent = para_indent;
        item.para_gap_before = para_indent;  // ebook/EPUB 段首行（与缩进同行）
        item.text = std::move(line);
        line.clear();
        cur.items.push_back(std::move(item));
    } catch (const std::bad_alloc&) {
        line.clear();
        return false;
    }
    y += stride;
    return true;
}


bool IsProgressMagic(const char magic[4]) {
    return std::memcmp(magic, "POS1", 4) == 0 || std::memcmp(magic, "POS2", 4) == 0 ||
           std::memcmp(magic, "POS3", 4) == 0;
}

bool MagicHasLifetime(const char magic[4]) {
    return std::memcmp(magic, "POS2", 4) == 0 || std::memcmp(magic, "POS3", 4) == 0;
}

bool MagicHasSeconds(const char magic[4]) {
    return MagicHasLifetime(magic);
}

bool MagicHasDaily(const char magic[4]) {
    return std::memcmp(magic, "POS3", 4) == 0;
}


}  // namespace book_session_detail
}  // namespace reader
