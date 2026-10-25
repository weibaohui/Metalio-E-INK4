// tools/_shared/gray_dither.h —— 1bpp 墨水屏上的「灰」：Bayer 有序抖动（网点）
//
// 墨水屏物理上只有黑/白两色，**没有真灰阶**。"灰"只能靠**有序抖动**做出来：
// 按固定比例把黑点铺满一块区域，远看是一片灰。做法跟固件首页的卡片灰底完全一样
// （main/display/screen/home_screen/home_screen.cc 的 GrayDitherImg()）：
//   - 生成一张 8×8 的 LV_COLOR_FORMAT_L8 小图（0x00 = 黑点，0xFF = 白点）
//   - LVGL 用 bg_image_src + bg_image_tiled 把它平铺到目标对象上
//   - LVGL 把 L8 混到 I1（1bpp）上是**硬阈值**（src > 127 → 白，否则黑，
//     见 lv_draw_sw_blend_to_i1.c 的 l8_image_blend），所以网点是精确的黑白点阵
//
// 4×4 Bayer 矩阵，阈值 t → 黑点占比 t/16：
//   t=2 → 12.5%（很淡）   t=4 → 25%（浅灰）   t=8 → 50%（中灰）
//
// ⚠ 每个 Bayer 格放大成 2×2 像素块（GRAY_DITHER_SCALE）：墨水屏上 1px 网点太细，
//   远看容易"糊"成一片白；放大后灰度立得住，也不容易出现摩尔纹。
//
// 用法：把网点用在一整块**实心区域**上（牌背、墙砖、地砖）。
//   ⚠ 拿去描边/画细线没意义——1bpp 下细线的网点会被抖没，
//     网点底上的黑线会消失，要用**白**色才看得见。
//   ⚠ 网点相位以对象自己的左上角为原点（lv_draw_image_tiled_helper 从对象原点开始铺），
//     每块区域相位都不同。
#ifndef GRAY_DITHER_H
#define GRAY_DITHER_H

#include <stdint.h>

#include "lvgl.h"

#define GRAY_DITHER_SCALE 2
#define GRAY_DITHER_TILE_W (4 * GRAY_DITHER_SCALE)
#define GRAY_DITHER_TILE_H (4 * GRAY_DITHER_SCALE)

/* 三档灰，数字越大越深 */
#define GRAY_DITHER_LIGHT 0 /* 12.5% 黑，几乎只是"微微有点灰" */
#define GRAY_DITHER_MID 1   /* 25%   黑 → 浅灰 */
#define GRAY_DITHER_DARK 2  /* 50%   黑 → 中灰 */
#define GRAY_DITHER_LEVELS 3

static const uint8_t kGrayBayer4[4][4] = {
    {0, 8, 2, 10},
    {12, 4, 14, 6},
    {3, 11, 1, 9},
    {15, 7, 13, 5},
};

static const uint8_t kGrayDitherThreshold[GRAY_DITHER_LEVELS] = {2, 4, 8};

static uint8_t sGrayDitherBuf[GRAY_DITHER_LEVELS][GRAY_DITHER_TILE_W * GRAY_DITHER_TILE_H];
static lv_image_dsc_t sGrayDitherImg[GRAY_DITHER_LEVELS];
static int sGrayDitherReady = 0;

/* 取某档灰的点阵图（首次调用时生成，之后复用同一张静态图） */
static const lv_image_dsc_t *gray_dither_img(int level)
{
    if (!sGrayDitherReady) {
        for (int l = 0; l < GRAY_DITHER_LEVELS; ++l) {
            for (int y = 0; y < GRAY_DITHER_TILE_H; ++y) {
                for (int x = 0; x < GRAY_DITHER_TILE_W; ++x) {
                    const int by = y / GRAY_DITHER_SCALE;
                    const int bx = x / GRAY_DITHER_SCALE;
                    const int black = kGrayBayer4[by & 3][bx & 3] < kGrayDitherThreshold[l];
                    sGrayDitherBuf[l][y * GRAY_DITHER_TILE_W + x] = black ? 0x00 : 0xFF;
                }
            }
            sGrayDitherImg[l].header.magic = LV_IMAGE_HEADER_MAGIC;
            sGrayDitherImg[l].header.cf = LV_COLOR_FORMAT_L8;
            sGrayDitherImg[l].header.flags = 0;
            sGrayDitherImg[l].header.w = GRAY_DITHER_TILE_W;
            sGrayDitherImg[l].header.h = GRAY_DITHER_TILE_H;
            sGrayDitherImg[l].header.stride = GRAY_DITHER_TILE_W;
            sGrayDitherImg[l].data_size = sizeof(sGrayDitherBuf[l]);
            sGrayDitherImg[l].data = sGrayDitherBuf[l];
        }
        sGrayDitherReady = 1;
    }
    return &sGrayDitherImg[level];
}

/* 把对象底色换成某档网点灰 */
static void gray_dither_fill(lv_obj_t *obj, int level)
{
    /* bg_opa 要设成透明，否则实色底会把网点盖住；
     * bg_image_opa 默认就是 LV_OPA_COVER，不用（也没导出符号）单独设。 */
    lv_obj_set_style_bg_opa(obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_image_src(obj, gray_dither_img(level), 0);
    lv_obj_set_style_bg_image_tiled(obj, true, 0);
}

/* 撤掉网点，恢复成实色底 */
static void gray_solid_fill(lv_obj_t *obj, lv_color_t color)
{
    lv_obj_set_style_bg_image_src(obj, NULL, 0);
    lv_obj_set_style_bg_image_tiled(obj, false, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(obj, color, 0);
}

#endif /* GRAY_DITHER_H */
