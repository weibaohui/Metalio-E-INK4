#include "book_cover_loader.h"

#include "reader/book_cover_sidecar.h"
#include "reader_core/ebook_document.h"
#include "reader_core/epub_document.h"
#include "reader/image_util.h"
#include "reader_core/reader_image_budget.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

#include <esp_log.h>

namespace book_ui {
namespace {
constexpr const char* TAG = "BookCover";

const char* CoverBaseName(const std::string& path) {
    const char* s = std::strrchr(path.c_str(), '/');
    return (s != nullptr && s[1] != '\0') ? (s + 1) : path.c_str();
}

bool LoadCoverFromDocument(reader::BookInfo& info, reader::RasterImage& auth_out) {
    auth_out.Reset();
    if (info.format == reader::BookFormat::kEpub) {
        reader::EpubDocument doc;
        if (!doc.Open(info.path.c_str())) {
            return false;
        }
        if (!doc.Title().empty()) {
            info.title = doc.Title();
        }
        if (!doc.Author().empty()) {
            info.author = doc.Author();
        }
        const bool ok = doc.DecodeCoverImageToL8(reader::kCoverSidecarTargetW,
                                                 reader::kCoverSidecarTargetH, auth_out);
        if (!ok) {
            auth_out.Reset();
        }
        doc.Close();
        return ok;
    }

    if (info.format == reader::BookFormat::kEbook) {
        reader::EbookDocument doc;
        if (!doc.Open(info.path.c_str())) {
            return false;
        }
        if (!doc.Title().empty()) {
            info.title = doc.Title();
        }
        if (!doc.Author().empty()) {
            info.author = doc.Author();
        }
        std::vector<uint8_t> bytes;
        const bool ok =
            doc.LoadCoverBytes(bytes) && !bytes.empty() &&
            reader::DecodeImageToL8(bytes.data(), bytes.size(), reader::kCoverSidecarTargetW,
                                    reader::kCoverSidecarTargetH, auth_out);
        if (!ok) {
            auth_out.Reset();
        }
        doc.Close();
        return ok;
    }

    return false;
}

bool LoadSidecarScaled(const reader::BookInfo& info, int max_w, int max_h, reader::RasterImage& out) {
    out.Reset();
    char sidecar[192];
    if (!reader::BookCoverSidecarPath(info.path.c_str(), sidecar, sizeof(sidecar))) {
        return false;
    }
    if (!reader::DecodeImageFileToL8(sidecar, max_w, max_h, out) || out.empty()) {
        out.Reset();
        return false;
    }
    return true;
}

}  // namespace

bool TryLoadBookDetailSidecar(const reader::BookInfo& info, int max_w, int max_h,
                              reader::RasterImage& out) {
    out.Reset();
    if (max_w <= 0 || max_h <= 0 || info.path.empty()) {
        return false;
    }
    if (!reader::BookCoverSidecarAdequate(info.path.c_str(), max_w, max_h)) {
        return false;
    }
    if (!LoadSidecarScaled(info, max_w, max_h, out)) {
        return false;
    }
    ESP_LOGI(TAG, "cover ok sidecar %s", CoverBaseName(info.path));
    return true;
}

bool TryLoadBookSidecarPreview(const reader::BookInfo& info, int max_w, int max_h,
                               reader::RasterImage& out) {
    out.Reset();
    if (max_w <= 0 || max_h <= 0 || info.path.empty()) {
        return false;
    }
    if (!LoadSidecarScaled(info, max_w, max_h, out)) {
        return false;
    }
    ESP_LOGI(TAG, "cover preview sidecar %s (%ux%u)", CoverBaseName(info.path), out.width,
             out.height);
    return true;
}

bool LoadBookDetailCover(reader::BookInfo& info, int max_w, int max_h, reader::RasterImage& out) {
    out.Reset();
    if (max_w <= 0 || max_h <= 0 || info.path.empty()) {
        return false;
    }
    // 1) 旁路够大：秒开缩放到槽位
    if (TryLoadBookDetailSidecar(info, max_w, max_h, out)) {
        return true;
    }
    // 2) 抽内嵌 → 权威档解码 → 只升不降写旁路 → 缩到槽位显示
    reader::RasterImage auth;
    if (LoadCoverFromDocument(info, auth) && !auth.empty()) {
        reader::SaveBookCoverSidecarFromL8(info.path.c_str(), auth);
        if (!reader::ScaleRasterToFit(auth, max_w, max_h, out) || out.empty()) {
            out = std::move(auth);
            out.BindDsc();
        }
        ESP_LOGI(TAG, "cover ok embed %s (%ux%u)", CoverBaseName(info.path), out.width, out.height);
        return !out.empty();
    }
    if (info.format == reader::BookFormat::kEpub || info.format == reader::BookFormat::kEbook) {
        ESP_LOGW(TAG, "cover miss %s", CoverBaseName(info.path));
    }
    return false;
}

namespace {

lv_coord_t BookCoverRightRadius(lv_coord_t w, lv_coord_t h) {
    lv_coord_t r = w / 6;
    if (r < 10) {
        r = 10;
    }
    const lv_coord_t max_r = std::min(w, h) / 2 - 2;
    if (max_r > 0 && r > max_r) {
        r = max_r;
    }
    return r > 0 ? r : 1;
}

void CoverInkFill(lv_layer_t* layer, const lv_draw_rect_dsc_t& ink, lv_coord_t x1,
                  lv_coord_t y1, lv_coord_t x2, lv_coord_t y2) {
    if (x1 > x2 || y1 > y2) {
        return;
    }
    lv_area_t a;
    a.x1 = x1;
    a.y1 = y1;
    a.x2 = x2;
    a.y2 = y2;
    lv_draw_rect(layer, &ink, &a);
}

void EraseBookCoverRightEars(lv_layer_t* layer, const lv_area_t& box, lv_coord_t r) {
    lv_draw_rect_dsc_t rd;
    lv_draw_rect_dsc_init(&rd);
    rd.bg_color = lv_color_white();
    rd.bg_opa = LV_OPA_COVER;
    rd.border_width = 0;
    rd.radius = 0;
    const lv_coord_t cx = box.x2 - r;
    const int rr = static_cast<int>(r) * static_cast<int>(r);
    for (lv_coord_t dy = 0; dy < r; ++dy) {
        const int t = static_cast<int>(r - dy);
        const lv_coord_t x0 =
            cx + static_cast<lv_coord_t>(std::floor(std::sqrt(static_cast<float>(rr - t * t)))) + 1;
        if (x0 > box.x2) {
            continue;
        }
        CoverInkFill(layer, rd, x0, box.y1 + dy, box.x2, box.y1 + dy);
        CoverInkFill(layer, rd, x0, box.y2 - dy, box.x2, box.y2 - dy);
    }
}

void DrawBookCoverCornerArcs(lv_layer_t* layer, const lv_draw_rect_dsc_t& ink,
                             const lv_area_t& box, lv_coord_t r) {
    const lv_coord_t bw = kCoverFrameBorder;
    const lv_coord_t cx = box.x2 - r;
    const lv_coord_t cy_top = box.y1 + r;
    const lv_coord_t cy_bot = box.y2 - r;
    auto plot = [&](lv_coord_t x, lv_coord_t y) {
        if (x < box.x1 || x > box.x2 || y < box.y1 || y > box.y2) {
            return;
        }
        CoverInkFill(layer, ink, x, y, x + bw - 1, y + bw - 1);
    };

    int x = 0;
    int y = static_cast<int>(r);
    int err = 1 - y;
    while (x <= y) {
        plot(cx + x, cy_top - y);
        plot(cx + y, cy_top - x);
        plot(cx + x, cy_bot + y);
        plot(cx + y, cy_bot + x);
        if (err < 0) {
            err += 2 * x + 3;
        } else {
            err += 2 * (x - y) + 5;
            --y;
        }
        ++x;
    }
}

void OnBookCoverFrameDraw(lv_event_t* e) {
    lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
    lv_layer_t* layer = lv_event_get_layer(e);
    if (obj == nullptr || layer == nullptr) {
        return;
    }
    lv_area_t box;
    lv_obj_get_coords(obj, &box);
    const lv_coord_t w = lv_area_get_width(&box);
    const lv_coord_t h = lv_area_get_height(&box);
    if (w < 8 || h < 8) {
        return;
    }
    const lv_coord_t r = BookCoverRightRadius(w, h);
    EraseBookCoverRightEars(layer, box, r);
    DrawBookCoverBorder(layer, box, r);
}

}  // namespace

void DrawBookCoverBorder(lv_layer_t* layer, const lv_area_t& box, lv_coord_t r) {
    lv_draw_rect_dsc_t ink;
    lv_draw_rect_dsc_init(&ink);
    ink.bg_color = lv_color_black();
    ink.bg_opa = LV_OPA_COVER;
    ink.border_width = 0;
    ink.radius = 0;
    const lv_coord_t bw = kCoverFrameBorder;
    const lv_coord_t cx = box.x2 - r;

    // 左边框（整高）
    CoverInkFill(layer, ink, box.x1, box.y1, box.x1 + bw - 1, box.y2);
    // 顶/底直线接到圆角起点（含 cx，与弧端点重叠）
    CoverInkFill(layer, ink, box.x1, box.y1, cx, box.y1 + bw - 1);
    CoverInkFill(layer, ink, box.x1, box.y2 - bw + 1, cx, box.y2);
    // 右侧直线（含圆角端点行，与弧重叠）
    CoverInkFill(layer, ink, box.x2 - bw + 1, box.y1 + r, box.x2, box.y2 - r);

    // 书脊细线（左边框内侧）
    CoverInkFill(layer, ink, box.x1 + kCoverSpineInset, box.y1 + bw,
                 box.x1 + kCoverSpineInset + bw - 1, box.y2 - bw);

    DrawBookCoverCornerArcs(layer, ink, box, r);
}

void StyleBookCoverFrame(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_clip_corner(obj, false, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_add_event_cb(obj, OnBookCoverFrameDraw, LV_EVENT_DRAW_POST_END, nullptr);
}

}  // namespace book_ui

