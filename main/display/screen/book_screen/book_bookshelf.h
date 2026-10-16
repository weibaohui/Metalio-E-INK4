#pragma once

#include "reader/reader.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <lvgl.h>

namespace book_screen_detail {

constexpr const char* kScreenBookshelf = "book_shelf";

lv_obj_t* CreateBookshelfScreen();
void RenderBookshelfPage();
void RequestRenderBookshelfPage();
void RequestShelfCheckMarksPaint();
void ExitShelfMultiMode(bool rebuild);
int ListPageCount();
void ClampListPage();

/** 书架 / 最近阅读：封面格（long_press_multi=true 时支持长按多选） */
lv_obj_t* CreateBookCoverCell(lv_obj_t* parent, const reader::BookInfo& info, int index,
                              lv_coord_t cell_w, lv_coord_t cover_h,
                              std::vector<std::unique_ptr<reader::RasterImage>>& cover_store,
                              bool long_press_multi, lv_coord_t title_gap, bool show_title);

// ---- hooks：由 book_screen.cc 注册，避免与正文/封面队列循环依赖 ----
using BookshelfPrepareEnterFn = void (*)();
using BookshelfCancelListCoverFn = void (*)();
using BookshelfScheduleListCoverFn = void (*)();
using BookshelfEnqueueListCoverFn = void (*)(const reader::BookInfo& info, int max_w, int max_h,
                                             lv_obj_t* host, reader::RasterImage* slot);
using BookshelfCreateCoverSlotFn = lv_obj_t* (*)(lv_obj_t* parent, const reader::BookInfo& info,
                                                 lv_coord_t w, lv_coord_t h,
                                                 reader::RasterImage* cover_out,
                                                 bool* need_embed_fill);
using BookshelfOpenDetailFn = void (*)(int index);
using BookshelfTruncateFn = std::string (*)(const std::string& text, const lv_font_t* font,
                                            lv_coord_t max_w);
using BookshelfAttachVkFn = void (*)(lv_obj_t* scr);

void SetBookshelfPrepareEnterHandler(BookshelfPrepareEnterFn fn);
void SetBookshelfCancelListCoverHandler(BookshelfCancelListCoverFn fn);
void SetBookshelfScheduleListCoverHandler(BookshelfScheduleListCoverFn fn);
void SetBookshelfEnqueueListCoverHandler(BookshelfEnqueueListCoverFn fn);
void SetBookshelfCreateCoverSlotHandler(BookshelfCreateCoverSlotFn fn);
void SetBookshelfOpenDetailHandler(BookshelfOpenDetailFn fn);
void SetBookshelfTruncateHandler(BookshelfTruncateFn fn);
void SetBookshelfAttachVkHandler(BookshelfAttachVkFn fn);

}  // namespace book_screen_detail
