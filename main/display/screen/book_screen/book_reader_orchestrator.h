#pragma once

#include "book_ui_state.h"
#include "reader/book_session.h"
#include <lvgl.h>

namespace book_screen_detail {

enum class ReaderGeomMode : uint8_t { kImmersive = 0, kOverlayList };

lv_obj_t* CreateReaderScreen(const reader::BookInfo& info);
lv_obj_t* ResumeReaderScreen();
void StartReadAsync(void* user_data);
void StartOpenWorker(int book_index);
void ShowOpenProgress(int percent);
bool ReaderWorkersBusy();
void ReleaseReaderSessionForLeave();
void FinishDeferredCleanup();

void ApplyReaderLayoutLive(bool reload_font);
void ScheduleLayoutApply(bool reload_font);
void FlushLayoutDebounce();
void CancelLayoutDebounce();
void StartLayoutWorker();
void StartChapterPageWorker();
void ReopenReaderAfterPrefsChange();

bool IsLayoutHintBusy();
void EnsureLayoutHintTimer();
void StopLayoutHintTimer();
void MarkLayoutHintBusy();
void MarkLayoutHintDone();

void BindSessionLayoutPrefs(reader::BookSession& session);
void ApplyReaderPageGeometry(ReaderGeomMode mode);
bool IsReadOverlayChrome(BookUiState::ReadChrome c);
lv_coord_t ReadContentWidth();
bool ReaderPageImagesHint();
void ApplyOverlayListContentPads(lv_coord_t top_pad);

void StopReadTimeCheckpointTimer();
void StartReadTimeCheckpointTimer();
void BeginReadingTimeTracking();
void EndReadingTimeTracking();
void PauseReadingTimeForStandby();
void ResumeReadingTimeAfterStandby();

void HideReadChrome();
void SetReadStatusVisible(bool visible);
void SetReadStatusCenterTitle(const char* title);
void ShowMessage(lv_obj_t* parent, const char* msg, const lv_font_t* font = nullptr);

// ---- hooks：由宿主或其它模块注入，保持解耦 ----
using ReaderCreateHomeScrFn = lv_obj_t* (*)();
using ReaderCreateDetailScrFn = lv_obj_t* (*)(int book_index);
using ReaderAttachVkFn = void (*)(lv_obj_t* scr);
using ReaderCancelListCoverFn = void (*)();

void SetReaderOrchestratorCreateHomeScrHandler(ReaderCreateHomeScrFn fn);
void SetReaderOrchestratorCreateDetailScrHandler(ReaderCreateDetailScrFn fn);
void SetReaderOrchestratorAttachVkHandler(ReaderAttachVkFn fn);
void SetReaderOrchestratorCancelListCoverHandler(ReaderCancelListCoverFn fn);

}  // namespace book_screen_detail
