#pragma once

#include "lvgl.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Flash fontpack → LVGL 字体桥。
 * 需先烧录 fonts.fontpack 到 font_data 分区（含 25@2 Regular 等）。
 * 失败时返回 nullptr。
 *
 * **只返回含汉字字形的字体。** fontpack 里 18@2 / 28@2 / 36@2 是纯数学符号包
 * （各 333 字，零汉字），若直接返回，用它们渲染的中文会全部变成空白、只剩
 * ASCII 露出来。因此本函数在目标规格没有汉字时回退到全局 UI 字体 25@2，
 * 与「该字号不存在」的既有处理保持一致。
 */
const lv_font_t* fontpack_lv_font_get(uint16_t size, uint16_t bpp);

/**
 * 取纯拉丁字母 / 数字 / 数学符号的小字体（fontpack 内 18@2 / 28@2 / 36@2）。
 * 这三个规格**不含汉字**，用于「需要小号字但内容是纯 ASCII」的场景
 * （分数数字、计时、步数等）；需要显示中文时请用 fontpack_lv_font_get()。
 * 规格不在包内时返回 nullptr。
 */
const lv_font_t* fontpack_lv_font_latin(uint16_t size);

/**
 * 全局 UI 默认字：fontpack **25@2**（MiSans-Mixed 中为 Regular）。
 * 可重复调用；首次成功后缓存。失败返回 nullptr。
 */
const lv_font_t* fontpack_lv_font_ui(void);

/** 若 font 由 fontpack_lv_font_get 创建，返回其 bpp；否则返回 0。 */
uint16_t fontpack_lv_font_bpp(const lv_font_t* font);

/** 若尚未 mmap，尝试 font_loader_init_flash；可重复调用。 */
bool fontpack_lv_ensure_ready(void);

#ifdef __cplusplus
}
#endif
