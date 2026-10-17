#include "book_reader_orchestrator.h"
#include "book_detail_screen.h"
#include "book_font_manager.h"
#include "book_reader.h"
#include "book_reader_prefs.h"
#include "book_settings_sheet.h"
#include "book_toc.h"
#include "book_ttf_panel.h"
#include "book_ui_state.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <unistd.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "assets/lang_config.h"
#include "display/lv_adapter_display.h"
#include "display/screen/screen_common.h"
#include "display/status_bar_manager.h"
#include "reader/book_cover_sidecar.h"
#include "reader_core/ebook_document.h"
#include "utils/psr_worker.h"

namespace book_screen_detail {

namespace {
const char* TAG = "BookReaderOrchestrator";

constexpr lv_coord_t kListPad = 16;
constexpr lv_coord_t kFooterH = 36;
constexpr lv_coord_t kReadViewportBottomSlack = 8;
constexpr uint32_t kLayoutDebounceMs = 300;
constexpr uint32_t kLayoutHintTickMs = 500;
constexpr int64_t kLayoutHintDoneUs = 1500 * 1000;
constexpr lv_coord_t kOpenBarW = 320;
constexpr lv_coord_t kOpenBarH = 28;
constexpr uint32_t kOpenWorkerStack = 16 * 1024;
constexpr uint32_t kReadTimeCheckpointMs = 45000;
constexpr const char* kScreenRead = "book_read";

using book_font::BookFont;
using book_font::EnsureBookFont;
using book_font::ListFont;
using book_font::ReleaseBookFont;

struct OrchestratorHooks {
    ReaderCreateHomeScrFn create_home_scr = nullptr;
    ReaderCreateDetailScrFn create_detail_scr = nullptr;
    ReaderAttachVkFn attach_vk = nullptr;
    ReaderCancelListCoverFn cancel_list_cover = nullptr;
};
OrchestratorHooks s_hooks;

struct OpenProgressMsg {
    uint32_t token = 0;
    int percent = 0;
};

struct OpenDoneMsg {
    uint32_t token = 0;
    bool ok = false;
};

struct OpenWorkerArg {
    uint32_t token = 0;
    reader::BookInfo info;
};

struct LayoutDoneMsg {
    uint32_t token = 0;
    bool ok = false;
};

struct LayoutWorkerArg {
    uint32_t token = 0;
    reader::BookSession::LayoutAnchor anchor{};
};

struct ChapterPagesDoneMsg {
    uint32_t token = 0;
    bool ok = false;
};

void DisableScroll(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(obj, LV_DIR_NONE);
}

inline lv_coord_t ContentWidth() {
    return LV_HOR_RES - kListPad * 2;
}

}  // namespace

void SetReaderOrchestratorCreateHomeScrHandler(ReaderCreateHomeScrFn fn) {
    s_hooks.create_home_scr = fn;
}

void SetReaderOrchestratorCreateDetailScrHandler(ReaderCreateDetailScrFn fn) {
    s_hooks.create_detail_scr = fn;
}

void SetReaderOrchestratorAttachVkHandler(ReaderAttachVkFn fn) {
    s_hooks.attach_vk = fn;
}

void SetReaderOrchestratorCancelListCoverHandler(ReaderCancelListCoverFn fn) {
    s_hooks.cancel_list_cover = fn;
}

bool IsReadOverlayChrome(BookUiState::ReadChrome c) {
    using C = BookUiState::ReadChrome;
    return c == C::kToc || c == C::kSettings;
}

void BindSessionLayoutPrefs(reader::BookSession& session) {
    BookReaderPrefsEnsureLoaded();
    session.SetFont(BookFont());
    session.SetGaps(static_cast<lv_coord_t>(BookReaderPrefsLineGap()),
                    static_cast<lv_coord_t>(BookReaderPrefsParaGap()));
}

lv_coord_t ReadContentWidth() {
    BookReaderPrefsEnsureLoaded();
    const int ml = BookReaderPrefsMarginLeft();
    const int mr = BookReaderPrefsMarginRight();
    lv_coord_t w = LV_HOR_RES - static_cast<lv_coord_t>(ml + mr);
    return w > 40 ? w : 40;
}

void ApplyOverlayListContentPads(lv_coord_t top_pad) {
    auto& st = State();
    if (st.content == nullptr) {
        return;
    }
    lv_obj_set_style_pad_top(st.content, top_pad, 0);
    lv_obj_set_style_pad_bottom(st.content, 0, 0);
    lv_obj_set_style_pad_left(st.content, kListPad, 0);
    lv_obj_set_style_pad_right(st.content, kListPad, 0);
}

bool ReaderPageImagesHint() {
    auto& st = State();
    if (st.session && st.session->IsOpen()) {
        return st.session->IsPageImagesMode();
    }
    if (st.selected < 0 || st.selected >= static_cast<int>(st.books.size())) {
        return false;
    }
    const reader::BookInfo& info = st.books[static_cast<size_t>(st.selected)];
    return info.format == reader::BookFormat::kEbook &&
           reader::EbookDocument::PeekPageImages(info.path.c_str());
}

void ApplyReaderPageGeometry(ReaderGeomMode mode) {
    auto& st = State();
    if (st.read_scr == nullptr || st.content == nullptr || !lv_obj_is_valid(st.content)) {
        return;
    }
    BookReaderPrefsEnsureLoaded();
    const bool show_footer =
        (mode == ReaderGeomMode::kOverlayList) || (BookReaderPrefsHideProgress() == 0);
    const lv_coord_t footer_h = show_footer ? kFooterH : 0;
    const lv_coord_t body_h = LV_VER_RES - footer_h;

    if (st.page_label != nullptr && lv_obj_is_valid(st.page_label)) {
        if (show_footer) {
            lv_obj_clear_flag(st.page_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(st.page_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    lv_obj_set_size(st.content, LV_HOR_RES, body_h);
    lv_obj_align(st.content, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_remove_flag(st.content, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    if (mode == ReaderGeomMode::kOverlayList) {
        return;
    }

    const bool page_images = ReaderPageImagesHint();
    const int mt = BookReaderPrefsMarginTop();
    const int mb = BookReaderPrefsMarginBottom();
    if (page_images) {
        lv_obj_set_style_pad_top(st.content, 0, 0);
        lv_obj_set_style_pad_bottom(st.content, 0, 0);
        lv_obj_set_style_pad_left(st.content, 0, 0);
        lv_obj_set_style_pad_right(st.content, 0, 0);
        st.viewport_w = LV_HOR_RES;
        st.viewport_h = body_h;
    } else {
        lv_obj_set_style_pad_top(st.content, static_cast<lv_coord_t>(mt), 0);
        lv_obj_set_style_pad_bottom(st.content, static_cast<lv_coord_t>(mb), 0);
        lv_obj_set_style_pad_left(st.content, 0, 0);
        lv_obj_set_style_pad_right(st.content, 0, 0);
        st.viewport_w = ReadContentWidth();
        lv_coord_t vh =
            body_h - static_cast<lv_coord_t>(mt + mb) - kReadViewportBottomSlack;
        st.viewport_h = vh > 40 ? vh : 40;
    }
}

void StopReadTimeCheckpointTimer() {
    auto& st = State();
    if (st.read_time_ckpt_timer == nullptr) {
        return;
    }
    lv_timer_del(st.read_time_ckpt_timer);
    st.read_time_ckpt_timer = nullptr;
}

static void OnReadTimeCheckpoint(lv_timer_t* /*t*/) {
    auto& st = State();
    if (st.opening.load() || !st.session || !st.session->IsOpen()) {
        return;
    }
    const bool ok = st.session->SaveProgress();
    const uint32_t sec = st.session->ReadingSeconds();
    const uint32_t daily = st.session->ReadingDailySeconds();
    const uint32_t min = sec / 60u;
    const uint32_t rem = sec % 60u;
    ESP_LOGI(TAG, "read-time checkpoint (%us): %s total=%u sec (%u min %u sec) daily=%u sec title=%s",
             static_cast<unsigned>(kReadTimeCheckpointMs / 1000u), ok ? "saved" : "FAIL",
             static_cast<unsigned>(sec), static_cast<unsigned>(min), static_cast<unsigned>(rem),
             static_cast<unsigned>(daily), st.session->Title().c_str());
}

void StartReadTimeCheckpointTimer() {
    auto& st = State();
    StopReadTimeCheckpointTimer();
    st.read_time_ckpt_timer =
        lv_timer_create(OnReadTimeCheckpoint, kReadTimeCheckpointMs, nullptr);
    if (st.read_time_ckpt_timer == nullptr) {
        ESP_LOGW(TAG, "read time checkpoint timer create failed");
    }
}

void BeginReadingTimeTracking() {
    auto& st = State();
    if (!st.session || !st.session->IsOpen()) {
        return;
    }
    const uint32_t sec = st.session->ReadingSeconds();
    const uint32_t daily = st.session->ReadingDailySeconds();
    const uint32_t min = sec / 60u;
    const uint32_t rem = sec % 60u;
    ESP_LOGI(TAG, "read-time open: total=%u sec (%u min %u sec) daily=%u sec title=%s path=%s",
             static_cast<unsigned>(sec), static_cast<unsigned>(min), static_cast<unsigned>(rem),
             static_cast<unsigned>(daily), st.session->Title().c_str(),
             st.session->Info().path.c_str());
    st.session->StartReadingClock();
    StartReadTimeCheckpointTimer();
}

void EndReadingTimeTracking() {
    StopReadTimeCheckpointTimer();
    auto& st = State();
    st.reading_paused_by_standby = false;
    if (st.session && st.session->IsOpen()) {
        st.session->StopReadingClock();
    }
}

void PauseReadingTimeForStandby() {
    auto& st = State();
    if (!st.session || !st.session->IsReadingClockActive()) {
        return;
    }
    EndReadingTimeTracking();
    st.reading_paused_by_standby = true;
    ESP_LOGI(TAG, "read-time paused for standby");
}

void ResumeReadingTimeAfterStandby() {
    auto& st = State();
    if (!st.reading_paused_by_standby) {
        return;
    }
    st.reading_paused_by_standby = false;
    BeginReadingTimeTracking();
    ESP_LOGI(TAG, "read-time resumed after standby");
}

void ShowMessage(lv_obj_t* parent, const char* msg, const lv_font_t* font) {
    auto& st = State();
    if (parent == st.content) {
        st.open_bar = nullptr;
        st.open_pct_lbl = nullptr;
    }
    lv_obj_clean(parent);
    lv_obj_set_style_layout(parent, LV_LAYOUT_NONE, 0);
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, msg != nullptr ? msg : "");
    lv_obj_set_style_text_font(label, font != nullptr ? font : ListFont(), 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, ContentWidth());
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_center(label);
    DisableScroll(label);
}

void FinishDeferredCleanup() {
    auto& st = State();
    if (!st.deferred_cleanup) {
        return;
    }
    st.deferred_cleanup = false;
    EndReadingTimeTracking();
    st.session.reset();
    ReleaseBookFont();
    ESP_LOGI(TAG, "deferred cleanup finished: session reset, font released");
}

bool ReaderWorkersBusy() {
    auto& st = State();
    return st.opening.load() || st.layout_busy.load() || st.chapter_pages_busy.load() ||
           st.page_image_busy.load();
}

void ReleaseReaderSessionForLeave() {
    auto& st = State();
    st.open_token.fetch_add(1);
    st.read_scr = nullptr;
    st.content = nullptr;
    st.page_label = nullptr;
    st.title_label = nullptr;
    st.open_bar = nullptr;
    st.open_pct_lbl = nullptr;
    st.status_bar = nullptr;
    st.status_overlay = nullptr;
    st.status_h = 0;
    st.settings_sheet = nullptr;
    ClearSettingsSheetWidgetRefs();
    StopTtfPollTimer();
    st.ttf_mode = BookUiState::TtfMode::kNone;
    st.ttf_pick.clear();
    st.ttf_page = 0;
    st.ttf_size_px = kTtfConvertSizeDefault;
    st.viewport_w = 0;
    st.viewport_h = 0;
    st.font_entries.clear();
    st.font_list_page = 0;
    st.font_multi = false;
    st.font_selected.clear();
    st.font_suppress_click_until_us = 0;
    st.font_suppress_click_idx = -1;
    st.font_suppress_next_click = false;
    st.read_chrome = BookUiState::ReadChrome::kReading;
    st.settings_resume_after_open = false;
    st.toc_list_page = 0;
    const bool layout_running = st.layout_busy.load();
    const bool chapter_pages_running = st.chapter_pages_busy.load();
    const bool page_image_running = st.page_image_busy.load();
    st.layout_token.fetch_add(1);
    st.layout_again = false;
    st.layout_reload_font = false;
    st.layout_anchor = {};
    st.chapter_pages_token.fetch_add(1);
    st.chapter_pages_again = false;
    CancelPageImageLoad();
    if (st.session) {
        st.session->InvalidateChapterPageTable();
        st.session->AbortTxtPaginate();
    }
    CancelLayoutDebounce();
    StopLayoutHintTimer();
    st.layout_hint_done_until_us = 0;
    ResetReaderPaint();
    ResetTocPaint();
    ResetSettingsSheetPaint();
    ClearReaderPageDelta();
    ResetReadTouch();
    if (st.opening.load() || layout_running || chapter_pages_running || page_image_running) {
        StopReadTimeCheckpointTimer();
        st.deferred_cleanup = true;
    } else {
        EndReadingTimeTracking();
        st.session.reset();
        ReleaseBookFont();
    }
}

lv_obj_t* ResumeReaderScreen() {
    auto& st = State();
    if (ReaderWorkersBusy()) {
        return s_hooks.create_home_scr ? s_hooks.create_home_scr() : nullptr;
    }
    if (st.deferred_cleanup) {
        FinishDeferredCleanup();
    } else if (st.session) {
        EndReadingTimeTracking();
        st.session.reset();
        ReleaseBookFont();
    }
    if (st.selected < 0 || st.selected >= static_cast<int>(st.books.size())) {
        return s_hooks.create_home_scr ? s_hooks.create_home_scr() : nullptr;
    }
    return CreateReaderScreen(st.books[static_cast<size_t>(st.selected)]);
}

void SetReadStatusVisible(bool visible) {
    auto& st = State();
    if (st.status_bar != nullptr && lv_obj_is_valid(st.status_bar)) {
        if (visible) {
            lv_obj_set_style_bg_color(st.status_bar, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(st.status_bar, LV_OPA_COVER, 0);
            lv_obj_clear_flag(st.status_bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(st.status_bar);
        } else {
            lv_obj_add_flag(st.status_bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_opa(st.status_bar, LV_OPA_TRANSP, 0);
        }
    }
    if (st.status_overlay != nullptr && lv_obj_is_valid(st.status_overlay)) {
        if (visible) {
            lv_obj_set_style_bg_opa(st.status_overlay, LV_OPA_TRANSP, 0);
            lv_obj_clear_flag(st.status_overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(st.status_overlay);
        } else {
            lv_obj_add_flag(st.status_overlay, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void SetReadStatusCenterTitle(const char* title) {
    if (title != nullptr && title[0] != '\0') {
        StatusBarManager::GetInstance().SetOngoing("book", title);
    } else {
        StatusBarManager::GetInstance().ClearOngoing("book");
    }
}

void HideReadChrome() {
    auto& st = State();
    CloseReadSettingsSheet();
    SetReadStatusCenterTitle(nullptr);
    SetReadStatusVisible(false);
    const bool was_overlay = IsReadOverlayChrome(st.read_chrome);
    const bool was_settings = (st.read_chrome == BookUiState::ReadChrome::kSettings);
    st.settings_resume_after_open = false;
    st.read_chrome = BookUiState::ReadChrome::kReading;
    if (was_overlay && !was_settings && st.session && st.session->IsOpen() &&
        st.content != nullptr) {
        if (s_hooks.cancel_list_cover) {
            s_hooks.cancel_list_cover();
        }
        ApplyReaderPageGeometry(ReaderGeomMode::kImmersive);
        RenderReaderPage();
    } else if (was_settings) {
        FlushLayoutDebounce();
        ApplyReaderPageGeometry(ReaderGeomMode::kImmersive);
        StartChapterPageWorker();
    }
}

void ShowOpenProgress(int percent) {
    auto& st = State();
    if (st.content == nullptr) {
        return;
    }
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }

    const bool need_build =
        st.open_bar == nullptr || lv_obj_get_parent(st.open_bar) != st.content ||
        st.open_pct_lbl == nullptr || lv_obj_get_parent(st.open_pct_lbl) != st.content;

    if (need_build) {
        st.open_bar = nullptr;
        st.open_pct_lbl = nullptr;
        lv_obj_clean(st.content);
        ApplyOverlayListContentPads(0);
        lv_obj_set_style_layout(st.content, LV_LAYOUT_NONE, 0);
        DisableScroll(st.content);

        lv_obj_t* panel = lv_obj_create(st.content);
        lv_obj_remove_style_all(panel);
        lv_obj_set_size(panel, ContentWidth(), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(panel, 18, 0);
        lv_obj_clear_flag(panel, LV_OBJ_FLAG_CLICKABLE);
        DisableScroll(panel);
        lv_obj_center(panel);

        lv_obj_t* title = lv_label_create(panel);
        lv_label_set_text(title, Lang::Strings::BOOK_OPENING);
        lv_obj_set_style_text_font(title, ListFont(), 0);
        lv_obj_set_style_text_color(title, lv_color_black(), 0);
        lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

        lv_coord_t bar_w = kOpenBarW;
        if (bar_w > ContentWidth() - 8) {
            bar_w = ContentWidth() - 8;
        }
        lv_obj_t* bar = lv_bar_create(panel);
        lv_obj_set_size(bar, bar_w, kOpenBarH);
        lv_bar_set_range(bar, 0, 100);
        lv_bar_set_mode(bar, LV_BAR_MODE_NORMAL);
        lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(bar, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(bar, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_pad_all(bar, 3, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(bar, lv_color_black(), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        st.open_bar = bar;

        lv_obj_t* pct = lv_label_create(panel);
        lv_obj_set_style_text_font(pct, ListFont(), 0);
        lv_obj_set_style_text_color(pct, lv_color_black(), 0);
        lv_obj_set_style_text_align(pct, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_clear_flag(pct, LV_OBJ_FLAG_CLICKABLE);
        st.open_pct_lbl = pct;
    }

    lv_bar_set_value(st.open_bar, percent, LV_ANIM_OFF);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d%%", percent);
    lv_label_set_text(st.open_pct_lbl, buf);
}

static void AsyncOpenProgress(void* user_data) {
    auto* msg = static_cast<OpenProgressMsg*>(user_data);
    auto& st = State();
    if (msg->token == st.open_token.load() && st.content != nullptr && st.opening.load()) {
        ShowOpenProgress(msg->percent);
    }
    delete msg;
}

static void AsyncOpenDone(void* user_data) {
    auto* msg = static_cast<OpenDoneMsg*>(user_data);
    auto& st = State();
    st.opening.store(false);

    if (msg->token != st.open_token.load() || st.deferred_cleanup || st.content == nullptr) {
        FinishDeferredCleanup();
        delete msg;
        return;
    }

    if (!msg->ok || !st.session || !st.session->IsOpen()) {
        EndReadingTimeTracking();
        ReleaseBookFont();
        if (st.opening_format == reader::BookFormat::kTxt) {
            ShowMessage(st.content, Lang::Strings::BOOK_OPEN_FAILED_TXT);
        } else if (st.opening_format == reader::BookFormat::kEbook) {
            ShowMessage(st.content, Lang::Strings::BOOK_OPEN_FAILED_CHAPTER);
        } else {
            ShowMessage(st.content, Lang::Strings::BOOK_OPEN_FAILED_EPUB);
        }
        delete msg;
        return;
    }

    if (st.selected >= 0 && st.selected < static_cast<int>(st.books.size()) &&
        st.books[static_cast<size_t>(st.selected)].path == st.session->Info().path) {
        auto& bi = st.books[static_cast<size_t>(st.selected)];
        if (!st.session->Title().empty()) {
            bi.title = st.session->Title();
        }
        if (!st.session->Author().empty()) {
            bi.author = st.session->Author();
        }
    }

    if (st.settings_resume_after_open) {
        st.settings_resume_after_open = false;
        st.read_chrome = BookUiState::ReadChrome::kReading;
        RenderReaderPage();
        ShowReadSettingsSheet();
        BeginReadingTimeTracking();
        if (st.session->NeedsChapterPageRebuild()) {
            StartChapterPageWorker();
        }
        delete msg;
        return;
    }

    RenderReaderPage();
    BeginReadingTimeTracking();
    if (st.session->NeedsChapterPageRebuild()) {
        StartChapterPageWorker();
    }
    delete msg;
}

static void OnOpenProgress(int percent, void* user) {
    auto* msg = new OpenProgressMsg{};
    msg->token = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(user));
    msg->percent = percent;
    if (!ScreenLvAsync(AsyncOpenProgress, msg)) {
        delete msg;
    }
}

static void OpenBookWorker(void* arg) {
    auto* ctx = static_cast<OpenWorkerArg*>(arg);
    auto& st = State();
    bool ok = false;

    const uint32_t token_now = st.open_token.load();
    ESP_LOGI(TAG, "book_open worker start token=%u cur=%u fmt=%d int=%u spiram=%u path=%s",
             static_cast<unsigned>(ctx->token), static_cast<unsigned>(token_now),
             static_cast<int>(ctx->info.format),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             ctx->info.path.c_str());

    if (st.session == nullptr) {
        ESP_LOGE(TAG, "book_open skip: session null");
    } else if (ctx->token != token_now) {
        ESP_LOGW(TAG, "book_open skip: token stale %u!=%u", static_cast<unsigned>(ctx->token),
                 static_cast<unsigned>(token_now));
    } else {
        st.session->SetOpenProgress(OnOpenProgress,
                                    reinterpret_cast<void*>(static_cast<uintptr_t>(ctx->token)));
        ok = st.session->Open(ctx->info);
        st.session->SetOpenProgress(nullptr, nullptr);
        if (ok && ctx->token == st.open_token.load() &&
            (ctx->info.format == reader::BookFormat::kEbook ||
             ctx->info.format == reader::BookFormat::kEpub)) {
            char sidecar[192];
            if (reader::BookCoverSidecarPath(ctx->info.path.c_str(), sidecar, sizeof(sidecar)) &&
                access(sidecar, R_OK) != 0) {
                reader::SaveBookCoverSidecar(ctx->info.path.c_str());
            }
        }
    }

    ESP_LOGI(TAG, "book_open worker done ok=%d int=%u spiram=%u", ok ? 1 : 0,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

    auto* done = new OpenDoneMsg{};
    done->token = ctx->token;
    done->ok = ok;
    if (!ScreenLvAsync(AsyncOpenDone, done)) {
        delete done;
        st.opening.store(false);
        st.deferred_cleanup = true;
        ESP_LOGW(TAG, "book_open done: ScreenLvAsync fail, deferred_cleanup");
    }
    delete ctx;
}

void StartOpenWorker(int book_index) {
    auto& st = State();
    if (st.opening.load()) {
        ESP_LOGW(TAG, "StartOpenWorker ignored: already opening");
        return;
    }
    if (book_index < 0 || book_index >= static_cast<int>(st.books.size()) || !st.session) {
        ESP_LOGE(TAG, "StartOpenWorker bad state idx=%d books=%u session=%d", book_index,
                 static_cast<unsigned>(st.books.size()), st.session ? 1 : 0);
        ShowMessage(st.content, Lang::Strings::BOOK_OPEN_FAILED);
        return;
    }

    const uint32_t token = st.open_token.fetch_add(1) + 1;
    st.opening.store(true);
    st.deferred_cleanup = false;
    st.opening_format = st.books[static_cast<size_t>(book_index)].format;
    ShowOpenProgress(0);

    auto* ctx = new OpenWorkerArg{};
    ctx->token = token;
    ctx->info = st.books[static_cast<size_t>(book_index)];
    ESP_LOGI(TAG, "StartOpenWorker token=%u fmt=%d int=%u spiram=%u path=%s",
             static_cast<unsigned>(token), static_cast<int>(ctx->info.format),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             ctx->info.path.c_str());
    if (!SpawnPsrWorker("book_open", kOpenWorkerStack, tskIDLE_PRIORITY + 2, OpenBookWorker, ctx,
                        nullptr, 0)) {
        const size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGE(TAG, "book_open task failed: int free=%u spiram=%u stack=%u",
                 static_cast<unsigned>(free_int), static_cast<unsigned>(free_spiram),
                 static_cast<unsigned>(kOpenWorkerStack));
        delete ctx;
        st.opening.store(false);
        ShowMessage(st.content, Lang::Strings::BOOK_OPEN_FAILED_TASK);
    }
}

void StartReadAsync(void* user_data) {
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(user_data));
    auto& st = State();
    if (index < 0 || index >= static_cast<int>(st.books.size())) {
        return;
    }
    if (ReaderWorkersBusy()) {
        ESP_LOGW(TAG, "start read ignored: worker busy");
        return;
    }
    st.selected = index;
    CancelDetailCoverLoad();
    ScreenLoadReplace(CreateReaderScreen(st.books[static_cast<size_t>(index)]));
}

void CancelLayoutDebounce() {
    auto& st = State();
    if (st.layout_debounce_timer != nullptr) {
        lv_timer_del(st.layout_debounce_timer);
        st.layout_debounce_timer = nullptr;
    }
    st.layout_debounce_reload_font = false;
}

bool IsLayoutHintBusy() {
    auto& st = State();
    if (st.chapter_pages_busy.load() || st.layout_busy.load()) {
        return true;
    }
    return st.session && st.session->IsOpen() && st.session->LiveRelayoutPending();
}

void StopLayoutHintTimer() {
    auto& st = State();
    if (st.layout_hint_timer != nullptr) {
        lv_timer_del(st.layout_hint_timer);
        st.layout_hint_timer = nullptr;
    }
}

static void OnLayoutHintTick(lv_timer_t* /*t*/) {
    auto& st = State();
    if (st.page_label == nullptr || !lv_obj_is_valid(st.page_label)) {
        StopLayoutHintTimer();
        return;
    }
    if (IsLayoutHintBusy()) {
        UpdateReadFooter();
        return;
    }
    if (esp_timer_get_time() < st.layout_hint_done_until_us) {
        UpdateReadFooter();
        return;
    }
    st.layout_hint_done_until_us = 0;
    StopLayoutHintTimer();
    UpdateReadFooter();
}

void EnsureLayoutHintTimer() {
    auto& st = State();
    if (st.layout_hint_timer != nullptr) {
        return;
    }
    st.layout_hint_timer = lv_timer_create(OnLayoutHintTick, kLayoutHintTickMs, nullptr);
}

void MarkLayoutHintBusy() {
    auto& st = State();
    st.layout_hint_done_until_us = 0;
    StopLayoutHintTimer();
    UpdateReadFooter();
}

void MarkLayoutHintDone() {
    auto& st = State();
    st.layout_hint_done_until_us = esp_timer_get_time() + kLayoutHintDoneUs;
    EnsureLayoutHintTimer();
    UpdateReadFooter();
}

static void OnLayoutDebounceTimer(lv_timer_t* t) {
    auto& st = State();
    if (st.layout_debounce_timer == t) {
        st.layout_debounce_timer = nullptr;
    }
    if (TouchUiFingerIsDown() || TouchUiHasPendingEdges()) {
        st.layout_debounce_timer =
            lv_timer_create(OnLayoutDebounceTimer, kLayoutDebounceMs, nullptr);
        if (st.layout_debounce_timer != nullptr) {
            lv_timer_set_repeat_count(st.layout_debounce_timer, 1);
        }
        return;
    }
    const bool reload = st.layout_debounce_reload_font;
    st.layout_debounce_reload_font = false;
    ApplyReaderLayoutLive(reload);
}

void ScheduleLayoutApply(bool reload_font) {
    auto& st = State();
    if (st.opening.load() || !st.session || !st.session->IsOpen()) {
        return;
    }
    st.layout_debounce_reload_font = st.layout_debounce_reload_font || reload_font;
    if (st.layout_debounce_timer != nullptr) {
        lv_timer_reset(st.layout_debounce_timer);
        return;
    }
    st.layout_debounce_timer = lv_timer_create(OnLayoutDebounceTimer, kLayoutDebounceMs, nullptr);
    if (st.layout_debounce_timer != nullptr) {
        lv_timer_set_repeat_count(st.layout_debounce_timer, 1);
    }
}

void FlushLayoutDebounce() {
    auto& st = State();
    if (st.layout_debounce_timer == nullptr) {
        return;
    }
    const bool reload = st.layout_debounce_reload_font;
    CancelLayoutDebounce();
    ApplyReaderLayoutLive(reload);
}

static void AsyncLayoutDone(void* user_data) {
    auto* msg = static_cast<LayoutDoneMsg*>(user_data);
    auto& st = State();
    const uint32_t token = msg->token;
    const bool ok = msg->ok;
    delete msg;

    const bool mine = (token == st.layout_token.load());
    st.layout_busy.store(false);
    if (st.deferred_cleanup && !st.opening.load() && !st.layout_busy.load() &&
        !st.chapter_pages_busy.load() && !st.page_image_busy.load()) {
        FinishDeferredCleanup();
        return;
    }
    if (!mine || st.content == nullptr || !st.session) {
        return;
    }
    if (!ok) {
        ESP_LOGW(TAG, "layout worker failed");
        if (st.session &&
            (st.session->LiveRelayoutPending() || st.session->NeedsTxtIndexRebuild())) {
            st.session->ClearLivePreview();
            st.settings_resume_after_open = (st.read_chrome == BookUiState::ReadChrome::kSettings);
            ReopenReaderAfterPrefsChange();
        } else if (st.session) {
            UpdateReadFooter();
        }
        return;
    }

    const bool keep_sheet = (st.read_chrome == BookUiState::ReadChrome::kSettings);
    RenderReaderPage();
    if (keep_sheet) {
        EnsureSettingsSheetAfterLayout();
    } else {
        UpdateReadFooter();
    }

    if (st.layout_again) {
        const bool reload = st.layout_reload_font;
        st.layout_again = false;
        st.layout_reload_font = false;
        ApplyReaderLayoutLive(reload);
        return;
    }
    if (!IsLayoutHintBusy()) {
        MarkLayoutHintDone();
    }
}

static void LayoutWorker(void* arg) {
    auto* ctx = static_cast<LayoutWorkerArg*>(arg);
    auto& st = State();
    bool ok = false;
    if (st.session && ctx->token == st.layout_token.load()) {
        ok = st.session->FinishTxtRelayout(ctx->anchor);
    }
    auto* done = new LayoutDoneMsg{};
    done->token = ctx->token;
    done->ok = ok;
    if (!ScreenLvAsync(AsyncLayoutDone, done)) {
        delete done;
        st.layout_busy.store(false);
    }
    delete ctx;
}

static void AsyncChapterPagesDone(void* user_data) {
    auto* msg = static_cast<ChapterPagesDoneMsg*>(user_data);
    auto& st = State();
    const uint32_t token = msg->token;
    delete msg;

    const bool mine = (token == st.chapter_pages_token.load());
    st.chapter_pages_busy.store(false);

    if (st.deferred_cleanup && !st.opening.load() && !st.layout_busy.load() &&
        !st.chapter_pages_busy.load() && !st.page_image_busy.load()) {
        FinishDeferredCleanup();
        return;
    }
    if (!mine || st.content == nullptr || !st.session) {
        return;
    }
    if (st.read_chrome == BookUiState::ReadChrome::kSettings) {
        EnsureSettingsSheetAfterLayout();
    } else {
        UpdateReadFooter();
    }
    if (st.layout_again) {
        const bool reload = st.layout_reload_font;
        st.layout_again = false;
        st.layout_reload_font = false;
        st.chapter_pages_again = false;
        ApplyReaderLayoutLive(reload);
        return;
    }
    if (st.chapter_pages_again || st.session->NeedsChapterPageRebuild()) {
        st.chapter_pages_again = false;
        StartChapterPageWorker();
        return;
    }
    MarkLayoutHintDone();
}

static void ChapterPageWorker(void* arg) {
    auto* ctx = static_cast<uint32_t*>(arg);
    const uint32_t token = *ctx;
    delete ctx;
    auto& st = State();
    bool ok = false;
    if (st.session && token == st.chapter_pages_token.load()) {
        ok = st.session->FinishChapterPageTable();
    }
    auto* done = new ChapterPagesDoneMsg{};
    done->token = token;
    done->ok = ok;
    if (!ScreenLvAsync(AsyncChapterPagesDone, done)) {
        delete done;
        st.chapter_pages_busy.store(false);
    }
}

void StartChapterPageWorker() {
    auto& st = State();
    if (!st.session || !st.session->NeedsChapterPageRebuild()) {
        return;
    }
    if (st.read_chrome == BookUiState::ReadChrome::kSettings) {
        return;
    }
    if (st.chapter_pages_busy.load()) {
        st.chapter_pages_again = true;
        return;
    }
    const uint32_t token = st.chapter_pages_token.fetch_add(1) + 1;
    st.chapter_pages_busy.store(true);
    st.chapter_pages_again = false;
    MarkLayoutHintBusy();
    auto* ctx = new uint32_t(token);
    if (!SpawnPsrWorker("book_cpages", kOpenWorkerStack, tskIDLE_PRIORITY + 1, ChapterPageWorker, ctx,
                        nullptr, 0)) {
        ESP_LOGE(TAG, "book_cpages task failed");
        delete ctx;
        st.chapter_pages_busy.store(false);
        MarkLayoutHintDone();
    }
}

void StartLayoutWorker() {
    auto& st = State();
    if (st.layout_busy.load() || !st.session) {
        return;
    }
    const uint32_t token = st.layout_token.fetch_add(1) + 1;
    st.layout_busy.store(true);
    MarkLayoutHintBusy();
    auto* ctx = new LayoutWorkerArg{};
    ctx->token = token;
    ctx->anchor = st.layout_anchor;
    if (!SpawnPsrWorker("book_layout", kOpenWorkerStack, tskIDLE_PRIORITY + 1, LayoutWorker, ctx,
                        nullptr, 0)) {
        ESP_LOGE(TAG, "book_layout task failed");
        delete ctx;
        st.layout_busy.store(false);
        st.session->ClearLivePreview();
        st.settings_resume_after_open = (st.read_chrome == BookUiState::ReadChrome::kSettings);
        ReopenReaderAfterPrefsChange();
    }
}

void ReopenReaderAfterPrefsChange() {
    auto& st = State();
    if (st.opening.load()) {
        return;
    }
    if (st.selected < 0 || st.selected >= static_cast<int>(st.books.size())) {
        return;
    }
    st.layout_token.fetch_add(1);
    st.layout_busy.store(false);
    st.layout_again = false;
    st.layout_reload_font = false;
    st.read_chrome = BookUiState::ReadChrome::kReading;
    SetReadStatusVisible(false);
    SetReadSettingsSheetVisible(false);
    ApplyReaderPageGeometry(ReaderGeomMode::kImmersive);
    if (st.viewport_w <= 0 || st.viewport_h <= 0) {
        ESP_LOGE(TAG, "reopen prefs: invalid viewport %d×%d", static_cast<int>(st.viewport_w),
                 static_cast<int>(st.viewport_h));
        st.settings_resume_after_open = false;
        return;
    }
    const lv_coord_t vw = st.viewport_w;
    const lv_coord_t vh = st.viewport_h;

    ClearReaderPageDelta();
    EndReadingTimeTracking();
    st.session.reset();
    ReleaseBookFont();
    EnsureBookFont();
    st.session = std::make_unique<reader::BookSession>();
    BindSessionLayoutPrefs(*st.session);
    st.session->SetViewport(vw, vh);
    StartOpenWorker(st.selected);
}

void ApplyReaderLayoutLive(bool reload_font) {
    auto& st = State();
    if (st.opening.load() || !st.session || !st.session->IsOpen()) {
        return;
    }
    if (st.viewport_w <= 0 || st.viewport_h <= 0) {
        return;
    }
    const bool layout_running = st.layout_busy.load();
    if (layout_running && reload_font) {
        st.layout_again = true;
        st.layout_reload_font = true;
        return;
    }
    if (layout_running) {
        st.layout_again = true;
    }
    if (reload_font && st.chapter_pages_busy.load()) {
        st.layout_again = true;
        st.layout_reload_font = true;
        st.chapter_pages_again = true;
        st.session->InvalidateChapterPageTable();
        return;
    }

    ClearReaderPageDelta();
    if (reload_font) {
        ReleaseBookFont();
        EnsureBookFont();
    }
    BindSessionLayoutPrefs(*st.session);
    ApplyReaderPageGeometry(ReaderGeomMode::kImmersive);
    st.session->SetViewport(st.viewport_w, st.viewport_h);

    reader::BookSession::LayoutAnchor anchor = st.session->CaptureLayoutAnchor();
    if (!st.session->BeginLiveRelayout(&anchor)) {
        ESP_LOGW(TAG, "live relayout failed, fallback reopen");
        st.settings_resume_after_open = (st.read_chrome == BookUiState::ReadChrome::kSettings);
        ReopenReaderAfterPrefsChange();
        return;
    }

    const bool keep_sheet = (st.read_chrome == BookUiState::ReadChrome::kSettings);
    RenderReaderPage();
    if (keep_sheet) {
        EnsureSettingsSheetAfterLayout();
    } else {
        UpdateReadFooter();
    }

    if (st.session->NeedsTxtIndexRebuild() && st.session->LiveRelayoutPending()) {
        st.layout_anchor = anchor;
        if (layout_running) {
            st.layout_again = true;
        } else {
            st.layout_again = false;
            st.layout_reload_font = false;
            StartLayoutWorker();
        }
    } else if (st.session->NeedsChapterPageRebuild()) {
        StartChapterPageWorker();
    }
}

lv_obj_t* CreateReaderScreen(const reader::BookInfo& info) {
    auto& st = State();
    if (ReaderWorkersBusy()) {
        ESP_LOGW(TAG, "CreateReaderScreen blocked: worker busy");
        if (s_hooks.create_detail_scr && st.selected >= 0 &&
            st.selected < static_cast<int>(st.books.size())) {
            return s_hooks.create_detail_scr(st.selected);
        }
        return s_hooks.create_home_scr ? s_hooks.create_home_scr() : nullptr;
    }
    if (st.deferred_cleanup) {
        FinishDeferredCleanup();
    }
    EnsureBookFont();
    ScanReadFonts();
    EnsureFontListPageShowsSelection();
    EndReadingTimeTracking();
    st.session = std::make_unique<reader::BookSession>();
    BindSessionLayoutPrefs(*st.session);
    st.opening_format = info.format;

    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, ListFont(), 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    DisableScroll(scr);

    EpdStatusBar status = ScreenCreateStatusBar(scr);
    st.status_bar = status.bar;
    st.status_overlay = status.overlay;
    st.status_h = status.height;
    lv_obj_add_event_cb(scr, [](lv_event_t*) {
        StatusBarManager::GetInstance().ClearOngoing("book");
    }, LV_EVENT_DELETE, nullptr);
    SetReadStatusVisible(false);

    st.title_label = nullptr;

    lv_obj_t* body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, LV_VER_RES - kFooterH);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    DisableScroll(body);
    ResetReadTouch();
    AttachReadBodyInput(body);
    st.content = body;
    st.read_chrome = BookUiState::ReadChrome::kReading;
    st.toc_list_page = 0;

    st.settings_sheet = CreateSettingsSheet(scr);

    lv_obj_t* footer = lv_label_create(scr);
    lv_obj_set_width(footer, LV_HOR_RES - 16);
    lv_obj_set_style_text_align(footer, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(footer, ListFont(), 0);
    lv_obj_set_style_text_color(footer, lv_color_black(), 0);
    lv_label_set_long_mode(footer, LV_LABEL_LONG_CLIP);
    lv_label_set_text(footer, "");
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, 0);
    DisableScroll(footer);
    st.page_label = footer;

    ShowOpenProgress(0);
    st.read_scr = scr;
    ApplyReaderPageGeometry(ReaderGeomMode::kImmersive);
    st.session->SetViewport(st.viewport_w, st.viewport_h);
    ScreenSetIsHome(false);
    if (s_hooks.attach_vk) {
        s_hooks.attach_vk(scr);
    }
    StartOpenWorker(st.selected);
    return scr;
}

}  // namespace book_screen_detail
