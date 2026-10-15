// main/display/screen/book_screen/book_font_manager.cc
#include "book_font_manager.h"
#include "book_reader_prefs.h"
#include "fontpack_lvgl.h"
#include "boards/common/sd_paths.h"
#include "assets/lang_config.h"

#include <esp_log.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <algorithm>

namespace book_font {

static const char* TAG = "book_font";

static constexpr const char* kBookFontPath = SD_PATH_BOOK_FONT;
static constexpr const char* kBookFontPathAlt = SD_PATH_BOOK_FONT_ALT;

static epdfont_t* s_epdfont = nullptr;

bool ParseEfSizePx(const std::string& name, int* size_px) {
    if (size_px == nullptr || name.size() < 8) {
        return false;
    }
    const size_t dot = name.rfind(".ef");
    if (dot == std::string::npos || dot + 3 != name.size()) {
        return false;
    }
    const size_t u2 = name.rfind('_', dot);
    if (u2 == std::string::npos || u2 == 0) {
        return false;
    }
    const size_t u1 = name.rfind('_', u2 - 1);
    if (u1 == std::string::npos) {
        return false;
    }
    int sz = 0;
    for (size_t i = u1 + 1; i < u2; ++i) {
        if (name[i] < '0' || name[i] > '9') {
            return false;
        }
        sz = sz * 10 + (name[i] - '0');
    }
    if (sz <= 0) {
        return false;
    }
    *size_px = sz;
    return true;
}

bool ReadFontEntryLess(const ReadFontEntry& a, const ReadFontEntry& b) {
    if (a.size_px != b.size_px) {
        return a.size_px < b.size_px;
    }
    return a.file < b.file;
}

void ScanReadFonts(std::vector<ReadFontEntry>& out_entries) {
    out_entries.clear();
    DIR* dir = opendir(SD_PATH_FONTS);
    if (dir == nullptr) {
        return;
    }
    while (dirent* ent = readdir(dir)) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        const size_t n = std::strlen(ent->d_name);
        if (n < 4 || n > 63 || std::strcmp(ent->d_name + (n - 3), ".ef") != 0) {
            continue;
        }
        char name[64];
        std::memcpy(name, ent->d_name, n + 1);
        char full[96];
        std::snprintf(full, sizeof(full), "%s/%s", SD_PATH_FONTS, name);
        struct stat st_info {};
        if (stat(full, &st_info) != 0 || !S_ISREG(st_info.st_mode)) {
            continue;
        }
        ReadFontEntry fe;
        fe.file = name;
        if (!ParseEfSizePx(fe.file, &fe.size_px)) {
            fe.size_px = 0;
        }
        out_entries.push_back(std::move(fe));
    }
    closedir(dir);
    std::sort(out_entries.begin(), out_entries.end(), ReadFontEntryLess);
}

bool DeleteReadFontFile(const std::string& file, std::string& err_out) {
    err_out.clear();
    if (file.empty() || file.size() > 63 || file.find('/') != std::string::npos ||
        file == "." || file == ".." || file.size() < 4 ||
        file.compare(file.size() - 3, 3, ".ef") != 0) {
        err_out = Lang::Strings::BOOK_FONT_NAME_INVALID;
        return false;
    }
    char path[96];
    std::snprintf(path, sizeof(path), "%s/%s", SD_PATH_FONTS, file.c_str());
    if (unlink(path) != 0 && errno != ENOENT) {
        err_out = Lang::Strings::BOOK_DELETE_FAILED;
        return false;
    }
    unlink((std::string(path) + ".tmp").c_str());
    ESP_LOGI(TAG, "deleted font %s", path);
    return true;
}

const lv_font_t* ListFont() {
    const lv_font_t* f = fontpack_lv_font_get(30, 2);
    return f != nullptr ? f : fontpack_lv_font_ui();
}

const lv_font_t* ItemFont() {
    return fontpack_lv_font_ui();
}

const lv_font_t* BookFont() {
    const lv_font_t* f = epdfont_get_lv_font(s_epdfont);
    return f != nullptr ? f : fontpack_lv_font_ui();
}

void EnsureBookFont() {
    if (s_epdfont != nullptr) {
        return;
    }
    char path[192];
    if (BookReaderPrefsFontFullPath(path, sizeof(path)) != nullptr) {
        s_epdfont = epdfont_open(path);
    }
    if (s_epdfont == nullptr) {
        s_epdfont = epdfont_open(kBookFontPath);
    }
    if (s_epdfont == nullptr) {
        s_epdfont = epdfont_open(kBookFontPathAlt);
    }
    if (s_epdfont == nullptr) {
        ESP_LOGW(TAG, "epdfont load failed (%s), fallback MiSans flash",
                 BookReaderPrefsFontFile());
    } else {
        ESP_LOGI(TAG, "epdfont loaded file=%s glyphs=%u bpp=%u", BookReaderPrefsFontFile(),
                 static_cast<unsigned>(epdfont_glyph_count(s_epdfont)), epdfont_get_bpp(s_epdfont));
    }
}

void ReleaseBookFont() {
    if (s_epdfont == nullptr) {
        return;
    }
    epdfont_close(s_epdfont);
    s_epdfont = nullptr;
}

void PrewarmPageFont(const reader::Page* page) {
    if (s_epdfont == nullptr || page == nullptr) {
        return;
    }
    std::string blob;
    blob.reserve(2048);
    for (const auto& item : page->items) {
        if (item.kind == reader::ContentKind::kText) {
            blob.append(item.text);
            blob.push_back('\n');
        }
    }
    if (!blob.empty()) {
        epdfont_prewarm_utf8(s_epdfont, blob.c_str(), blob.size());
    }
}

epdfont_t* GetActiveEpdFont() {
    return s_epdfont;
}

} // namespace book_font
