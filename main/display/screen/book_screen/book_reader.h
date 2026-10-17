#pragma once

#include <cstdint>
#include <string>

#include <lvgl.h>

namespace book_screen_detail {

/** 正文翻页 / 页图 / 底栏进度；与 session 打开编排解耦 */
void ResetReadTouch();
void DetachReadBodyInput(lv_obj_t* obj);
void AttachReadBodyInput(lv_obj_t* obj);
bool QueueReaderPageTurn(int dir);
void ApplyReaderPageDelta();
void RequestRenderReaderPage();
void RenderReaderPage();
void UpdateReadFooter();
void ResetPointerLongPress();
void CancelPageImageLoad();
/** 离开阅读会话时作废 coalesce / 翻页积压 */
void ResetReaderPaint();
void ClearReaderPageDelta();
bool HasPendingReaderPageDelta();

// ---- hooks：由 book_screen.cc 注册，避免与设置卡/几何/延后清理循环依赖 ----
using ReaderHideChromeFn = void (*)();
using ReaderShowSettingsSheetFn = void (*)();
using ReaderSetStatusVisibleFn = void (*)(bool visible);
using ReaderSetSettingsSheetVisibleFn = void (*)(bool visible);
using ReaderApplyImmersiveGeometryFn = void (*)();
using ReaderApplyLayoutLiveFn = void (*)(bool reload_font);
using ReaderIsLayoutHintBusyFn = bool (*)();
using ReaderOnPageImageIdleFn = void (*)();
using ReaderTruncateFn = std::string (*)(const std::string& text, const lv_font_t* font,
                                         lv_coord_t max_w);

void SetReaderHideChromeHandler(ReaderHideChromeFn fn);
void SetReaderShowSettingsSheetHandler(ReaderShowSettingsSheetFn fn);
void SetReaderSetStatusVisibleHandler(ReaderSetStatusVisibleFn fn);
void SetReaderSetSettingsSheetVisibleHandler(ReaderSetSettingsSheetVisibleFn fn);
void SetReaderApplyImmersiveGeometryHandler(ReaderApplyImmersiveGeometryFn fn);
void SetReaderApplyLayoutLiveHandler(ReaderApplyLayoutLiveFn fn);
void SetReaderIsLayoutHintBusyHandler(ReaderIsLayoutHintBusyFn fn);
void SetReaderOnPageImageIdleHandler(ReaderOnPageImageIdleFn fn);
void SetReaderTruncateHandler(ReaderTruncateFn fn);

}  // namespace book_screen_detail
