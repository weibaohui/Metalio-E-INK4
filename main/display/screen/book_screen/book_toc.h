#pragma once

#include "reader/reader.h"

#include <string>

#include <lvgl.h>

namespace book_screen_detail {

void OpenReadToc();
void RenderTocList();
void RequestRenderTocList();
/** 离开阅读会话时作废目录 coalesce 序号 */
void ResetTocPaint();

// ---- hooks：由 book_screen.cc 注册，避免与正文/封面队列循环依赖 ----
using TocFlushLayoutFn = void (*)();
using TocPrepareEnterFn = void (*)();
using TocCancelListCoverFn = void (*)();
using TocScheduleListCoverFn = void (*)();
using TocEnqueueListCoverFn = void (*)(const reader::BookInfo& info, int max_w, int max_h,
                                       lv_obj_t* host, reader::RasterImage* slot);
using TocSetStatusVisibleFn = void (*)(bool visible);
using TocSetStatusTitleFn = void (*)(const char* title);
using TocApplyOverlayPadsFn = void (*)(lv_coord_t top_pad);
using TocDetachBodyInputFn = void (*)(lv_obj_t* obj);
using TocTruncateFn = std::string (*)(const std::string& text, const lv_font_t* font,
                                      lv_coord_t max_w);
using TocUpdateFooterFn = void (*)();
using TocResetLongPressFn = void (*)();
using TocRenderReaderPageFn = void (*)();

void SetTocFlushLayoutHandler(TocFlushLayoutFn fn);
void SetTocPrepareEnterHandler(TocPrepareEnterFn fn);
void SetTocCancelListCoverHandler(TocCancelListCoverFn fn);
void SetTocScheduleListCoverHandler(TocScheduleListCoverFn fn);
void SetTocEnqueueListCoverHandler(TocEnqueueListCoverFn fn);
void SetTocSetStatusVisibleHandler(TocSetStatusVisibleFn fn);
void SetTocSetStatusTitleHandler(TocSetStatusTitleFn fn);
void SetTocApplyOverlayPadsHandler(TocApplyOverlayPadsFn fn);
void SetTocDetachBodyInputHandler(TocDetachBodyInputFn fn);
void SetTocTruncateHandler(TocTruncateFn fn);
void SetTocUpdateFooterHandler(TocUpdateFooterFn fn);
void SetTocResetLongPressHandler(TocResetLongPressFn fn);
void SetTocRenderReaderPageHandler(TocRenderReaderPageFn fn);

}  // namespace book_screen_detail
