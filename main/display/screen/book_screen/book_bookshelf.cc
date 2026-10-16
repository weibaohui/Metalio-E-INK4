#include "book_bookshelf.h"

#include "book_cover_loader.h"
#include "book_font_manager.h"
#include "book_ui_state.h"

#include "SdCardManager.hpp"
#include "assets/lang_config.h"
#include "display/status_bar_manager.h"
#include "haptic_feedback.h"
#include "reader/book_cover_sidecar.h"
#include "reader/book_home_snapshot.h"
#include "reader/book_progress_cache.h"
#include "reader/reader.h"
#include "screen_common.h"
#include "sd_paths.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <unistd.h>

#include <esp_log.h>
#include <esp_timer.h>

namespace book_screen_detail {

namespace {

constexpr const char* TAG = "BookShelf";
constexpr lv_coord_t kListPad = 16;
constexpr lv_coord_t kFooterH = 36;
constexpr lv_coord_t kShelfMultiBtnH = 36;
constexpr lv_coord_t kShelfUnderlineH = 1;
constexpr lv_coord_t kShelfUnderlinePadHor = 5;
constexpr lv_coord_t kShelfMultiDotSize = 6;
constexpr lv_coord_t kShelfCheckSize = 24;
constexpr int64_t kShelfSuppressRowClickUs = 400000;
constexpr uint64_t kShelfCheckPaintDebounceUs = 280000;
constexpr lv_coord_t kShelfTitleGap = 6;
constexpr int kShelfCols = 3;
constexpr int kShelfRows = 3;
constexpr lv_coord_t kShelfColGap = 12;
constexpr lv_coord_t kShelfRowGap = 16;
constexpr lv_coord_t kRowBorderW = 2;

BookshelfPrepareEnterFn s_prepare_enter = nullptr;
BookshelfCancelListCoverFn s_cancel_list_cover = nullptr;
BookshelfScheduleListCoverFn s_schedule_list_cover = nullptr;
BookshelfEnqueueListCoverFn s_enqueue_list_cover = nullptr;
BookshelfCreateCoverSlotFn s_create_cover_slot = nullptr;
BookshelfOpenDetailFn s_open_detail = nullptr;
BookshelfTruncateFn s_truncate = nullptr;
BookshelfAttachVkFn s_attach_vk = nullptr;

ScreenPaintCoalesce s_library_paint{};
ScreenPaintCoalesce s_shelf_check_paint{};

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

lv_obj_t* CallCreateCoverSlot(lv_obj_t* parent, const reader::BookInfo& info, lv_coord_t w,
                              lv_coord_t h, reader::RasterImage* cover_out, bool* need_embed_fill) {
    if (s_create_cover_slot == nullptr) {
        return nullptr;
    }
    return s_create_cover_slot(parent, info, w, h, cover_out, need_embed_fill);
}

void CallOpenDetail(int index) {
    if (s_open_detail != nullptr) {
        s_open_detail(index);
    }
}

std::string CallTruncate(const std::string& text, const lv_font_t* font, lv_coord_t max_w) {
    if (s_truncate != nullptr) {
        return s_truncate(text, font, max_w);
    }
    return text;
}

void CallAttachVk(lv_obj_t* scr) {
    if (s_attach_vk != nullptr) {
        s_attach_vk(scr);
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

void ShowMessage(lv_obj_t* parent, const char* msg, const lv_font_t* font) {
    lv_obj_clean(parent);
    lv_obj_set_style_layout(parent, LV_LAYOUT_NONE, 0);
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, msg != nullptr ? msg : "");
    lv_obj_set_style_text_font(label, font != nullptr ? font : book_font::ListFont(), 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, ContentWidth());
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_center(label);
    DisableScroll(label);
}

using book_font::ItemFont;
using book_font::ListFont;

}  // namespace

using book_font::ItemFont;
using book_font::ListFont;

int ListPageCount() {
    auto& st = State();
    if (st.books.empty() || st.list_page_size <= 0) {
        return 1;
    }
    return static_cast<int>((st.books.size() + static_cast<size_t>(st.list_page_size) - 1) /
                            static_cast<size_t>(st.list_page_size));
}

void ClampListPage() {
    auto& st = State();
    const int pages = ListPageCount();
    if (st.list_page < 0) {
        st.list_page = 0;
    }
    if (st.list_page >= pages) {
        st.list_page = pages - 1;
    }
}

void RequestRenderBookshelfPage();
void RequestShelfCheckMarksPaint();
void PaintBookshelfPage();
void PatchShelfVisibleCheckMarks();

void SyncShelfSelectedSize() {
    auto& st = State();
    if (st.shelf_selected.size() != st.books.size()) {
        st.shelf_selected.assign(st.books.size(), 0);
    }
}

int ShelfSelectedCount() {
    SyncShelfSelectedSize();
    auto& st = State();
    int n = 0;
    for (uint8_t v : st.shelf_selected) {
        if (v != 0) {
            ++n;
        }
    }
    return n;
}

bool ShelfItemSelected(int idx) {
    SyncShelfSelectedSize();
    auto& st = State();
    return idx >= 0 && idx < static_cast<int>(st.shelf_selected.size()) &&
           st.shelf_selected[static_cast<size_t>(idx)] != 0;
}

void ToggleShelfItemSelected(int idx) {
    SyncShelfSelectedSize();
    auto& st = State();
    if (idx < 0 || idx >= static_cast<int>(st.shelf_selected.size())) {
        return;
    }
    st.shelf_selected[static_cast<size_t>(idx)] =
        st.shelf_selected[static_cast<size_t>(idx)] ? 0 : 1;
}

void RefreshShelfFooterMode() {
    auto& st = State();
    if (st.shelf_multi) {
        if (st.multi_bar != nullptr) {
            lv_obj_set_width(st.multi_bar, lv_pct(100));
            lv_obj_align(st.multi_bar, LV_ALIGN_CENTER, 0, -3); // 整体居中并上移 6px
            lv_obj_clear_flag(st.multi_bar, LV_OBJ_FLAG_HIDDEN);
        }
        if (st.list_footer != nullptr) {
            lv_obj_set_width(st.list_footer, LV_SIZE_CONTENT);
            lv_obj_set_style_text_align(st.list_footer, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_style_text_font(st.list_footer, ItemFont(), 0);
            lv_obj_align(st.list_footer, LV_ALIGN_RIGHT_MID, -10, 0);
            lv_obj_clear_flag(st.list_footer, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(st.list_footer);
            char buf[32];
            std::snprintf(buf, sizeof(buf), Lang::Strings::BOOK_SELECTED_FMT, ShelfSelectedCount());
            StatusBarManager::GetInstance().SetOngoing("book", buf);
        }
    } else {
        if (st.multi_bar != nullptr) {
            lv_obj_add_flag(st.multi_bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_width(st.multi_bar, lv_pct(100));
            lv_obj_align(st.multi_bar, LV_ALIGN_CENTER, 0, 0);
        }
        if (st.list_footer != nullptr) {
            lv_obj_set_width(st.list_footer, LV_HOR_RES - 16);
            lv_obj_set_style_text_align(st.list_footer, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_set_style_text_font(st.list_footer, ItemFont(), 0);
            lv_obj_align(st.list_footer, LV_ALIGN_CENTER, 0, 0);
            lv_obj_clear_flag(st.list_footer, LV_OBJ_FLAG_HIDDEN);
        }
        StatusBarManager::GetInstance().SetOngoing("book", Lang::Strings::BOOK_SHELF_TITLE);
    }
}

void ExitShelfMultiMode(bool rebuild) {
    auto& st = State();
    st.shelf_multi = false;
    st.shelf_suppress_click_until_us = 0;
    st.shelf_suppress_click_idx = -1;
    st.shelf_selected.assign(st.books.size(), 0);
    RefreshShelfFooterMode();
    if (rebuild) {
        RequestRenderBookshelfPage();
    }
}

void EnterShelfMultiModeSelect(int idx) {
    auto& st = State();
    st.shelf_multi = true;
    SyncShelfSelectedSize();
    st.shelf_selected.assign(st.books.size(), 0);
    if (idx >= 0 && idx < static_cast<int>(st.shelf_selected.size())) {
        st.shelf_selected[static_cast<size_t>(idx)] = 1;
    }
    RefreshShelfFooterMode();
    RequestRenderBookshelfPage();
}

bool DeleteLibraryBookFile(const reader::BookInfo& info, std::string& err_out) {
    err_out.clear();
    if (info.path.empty()) {
        err_out = Lang::Strings::BOOK_PATH_INVALID;
        return false;
    }
    const std::string books_prefix = std::string(SD_PATH_BOOKS) + "/";
    if (info.path.rfind(books_prefix, 0) != 0) {
        err_out = Lang::Strings::BOOK_PATH_INVALID;
        return false;
    }
    // 先 Peek/缓存 再删 .pos：供首页聚合扣减（删后无法再读）
    reader::BookSession::ProgressPeek peek;
    if (!reader::book_progress_cache::TryGet(info.path.c_str(), peek)) {
        peek = reader::BookSession::PeekProgress(info.path.c_str());
    }
    if (unlink(info.path.c_str()) != 0) {
        err_out = Lang::Strings::BOOK_DELETE_FAILED;
        return false;
    }
    unlink((info.path + ".tmp").c_str());
    unlink((info.path + ".pos").c_str());      // 进度+终身累计+当日阅读秒
    unlink((info.path + ".pos.tmp").c_str());  // 原子写残留
    reader::DeleteBookCoverSidecar(info.path.c_str());
    // TXT 分页索引缓存：同目录 <name>.txt.idx
    if (info.format == reader::BookFormat::kTxt) {
        unlink((info.path + ".idx").c_str());
    }
    ESP_LOGI(TAG, "deleted library book %s", info.path.c_str());
    reader::book_progress_cache::Erase(info.path.c_str());
    reader::book_home_snapshot::RemoveBook(info.path.c_str());
    reader::book_home_snapshot::SubtractAggregateContribution(
        peek.reading_seconds, peek.progress_x10 >= 1000);
    return true;
}

lv_obj_t* FindShelfCell(int index) {
    auto& st = State();
    if (st.list_body == nullptr || !lv_obj_is_valid(st.list_body)) {
        return nullptr;
    }
    const uint32_t n = lv_obj_get_child_count(st.list_body);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* cell = lv_obj_get_child(st.list_body, i);
        if (cell != nullptr &&
            static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(cell))) == index) {
            return cell;
        }
    }
    return nullptr;
}

lv_obj_t* FindShelfCheckBox(lv_obj_t* cell) {
    if (cell == nullptr) {
        return nullptr;
    }
    // 封面 host 为 cell 首子；勾选框挂在 host 上
    lv_obj_t* host = lv_obj_get_child(cell, 0);
    if (host == nullptr) {
        return nullptr;
    }
    const uint32_t n = lv_obj_get_child_count(host);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* c = lv_obj_get_child(host, i);
        if (c != nullptr && lv_obj_get_width(c) == kShelfCheckSize &&
            lv_obj_get_height(c) == kShelfCheckSize) {
            return c;
        }
    }
    return nullptr;
}

void PatchShelfRowCheckMark(int index) {
    auto& st = State();
    if (!st.shelf_multi) {
        return;
    }
    lv_obj_t* check = FindShelfCheckBox(FindShelfCell(index));
    if (check == nullptr) {
        RequestRenderBookshelfPage();
        return;
    }
    lv_obj_clean(check);
    if (!ShelfItemSelected(index)) {
        return;
    }
    lv_obj_t* mark = lv_label_create(check);
    lv_label_set_text(mark, "√");
    lv_obj_set_style_text_font(mark, ItemFont(), 0);
    lv_obj_set_style_text_color(mark, lv_color_black(), 0);
    lv_obj_center(mark);
    lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE);
}

void PatchShelfVisibleCheckMarks() {
    auto& st = State();
    if (!st.shelf_multi || st.list_body == nullptr || !lv_obj_is_valid(st.list_body)) {
        return;
    }
    const uint32_t n = lv_obj_get_child_count(st.list_body);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* cell = lv_obj_get_child(st.list_body, i);
        if (cell == nullptr) {
            continue;
        }
        PatchShelfRowCheckMark(
            static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(cell))));
    }
    char buf[32];
    std::snprintf(buf, sizeof(buf), Lang::Strings::BOOK_SELECTED_FMT, ShelfSelectedCount());
    StatusBarManager::GetInstance().SetOngoing("book", buf);
}

void OnShelfMultiCancel(lv_event_t* /*e*/) {
    ExitShelfMultiMode(true);
}

void OnShelfMultiSelectAll(lv_event_t* /*e*/) {
    auto& st = State();
    if (!st.shelf_multi || st.books.empty()) {
        return;
    }
    SyncShelfSelectedSize();
    const int n = static_cast<int>(st.books.size());
    int selected_n = ShelfSelectedCount();
    const bool clear = (selected_n >= n);
    st.shelf_selected.assign(static_cast<size_t>(n), clear ? 0 : 1);
    RequestShelfCheckMarksPaint();
}

void OnShelfMultiRemove(lv_event_t* /*e*/) {
    auto& st = State();
    if (!st.shelf_multi || st.opening.load()) {
        return;
    }
    SyncShelfSelectedSize();
    if (ShelfSelectedCount() <= 0) {
        return;
    }
    // 自大下标删除，避免下标漂移
    for (int i = static_cast<int>(st.books.size()) - 1; i >= 0; --i) {
        if (i >= static_cast<int>(st.shelf_selected.size()) || st.shelf_selected[static_cast<size_t>(i)] == 0) {
            continue;
        }
        std::string err;
        if (!DeleteLibraryBookFile(st.books[static_cast<size_t>(i)], err)) {
            ESP_LOGW(TAG, "batch delete fail idx=%d: %s", i, err.c_str());
            continue;
        }
        st.books.erase(st.books.begin() + i);
        if (st.selected == i) {
            st.selected = -1;
        } else if (st.selected > i) {
            --st.selected;
        }
    }
    // 删除后以内存列表为权威，刷新缓存，避免下次 Load 复用含已删书的开机快照
    reader::ReplaceBookLibraryCache(st.books);
    st.shelf_multi = false;
    st.shelf_suppress_click_until_us = 0;
    st.shelf_suppress_click_idx = -1;
    st.shelf_selected.assign(st.books.size(), 0);
    ClampListPage();
    RefreshShelfFooterMode();
    RequestRenderBookshelfPage();
}

void OnBookRowLongPressed(lv_event_t* e) {
    auto& st = State();
    if (st.opening.load()) {
        return;
    }
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (index < 0 || index >= static_cast<int>(st.books.size())) {
        return;
    }
    if (!st.shelf_multi) {
        st.shelf_suppress_click_idx = index;
        st.shelf_suppress_click_until_us = esp_timer_get_time() + kShelfSuppressRowClickUs;
        EnterShelfMultiModeSelect(index);
        return;
    }
    ToggleShelfItemSelected(index);
    RequestShelfCheckMarksPaint();
}

void OnBookRowClicked(lv_event_t* e) {
    auto& st = State();
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (index == st.shelf_suppress_click_idx &&
        esp_timer_get_time() < st.shelf_suppress_click_until_us) {
        return;
    }
    if (st.shelf_multi) {
        ToggleShelfItemSelected(index);
        RequestShelfCheckMarksPaint();
        return;
    }
    CallOpenDetail(index);
}

void PaintBookshelfPage() {
    ESP_LOGI(TAG, "async RenderBookshelfPage page=%d begin", State().list_page);
    RenderBookshelfPage();
    ESP_LOGI(TAG, "async RenderBookshelfPage done");
}

void RequestRenderBookshelfPage() {
    if (s_library_paint.paint == nullptr) {
        s_library_paint.paint = PaintBookshelfPage;
    }
    ScreenPaintCoalesceRequest(&s_library_paint);
}

void RequestShelfCheckMarksPaint() {
    if (s_shelf_check_paint.paint == nullptr) {
        s_shelf_check_paint.paint = PatchShelfVisibleCheckMarks;
    }
    ScreenPaintCoalesceRequestDebounced(&s_shelf_check_paint, kShelfCheckPaintDebounceUs);
}

lv_obj_t* CreateBookCoverCell(lv_obj_t* parent, const reader::BookInfo& info, int index,
                              lv_coord_t cell_w, lv_coord_t cover_h,
                              std::vector<std::unique_ptr<reader::RasterImage>>& cover_store,
                              bool long_press_delete, lv_coord_t title_gap, bool show_title) {
    auto& st = State();
    lv_obj_t* cell = lv_obj_create(parent);
    lv_obj_remove_style_all(cell);
    lv_obj_set_width(cell, cell_w);
    lv_obj_set_height(cell, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(cell, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(cell, show_title ? title_gap : 0, 0);
    lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
    DisableScroll(cell);
    lv_obj_set_user_data(cell, reinterpret_cast<void*>(static_cast<intptr_t>(index)));
    HapticAttachClick(cell);
    lv_obj_add_event_cb(cell, OnBookRowClicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<intptr_t>(index)));
    if (long_press_delete) {
        lv_obj_add_event_cb(cell, OnBookRowLongPressed, LV_EVENT_LONG_PRESSED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(index)));
    }

    auto cover = std::make_unique<reader::RasterImage>();
    bool need_embed_fill = false;
    lv_obj_t* host =
        CallCreateCoverSlot(cell, info, cell_w, cover_h, cover.get(), &need_embed_fill);
    cover_store.push_back(std::move(cover));
    if (need_embed_fill) {
        CallEnqueueListCover(info, book_ui::CoverFrameInner(cell_w), book_ui::CoverFrameInner(cover_h), host,
                             cover_store.back().get());
    }

    if (long_press_delete && st.shelf_multi) {
        lv_obj_t* check = lv_obj_create(host);
        lv_obj_remove_style_all(check);
        lv_obj_set_size(check, kShelfCheckSize, kShelfCheckSize);
        lv_obj_set_style_bg_color(check, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(check, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(check, lv_color_black(), 0);
        lv_obj_set_style_border_width(check, kRowBorderW, 0);
        lv_obj_set_style_radius(check, 4, 0);
        lv_obj_align(check, LV_ALIGN_TOP_RIGHT, -4, 4);
        DisableScroll(check);
        lv_obj_clear_flag(check, LV_OBJ_FLAG_CLICKABLE);
        if (ShelfItemSelected(index)) {
            lv_obj_t* mark = lv_label_create(check);
            lv_label_set_text(mark, "√");
            lv_obj_set_style_text_font(mark, ItemFont(), 0);
            lv_obj_set_style_text_color(mark, lv_color_black(), 0);
            lv_obj_center(mark);
            lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE);
        }
    }

    if (show_title) {
        const lv_font_t* item_font = ItemFont();
        lv_obj_t* title = lv_label_create(cell);
        lv_obj_set_width(title, cell_w);
        lv_obj_set_style_text_font(title, item_font, 0);
        lv_obj_set_style_text_color(title, lv_color_black(), 0);
        lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        const std::string clipped = CallTruncate(info.title, item_font, cell_w);
        lv_label_set_text(title, clipped.c_str());
        lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);
    }
    return cell;
}

void RenderBookshelfPage() {
    auto& st = State();
    if (st.list_body == nullptr) {
        return;
    }
    CallCancelListCover();
    lv_obj_set_style_layout(st.list_body, LV_LAYOUT_FLEX, 0);
    lv_obj_set_flex_flow(st.list_body, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(st.list_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(st.list_body, kShelfRowGap, 0);
    lv_obj_set_style_pad_column(st.list_body, kShelfColGap, 0);

    ClampListPage();
    lv_obj_clean(st.list_body);
    st.shelf_covers.clear();
    DisableScroll(st.list_body);

    if (st.books.empty()) {
        lv_obj_set_flex_flow(st.list_body, LV_FLEX_FLOW_COLUMN);
        char tip[160];
        std::snprintf(tip, sizeof(tip), Lang::Strings::BOOK_EMPTY_SHELF_FMT,
                      SdUserPath(reader::kDefaultBooksDir));
        ShowMessage(st.list_body, tip, ListFont());
        if (st.list_footer != nullptr) {
            lv_label_set_text(st.list_footer, "1/1");
        }
        RefreshShelfFooterMode();
        return;
    }

    const lv_coord_t content_w = ContentWidth();
    const lv_coord_t cell_w =
        (content_w - kShelfColGap * (kShelfCols - 1)) / kShelfCols;
    const lv_coord_t cover_h =
        st.shelf_cover_h > 0 ? st.shelf_cover_h : (cell_w * 4 / 3);

    const int start = st.list_page * st.list_page_size;
    const int end = std::min(start + st.list_page_size, static_cast<int>(st.books.size()));
    for (int i = start; i < end; ++i) {
        CreateBookCoverCell(st.list_body, st.books[static_cast<size_t>(i)], i, cell_w, cover_h,
                            st.shelf_covers, true, kShelfTitleGap, true);
    }

    if (st.list_footer != nullptr) {
        char foot[48];
        std::snprintf(foot, sizeof(foot), "%d/%d", st.list_page + 1, ListPageCount());
        lv_label_set_text(st.list_footer, foot);
    }
    RefreshShelfFooterMode();
    CallScheduleListCover();
}


lv_obj_t* CreateBookshelfScreen() {
    CallPrepareEnter();
    auto& st = State();
    st.shelf_multi = false;
    st.shelf_selected.clear();
    st.shelf_suppress_click_until_us = 0;
    st.shelf_suppress_click_idx = -1;
    st.multi_bar = nullptr;
    st.back_root = BookUiState::NavRoot::kShelf;
    CallCancelListCover();
    st.recent_covers.clear();
    st.shelf_covers.clear();
    ScreenPaintCoalesceReset(&s_shelf_check_paint);

    ScreenSetIsHome(false);

    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, ListFont(), 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    DisableScroll(scr);
    st.list_scr = scr;
    lv_obj_add_event_cb(
        scr,
        [](lv_event_t* e) {
            auto& s = State();
            const auto* tgt = static_cast<lv_obj_t*>(lv_event_get_target(e));
            if (s.list_scr == tgt) {
                CallCancelListCover();
                s.list_scr = nullptr;
                s.list_body = nullptr;
                s.list_footer = nullptr;
                s.multi_bar = nullptr;
                s.shelf_multi = false;
                s.shelf_selected.clear();
                ScreenPaintCoalesceReset(&s_shelf_check_paint);
                s.recent_covers.clear();
                s.shelf_covers.clear();
            }
        },
        LV_EVENT_DELETE, nullptr);

    EpdStatusBar status = ScreenCreateStatusBar(scr);
    StatusBarManager::GetInstance().SetOngoing("book", Lang::Strings::BOOK_SHELF_TITLE);
    lv_obj_add_event_cb(scr, [](lv_event_t*) {
        StatusBarManager::GetInstance().ClearOngoing("book");
    }, LV_EVENT_DELETE, nullptr);

    const lv_coord_t body_h = LV_VER_RES - status.height - kFooterH;
    if (st.list_page < 0) {
        st.list_page = 0;
    }

    lv_obj_t* body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, body_h);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, status.height);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(body, kListPad, 0);
    DisableScroll(body);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* list_body = lv_obj_create(body);
    lv_obj_remove_style_all(list_body);
    lv_obj_set_size(list_body, ContentWidth(), body_h - kListPad * 2);
    lv_obj_align(list_body, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_flex_flow(list_body, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(list_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(list_body, kShelfRowGap, 0);
    lv_obj_set_style_pad_column(list_body, kShelfColGap, 0);
    DisableScroll(list_body);
    st.list_body = list_body;

    // 底栏槽：页码与多选共用，不改书籍区高度
    lv_obj_t* foot = lv_obj_create(scr);
    lv_obj_remove_style_all(foot);
    lv_obj_set_size(foot, LV_HOR_RES, kFooterH);
    lv_obj_align(foot, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_opa(foot, LV_OPA_TRANSP, 0);
    DisableScroll(foot);
    lv_obj_clear_flag(foot, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* page_lbl = lv_label_create(foot);
    lv_obj_set_width(page_lbl, LV_HOR_RES - 16);
    lv_obj_set_style_text_align(page_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(page_lbl, ItemFont(), 0);
    lv_obj_set_style_text_color(page_lbl, lv_color_black(), 0);
    lv_label_set_text(page_lbl, "1/1");
    lv_obj_align(page_lbl, LV_ALIGN_CENTER, 0, 0);
    DisableScroll(page_lbl);
    st.list_footer = page_lbl;

    st.multi_bar = lv_obj_create(foot);
    lv_obj_remove_style_all(st.multi_bar);
    lv_obj_set_width(st.multi_bar, lv_pct(100));
    lv_obj_set_height(st.multi_bar, kShelfMultiBtnH);
    lv_obj_set_style_bg_opa(st.multi_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(st.multi_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(st.multi_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(st.multi_bar, 10, 0);
    lv_obj_align(st.multi_bar, LV_ALIGN_CENTER, 0, 0);
    DisableScroll(st.multi_bar);
    lv_obj_clear_flag(st.multi_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(st.multi_bar, LV_OBJ_FLAG_HIDDEN);

    auto make_multi_action = [](lv_obj_t* parent, const char* text, lv_event_cb_t cb) {
        lv_obj_t* btn = lv_obj_create(parent);
        lv_obj_remove_style_all(btn);
        lv_obj_set_height(btn, kShelfMultiBtnH);
        lv_obj_set_width(btn, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_hor(btn, 4, 0);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        DisableScroll(btn);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(btn);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);

        lv_obj_t* text_wrap = lv_obj_create(btn);
        lv_obj_remove_style_all(text_wrap);
        lv_obj_set_size(text_wrap, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_border_side(text_wrap, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(text_wrap, kShelfUnderlineH, 0);
        lv_obj_set_style_border_color(text_wrap, lv_color_black(), 0);
        lv_obj_set_style_pad_bottom(text_wrap, 2, 0);
        lv_obj_set_style_pad_hor(text_wrap, kShelfUnderlinePadHor, 0);
        lv_obj_set_style_bg_opa(text_wrap, LV_OPA_TRANSP, 0);
        DisableScroll(text_wrap);
        lv_obj_clear_flag(text_wrap, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* lbl = lv_label_create(text_wrap);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_font(lbl, ItemFont(), 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        return btn;
    };
    auto make_multi_dot = [](lv_obj_t* parent) {
        lv_obj_t* dot = lv_obj_create(parent);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, kShelfMultiDotSize, kShelfMultiDotSize);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        DisableScroll(dot);
        lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
        return dot;
    };
    make_multi_action(st.multi_bar, Lang::Strings::COMMON_CANCEL, OnShelfMultiCancel);
    make_multi_dot(st.multi_bar);
    make_multi_action(st.multi_bar, Lang::Strings::COMMON_SELECT_ALL, OnShelfMultiSelectAll);
    make_multi_dot(st.multi_bar);
    make_multi_action(st.multi_bar, Lang::Strings::COMMON_REMOVE, OnShelfMultiRemove);

    // 固定 3×3：按可用高度反推封面高，保证一页九本
    st.list_page_size = kShelfCols * kShelfRows;
    const lv_coord_t cell_w =
        (ContentWidth() - kShelfColGap * (kShelfCols - 1)) / kShelfCols;
    const lv_coord_t title_h =
        (ItemFont() != nullptr && ItemFont()->line_height > 0)
            ? ItemFont()->line_height + kShelfTitleGap + 2
            : 34;
    const lv_coord_t usable = body_h - kListPad * 2;
    const lv_coord_t cell_budget =
        (usable - kShelfRowGap * (kShelfRows - 1)) / kShelfRows;
    lv_coord_t cover_h = cell_budget - title_h;
    const lv_coord_t cover_max = cell_w * 4 / 3;
    if (cover_h > cover_max) {
        cover_h = cover_max;
    }
    if (cover_h < 72) {
        cover_h = 72;
    }
    st.shelf_cover_h = cover_h;

    auto& sd = SdCardManager::GetInstance();
    if (!sd.IsMounted() && !sd.Mount()) {
        ShowMessage(list_body, Lang::Strings::BOOK_NO_SD, ListFont());
        CallAttachVk(scr);
        return scr;
    }

    // 与首页一致走 Load：缓存命中只拷贝；USB/云端 Invalidate 后在此重扫
    (void)reader::LoadBookLibrary(reader::kDefaultBooksDir, st.books);
    RenderBookshelfPage();

    CallAttachVk(scr);
    return scr;
}

void SetBookshelfPrepareEnterHandler(BookshelfPrepareEnterFn fn) { s_prepare_enter = fn; }
void SetBookshelfCancelListCoverHandler(BookshelfCancelListCoverFn fn) { s_cancel_list_cover = fn; }
void SetBookshelfScheduleListCoverHandler(BookshelfScheduleListCoverFn fn) {
    s_schedule_list_cover = fn;
}
void SetBookshelfEnqueueListCoverHandler(BookshelfEnqueueListCoverFn fn) { s_enqueue_list_cover = fn; }
void SetBookshelfCreateCoverSlotHandler(BookshelfCreateCoverSlotFn fn) { s_create_cover_slot = fn; }
void SetBookshelfOpenDetailHandler(BookshelfOpenDetailFn fn) { s_open_detail = fn; }
void SetBookshelfTruncateHandler(BookshelfTruncateFn fn) { s_truncate = fn; }
void SetBookshelfAttachVkHandler(BookshelfAttachVkFn fn) { s_attach_vk = fn; }

}  // namespace book_screen_detail
