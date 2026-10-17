#include "book_detail_screen.h"
#include "book_bookshelf.h"
#include "book_cover_loader.h"
#include "book_font_manager.h"
#include "book_ui_state.h"

#include <algorithm>
#include <cstring>
#include <string>

#include <esp_log.h>

#include "assets/lang_config.h"
#include "fontpack_lvgl.h"
#include "display/screen/screen_common.h"
#include "display/screen/vk_key_handler.h"
#include "display/status_bar_manager.h"
#include "haptic_feedback.h"
#include "reader/book_cover_sidecar.h"
#include "reader_core/ebook_document.h"
#include "reader/book_stats_service.h"
#include "utils/psr_worker.h"

namespace book_screen_detail {

using reader::BookStatsService;

namespace {
const char* TAG = "BookDetail";

constexpr lv_coord_t kListPad = 16;
constexpr lv_coord_t kDetailCtaBottom = 24;
constexpr lv_coord_t kDetailSectionGap = 8;
constexpr lv_coord_t kDetailTitleLines = 1;
constexpr lv_coord_t kHomeShelfBtnH = 76;
constexpr uint32_t kDetailCoverWorkerStack = 16 * 1024;
constexpr const char* kScreenDetail = "book_detail";

using book_font::ItemFont;
using book_font::ListFont;
using book_ui::CoverFrameInner;
using book_ui::kCoverFrameBorder;
using book_ui::StyleBookCoverFrame;

struct DetailHooks {
    DetailStartReadFn start_read = nullptr;
    DetailCreateHomeScrFn create_home_scr = nullptr;
    DetailAttachVkFn attach_vk = nullptr;
    DetailReleaseSessionFn release_session = nullptr;
};
DetailHooks s_hooks;

struct DetailCoverWorkerArg {
    uint32_t token = 0;
    reader::BookInfo info;
    int max_w = 0;
    int max_h = 0;
};

struct DetailCoverDoneMsg {
    uint32_t token = 0;
    bool cover_ok = false;
    reader::RasterImage* cover = nullptr;
    std::string title;
    std::string author;
    std::string path;
};

void DisableScroll(lv_obj_t* obj) {
    if (obj != nullptr) {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    }
}

inline lv_coord_t ContentWidth() {
    return LV_HOR_RES - kListPad * 2;
}

void FillDetailCoverPlaceholder(lv_obj_t* host, reader::BookFormat format) {
    auto& st = State();
    if (host == nullptr) {
        return;
    }
    lv_obj_clean(host);
    lv_obj_t* ph = lv_obj_create(host);
    lv_obj_remove_style_all(ph);
    lv_obj_set_size(ph, st.detail_cover_fw, st.detail_cover_fh);
    lv_obj_set_style_pad_all(ph, kCoverFrameBorder, 0);
    lv_obj_set_style_bg_color(ph, lv_color_hex(0xF0F0F0), 0);
    lv_obj_set_style_bg_opa(ph, LV_OPA_COVER, 0);
    StyleBookCoverFrame(ph);
    lv_obj_t* ph_lbl = lv_label_create(ph);
    lv_label_set_text(ph_lbl, reader::FormatLabel(format));
    lv_obj_set_style_text_font(ph_lbl, ListFont(), 0);
    lv_obj_center(ph_lbl);
    DisableScroll(ph);
}

void ApplyDetailCoverImage() {
    auto& st = State();
    if (st.detail_cover_host == nullptr || !lv_obj_is_valid(st.detail_cover_host) ||
        st.detail_cover.empty()) {
        return;
    }
    st.detail_cover.BindDsc();
    lv_obj_clean(st.detail_cover_host);
    lv_obj_t* frame = lv_obj_create(st.detail_cover_host);
    lv_obj_remove_style_all(frame);
    lv_obj_set_size(frame, st.detail_cover_fw, st.detail_cover_fh);
    lv_obj_set_style_pad_all(frame, kCoverFrameBorder, 0);
    lv_obj_set_style_bg_color(frame, lv_color_hex(0xF0F0F0), 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    StyleBookCoverFrame(frame);
    lv_obj_clear_flag(frame, LV_OBJ_FLAG_CLICKABLE);
    DisableScroll(frame);
    lv_obj_t* img = lv_image_create(frame);
    lv_image_set_src(img, &st.detail_cover.dsc);
    lv_obj_center(img);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
}

void UpdateDetailMetaLabels(const reader::BookInfo& shown) {
    auto& st = State();
    if (st.detail_title_lbl != nullptr && lv_obj_is_valid(st.detail_title_lbl)) {
        lv_label_set_text(st.detail_title_lbl, shown.title.c_str());
    }
    if (st.detail_meta_lbl != nullptr && lv_obj_is_valid(st.detail_meta_lbl)) {
        lv_label_set_text(st.detail_meta_lbl, shown.author.empty() ? Lang::Strings::COMMON_UNKNOWN : shown.author.c_str());
    }
}

void AsyncDetailCoverDone(void* user_data) {
    auto* msg = static_cast<DetailCoverDoneMsg*>(user_data);
    auto& st = State();

    if (!msg->cover_ok) {
        RememberCoverEmbedMiss(msg->path);
    }

    if (msg->token != st.detail_cover_token.load()) {
        delete msg->cover;
        msg->cover = nullptr;
        delete msg;
        return;
    }

    const bool ui_gone = st.detail_scr == nullptr || !lv_obj_is_valid(st.detail_scr) ||
                         st.detail_cover_host == nullptr || !lv_obj_is_valid(st.detail_cover_host);
    if (ui_gone) {
        delete msg->cover;
        msg->cover = nullptr;
        delete msg;
        return;
    }

    if (st.selected >= 0 && st.selected < static_cast<int>(st.books.size()) &&
        st.books[static_cast<size_t>(st.selected)].path == msg->path) {
        if (!msg->title.empty()) {
            st.books[static_cast<size_t>(st.selected)].title = msg->title;
        }
        st.books[static_cast<size_t>(st.selected)].author = msg->author;
        UpdateDetailMetaLabels(st.books[static_cast<size_t>(st.selected)]);
    }

    if (msg->cover != nullptr && msg->cover_ok && !msg->cover->empty()) {
        st.detail_cover = std::move(*msg->cover);
        delete msg->cover;
        msg->cover = nullptr;
        ApplyDetailCoverImage();
    } else {
        delete msg->cover;
        msg->cover = nullptr;
    }
    delete msg;
}

void DetailCoverWorker(void* arg) {
    auto* ctx = static_cast<DetailCoverWorkerArg*>(arg);

    auto* cover = new reader::RasterImage();
    reader::BookInfo info = ctx->info;
    const bool ok = book_ui::LoadBookDetailCover(info, ctx->max_w, ctx->max_h, *cover) &&
                    !cover->empty();

    auto* done = new DetailCoverDoneMsg{};
    done->token = ctx->token;
    done->path = ctx->info.path;
    done->title = info.title;
    done->author = info.author;
    done->cover_ok = ok;
    if (ok) {
        done->cover = cover;
    } else {
        delete cover;
        done->cover = nullptr;
    }

    if (!ScreenLvAsync(AsyncDetailCoverDone, done)) {
        if (!ok) {
            PostCoverEmbedMiss(ctx->info.path);
        }
        delete done->cover;
        delete done;
    }
    delete ctx;
}

void OnDetailStartClicked(lv_event_t* /*e*/) {
    auto& st = State();
    if (s_hooks.start_read) {
        s_hooks.start_read(st.selected);
    }
}

}  // namespace

void SetDetailStartReadHandler(DetailStartReadFn fn) {
    s_hooks.start_read = fn;
}

void SetDetailCreateHomeScrHandler(DetailCreateHomeScrFn fn) {
    s_hooks.create_home_scr = fn;
}

void SetDetailAttachVkHandler(DetailAttachVkFn fn) {
    s_hooks.attach_vk = fn;
}

void SetDetailReleaseSessionHandler(DetailReleaseSessionFn fn) {
    s_hooks.release_session = fn;
}

void ClearDetailCoverUiPtrs() {
    auto& st = State();
    st.detail_cover_host = nullptr;
    st.detail_title_lbl = nullptr;
    st.detail_meta_lbl = nullptr;
}

void CancelDetailCoverLoad() {
    auto& st = State();
    st.detail_cover_token.fetch_add(1);
    ClearDetailCoverUiPtrs();
}

void StartDetailCoverWorker(int book_index) {
    auto& st = State();
    if (book_index < 0 || book_index >= static_cast<int>(st.books.size())) {
        return;
    }
    const uint32_t token = st.detail_cover_token.fetch_add(1) + 1;
    st.detail_format = st.books[static_cast<size_t>(book_index)].format;

    auto* ctx = new DetailCoverWorkerArg{};
    ctx->token = token;
    ctx->info = st.books[static_cast<size_t>(book_index)];
    ctx->max_w = CoverFrameInner(st.detail_cover_fw);
    ctx->max_h = CoverFrameInner(st.detail_cover_fh);
    if (!SpawnPsrWorker("book_cover", kDetailCoverWorkerStack, tskIDLE_PRIORITY + 2, DetailCoverWorker,
                        ctx, nullptr, 0)) {
        ESP_LOGW(TAG, "book_cover task failed; keep placeholder");
        delete ctx;
        st.detail_cover_token.fetch_add(1);
    }
}

lv_obj_t* CreateDetailScreen(int book_index) {
    auto& st = State();
    if (book_index < 0 || book_index >= static_cast<int>(st.books.size())) {
        return s_hooks.create_home_scr ? s_hooks.create_home_scr() : nullptr;
    }
    const reader::BookInfo& info = st.books[static_cast<size_t>(book_index)];
    {
        std::string book_id;
        if (info.format == reader::BookFormat::kEbook) {
            (void)reader::EbookDocument::PeekBookId(info.path.c_str(), book_id);
        }
        ESP_LOGI(TAG, "detail book_id=%s title=%s path=%s",
                 book_id.empty() ? "-" : book_id.c_str(), info.title.c_str(),
                 info.path.c_str());
    }
    st.selected = book_index;
    CancelDetailCoverLoad();
    if (st.detail_scr == nullptr) {
        st.detail_cover.Reset();
    }
    st.detail_format = info.format;

    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, ListFont(), 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    DisableScroll(scr);
    st.detail_scr = scr;
    lv_obj_add_event_cb(
        scr,
        [](lv_event_t* e) {
            auto& s = State();
            const auto* tgt = static_cast<lv_obj_t*>(lv_event_get_target(e));
            if (s.detail_scr == tgt) {
                s.detail_scr = nullptr;
                s.detail_cover_token.fetch_add(1);
                ClearDetailCoverUiPtrs();
            }
            if (s.detail_scr == nullptr) {
                s.detail_cover.Reset();
            }
        },
        LV_EVENT_DELETE, nullptr);

    EpdStatusBar status = ScreenCreateStatusBar(scr);
    StatusBarManager::GetInstance().SetOngoing("book", Lang::Strings::BOOK_DETAIL);
    lv_obj_add_event_cb(scr, [](lv_event_t*) {
        StatusBarManager::GetInstance().ClearOngoing("book");
    }, LV_EVENT_DELETE, nullptr);

    lv_obj_t* body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, LV_VER_RES - status.height);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, status.height);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_left(body, kListPad, 0);
    lv_obj_set_style_pad_right(body, kListPad, 0);
    lv_obj_set_style_pad_top(body, 10, 0);
    lv_obj_set_style_pad_bottom(body, kDetailCtaBottom, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(body, kDetailSectionGap, 0);
    DisableScroll(body);

    const reader::BookInfo& shown = st.books[static_cast<size_t>(book_index)];
    const auto peek = reader::BookSession::PeekProgress(shown.path.c_str());
    const lv_font_t* title_font = fontpack_lv_font_get(30, 4);
    if (title_font == nullptr) {
        title_font = ListFont();
    }
    const lv_font_t* item_font = ItemFont();
    const lv_coord_t title_lh =
        (title_font != nullptr && title_font->line_height > 0) ? title_font->line_height : 30;
    const lv_coord_t item_lh =
        (item_font != nullptr && item_font->line_height > 0) ? item_font->line_height : 25;
    const lv_font_t* num_font = fontpack_lv_font_get(30, 4);
    if (num_font == nullptr) {
        num_font = title_font;
    }

    const lv_coord_t usable_h =
        (LV_VER_RES - status.height) - 10 - kDetailCtaBottom - kDetailSectionGap;
    const lv_coord_t cover_slot_h = usable_h * 3 / 5 - 12;
    const lv_coord_t bottom_h = usable_h - cover_slot_h;
    {
        lv_coord_t cover_h = cover_slot_h;
        lv_coord_t cover_w = cover_h * 3 / 4;
        const lv_coord_t max_w = ContentWidth();
        if (cover_w > max_w) {
            cover_w = max_w;
            cover_h = cover_w * 4 / 3;
            if (cover_h > cover_slot_h) {
                cover_h = cover_slot_h;
                cover_w = cover_h * 3 / 4;
            }
        }
        if (cover_w < 80) {
            cover_w = 80;
        }
        if (cover_h < 100) {
            cover_h = 100;
        }
        st.detail_cover_fw = cover_w;
        st.detail_cover_fh = cover_h;
    }

    lv_obj_t* cover_host = lv_obj_create(body);
    st.detail_cover_host = cover_host;
    lv_obj_remove_style_all(cover_host);
    lv_obj_set_size(cover_host, ContentWidth(), cover_slot_h);
    lv_obj_set_style_bg_opa(cover_host, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(cover_host, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cover_host, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(cover_host, LV_OBJ_FLAG_CLICKABLE);
    DisableScroll(cover_host);

    const int detail_inner_w = CoverFrameInner(st.detail_cover_fw);
    const int detail_inner_h = CoverFrameInner(st.detail_cover_fh);
    const bool sidecar_adequate =
        reader::BookCoverSidecarAdequate(info.path.c_str(), detail_inner_w, detail_inner_h);
    const bool preview_ok = book_ui::TryLoadBookSidecarPreview(
        st.books[static_cast<size_t>(book_index)], detail_inner_w, detail_inner_h, st.detail_cover);
    if (preview_ok && !st.detail_cover.empty()) {
        ApplyDetailCoverImage();
    } else {
        st.detail_cover.Reset();
        FillDetailCoverPlaceholder(cover_host, info.format);
    }
    if (!sidecar_adequate &&
        (info.format == reader::BookFormat::kEpub || info.format == reader::BookFormat::kEbook) &&
        !IsCoverEmbedMiss(info.path)) {
        StartDetailCoverWorker(book_index);
    }

    lv_obj_t* bottom = lv_obj_create(body);
    lv_obj_remove_style_all(bottom);
    lv_obj_set_size(bottom, ContentWidth(), bottom_h);
    lv_obj_set_style_bg_opa(bottom, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(bottom, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(bottom, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(bottom, 6, 0);
    lv_obj_clear_flag(bottom, LV_OBJ_FLAG_CLICKABLE);
    DisableScroll(bottom);

    lv_obj_t* title = lv_label_create(bottom);
    st.detail_title_lbl = title;
    lv_label_set_text(title, shown.title.c_str());
    lv_obj_set_size(title, ContentWidth(), title_lh * kDetailTitleLines);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(title, title_font, 0);
    DisableScroll(title);

    lv_obj_t* author = lv_label_create(bottom);
    st.detail_meta_lbl = author;
    lv_label_set_text(author, shown.author.empty() ? Lang::Strings::COMMON_UNKNOWN : shown.author.c_str());
    lv_obj_set_size(author, ContentWidth(), item_lh);
    lv_label_set_long_mode(author, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(author, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(author, item_font, 0);
    lv_obj_set_style_text_color(author, lv_color_hex(0x666666), 0);
    DisableScroll(author);

    {
        lv_obj_t* meta = lv_obj_create(bottom);
        lv_obj_remove_style_all(meta);
        lv_obj_set_size(meta, ContentWidth(), item_lh + 20);
        lv_obj_set_style_layout(meta, LV_LAYOUT_NONE, 0);
        lv_obj_clear_flag(meta, LV_OBJ_FLAG_CLICKABLE);
        DisableScroll(meta);

        lv_obj_t* fmt = lv_label_create(meta);
        lv_label_set_text(fmt, reader::FormatLabel(shown.format));
        lv_obj_set_style_text_font(fmt, item_font, 0);
        lv_obj_align(fmt, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_clear_flag(fmt, LV_OBJ_FLAG_CLICKABLE);

        char size_buf[16];
        reader::FormatFileSize(size_buf, sizeof(size_buf), shown.file_size);
        lv_obj_t* sz = lv_label_create(meta);
        lv_label_set_text(sz, size_buf);
        lv_obj_set_style_text_font(sz, item_font, 0);
        lv_obj_align(sz, LV_ALIGN_TOP_RIGHT, 0, 0);
        lv_obj_clear_flag(sz, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* line_bot = lv_obj_create(meta);
        lv_obj_remove_style_all(line_bot);
        lv_obj_set_size(line_bot, ContentWidth(), 2);
        lv_obj_set_style_bg_color(line_bot, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(line_bot, LV_OPA_COVER, 0);
        lv_obj_align(line_bot, LV_ALIGN_BOTTOM_MID, 0, 0);
        lv_obj_clear_flag(line_bot, LV_OBJ_FLAG_CLICKABLE);
        DisableScroll(line_bot);

        lv_obj_t* line_top = lv_obj_create(meta);
        lv_obj_remove_style_all(line_top);
        lv_obj_set_size(line_top, ContentWidth(), 4);
        lv_obj_set_style_bg_color(line_top, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(line_top, LV_OPA_COVER, 0);
        lv_obj_align(line_top, LV_ALIGN_BOTTOM_MID, 0, -5);
        lv_obj_clear_flag(line_top, LV_OBJ_FLAG_CLICKABLE);
        DisableScroll(line_top);
    }

    {
        lv_obj_t* stats = lv_obj_create(bottom);
        lv_obj_remove_style_all(stats);
        lv_obj_set_size(stats, ContentWidth(), item_lh + 4 + 34);
        lv_obj_set_flex_flow(stats, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(stats, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(stats, LV_OBJ_FLAG_CLICKABLE);
        DisableScroll(stats);

        auto make_stat = [&](const char* key, uint32_t seconds) {
            lv_obj_t* col = lv_obj_create(stats);
            lv_obj_remove_style_all(col);
            lv_obj_set_width(col, (ContentWidth() - 20) / 2);
            lv_obj_set_height(col, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
            lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_row(col, 4, 0);
            lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);
            DisableScroll(col);
            lv_obj_t* k = lv_label_create(col);
            lv_label_set_text(k, key);
            lv_obj_set_style_text_font(k, item_font, 0);
            lv_obj_set_style_text_color(k, lv_color_hex(0x666666), 0);
            lv_obj_clear_flag(k, LV_OBJ_FLAG_CLICKABLE);
            char val[24];
            BookStatsService::FormatDurationShort(val, sizeof(val), seconds);
            lv_obj_t* v = lv_label_create(col);
            lv_label_set_text(v, val);
            lv_obj_set_style_text_font(v, num_font, 0);
            lv_obj_clear_flag(v, LV_OBJ_FLAG_CLICKABLE);
        };
        make_stat(Lang::Strings::BOOK_TODAY_DURATION, peek.daily_seconds);
        lv_obj_t* sep = lv_obj_create(stats);
        lv_obj_remove_style_all(sep);
        lv_obj_set_size(sep, 2, 40);
        lv_obj_set_style_bg_color(sep, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(sep, LV_OPA_COVER, 0);
        lv_obj_clear_flag(sep, LV_OBJ_FLAG_CLICKABLE);
        make_stat(Lang::Strings::BOOK_READ_DURATION, peek.reading_seconds);
    }

    {
        const int px10 = peek.progress_x10 >= 0 ? peek.progress_x10 : 0;
        const int bar_pct = px10 / 10;

        lv_obj_t* start_btn = lv_obj_create(bottom);
        lv_obj_remove_style_all(start_btn);
        lv_obj_set_size(start_btn, ContentWidth(), kHomeShelfBtnH);
        lv_obj_set_style_bg_color(start_btn, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(start_btn, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(start_btn, 14, 0);
        lv_obj_set_style_pad_hor(start_btn, 16, 0);
        lv_obj_set_flex_flow(start_btn, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(start_btn, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_add_flag(start_btn, LV_OBJ_FLAG_CLICKABLE);
        DisableScroll(start_btn);
        HapticAttachClick(start_btn);
        lv_obj_add_event_cb(start_btn, OnDetailStartClicked, LV_EVENT_CLICKED, nullptr);

        constexpr lv_coord_t kCtaBarH = 16;
        constexpr lv_coord_t kCtaBarInset = 3;
        const lv_coord_t cta_bar_w = ContentWidth() / 2;
        lv_obj_t* bar_track = lv_obj_create(start_btn);
        lv_obj_remove_style_all(bar_track);
        lv_obj_set_size(bar_track, cta_bar_w, kCtaBarH);
        lv_obj_set_style_bg_color(bar_track, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(bar_track, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar_track, kCtaBarH / 2, 0);
        lv_obj_set_style_clip_corner(bar_track, true, 0);
        lv_obj_set_style_pad_all(bar_track, 0, 0);
        lv_obj_set_style_layout(bar_track, LV_LAYOUT_NONE, 0);
        lv_obj_clear_flag(bar_track, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_clear_flag(bar_track, LV_OBJ_FLAG_SCROLLABLE);
        DisableScroll(bar_track);

        if (bar_pct > 0) {
            const lv_coord_t inner_h = kCtaBarH - 2 * kCtaBarInset;
            lv_coord_t fg_w =
                static_cast<lv_coord_t>((cta_bar_w - 2 * kCtaBarInset) * bar_pct / 100);
            if (fg_w < inner_h) {
                fg_w = inner_h;
            }
            if (fg_w > cta_bar_w - 2 * kCtaBarInset) {
                fg_w = cta_bar_w - 2 * kCtaBarInset;
            }
            lv_obj_t* bar_fg = lv_obj_create(bar_track);
            lv_obj_remove_style_all(bar_fg);
            lv_obj_set_size(bar_fg, fg_w, inner_h);
            lv_obj_set_pos(bar_fg, kCtaBarInset, kCtaBarInset);
            lv_obj_set_style_bg_color(bar_fg, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(bar_fg, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(bar_fg, inner_h / 2, 0);
            lv_obj_clear_flag(bar_fg, LV_OBJ_FLAG_CLICKABLE);
            DisableScroll(bar_fg);
        }

        lv_obj_t* cta_row = lv_obj_create(start_btn);
        lv_obj_remove_style_all(cta_row);
        lv_obj_set_size(cta_row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(cta_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(cta_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(cta_row, 8, 0);
        lv_obj_clear_flag(cta_row, LV_OBJ_FLAG_CLICKABLE);
        DisableScroll(cta_row);

        lv_obj_t* start_lbl = lv_label_create(cta_row);
        lv_label_set_text(start_lbl,
                          peek.progress_x10 >= 0 ? Lang::Strings::BOOK_CONTINUE
                                                 : Lang::Strings::BOOK_START);
        lv_obj_set_style_text_font(start_lbl, ListFont(), 0);
        lv_obj_set_style_text_color(start_lbl, lv_color_white(), 0);
        lv_obj_clear_flag(start_lbl, LV_OBJ_FLAG_CLICKABLE);

        const lv_coord_t play_h = title_lh > 0 ? title_lh : 28;
        const lv_coord_t play_w = play_h * 7 / 8;
        lv_obj_t* play = lv_obj_create(cta_row);
        lv_obj_remove_style_all(play);
        lv_obj_set_size(play, play_w, play_h);
        lv_obj_set_style_bg_opa(play, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(play, LV_OBJ_FLAG_CLICKABLE);
        DisableScroll(play);
        lv_obj_add_event_cb(
            play,
            [](lv_event_t* e) {
                if (lv_event_get_code(e) != LV_EVENT_DRAW_MAIN) {
                    return;
                }
                lv_obj_t* obj = static_cast<lv_obj_t*>(lv_event_get_target(e));
                lv_layer_t* layer = lv_event_get_layer(e);
                if (obj == nullptr || layer == nullptr) {
                    return;
                }
                lv_area_t a;
                lv_obj_get_content_coords(obj, &a);
                lv_draw_triangle_dsc_t tri;
                lv_draw_triangle_dsc_init(&tri);
                tri.color = lv_color_white();
                tri.opa = LV_OPA_COVER;
                tri.p[0].x = a.x1;
                tri.p[0].y = a.y1 + 1;
                tri.p[1].x = a.x1;
                tri.p[1].y = a.y2 - 1;
                tri.p[2].x = a.x2;
                tri.p[2].y = (a.y1 + a.y2) / 2;
                lv_draw_triangle(layer, &tri);
            },
            LV_EVENT_DRAW_MAIN, nullptr);
    }

    ScreenSetIsHome(false);
    if (s_hooks.attach_vk) {
        s_hooks.attach_vk(scr);
    }
    return scr;
}

lv_obj_t* ResumeDetailScreen() {
    auto& st = State();
    if (st.selected < 0 || st.selected >= static_cast<int>(st.books.size())) {
        return s_hooks.create_home_scr ? s_hooks.create_home_scr() : nullptr;
    }
    return CreateDetailScreen(st.selected);
}

void OpenDetailAsync(void* user_data) {
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(user_data));
    auto& st = State();
    if (index < 0 || index >= static_cast<int>(st.books.size())) {
        return;
    }
    st.selected = index;
    const char* screen = VkKey_ActiveScreen();
    if (screen != nullptr && std::strcmp(screen, kScreenBookshelf) == 0) {
        st.back_root = BookUiState::NavRoot::kShelf;
    } else {
        st.back_root = BookUiState::NavRoot::kHome;
    }
    ScreenLoadReplace(CreateDetailScreen(index));
}

void BackToDetailAsync(void* /*user_data*/) {
    auto& st = State();
    const int index = st.selected;
    if (s_hooks.release_session) {
        s_hooks.release_session();
    }
    if (index < 0 || index >= static_cast<int>(st.books.size())) {
        if (s_hooks.create_home_scr) {
            ScreenLoadReplace(s_hooks.create_home_scr());
        }
        return;
    }
    ScreenLoadReplace(CreateDetailScreen(index));
}

}  // namespace book_screen_detail
