#pragma once

#include <lvgl.h>

namespace book_screen_detail {

lv_obj_t* CreateDetailScreen(int book_index);
lv_obj_t* ResumeDetailScreen();
void ClearDetailCoverUiPtrs();
void CancelDetailCoverLoad();
void StartDetailCoverWorker(int book_index);
void OpenDetailAsync(void* user_data);
void BackToDetailAsync(void* user_data);

// ---- hooks：由主屏幕/阅读编排注册，解耦模块依赖 ----
using DetailStartReadFn = void (*)(int book_index);
using DetailCreateHomeScrFn = lv_obj_t* (*)();
using DetailAttachVkFn = void (*)(lv_obj_t* scr);
using DetailReleaseSessionFn = void (*)();

void SetDetailStartReadHandler(DetailStartReadFn fn);
void SetDetailCreateHomeScrHandler(DetailCreateHomeScrFn fn);
void SetDetailAttachVkHandler(DetailAttachVkFn fn);
void SetDetailReleaseSessionHandler(DetailReleaseSessionFn fn);

}  // namespace book_screen_detail
