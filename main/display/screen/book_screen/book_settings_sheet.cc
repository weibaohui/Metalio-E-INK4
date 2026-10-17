#include "book_settings_sheet.h"
#include "book_font_manager.h"
#include "book_reader.h"
#include "book_reader_prefs.h"
#include "book_toc.h"
#include "book_ttf_panel.h"
#include "book_ui_state.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <esp_log.h>
#include <esp_timer.h>

#include "assets/lang_config.h"
#include "display/screen/screen_common.h"
#include "haptic_feedback.h"
#include "sd_paths.h"

namespace book_screen_detail {

const char* TAG = "BookSettingsSheet";

constexpr lv_coord_t kSheetBorderW = 2;
constexpr lv_coord_t kFontListRowH = 44;
constexpr lv_coord_t kSheetGap = 10;
constexpr lv_coord_t kSheetIconBtn = 36;
constexpr lv_coord_t kSheetSidePad = 12;
constexpr lv_coord_t kSheetInnerPad = 12;
constexpr lv_coord_t kSheetRadius = 16;
constexpr lv_coord_t kShelfUnderlineH = 1;
constexpr lv_coord_t kShelfUnderlinePadHor = 5;
constexpr lv_coord_t kShelfMultiDotSize = 4;
constexpr lv_coord_t kShelfCheckSize = 22;
constexpr lv_coord_t kRowBorderW = 2;
constexpr lv_coord_t kTocLineCur = 3;
constexpr lv_coord_t kTocLineThin = 1;
constexpr int64_t kFontSuppressRowClickUs = 400000;

using namespace book_font;

void DisableScroll(lv_obj_t* obj) {
    if (obj != nullptr) {
        lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    }
}

struct SettingsHooks {
    SettingsScheduleLayoutApplyFn schedule_layout_apply = nullptr;
    SettingsApplyReaderLayoutLiveFn apply_reader_layout_live = nullptr;
    SettingsFlushLayoutDebounceFn flush_layout_debounce = nullptr;
    SettingsRenderReaderPageFn render_reader_page = nullptr;
    SettingsSetStatusVisibleFn set_status_visible = nullptr;
    SettingsBindSessionLayoutPrefsFn bind_session_prefs = nullptr;
};
SettingsHooks s_hooks;

ScreenPaintCoalesce s_settings_sheet_paint{};

void OnSheetTocClicked(lv_event_t* e);
void OnSheetFontSelect(lv_event_t* e);
void OnSheetFontLongPressed(lv_event_t* e);
void OnSheetFontPagePrev(lv_event_t* e);
void OnSheetFontPageNext(lv_event_t* e);
void OnSheetMarginDec(lv_event_t* e);
void OnSheetMarginInc(lv_event_t* e);
void OnSheetGapDec(lv_event_t* e);
void OnSheetGapInc(lv_event_t* e);
void OnSheetProgShow(lv_event_t* e);
void OnSheetProgHide(lv_event_t* e);
void OnSheetUnderlineMode(lv_event_t* e);
void OnFontMultiCancel(lv_event_t* e);
void OnFontMultiSelectAll(lv_event_t* e);
void OnFontMultiRemove(lv_event_t* e);
void ExitFontMultiMode(bool paint);
void EnterFontMultiModeSelect(int idx);
void RefreshFontMultiFooter();
void SyncFontSelectedSize();
int FontSelectedCount();
bool FontItemSelected(int idx);
void ToggleFontItemSelected(int idx);

lv_obj_t* MakeSheetIconBtn(lv_obj_t* parent, const char* txt, lv_event_cb_t cb, bool enabled) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, kSheetIconBtn, kSheetIconBtn);
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, kSheetBorderW, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_radius(btn, kSheetIconBtn / 2, 0);
    DisableScroll(btn);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(
        btn, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED, nullptr);
    SetSheetBtnEnabled(btn, enabled);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_font(lbl, ListFont(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

void SetSettingsScheduleLayoutApplyHandler(SettingsScheduleLayoutApplyFn fn) {
    s_hooks.schedule_layout_apply = fn;
}

void SetSettingsApplyReaderLayoutLiveHandler(SettingsApplyReaderLayoutLiveFn fn) {
    s_hooks.apply_reader_layout_live = fn;
}

void SetSettingsFlushLayoutDebounceHandler(SettingsFlushLayoutDebounceFn fn) {
    s_hooks.flush_layout_debounce = fn;
}

void SetSettingsRenderReaderPageHandler(SettingsRenderReaderPageFn fn) {
    s_hooks.render_reader_page = fn;
}

void SetSettingsSetStatusVisibleHandler(SettingsSetStatusVisibleFn fn) {
    s_hooks.set_status_visible = fn;
}

void SetSettingsBindSessionLayoutPrefsHandler(SettingsBindSessionLayoutPrefsFn fn) {
    s_hooks.bind_session_prefs = fn;
}

lv_obj_t* CreateSettingsSheet(lv_obj_t* scr) {
    auto& st = State();
    lv_obj_t* sheet = lv_obj_create(scr);
    st.settings_sheet = sheet;
    lv_obj_remove_style_all(sheet);
    lv_obj_set_width(sheet, LV_HOR_RES - kSheetSidePad * 2);
    lv_obj_set_height(sheet, LV_SIZE_CONTENT);
    lv_obj_align(sheet, LV_ALIGN_TOP_MID, 0, (st.status_h > 0 ? st.status_h : 40) + 10);
    lv_obj_set_style_bg_color(sheet, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(sheet, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(sheet, kSheetBorderW, 0);
    lv_obj_set_style_border_color(sheet, lv_color_black(), 0);
    lv_obj_set_style_radius(sheet, kSheetRadius, 0);
    lv_obj_set_style_pad_all(sheet, kSheetInnerPad, 0);
    lv_obj_set_style_pad_row(sheet, kSheetGap, 0);
    lv_obj_set_flex_flow(sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(sheet, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    DisableScroll(sheet);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(
        sheet, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(sheet, LV_OBJ_FLAG_HIDDEN);
    return sheet;
}

void ResetSettingsSheetPaint() {
    ScreenPaintCoalesceReset(&s_settings_sheet_paint);
}

void RequestSettingsSheetPaint() {
    if (s_settings_sheet_paint.paint == nullptr) {
        s_settings_sheet_paint.paint = RefreshSettingsSheet;
    }
    ScreenPaintCoalesceRequest(&s_settings_sheet_paint);
}

void SetReadSettingsSheetVisible(bool visible) {
    auto& st = State();
    if (st.settings_sheet == nullptr || !lv_obj_is_valid(st.settings_sheet)) {
        return;
    }
    if (visible) {
        lv_obj_clear_flag(st.settings_sheet, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(st.settings_sheet);
        if (st.status_bar != nullptr && lv_obj_is_valid(st.status_bar)) {
            lv_obj_move_foreground(st.status_bar);
        }
        if (st.status_overlay != nullptr && lv_obj_is_valid(st.status_overlay)) {
            lv_obj_move_foreground(st.status_overlay);
        }
    } else {
        lv_obj_add_flag(st.settings_sheet, LV_OBJ_FLAG_HIDDEN);
    }
}

void CloseReadSettingsSheet() {
    auto& st = State();
    ResetSettingsSheetPaint();
    st.font_multi = false;
    st.font_suppress_click_until_us = 0;
    st.font_suppress_click_idx = -1;
    st.font_suppress_next_click = false;
    st.font_selected.assign(st.font_entries.size(), 0);
    SetReadSettingsSheetVisible(false);
}

void ClearSettingsSheetWidgetRefs() {
    auto& st = State();
    for (int i = 0; i < kFontListPageSize; ++i) {
        st.sheet_font_rows[i] = {};
    }
    st.sheet_font_title = nullptr;
    st.sheet_font_page_prev = nullptr;
    st.sheet_font_page_next = nullptr;
    st.sheet_font_page_lab = nullptr;
    st.sheet_font_multi_bar = nullptr;
    st.sheet_margin_value = nullptr;
    st.sheet_margin_dec = nullptr;
    st.sheet_margin_inc = nullptr;
    st.sheet_margin_bound = nullptr;
    st.sheet_gap_value = nullptr;
    st.sheet_gap_dec = nullptr;
    st.sheet_gap_inc = nullptr;
    st.sheet_gap_bound = nullptr;
    st.sheet_font_import = nullptr;
    ClearTtfWidgetRefs();
}

bool SettingsSheetWidgetsReady() {
    auto& st = State();
    return st.settings_sheet != nullptr && lv_obj_is_valid(st.settings_sheet) &&
           st.sheet_margin_value != nullptr && lv_obj_is_valid(st.sheet_margin_value) &&
           st.sheet_font_page_lab != nullptr && lv_obj_is_valid(st.sheet_font_page_lab);
}

void SetSheetBtnEnabled(lv_obj_t* btn, bool enabled) {
    if (btn == nullptr || !lv_obj_is_valid(btn)) {
        return;
    }
    if (enabled) {
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(btn, LV_OPA_COVER, 0);
    } else {
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(btn, LV_OPA_40, 0);
    }
}

void UpdateSheetBoundTip(lv_obj_t* tip, bool can_dec, bool can_inc) {
    if (tip == nullptr || !lv_obj_is_valid(tip)) {
        return;
    }
    if (!can_dec) {
        lv_label_set_text(tip, "MIN");
        lv_obj_clear_flag(tip, LV_OBJ_FLAG_HIDDEN);
    } else if (!can_inc) {
        lv_label_set_text(tip, "MAX");
        lv_obj_clear_flag(tip, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(tip, LV_OBJ_FLAG_HIDDEN);
    }
}

void RefreshSettingsSheetAdj() {
    auto& st = State();
    if (!SettingsSheetWidgetsReady()) {
        return;
    }
    const int margin_i = BookReaderPrefsMarginPreset();
    const int gap_i = BookReaderPrefsSpacingPreset();
    const bool margin_dec = margin_i > 0;
    const bool margin_inc = margin_i + 1 < kBookReaderMarginPresetCount;
    const bool gap_dec = gap_i > 0;
    const bool gap_inc = gap_i + 1 < kBookReaderSpacingPresetCount;

    lv_label_set_text(st.sheet_margin_value, BookReaderPrefsMarginLabel(margin_i));
    if (st.sheet_gap_value != nullptr && lv_obj_is_valid(st.sheet_gap_value)) {
        lv_label_set_text(st.sheet_gap_value, BookReaderPrefsSpacingLabel(gap_i));
    }
    SetSheetBtnEnabled(st.sheet_margin_dec, margin_dec);
    SetSheetBtnEnabled(st.sheet_margin_inc, margin_inc);
    SetSheetBtnEnabled(st.sheet_gap_dec, gap_dec);
    SetSheetBtnEnabled(st.sheet_gap_inc, gap_inc);
    UpdateSheetBoundTip(st.sheet_margin_bound, margin_dec, margin_inc);
    UpdateSheetBoundTip(st.sheet_gap_bound, gap_dec, gap_inc);
}

void RefreshSettingsSheetFontList() {
    auto& st = State();
    if (!SettingsSheetWidgetsReady()) {
        return;
    }
    ClampFontListPage();
    SyncFontSelectedSize();
    const int font_i = CurrentFontIndex();
    const int font_n = static_cast<int>(st.font_entries.size());
    const int font_pages = FontListPageCount();
    const int font_page_start = st.font_list_page * kFontListPageSize;

    for (int i = 0; i < kFontListPageSize; ++i) {
        auto& fr = st.sheet_font_rows[i];
        if (fr.row == nullptr || !lv_obj_is_valid(fr.row)) {
            continue;
        }
        const int idx = font_page_start + i;
        if (idx >= font_n) {
            lv_obj_clear_flag(fr.row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_border_width(fr.row, 0, 0);
            lv_obj_set_style_border_side(fr.row, LV_BORDER_SIDE_NONE, 0);
            lv_obj_set_user_data(fr.row, reinterpret_cast<void*>(static_cast<intptr_t>(-1)));
            if (fr.name != nullptr && lv_obj_is_valid(fr.name)) {
                lv_label_set_text(fr.name, "");
            }
            if (fr.px != nullptr && lv_obj_is_valid(fr.px)) {
                lv_label_set_text(fr.px, "");
            }
            if (fr.check != nullptr && lv_obj_is_valid(fr.check)) {
                lv_obj_add_flag(fr.check, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clean(fr.check);
            }
            continue;
        }

        const ReadFontEntry& fe = st.font_entries[static_cast<size_t>(idx)];
        const bool on = (!st.font_multi && idx == font_i);
        const bool next_on = (!st.font_multi) && (i + 1 < kFontListPageSize) &&
                             (font_page_start + i + 1 == font_i) &&
                             (font_page_start + i + 1 < font_n);
        if (on) {
            lv_obj_set_style_border_width(fr.row, kTocLineCur, 0);
            lv_obj_set_style_border_side(
                fr.row, static_cast<lv_border_side_t>(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM),
                0);
        } else if (next_on) {
            lv_obj_set_style_border_width(fr.row, 0, 0);
            lv_obj_set_style_border_side(fr.row, LV_BORDER_SIDE_NONE, 0);
        } else if (i + 1 < kFontListPageSize) {
            lv_obj_set_style_border_width(fr.row, kTocLineThin, 0);
            lv_obj_set_style_border_side(fr.row, LV_BORDER_SIDE_BOTTOM, 0);
        } else {
            lv_obj_set_style_border_width(fr.row, 0, 0);
            lv_obj_set_style_border_side(fr.row, LV_BORDER_SIDE_NONE, 0);
        }
        lv_obj_set_style_border_color(fr.row, lv_color_black(), 0);
        lv_obj_set_style_border_opa(fr.row, LV_OPA_COVER, 0);

        lv_obj_add_flag(fr.row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(fr.row, reinterpret_cast<void*>(static_cast<intptr_t>(idx)));

        char name_buf[40];
        FormatFontListName(fe.file, name_buf, sizeof(name_buf));
        if (fr.name != nullptr && lv_obj_is_valid(fr.name)) {
            lv_label_set_text(fr.name, name_buf);
            lv_obj_set_style_text_font(fr.name, on ? ListFont() : ItemFont(), 0);
        }
        char px_buf[16];
        if (fe.size_px > 0) {
            std::snprintf(px_buf, sizeof(px_buf), "%dpx", fe.size_px);
        } else {
            std::snprintf(px_buf, sizeof(px_buf), "--");
        }
        if (fr.px != nullptr && lv_obj_is_valid(fr.px)) {
            lv_label_set_text(fr.px, px_buf);
            lv_obj_set_style_text_opa(fr.px, on ? LV_OPA_COVER : LV_OPA_70, 0);
        }
        if (fr.check != nullptr && lv_obj_is_valid(fr.check)) {
            if (st.font_multi) {
                lv_obj_clear_flag(fr.check, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clean(fr.check);
                if (FontItemSelected(idx)) {
                    lv_obj_t* mark = lv_label_create(fr.check);
                    lv_label_set_text(mark, "√");
                    lv_obj_set_style_text_font(mark, ItemFont(), 0);
                    lv_obj_set_style_text_color(mark, lv_color_black(), 0);
                    lv_obj_center(mark);
                    lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE);
                }
            } else {
                lv_obj_add_flag(fr.check, LV_OBJ_FLAG_HIDDEN);
                lv_obj_clean(fr.check);
            }
        }
    }

    const bool can_prev = st.font_list_page > 0;
    const bool can_next = st.font_list_page + 1 < font_pages;
    SetSheetBtnEnabled(st.sheet_font_page_prev, can_prev);
    SetSheetBtnEnabled(st.sheet_font_page_next, can_next);
    if (st.sheet_font_page_lab != nullptr && lv_obj_is_valid(st.sheet_font_page_lab)) {
        char page_meta[24];
        std::snprintf(page_meta, sizeof(page_meta), "%d / %d", st.font_list_page + 1, font_pages);
        lv_label_set_text(st.sheet_font_page_lab, page_meta);
    }
    RefreshFontMultiFooter();
}

void RefreshSettingsSheet() {
    if (State().read_chrome != BookUiState::ReadChrome::kSettings) {
        return;
    }
    RefreshSettingsSheetFontList();
    RefreshSettingsSheetAdj();
}

void EnsureSettingsSheetAfterLayout() {
    if (s_hooks.set_status_visible) {
        s_hooks.set_status_visible(true);
    }
    SetReadSettingsSheetVisible(true);
    if (!SettingsSheetWidgetsReady()) {
        EnsureFontListPageShowsSelection();
        RebuildSettingsSheet();
    }
}

void ShowReadSettingsSheet() {
    auto& st = State();
    if (st.read_scr == nullptr || st.opening.load() || !st.session || !st.session->IsOpen()) {
        return;
    }
    ClearReaderPageDelta();
    st.read_chrome = BookUiState::ReadChrome::kSettings;
    if (s_hooks.set_status_visible) {
        s_hooks.set_status_visible(true);
    }
    SetReadSettingsSheetVisible(true);

    EnsureFontListPageShowsSelection();
    if (SettingsSheetWidgetsReady()) {
        RequestSettingsSheetPaint();
        return;
    }
    RebuildSettingsSheet();
}

void ScanReadFonts() {
    book_font::ScanReadFonts(State().font_entries);
}

int CurrentFontIndex() {
    auto& st = State();
    if (st.font_entries.empty()) {
        ScanReadFonts();
    }
    const char* cur = BookReaderPrefsFontFile();
    for (int i = 0; i < static_cast<int>(st.font_entries.size()); ++i) {
        if (st.font_entries[static_cast<size_t>(i)].file == cur) {
            return i;
        }
    }
    return st.font_entries.empty() ? -1 : 0;
}

int FontListPageCount() {
    const int n = static_cast<int>(State().font_entries.size());
    if (n <= 0) {
        return 1;
    }
    return (n + kFontListPageSize - 1) / kFontListPageSize;
}

void ClampFontListPage() {
    auto& st = State();
    const int pages = FontListPageCount();
    if (st.font_list_page < 0) {
        st.font_list_page = 0;
    }
    if (st.font_list_page >= pages) {
        st.font_list_page = pages - 1;
    }
}

void EnsureFontListPageShowsSelection() {
    auto& st = State();
    const int cur = CurrentFontIndex();
    if (cur < 0) {
        st.font_list_page = 0;
        return;
    }
    st.font_list_page = cur / kFontListPageSize;
    ClampFontListPage();
}

void FormatFontListName(const std::string& file, char* out, size_t out_len) {
    if (out == nullptr || out_len == 0) {
        return;
    }
    std::string stem = file;
    if (stem.size() > 3 && stem.compare(stem.size() - 3, 3, ".ef") == 0) {
        stem.resize(stem.size() - 3);
    }
    if (stem.empty()) {
        stem = file;
    }
    constexpr size_t kMax = 22;
    if (stem.size() <= kMax) {
        std::snprintf(out, out_len, "%s", stem.c_str());
        return;
    }
    std::snprintf(out, out_len, "%.19s...", stem.c_str());
}

static void OnTtfFontsChangedFromPanel() {
    ScanReadFonts();
    ClampFontListPage();
    auto& st = State();
    if (st.settings_sheet != nullptr && lv_obj_is_valid(st.settings_sheet)) {
        RefreshSettingsSheetFontList();
    }
}

static void FillTtfEfTemplatePath(char* out, size_t out_len) {
    if (out == nullptr || out_len == 0) {
        return;
    }
    out[0] = '\0';
    auto& st = State();
    const int fi = CurrentFontIndex();
    if (fi >= 0 && fi < static_cast<int>(st.font_entries.size())) {
        std::snprintf(out, out_len, "%s/%s", SD_PATH_FONTS,
                      st.font_entries[static_cast<size_t>(fi)].file.c_str());
    } else {
        std::snprintf(out, out_len, "%s", SD_PATH_BOOK_FONT);
    }
}

struct SettingsTtfPanelHooksInit {
    SettingsTtfPanelHooksInit() {
        SetTtfFontsChangedHandler(&OnTtfFontsChangedFromPanel);
        SetTtfEfTemplateHandler(&FillTtfEfTemplatePath);
    }
};
static SettingsTtfPanelHooksInit s_settings_ttf_panel_hooks_init;

void OnSheetTocClicked(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    OpenReadToc();
}

void SyncFontSelectedSize() {
    auto& st = State();
    if (st.font_selected.size() != st.font_entries.size()) {
        st.font_selected.assign(st.font_entries.size(), 0);
    }
}

int FontSelectedCount() {
    SyncFontSelectedSize();
    int n = 0;
    for (uint8_t v : State().font_selected) {
        if (v != 0) {
            ++n;
        }
    }
    return n;
}

bool FontItemSelected(int idx) {
    SyncFontSelectedSize();
    auto& st = State();
    return idx >= 0 && idx < static_cast<int>(st.font_selected.size()) &&
           st.font_selected[static_cast<size_t>(idx)] != 0;
}

void ToggleFontItemSelected(int idx) {
    SyncFontSelectedSize();
    auto& st = State();
    if (idx < 0 || idx >= static_cast<int>(st.font_selected.size())) {
        return;
    }
    st.font_selected[static_cast<size_t>(idx)] =
        st.font_selected[static_cast<size_t>(idx)] ? 0 : 1;
}

void RefreshFontMultiFooter() {
    auto& st = State();
    if (st.font_multi) {
        if (st.sheet_font_multi_bar != nullptr && lv_obj_is_valid(st.sheet_font_multi_bar)) {
            lv_obj_clear_flag(st.sheet_font_multi_bar, LV_OBJ_FLAG_HIDDEN);
        }
        if (st.sheet_font_title != nullptr && lv_obj_is_valid(st.sheet_font_title)) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), Lang::Strings::BOOK_SELECTED_FMT, FontSelectedCount());
            lv_label_set_text(st.sheet_font_title, buf);
            lv_obj_set_style_text_opa(st.sheet_font_title, LV_OPA_COVER, 0);
        }
    } else {
        if (st.sheet_font_multi_bar != nullptr && lv_obj_is_valid(st.sheet_font_multi_bar)) {
            lv_obj_add_flag(st.sheet_font_multi_bar, LV_OBJ_FLAG_HIDDEN);
        }
        if (st.sheet_font_title != nullptr && lv_obj_is_valid(st.sheet_font_title)) {
            lv_label_set_text(st.sheet_font_title, Lang::Strings::BOOK_FONT_TITLE);
            lv_obj_set_style_text_opa(st.sheet_font_title, LV_OPA_70, 0);
        }
    }
}

void ExitFontMultiMode(bool paint) {
    auto& st = State();
    st.font_multi = false;
    st.font_suppress_click_until_us = 0;
    st.font_suppress_click_idx = -1;
    st.font_suppress_next_click = false;
    st.font_selected.assign(st.font_entries.size(), 0);
    if (paint) {
        RequestSettingsSheetPaint();
    } else {
        RefreshFontMultiFooter();
    }
}

void EnterFontMultiModeSelect(int idx) {
    auto& st = State();
    st.font_multi = true;
    st.font_selected.assign(st.font_entries.size(), 0);
    if (idx >= 0 && idx < static_cast<int>(st.font_selected.size())) {
        st.font_selected[static_cast<size_t>(idx)] = 1;
    }
    RequestSettingsSheetPaint();
}

void OnFontMultiCancel(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    if (State().opening.load()) {
        return;
    }
    ExitFontMultiMode(true);
}

void OnFontMultiSelectAll(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = State();
    if (!st.font_multi || st.font_entries.empty() || st.opening.load()) {
        return;
    }
    SyncFontSelectedSize();
    const int n = static_cast<int>(st.font_entries.size());
    const bool clear = (FontSelectedCount() >= n);
    st.font_selected.assign(static_cast<size_t>(n), clear ? 0 : 1);
    RequestSettingsSheetPaint();
}

void OnFontMultiRemove(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = State();
    if (!st.font_multi || st.opening.load()) {
        return;
    }
    SyncFontSelectedSize();
    if (FontSelectedCount() <= 0) {
        return;
    }

    const std::string cur = BookReaderPrefsFontFile();
    bool touch_cur = false;
    for (int i = 0; i < static_cast<int>(st.font_entries.size()); ++i) {
        if (i < static_cast<int>(st.font_selected.size()) &&
            st.font_selected[static_cast<size_t>(i)] != 0 &&
            st.font_entries[static_cast<size_t>(i)].file == cur) {
            touch_cur = true;
            break;
        }
    }

    if (touch_cur) {
        if (st.chapter_pages_busy.load() || st.layout_busy.load()) {
            st.layout_token.fetch_add(1);
            st.chapter_pages_token.fetch_add(1);
            st.layout_again = true;
            st.layout_reload_font = true;
            st.chapter_pages_again = true;
            if (st.session) {
                st.session->InvalidateChapterPageTable();
            }
        }
        ReleaseBookFont();
        if (st.session && st.session->IsOpen()) {
            if (s_hooks.bind_session_prefs) {
                s_hooks.bind_session_prefs(*st.session);
            }
            const bool keep_sheet = (st.read_chrome == BookUiState::ReadChrome::kSettings);
            if (s_hooks.render_reader_page) {
                s_hooks.render_reader_page();
            }
            if (keep_sheet) {
                EnsureSettingsSheetAfterLayout();
            }
        }
    }

    for (int i = static_cast<int>(st.font_entries.size()) - 1; i >= 0; --i) {
        if (i >= static_cast<int>(st.font_selected.size()) ||
            st.font_selected[static_cast<size_t>(i)] == 0) {
            continue;
        }
        std::string err;
        if (!DeleteReadFontFile(st.font_entries[static_cast<size_t>(i)].file, err)) {
            ESP_LOGW(TAG, "font batch delete fail %s: %s",
                     st.font_entries[static_cast<size_t>(i)].file.c_str(), err.c_str());
            continue;
        }
        st.font_entries.erase(st.font_entries.begin() + i);
    }

    bool need_layout = false;
    auto still_has = [&](const char* file) {
        if (file == nullptr || file[0] == '\0') {
            return false;
        }
        for (const auto& fe : st.font_entries) {
            if (fe.file == file) {
                return true;
            }
        }
        return false;
    };
    if (!still_has(BookReaderPrefsFontFile())) {
        if (!st.font_entries.empty()) {
            BookReaderPrefsSetFontFile(st.font_entries.front().file.c_str());
        }
        need_layout = true;
    } else if (touch_cur) {
        need_layout = true;
    }

    st.font_multi = false;
    st.font_suppress_click_until_us = 0;
    st.font_suppress_click_idx = -1;
    st.font_suppress_next_click = false;
    st.font_selected.assign(st.font_entries.size(), 0);
    ClampFontListPage();
    EnsureFontListPageShowsSelection();
    if (st.font_entries.empty() || !SettingsSheetWidgetsReady()) {
        RebuildSettingsSheet();
    } else {
        RequestSettingsSheetPaint();
    }
    if (need_layout && s_hooks.schedule_layout_apply) {
        s_hooks.schedule_layout_apply(true);
    }
}

void OnSheetFontLongPressed(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    if (State().opening.load()) {
        return;
    }
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
    auto& st = State();
    if (idx < 0 || idx >= static_cast<int>(st.font_entries.size())) {
        return;
    }
    if (!st.font_multi) {
        st.font_suppress_click_idx = idx;
        st.font_suppress_click_until_us = esp_timer_get_time() + kFontSuppressRowClickUs;
        st.font_suppress_next_click = true;
        EnterFontMultiModeSelect(idx);
        return;
    }
    ToggleFontItemSelected(idx);
    RequestSettingsSheetPaint();
}

void OnSheetFontSelect(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    if (State().opening.load()) {
        return;
    }
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
    auto& st = State();
    if (idx < 0 || idx >= static_cast<int>(st.font_entries.size())) {
        return;
    }
    if (st.font_suppress_next_click && idx == st.font_suppress_click_idx) {
        st.font_suppress_next_click = false;
        return;
    }
    if (idx == st.font_suppress_click_idx &&
        esp_timer_get_time() < st.font_suppress_click_until_us) {
        return;
    }
    if (st.font_multi) {
        ToggleFontItemSelected(idx);
        RequestSettingsSheetPaint();
        return;
    }
    const char* file = st.font_entries[static_cast<size_t>(idx)].file.c_str();
    if (std::strcmp(BookReaderPrefsFontFile(), file) == 0) {
        return;
    }
    BookReaderPrefsSetFontFile(file);
    EnsureFontListPageShowsSelection();
    RequestSettingsSheetPaint();
    if (s_hooks.schedule_layout_apply) {
        s_hooks.schedule_layout_apply(true);
    }
}

void OnSheetFontPagePrev(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = State();
    if (st.font_list_page <= 0) {
        return;
    }
    st.font_list_page -= 1;
    RequestSettingsSheetPaint();
}

void OnSheetFontPageNext(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = State();
    if (st.font_list_page + 1 >= FontListPageCount()) {
        return;
    }
    st.font_list_page += 1;
    RequestSettingsSheetPaint();
}

void OnSheetMarginDec(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    if (State().opening.load()) {
        return;
    }
    const int cur = BookReaderPrefsMarginPreset();
    if (cur <= 0) {
        return;
    }
    BookReaderPrefsSetMarginPreset(cur - 1);
    RequestSettingsSheetPaint();
    if (s_hooks.schedule_layout_apply) {
        s_hooks.schedule_layout_apply(false);
    }
}

void OnSheetMarginInc(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    if (State().opening.load()) {
        return;
    }
    const int cur = BookReaderPrefsMarginPreset();
    if (cur + 1 >= kBookReaderMarginPresetCount) {
        return;
    }
    BookReaderPrefsSetMarginPreset(cur + 1);
    RequestSettingsSheetPaint();
    if (s_hooks.schedule_layout_apply) {
        s_hooks.schedule_layout_apply(false);
    }
}

void OnSheetGapDec(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    if (State().opening.load()) {
        return;
    }
    const int cur = BookReaderPrefsSpacingPreset();
    if (cur <= 0) {
        return;
    }
    BookReaderPrefsSetSpacingPreset(cur - 1);
    RequestSettingsSheetPaint();
    if (s_hooks.schedule_layout_apply) {
        s_hooks.schedule_layout_apply(false);
    }
}

void OnSheetGapInc(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    if (State().opening.load()) {
        return;
    }
    const int cur = BookReaderPrefsSpacingPreset();
    if (cur + 1 >= kBookReaderSpacingPresetCount) {
        return;
    }
    BookReaderPrefsSetSpacingPreset(cur + 1);
    RequestSettingsSheetPaint();
    if (s_hooks.schedule_layout_apply) {
        s_hooks.schedule_layout_apply(false);
    }
}

void OnSheetProgShow(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    if (BookReaderPrefsHideProgress() == 0) {
        return;
    }
    BookReaderPrefsSetHideProgress(0);
    RebuildSettingsSheet();
    if (State().layout_debounce_timer != nullptr) {
        if (s_hooks.flush_layout_debounce) {
            s_hooks.flush_layout_debounce();
        }
    } else {
        if (s_hooks.apply_reader_layout_live) {
            s_hooks.apply_reader_layout_live(false);
        }
    }
}

void OnSheetProgHide(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    if (BookReaderPrefsHideProgress() != 0) {
        return;
    }
    BookReaderPrefsSetHideProgress(1);
    RebuildSettingsSheet();
    if (State().layout_debounce_timer != nullptr) {
        if (s_hooks.flush_layout_debounce) {
            s_hooks.flush_layout_debounce();
        }
    } else {
        if (s_hooks.apply_reader_layout_live) {
            s_hooks.apply_reader_layout_live(false);
        }
    }
}

void OnSheetUnderlineMode(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    const int mode = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (mode < 0 || mode >= kBookReaderUnderlineModeCount) {
        return;
    }
    if (BookReaderPrefsUnderlineMode() == mode) {
        return;
    }
    BookReaderPrefsSetUnderlineMode(mode);
    RebuildSettingsSheet();
    if (s_hooks.render_reader_page) {
        s_hooks.render_reader_page();
    }
}

void RebuildSettingsSheet() {
    auto& st = State();
    if (st.settings_sheet == nullptr || !lv_obj_is_valid(st.settings_sheet)) {
        return;
    }
    ClearSettingsSheetWidgetRefs();
    lv_obj_clean(st.settings_sheet);
    lv_obj_set_style_min_height(st.settings_sheet, 0, 0);
    if (st.font_entries.empty()) {
        ScanReadFonts();
    }
    SyncFontSelectedSize();
    ClampFontListPage();
    const int font_i = CurrentFontIndex();
    const int font_n = static_cast<int>(st.font_entries.size());
    const int font_pages = FontListPageCount();
    const int font_page_start = st.font_list_page * kFontListPageSize;

    auto add_title_with_bound = [&](lv_obj_t* parent, const char* title, bool can_dec,
                                    bool can_inc, bool center, lv_obj_t** out_bound) {
        lv_obj_t* row = lv_obj_create(parent);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, center ? LV_FLEX_ALIGN_CENTER : LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(row, 6, 0);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        DisableScroll(row);

        lv_obj_t* t = lv_label_create(row);
        lv_label_set_text(t, title);
        lv_obj_set_style_text_font(t, ItemFont(), 0);
        lv_obj_clear_flag(t, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* tip = lv_label_create(row);
        lv_obj_set_style_text_font(tip, ItemFont(), 0);
        lv_obj_set_style_text_opa(tip, LV_OPA_70, 0);
        lv_obj_clear_flag(tip, LV_OBJ_FLAG_CLICKABLE);
        UpdateSheetBoundTip(tip, can_dec, can_inc);
        if (out_bound != nullptr) {
            *out_bound = tip;
        }
    };

    lv_obj_t* font_title_row = lv_obj_create(st.settings_sheet);
    lv_obj_remove_style_all(font_title_row);
    lv_obj_set_width(font_title_row, lv_pct(100));
    lv_obj_set_height(font_title_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(font_title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(font_title_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(font_title_row, LV_OBJ_FLAG_CLICKABLE);
    DisableScroll(font_title_row);

    lv_obj_t* font_title = lv_label_create(font_title_row);
    lv_label_set_text(font_title, Lang::Strings::BOOK_FONT_TITLE);
    lv_obj_set_style_text_font(font_title, ItemFont(), 0);
    lv_obj_set_style_text_opa(font_title, LV_OPA_70, 0);
    lv_obj_clear_flag(font_title, LV_OBJ_FLAG_CLICKABLE);
    st.sheet_font_title = font_title;

    lv_obj_t* import_btn = lv_obj_create(font_title_row);
    lv_obj_remove_style_all(import_btn);
    lv_obj_set_height(import_btn, 36);
    lv_obj_set_width(import_btn, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(import_btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_hor(import_btn, 4, 0);
    lv_obj_set_flex_flow(import_btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(import_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    DisableScroll(import_btn);
    lv_obj_add_flag(import_btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(import_btn);
    lv_obj_add_event_cb(import_btn, OnFontImportClick, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(
        import_btn, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* import_wrap = lv_obj_create(import_btn);
    lv_obj_remove_style_all(import_wrap);
    lv_obj_set_size(import_wrap, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_border_side(import_wrap, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(import_wrap, kShelfUnderlineH, 0);
    lv_obj_set_style_border_color(import_wrap, lv_color_black(), 0);
    lv_obj_set_style_pad_bottom(import_wrap, 2, 0);
    lv_obj_set_style_pad_hor(import_wrap, kShelfUnderlinePadHor, 0);
    lv_obj_set_style_bg_opa(import_wrap, LV_OPA_TRANSP, 0);
    DisableScroll(import_wrap);
    lv_obj_clear_flag(import_wrap, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* import_lbl = lv_label_create(import_wrap);
    lv_label_set_text(import_lbl, "导入 TTF");
    lv_obj_set_style_text_font(import_lbl, ItemFont(), 0);
    lv_obj_set_style_text_color(import_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(import_lbl, LV_OBJ_FLAG_CLICKABLE);
    st.sheet_font_import = import_btn;

    st.sheet_font_multi_bar = lv_obj_create(st.settings_sheet);
    lv_obj_remove_style_all(st.sheet_font_multi_bar);
    lv_obj_set_width(st.sheet_font_multi_bar, lv_pct(100));
    lv_obj_set_height(st.sheet_font_multi_bar, 36);
    lv_obj_set_style_bg_opa(st.sheet_font_multi_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(st.sheet_font_multi_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(st.sheet_font_multi_bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(st.sheet_font_multi_bar, 10, 0);
    DisableScroll(st.sheet_font_multi_bar);
    lv_obj_clear_flag(st.sheet_font_multi_bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(st.sheet_font_multi_bar, LV_OBJ_FLAG_HIDDEN);

    auto make_font_multi_action = [](lv_obj_t* parent, const char* text, lv_event_cb_t cb) {
        lv_obj_t* btn = lv_obj_create(parent);
        lv_obj_remove_style_all(btn);
        lv_obj_set_height(btn, 36);
        lv_obj_set_width(btn, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_hor(btn, 4, 0);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        DisableScroll(btn);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(btn);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_add_event_cb(
            btn, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED, nullptr);

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
    auto make_font_multi_dot = [](lv_obj_t* parent) {
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
    make_font_multi_action(st.sheet_font_multi_bar, Lang::Strings::COMMON_CANCEL, OnFontMultiCancel);
    make_font_multi_dot(st.sheet_font_multi_bar);
    make_font_multi_action(st.sheet_font_multi_bar, Lang::Strings::COMMON_SELECT_ALL, OnFontMultiSelectAll);
    make_font_multi_dot(st.sheet_font_multi_bar);
    make_font_multi_action(st.sheet_font_multi_bar, Lang::Strings::COMMON_REMOVE, OnFontMultiRemove);

    constexpr lv_coord_t kFontListH = kFontListRowH * kFontListPageSize;
    lv_obj_t* font_panel = lv_obj_create(st.settings_sheet);
    lv_obj_remove_style_all(font_panel);
    lv_obj_set_width(font_panel, lv_pct(100));
    lv_obj_set_height(font_panel, kFontListH + 40);
    lv_obj_set_style_bg_color(font_panel, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(font_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(font_panel, kSheetBorderW, 0);
    lv_obj_set_style_border_color(font_panel, lv_color_black(), 0);
    lv_obj_set_style_radius(font_panel, 12, 0);
    lv_obj_set_flex_flow(font_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(font_panel, LV_OBJ_FLAG_CLICKABLE);
    DisableScroll(font_panel);

    lv_obj_t* font_list = lv_obj_create(font_panel);
    lv_obj_remove_style_all(font_list);
    lv_obj_set_width(font_list, lv_pct(100));
    lv_obj_set_height(font_list, kFontListH);
    lv_obj_set_flex_flow(font_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_clear_flag(font_list, LV_OBJ_FLAG_CLICKABLE);
    DisableScroll(font_list);

    if (font_n <= 0) {
        lv_obj_t* empty = lv_label_create(font_list);
        lv_label_set_text(empty, Lang::Strings::BOOK_FONT_EMPTY);
        lv_obj_set_style_text_font(empty, ItemFont(), 0);
        lv_obj_set_style_pad_all(empty, 12, 0);
        lv_obj_clear_flag(empty, LV_OBJ_FLAG_CLICKABLE);
    } else {
        for (int i = 0; i < kFontListPageSize; ++i) {
            const int idx = font_page_start + i;
            lv_obj_t* row = lv_obj_create(font_list);
            lv_obj_remove_style_all(row);
            lv_obj_set_width(row, lv_pct(100));
            lv_obj_set_height(row, kFontListRowH);
            lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
            lv_obj_set_style_pad_hor(row, 12, 0);
            lv_obj_set_style_pad_column(row, 8, 0);
            lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            DisableScroll(row);

            lv_obj_t* name = lv_label_create(row);
            lv_obj_set_flex_grow(name, 1);
            lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
            lv_obj_clear_flag(name, LV_OBJ_FLAG_CLICKABLE);

            lv_obj_t* px = lv_label_create(row);
            lv_obj_set_style_text_font(px, ItemFont(), 0);
            lv_obj_clear_flag(px, LV_OBJ_FLAG_CLICKABLE);

            lv_obj_t* check = lv_obj_create(row);
            lv_obj_remove_style_all(check);
            lv_obj_set_size(check, kShelfCheckSize, kShelfCheckSize);
            lv_obj_set_style_bg_color(check, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(check, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(check, lv_color_black(), 0);
            lv_obj_set_style_border_width(check, kRowBorderW, 0);
            lv_obj_set_style_radius(check, 4, 0);
            DisableScroll(check);
            lv_obj_clear_flag(check, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_add_flag(check, LV_OBJ_FLAG_HIDDEN);

            st.sheet_font_rows[i].row = row;
            st.sheet_font_rows[i].name = name;
            st.sheet_font_rows[i].px = px;
            st.sheet_font_rows[i].check = check;

            HapticAttachClick(row);
            lv_obj_add_event_cb(row, OnSheetFontSelect, LV_EVENT_CLICKED, nullptr);
            lv_obj_add_event_cb(row, OnSheetFontLongPressed, LV_EVENT_LONG_PRESSED, nullptr);
            lv_obj_add_event_cb(
                row, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED, nullptr);

            if (idx >= font_n) {
                lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_set_user_data(row, reinterpret_cast<void*>(static_cast<intptr_t>(-1)));
                lv_label_set_text(name, "");
                lv_label_set_text(px, "");
                continue;
            }

            const ReadFontEntry& fe = st.font_entries[static_cast<size_t>(idx)];
            const bool on = (!st.font_multi && idx == font_i);
            const bool next_on = (!st.font_multi) && (i + 1 < kFontListPageSize) &&
                                 (font_page_start + i + 1 == font_i) &&
                                 (font_page_start + i + 1 < font_n);
            if (on) {
                lv_obj_set_style_border_width(row, kTocLineCur, 0);
                lv_obj_set_style_border_side(
                    row, static_cast<lv_border_side_t>(LV_BORDER_SIDE_TOP | LV_BORDER_SIDE_BOTTOM),
                    0);
            } else if (next_on) {
                lv_obj_set_style_border_width(row, 0, 0);
                lv_obj_set_style_border_side(row, LV_BORDER_SIDE_NONE, 0);
            } else if (i + 1 < kFontListPageSize) {
                lv_obj_set_style_border_width(row, kTocLineThin, 0);
                lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
            } else {
                lv_obj_set_style_border_width(row, 0, 0);
            }
            lv_obj_set_style_border_color(row, lv_color_black(), 0);
            lv_obj_set_style_border_opa(row, LV_OPA_COVER, 0);

            lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_user_data(row, reinterpret_cast<void*>(static_cast<intptr_t>(idx)));

            char name_buf[40];
            FormatFontListName(fe.file, name_buf, sizeof(name_buf));
            lv_label_set_text(name, name_buf);
            lv_obj_set_style_text_font(name, on ? ListFont() : ItemFont(), 0);

            char px_buf[16];
            if (fe.size_px > 0) {
                std::snprintf(px_buf, sizeof(px_buf), "%dpx", fe.size_px);
            } else {
                std::snprintf(px_buf, sizeof(px_buf), "--");
            }
            lv_label_set_text(px, px_buf);
            lv_obj_set_style_text_opa(px, on ? LV_OPA_COVER : LV_OPA_70, 0);

            if (st.font_multi) {
                lv_obj_clear_flag(check, LV_OBJ_FLAG_HIDDEN);
                if (FontItemSelected(idx)) {
                    lv_obj_t* mark = lv_label_create(check);
                    lv_label_set_text(mark, "√");
                    lv_obj_set_style_text_font(mark, ItemFont(), 0);
                    lv_obj_set_style_text_color(mark, lv_color_black(), 0);
                    lv_obj_center(mark);
                    lv_obj_clear_flag(mark, LV_OBJ_FLAG_CLICKABLE);
                }
            }
        }
    }

    lv_obj_t* pager = lv_obj_create(font_panel);
    lv_obj_remove_style_all(pager);
    lv_obj_set_width(pager, lv_pct(100));
    lv_obj_set_height(pager, 40);
    lv_obj_set_style_pad_hor(pager, 8, 0);
    lv_obj_set_style_bg_opa(pager, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(pager, kTocLineThin, 0);
    lv_obj_set_style_border_side(pager, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_color(pager, lv_color_black(), 0);
    lv_obj_set_flex_flow(pager, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(pager, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(pager, LV_OBJ_FLAG_CLICKABLE);
    DisableScroll(pager);

    const bool can_prev = st.font_list_page > 0;
    const bool can_next = st.font_list_page + 1 < font_pages;
    auto make_page_btn = [&](const char* txt, lv_event_cb_t cb, bool enabled) -> lv_obj_t* {
        lv_obj_t* btn = lv_obj_create(pager);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, 40, 32);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        DisableScroll(btn);
        HapticAttachClick(btn);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_add_event_cb(
            btn, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED, nullptr);
        SetSheetBtnEnabled(btn, enabled);
        lv_obj_t* lbl = lv_label_create(btn);
        lv_label_set_text(lbl, txt);
        lv_obj_set_style_text_font(lbl, ItemFont(), 0);
        lv_obj_center(lbl);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        return btn;
    };
    st.sheet_font_page_prev = make_page_btn("▲", OnSheetFontPagePrev, can_prev);
    char page_meta[24];
    std::snprintf(page_meta, sizeof(page_meta), "%d / %d", st.font_list_page + 1, font_pages);
    lv_obj_t* page_lab = lv_label_create(pager);
    lv_label_set_text(page_lab, page_meta);
    lv_obj_set_style_text_font(page_lab, ItemFont(), 0);
    lv_obj_set_style_text_opa(page_lab, LV_OPA_70, 0);
    lv_obj_clear_flag(page_lab, LV_OBJ_FLAG_CLICKABLE);
    st.sheet_font_page_lab = page_lab;
    st.sheet_font_page_next = make_page_btn("▼", OnSheetFontPageNext, can_next);
    RefreshFontMultiFooter();

    lv_obj_t* pair = lv_obj_create(st.settings_sheet);
    lv_obj_remove_style_all(pair);
    lv_obj_set_width(pair, lv_pct(100));
    lv_obj_set_height(pair, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(pair, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(pair, kSheetGap, 0);
    lv_obj_clear_flag(pair, LV_OBJ_FLAG_CLICKABLE);
    DisableScroll(pair);

    const int margin_i = BookReaderPrefsMarginPreset();
    const int gap_i = BookReaderPrefsSpacingPreset();

    auto make_adj = [&](const char* title, const char* value, lv_event_cb_t dec, lv_event_cb_t inc,
                        bool can_dec, bool can_inc, lv_obj_t** out_value, lv_obj_t** out_dec,
                        lv_obj_t** out_inc, lv_obj_t** out_bound) {
        lv_obj_t* card = lv_obj_create(pair);
        lv_obj_remove_style_all(card);
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(card, 1);
        lv_obj_set_style_bg_color(card, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(card, kSheetBorderW, 0);
        lv_obj_set_style_border_color(card, lv_color_black(), 0);
        lv_obj_set_style_radius(card, 12, 0);
        lv_obj_set_style_pad_all(card, 8, 0);
        lv_obj_set_style_pad_row(card, 6, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);
        DisableScroll(card);

        add_title_with_bound(card, title, can_dec, can_inc, true, out_bound);

        lv_obj_t* row = lv_obj_create(card);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, kSheetIconBtn);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        DisableScroll(row);
        lv_obj_t* dec_btn = MakeSheetIconBtn(row, "-", dec, can_dec);
        lv_obj_t* v = lv_label_create(row);
        lv_label_set_text(v, value);
        lv_obj_set_style_text_font(v, ListFont(), 0);
        lv_obj_clear_flag(v, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* inc_btn = MakeSheetIconBtn(row, "+", inc, can_inc);
        if (out_value != nullptr) {
            *out_value = v;
        }
        if (out_dec != nullptr) {
            *out_dec = dec_btn;
        }
        if (out_inc != nullptr) {
            *out_inc = inc_btn;
        }
    };
    make_adj(Lang::Strings::BOOK_MARGIN, BookReaderPrefsMarginLabel(margin_i), OnSheetMarginDec, OnSheetMarginInc,
             margin_i > 0, margin_i + 1 < kBookReaderMarginPresetCount, &st.sheet_margin_value,
             &st.sheet_margin_dec, &st.sheet_margin_inc, &st.sheet_margin_bound);
    make_adj(Lang::Strings::BOOK_LINE_GAP, BookReaderPrefsSpacingLabel(gap_i), OnSheetGapDec, OnSheetGapInc, gap_i > 0,
             gap_i + 1 < kBookReaderSpacingPresetCount, &st.sheet_gap_value, &st.sheet_gap_dec,
             &st.sheet_gap_inc, &st.sheet_gap_bound);

    constexpr lv_coord_t kSegPad = 2;
    constexpr lv_coord_t kSegBtnH = 32;
    constexpr lv_coord_t kSegBtnW = 68;
    const lv_coord_t seg_h = kSegBtnH + kSegPad * 2 + kSheetBorderW * 2;
    auto make_on_off_row = [&](const char* title, bool on, lv_event_cb_t on_cb, lv_event_cb_t off_cb) {
        lv_obj_t* row = lv_obj_create(st.settings_sheet);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, 48);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        DisableScroll(row);
        lv_obj_t* lab = lv_label_create(row);
        lv_label_set_text(lab, title);
        lv_obj_set_style_text_font(lab, ItemFont(), 0);
        lv_obj_clear_flag(lab, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* seg = lv_obj_create(row);
        lv_obj_remove_style_all(seg);
        lv_obj_set_size(seg, LV_SIZE_CONTENT, seg_h);
        lv_obj_set_style_bg_color(seg, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(seg, kSheetBorderW, 0);
        lv_obj_set_style_border_color(seg, lv_color_black(), 0);
        lv_obj_set_style_radius(seg, seg_h / 2, 0);
        lv_obj_set_style_pad_all(seg, kSegPad, 0);
        lv_obj_set_style_pad_column(seg, 2, 0);
        lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(seg, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(seg, LV_OBJ_FLAG_CLICKABLE);
        DisableScroll(seg);
        auto make_seg = [&](const char* txt, bool selected, lv_event_cb_t cb) {
            lv_obj_t* b = lv_obj_create(seg);
            lv_obj_remove_style_all(b);
            lv_obj_set_size(b, kSegBtnW, kSegBtnH);
            lv_obj_set_style_radius(b, kSegBtnH / 2, 0);
            lv_obj_set_style_bg_color(b, selected ? lv_color_black() : lv_color_white(), 0);
            lv_obj_set_style_bg_opa(b, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
            HapticAttachClick(b);
            lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);
            lv_obj_add_event_cb(
                b, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED, nullptr);
            lv_obj_t* l = lv_label_create(b);
            lv_label_set_text(l, txt);
            lv_obj_set_style_text_font(l, ItemFont(), 0);
            lv_obj_set_style_text_color(l, selected ? lv_color_white() : lv_color_black(), 0);
            lv_obj_center(l);
            lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
        };
        make_seg(Lang::Strings::COMMON_ON, on, on_cb);
        make_seg(Lang::Strings::COMMON_OFF, !on, off_cb);
    };
    make_on_off_row(Lang::Strings::BOOK_CHAPTER_PROGRESS, BookReaderPrefsHideProgress() == 0,
                    OnSheetProgShow, OnSheetProgHide);

    {
        constexpr lv_coord_t kUlBtnW = 68;
        lv_obj_t* row = lv_obj_create(st.settings_sheet);
        lv_obj_remove_style_all(row);
        lv_obj_set_width(row, lv_pct(100));
        lv_obj_set_height(row, 48);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
        DisableScroll(row);
        lv_obj_t* lab = lv_label_create(row);
        lv_label_set_text(lab, Lang::Strings::BOOK_UNDERLINE);
        lv_obj_set_style_text_font(lab, ItemFont(), 0);
        lv_obj_clear_flag(lab, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* seg = lv_obj_create(row);
        lv_obj_remove_style_all(seg);
        lv_obj_set_size(seg, LV_SIZE_CONTENT, seg_h);
        lv_obj_set_style_bg_color(seg, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(seg, kSheetBorderW, 0);
        lv_obj_set_style_border_color(seg, lv_color_black(), 0);
        lv_obj_set_style_radius(seg, seg_h / 2, 0);
        lv_obj_set_style_pad_all(seg, kSegPad, 0);
        lv_obj_set_style_pad_column(seg, 2, 0);
        lv_obj_set_flex_flow(seg, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(seg, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_clear_flag(seg, LV_OBJ_FLAG_CLICKABLE);
        DisableScroll(seg);
        const int ul = BookReaderPrefsUnderlineMode();
        auto make_ul = [&](const char* txt, int mode) {
            const bool selected = (ul == mode);
            lv_obj_t* b = lv_obj_create(seg);
            lv_obj_remove_style_all(b);
            lv_obj_set_size(b, kUlBtnW, kSegBtnH);
            lv_obj_set_style_radius(b, kSegBtnH / 2, 0);
            lv_obj_set_style_bg_color(b, selected ? lv_color_black() : lv_color_white(), 0);
            lv_obj_set_style_bg_opa(b, selected ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
            lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
            HapticAttachClick(b);
            lv_obj_add_event_cb(b, OnSheetUnderlineMode, LV_EVENT_CLICKED,
                                reinterpret_cast<void*>(static_cast<intptr_t>(mode)));
            lv_obj_add_event_cb(
                b, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED, nullptr);
            lv_obj_t* l = lv_label_create(b);
            lv_label_set_text(l, txt);
            lv_obj_set_style_text_font(l, ItemFont(), 0);
            lv_obj_set_style_text_color(l, selected ? lv_color_white() : lv_color_black(), 0);
            lv_obj_center(l);
            lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
        };
        make_ul(Lang::Strings::BOOK_UNDERLINE_SOLID, kBookReaderUnderlineSolid);
        make_ul(Lang::Strings::BOOK_UNDERLINE_DASHED, kBookReaderUnderlineDashed);
        make_ul(Lang::Strings::COMMON_OFF, kBookReaderUnderlineOff);
    }

    lv_obj_t* bar = lv_obj_create(st.settings_sheet);
    lv_obj_remove_style_all(bar);
    lv_obj_set_width(bar, lv_pct(100));
    lv_obj_set_height(bar, 40);
    lv_obj_set_style_pad_top(bar, 6, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(bar, kSheetBorderW, 0);
    lv_obj_set_style_border_color(bar, lv_color_black(), 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    DisableScroll(bar);

    lv_obj_t* toc_btn = lv_obj_create(bar);
    lv_obj_remove_style_all(toc_btn);
    lv_obj_set_size(toc_btn, LV_SIZE_CONTENT, 36);
    lv_obj_set_style_pad_hor(toc_btn, 4, 0);
    lv_obj_set_flex_flow(toc_btn, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(toc_btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(toc_btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(toc_btn);
    lv_obj_add_event_cb(toc_btn, OnSheetTocClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(
        toc_btn, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED, nullptr);
    DisableScroll(toc_btn);

    lv_obj_t* toc_text = lv_obj_create(toc_btn);
    lv_obj_remove_style_all(toc_text);
    lv_obj_set_size(toc_text, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_border_side(toc_text, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(toc_text, 1, 0);
    lv_obj_set_style_border_color(toc_text, lv_color_black(), 0);
    lv_obj_set_style_pad_bottom(toc_text, 2, 0);
    lv_obj_set_style_pad_hor(toc_text, 5, 0);
    lv_obj_set_style_bg_opa(toc_text, LV_OPA_TRANSP, 0);
    DisableScroll(toc_text);
    lv_obj_clear_flag(toc_text, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* toc_lbl = lv_label_create(toc_text);
    lv_label_set_text(toc_lbl, Lang::Strings::BOOK_TOC);
    lv_obj_set_style_text_font(toc_lbl, ItemFont(), 0);
    lv_obj_set_style_text_color(toc_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(toc_lbl, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_update_layout(st.settings_sheet);
    if (st.ttf_mode != BookUiState::TtfMode::kNone) {
        TtfEnsurePanel();
    }
}

}  // namespace book_screen_detail
