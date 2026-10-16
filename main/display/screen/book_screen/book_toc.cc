#include "book_toc.h"

#include "book_cover_loader.h"
#include "book_font_manager.h"
#include "book_ui_state.h"

#include "assets/lang_config.h"
#include "haptic_feedback.h"
#include "screen_common.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <esp_log.h>

namespace book_screen_detail {

namespace {

constexpr const char* TAG = "BookToc";
constexpr lv_coord_t kListPad = 16;
constexpr lv_coord_t kReadPadTop = kListPad;
constexpr lv_coord_t kTocCoverW = 194;
constexpr lv_coord_t kTocCoverH = 259;
constexpr lv_coord_t kTocHeadGap = 16;
constexpr lv_coord_t kTocRowH = 44;
constexpr lv_coord_t kTocRowPad = 6;
constexpr lv_coord_t kTocHeadListGap = 12;
constexpr lv_coord_t kTocBelowStatus = 12;
constexpr lv_coord_t kTocLineThin = 1;
constexpr lv_coord_t kTocLineCur = 3;

TocFlushLayoutFn s_flush_layout = nullptr;
TocPrepareEnterFn s_prepare_enter = nullptr;
TocCancelListCoverFn s_cancel_list_cover = nullptr;
TocScheduleListCoverFn s_schedule_list_cover = nullptr;
TocEnqueueListCoverFn s_enqueue_list_cover = nullptr;
TocSetStatusVisibleFn s_set_status_visible = nullptr;
TocSetStatusTitleFn s_set_status_title = nullptr;
TocApplyOverlayPadsFn s_apply_overlay_pads = nullptr;
TocDetachBodyInputFn s_detach_body_input = nullptr;
TocTruncateFn s_truncate = nullptr;
TocUpdateFooterFn s_update_footer = nullptr;
TocResetLongPressFn s_reset_long_press = nullptr;
TocRenderReaderPageFn s_render_reader_page = nullptr;

ScreenPaintCoalesce s_toc_paint{};

void CallFlushLayout() {
    if (s_flush_layout != nullptr) {
        s_flush_layout();
    }
}

void CallPrepareEnter() {
    if (s_prepare_enter != nullptr) {
        s_prepare_enter();
    }
}

void CallCancelListCover() {
    if (s_cancel_list_cover != nullptr) {
        s_cancel_list_cover();
    }
}

void CallScheduleListCover() {
    if (s_schedule_list_cover != nullptr) {
        s_schedule_list_cover();
    }
}

void CallEnqueueListCover(const reader::BookInfo& info, int max_w, int max_h, lv_obj_t* host,
                          reader::RasterImage* slot) {
    if (s_enqueue_list_cover != nullptr) {
        s_enqueue_list_cover(info, max_w, max_h, host, slot);
    }
}

void CallSetStatusVisible(bool visible) {
    if (s_set_status_visible != nullptr) {
        s_set_status_visible(visible);
    }
}

void CallSetStatusTitle(const char* title) {
    if (s_set_status_title != nullptr) {
        s_set_status_title(title);
    }
}

void CallApplyOverlayPads(lv_coord_t top_pad) {
    if (s_apply_overlay_pads != nullptr) {
        s_apply_overlay_pads(top_pad);
    }
}

void CallDetachBodyInput(lv_obj_t* obj) {
    if (s_detach_body_input != nullptr) {
        s_detach_body_input(obj);
    }
}

std::string CallTruncate(const std::string& text, const lv_font_t* font, lv_coord_t max_w) {
    if (s_truncate != nullptr) {
        return s_truncate(text, font, max_w);
    }
    return text;
}

void CallUpdateFooter() {
    if (s_update_footer != nullptr) {
        s_update_footer();
    }
}

void CallResetLongPress() {
    if (s_reset_long_press != nullptr) {
        s_reset_long_press();
    }
}

void CallRenderReaderPage() {
    if (s_render_reader_page != nullptr) {
        s_render_reader_page();
    }
}

lv_coord_t ContentWidth() {
    return LV_HOR_RES - kListPad * 2;
}

void DisableScroll(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(obj, LV_DIR_NONE);
}

lv_coord_t MeasureTextWidth(const lv_font_t* font, const char* text) {
    if (font == nullptr || text == nullptr || text[0] == '\0') {
        return 0;
    }
    lv_point_t sz = {};
    lv_text_get_size(&sz, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return static_cast<lv_coord_t>(sz.x);
}

using book_font::ItemFont;
using book_font::ListFont;
using book_ui::CoverFrameInner;
using book_ui::StyleBookCoverFrame;
using book_ui::kCoverFrameBorder;

void OnTocRowClicked(lv_event_t* e) {
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    auto& st = State();
    if (!st.session || !st.session->IsOpen()) {
        return;
    }
    if (!st.session->GoToToc(index)) {
        return;
    }
    st.read_chrome = BookUiState::ReadChrome::kReading;
    CallRenderReaderPage();
}

void PaintTocList() {
    ESP_LOGI(TAG, "async RenderTocList page=%d begin", State().toc_list_page);
    RenderTocList();
    ESP_LOGI(TAG, "async RenderTocList done");
}

void EnsureTocPaintFn() {
    if (s_toc_paint.paint == nullptr) {
        s_toc_paint.paint = PaintTocList;
    }
}

}  // namespace

void ResetTocPaint() {
    ScreenPaintCoalesceReset(&s_toc_paint);
}

void RequestRenderTocList() {
    EnsureTocPaintFn();
    ScreenPaintCoalesceRequest(&s_toc_paint);
}

void OpenReadToc() {
    auto& st = State();
    if (st.read_scr == nullptr || st.opening.load() || !st.session || !st.session->IsOpen()) {
        return;
    }
    CallFlushLayout();
    if (st.read_chrome == BookUiState::ReadChrome::kToc) {
        return;
    }
    CallPrepareEnter();
    st.read_chrome = BookUiState::ReadChrome::kToc;
    // -1：等 RenderTocList 算出本页行数后再落到当前章所在页
    st.toc_list_page = -1;
    RenderTocList();
}

void RenderTocList() {
    auto& st = State();
    if (st.content == nullptr || !st.session || !st.session->IsOpen()) {
        return;
    }
    CallCancelListCover();
    st.open_bar = nullptr;
    st.open_pct_lbl = nullptr;
    CallSetStatusVisible(true);
    CallSetStatusTitle(nullptr);
    lv_obj_clean(st.content);
    // 顶栏盖住正文：书头下移，并与顶栏留一缝，避免封面顶死
    const lv_coord_t top_pad =
        (st.status_h > 0 ? st.status_h : kReadPadTop) + kTocBelowStatus;
    CallApplyOverlayPads(top_pad);
    lv_obj_set_style_layout(st.content, LV_LAYOUT_FLEX, 0);
    lv_obj_set_flex_flow(st.content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(st.content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(st.content, kTocHeadListGap, 0);
    DisableScroll(st.content);
    // 目录页不挂正文触摸（不退出/不翻页）；章行自带点击
    CallDetachBodyInput(st.content);
    lv_obj_clear_flag(st.content, LV_OBJ_FLAG_CLICKABLE);

    const reader::BookInfo& info = st.session->Info();
    const bool toc_has_page = (info.format == reader::BookFormat::kTxt);
    const int toc_n = st.session->TocCount();

    // 书头：封面 + 书名/作者（白底黑字，顶栏仍用现有状态栏）
    lv_obj_t* head = lv_obj_create(st.content);
    lv_obj_remove_style_all(head);
    lv_obj_set_width(head, ContentWidth());
    lv_obj_set_height(head, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(head, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(head, kTocHeadGap, 0);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_CLICKABLE);
    DisableScroll(head);

    lv_obj_t* cover_host = lv_obj_create(head);
    lv_obj_remove_style_all(cover_host);
    lv_obj_set_size(cover_host, kTocCoverW, kTocCoverH);
    lv_obj_set_style_bg_color(cover_host, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(cover_host, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(cover_host, kCoverFrameBorder, 0);
    StyleBookCoverFrame(cover_host);
    lv_obj_clear_flag(cover_host, LV_OBJ_FLAG_CLICKABLE);
    DisableScroll(cover_host);

    const int toc_inner_w = CoverFrameInner(kTocCoverW);
    const int toc_inner_h = CoverFrameInner(kTocCoverH);
    // 详情已删时复用 detail_cover；否则旁路秒读，无则占位并后台抽内嵌回写 .a2i1。
    if (st.detail_scr == nullptr || !lv_obj_is_valid(st.detail_scr)) {
        if (st.detail_cover.empty() || st.detail_cover.width + 8 < toc_inner_w ||
            st.detail_cover.height + 8 < toc_inner_h) {
            st.detail_cover.Reset();
            book_ui::TryLoadBookDetailSidecar(info, toc_inner_w, toc_inner_h, st.detail_cover);
        }
    }
    if (!st.detail_cover.empty()) {
        st.detail_cover.BindDsc();
        lv_obj_t* img = lv_image_create(cover_host);
        lv_image_set_src(img, &st.detail_cover.dsc);
        lv_obj_center(img);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_t* ph = lv_label_create(cover_host);
        lv_label_set_text(ph, reader::FormatLabel(info.format));
        lv_obj_set_style_text_font(ph, ItemFont(), 0);
        lv_obj_set_style_text_color(ph, lv_color_black(), 0);
        lv_obj_center(ph);
        lv_obj_clear_flag(ph, LV_OBJ_FLAG_CLICKABLE);
        CallEnqueueListCover(info, toc_inner_w, toc_inner_h, cover_host, &st.detail_cover);
        CallScheduleListCover();
    }

    lv_obj_t* meta = lv_obj_create(head);
    lv_obj_remove_style_all(meta);
    lv_obj_set_flex_grow(meta, 1);
    lv_obj_set_height(meta, kTocCoverH); // 与封面同高，便于作者贴底
    lv_obj_set_style_bg_opa(meta, LV_OPA_TRANSP, 0);
    lv_obj_set_style_layout(meta, LV_LAYOUT_NONE, 0);
    lv_obj_clear_flag(meta, LV_OBJ_FLAG_CLICKABLE);
    DisableScroll(meta);

    const lv_coord_t meta_w = ContentWidth() - kTocCoverW - kTocHeadGap;
    std::string book_title = st.session->Title();
    if (book_title.empty()) {
        book_title = info.title;
    }
    const lv_font_t* title_font = ListFont();
    const lv_coord_t title_lh =
        (title_font != nullptr && title_font->line_height > 0) ? title_font->line_height : 30;
    lv_obj_t* title_lbl = lv_label_create(meta);
    lv_obj_set_style_text_font(title_lbl, title_font, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_black(), 0);
    lv_obj_set_size(title_lbl, meta_w, title_lh * 3); // 最多三行，超出省略
    lv_label_set_long_mode(title_lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text(title_lbl, book_title.c_str());
    lv_obj_align(title_lbl, LV_ALIGN_TOP_LEFT, 0, 40); // 相对 meta 顶下移
    lv_obj_clear_flag(title_lbl, LV_OBJ_FLAG_CLICKABLE);

    std::string author = st.session->Author();
    if (author.empty()) {
        author = info.author;
    }
    if (author.empty()) {
        author = Lang::Strings::COMMON_UNKNOWN;
    }
    const lv_font_t* author_font = ItemFont();
    const lv_coord_t author_lh =
        (author_font != nullptr && author_font->line_height > 0) ? author_font->line_height : 25;
    lv_obj_t* author_lbl = lv_label_create(meta);
    lv_obj_set_style_text_font(author_lbl, author_font, 0);
    lv_obj_set_style_text_color(author_lbl, lv_color_black(), 0);
    lv_obj_set_size(author_lbl, meta_w, author_lh);
    lv_label_set_long_mode(author_lbl, LV_LABEL_LONG_DOT);
    lv_label_set_text(author_lbl, CallTruncate(author, author_font, meta_w).c_str());
    lv_obj_align(author_lbl, LV_ALIGN_BOTTOM_LEFT, 0, -12); // 相对 meta 底上移 12px
    lv_obj_clear_flag(author_lbl, LV_OBJ_FLAG_CLICKABLE);

    const lv_coord_t body_h = lv_obj_get_height(st.content);
    const lv_coord_t usable = body_h - top_pad - kTocCoverH - kTocHeadListGap;
    st.toc_page_size = std::max(1, static_cast<int>(usable / std::max<lv_coord_t>(1, kTocRowH)));
    // 进入目录时 toc_list_page==-1：按当前章定位（须在算出 page_size 之后）
    if (st.toc_list_page < 0) {
        const int cur0 = st.session->CurrentTocIndex();
        st.toc_list_page =
            (cur0 >= 0 && st.toc_page_size > 0) ? (cur0 / st.toc_page_size) : 0;
    }
    const int page_count =
        toc_n <= 0 ? 1
                   : static_cast<int>((toc_n + st.toc_page_size - 1) / st.toc_page_size);
    if (st.toc_list_page >= page_count) {
        st.toc_list_page = page_count - 1;
    }

    if (toc_n <= 0) {
        lv_obj_t* empty = lv_label_create(st.content);
        lv_obj_set_style_text_font(empty, ListFont(), 0);
        lv_obj_set_style_text_color(empty, lv_color_black(), 0);
        lv_label_set_text(empty, Lang::Strings::BOOK_TOC_EMPTY);
        lv_obj_set_width(empty, ContentWidth());
        lv_obj_clear_flag(empty, LV_OBJ_FLAG_CLICKABLE);
        CallUpdateFooter();
        CallResetLongPress();
        return;
    }

    lv_obj_t* list = lv_obj_create(st.content);
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, ContentWidth());
    lv_obj_set_height(list, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(list, 0, 0);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_CLICKABLE);
    DisableScroll(list);

    const int begin = st.toc_list_page * st.toc_page_size;
    const int end = std::min(toc_n, begin + st.toc_page_size);
    const int cur = st.session->CurrentTocIndex();
    const lv_font_t* row_font = ItemFont();
    for (int i = begin; i < end; ++i) {
        std::string title;
        int page_1based = -1;
        if (const reader::TocEntry* ent = st.session->TocAt(i)) {
            title = ent->title;
            if (toc_has_page) {
                page_1based = ent->first_page + 1;
            }
        } else {
            char b[48];
            std::snprintf(b, sizeof(b), Lang::Strings::BOOK_CHAPTER_FMT, i + 1);
            title = b;
        }
        const bool is_cur = (i == cur);
        const bool next_is_cur = (i + 1 == cur);
        const lv_coord_t line_h =
            (row_font != nullptr && row_font->line_height > 0) ? row_font->line_height : 30;
        const lv_coord_t row_h =
            std::max(kTocRowH, static_cast<lv_coord_t>(line_h + kTocRowPad * 2));

        char page_buf[16] = {};
        if (page_1based > 0) {
            std::snprintf(page_buf, sizeof(page_buf), "%d", page_1based);
        }
        const lv_coord_t page_w = MeasureTextWidth(row_font, page_buf);
        const lv_coord_t title_budget =
            std::max<lv_coord_t>(24, ContentWidth() - page_w - (page_w > 0 ? 16 : 0));
        const std::string display_title = CallTruncate(title, row_font, title_budget);

        lv_obj_t* row = lv_obj_create(list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, ContentWidth(), row_h);
        lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
        // 当前章：上下横线加粗；上一行若紧挨当前章则不画底线，避免叠细线
        if (is_cur) {
            lv_obj_set_style_border_width(row, kTocLineCur, 0);
            lv_obj_set_style_border_side(
                row, static_cast<lv_border_side_t>(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM), 0);
        } else if (next_is_cur) {
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_NONE, 0);
        } else {
            lv_obj_set_style_border_width(row, kTocLineThin, 0);
            lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        }
        lv_obj_set_style_border_color(row, lv_color_black(), 0);
        lv_obj_set_style_pad_ver(row, kTocRowPad, 0);
        lv_obj_set_style_layout(row, LV_LAYOUT_NONE, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(row);
        DisableScroll(row);
        lv_obj_add_event_cb(row, OnTocRowClicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(i)));
        lv_obj_add_event_cb(
            row,
            [](lv_event_t* ev) { lv_event_stop_bubbling(ev); },
            LV_EVENT_CLICKED, nullptr);

        lv_obj_t* lbl = lv_label_create(row);
        lv_obj_set_style_text_font(lbl, row_font, 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_set_size(lbl, title_budget, line_h);
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_CLIP);
        lv_label_set_text(lbl, display_title.c_str());
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);

        if (page_buf[0] != '\0') {
            lv_obj_t* page_lbl = lv_label_create(row);
            lv_obj_set_style_text_font(page_lbl, row_font, 0);
            lv_obj_set_style_text_color(page_lbl, lv_color_black(), 0);
            lv_label_set_text(page_lbl, page_buf);
            lv_obj_align(page_lbl, LV_ALIGN_RIGHT_MID, 0, 0);
            lv_obj_clear_flag(page_lbl, LV_OBJ_FLAG_CLICKABLE);
        }
    }
    CallUpdateFooter();
    CallResetLongPress();
    CallSetStatusVisible(true);
}

void SetTocFlushLayoutHandler(TocFlushLayoutFn fn) { s_flush_layout = fn; }
void SetTocPrepareEnterHandler(TocPrepareEnterFn fn) { s_prepare_enter = fn; }
void SetTocCancelListCoverHandler(TocCancelListCoverFn fn) { s_cancel_list_cover = fn; }
void SetTocScheduleListCoverHandler(TocScheduleListCoverFn fn) { s_schedule_list_cover = fn; }
void SetTocEnqueueListCoverHandler(TocEnqueueListCoverFn fn) { s_enqueue_list_cover = fn; }
void SetTocSetStatusVisibleHandler(TocSetStatusVisibleFn fn) { s_set_status_visible = fn; }
void SetTocSetStatusTitleHandler(TocSetStatusTitleFn fn) { s_set_status_title = fn; }
void SetTocApplyOverlayPadsHandler(TocApplyOverlayPadsFn fn) { s_apply_overlay_pads = fn; }
void SetTocDetachBodyInputHandler(TocDetachBodyInputFn fn) { s_detach_body_input = fn; }
void SetTocTruncateHandler(TocTruncateFn fn) { s_truncate = fn; }
void SetTocUpdateFooterHandler(TocUpdateFooterFn fn) { s_update_footer = fn; }
void SetTocResetLongPressHandler(TocResetLongPressFn fn) { s_reset_long_press = fn; }
void SetTocRenderReaderPageHandler(TocRenderReaderPageFn fn) { s_render_reader_page = fn; }

}  // namespace book_screen_detail
