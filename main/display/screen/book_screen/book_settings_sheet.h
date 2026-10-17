#pragma once

#include "book_font_manager.h"
#include "reader/book_session.h"
#include <lvgl.h>
#include <string>

namespace book_screen_detail {

lv_obj_t* CreateSettingsSheet(lv_obj_t* parent);
void ShowReadSettingsSheet();
void SetReadSettingsSheetVisible(bool visible);
void CloseReadSettingsSheet();
void RebuildSettingsSheet();
void RefreshSettingsSheet();
void RefreshSettingsSheetAdj();
void RefreshSettingsSheetFontList();
void ClearSettingsSheetWidgetRefs();
bool SettingsSheetWidgetsReady();
void EnsureSettingsSheetAfterLayout();
void RequestSettingsSheetPaint();
void ResetSettingsSheetPaint();
void ExitFontMultiMode(bool paint);

void ScanReadFonts();
int CurrentFontIndex();
int FontListPageCount();
void ClampFontListPage();
void EnsureFontListPageShowsSelection();
void FormatFontListName(const std::string& file, char* out, size_t out_len);

void SetSheetBtnEnabled(lv_obj_t* btn, bool enabled);
void UpdateSheetBoundTip(lv_obj_t* tip, bool can_dec, bool can_inc);

// ---- hooks：由 book_screen.cc 注册，解耦排版与正文重绘 ----
using SettingsScheduleLayoutApplyFn = void (*)(bool reload_font);
using SettingsApplyReaderLayoutLiveFn = void (*)(bool reload_font);
using SettingsFlushLayoutDebounceFn = void (*)();
using SettingsRenderReaderPageFn = void (*)();
using SettingsSetStatusVisibleFn = void (*)(bool visible);
using SettingsBindSessionLayoutPrefsFn = void (*)(reader::BookSession& session);

void SetSettingsScheduleLayoutApplyHandler(SettingsScheduleLayoutApplyFn fn);
void SetSettingsApplyReaderLayoutLiveHandler(SettingsApplyReaderLayoutLiveFn fn);
void SetSettingsFlushLayoutDebounceHandler(SettingsFlushLayoutDebounceFn fn);
void SetSettingsRenderReaderPageHandler(SettingsRenderReaderPageFn fn);
void SetSettingsSetStatusVisibleHandler(SettingsSetStatusVisibleFn fn);
void SetSettingsBindSessionLayoutPrefsHandler(SettingsBindSessionLayoutPrefsFn fn);

}  // namespace book_screen_detail
