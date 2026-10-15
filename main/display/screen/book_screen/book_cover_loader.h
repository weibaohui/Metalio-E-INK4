#pragma once

#include "reader/reader.h"

namespace book_ui {

// 封面统一策略（快显 + 槽位尺寸正确）：
// - 同步：旁路 .a2i1 秒开（够大则定稿；偏小也可先预览）
// - 异步：旁路不够当前槽 → 权威档抽内嵌升级旁路，再缩到槽位

/** 仅旁路且尺寸足够；不 Open 文档。 */
bool TryLoadBookDetailSidecar(const reader::BookInfo& info, int max_w, int max_h,
                              reader::RasterImage& out);

/**
 * @brief 有旁路即读出缩放到槽（可不盖满）；用于详情先出图再后台升级
 */
bool TryLoadBookSidecarPreview(const reader::BookInfo& info, int max_w, int max_h,
                               reader::RasterImage& out);

/** 旁路优先，失败再读文件内封面并回写旁路（可能 Open EPUB/EBOOK；须在 worker）。 */
bool LoadBookDetailCover(reader::BookInfo& info, int max_w, int max_h, reader::RasterImage& out);

constexpr lv_coord_t kCoverFrameBorder = 1;
constexpr lv_coord_t kCoverSpineInset = 6;

/** 扣除双边描边后的可绘区，避免封面 L8 盖住外框 */
inline int CoverFrameInner(lv_coord_t outer) {
    const lv_coord_t inset = 2 * kCoverFrameBorder;
    return static_cast<int>(outer > inset ? outer - inset : 1);
}

/** 封面外框：左直脊、右圆角（子控件绘完后再切角描边） */
void StyleBookCoverFrame(lv_obj_t* obj);

/** 绘制书册边框与书脊 */
void DrawBookCoverBorder(lv_layer_t* layer, const lv_area_t& box, lv_coord_t r);

}  // namespace book_ui
