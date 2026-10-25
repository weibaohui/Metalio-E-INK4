#!/usr/bin/env python3
"""生成 96px 纯数字 LVGL 字体 (MiSans-Bold)"""
import sys
from PIL import Image, ImageFont, ImageDraw

FONT_PATH = "tools/ttf-fonts/MiSans/MiSans-Bold.ttf"
CHARS = " 0123456789-"
FONT_SIZE = 96

font = ImageFont.truetype(FONT_PATH, FONT_SIZE)

# 计算通用 line height
ascent, descent = font.getmetrics()
line_height = ascent + descent

glyphs = []
bitmaps = bytearray()

for ch in CHARS:
    bbox = font.getbbox(ch)
    if ch == ' ':
        adv_w = 40
        w, h = 0, 0
        box_w, box_h = 0, 0
        ofs_x, ofs_y = 0, 0
        bitmap_index = len(bitmaps)
    else:
        # 渲染字符
        w = bbox[2] - bbox[0]
        h = bbox[3] - bbox[1]
        im = Image.new("1", (w + 4, h + 4), 0)
        draw = ImageDraw.Draw(im)
        draw.text((-bbox[0] + 2, -bbox[1] + 2), ch, font=font, fill=1)
        
        box_w = im.size[0]
        box_h = im.size[1]
        adv_w = bbox[2] + 4
        ofs_x = bbox[0] - 2
        ofs_y = ascent - bbox[3] - 2

        bitmap_index = len(bitmaps)
        # 将 1bpp 像素打包成字节
        row_stride = (box_w + 7) // 8
        for y in range(box_h):
            for byte_i in range(row_stride):
                b = 0
                for bit in range(8):
                    px = byte_i * 8 + bit
                    if px < box_w and im.getpixel((px, y)):
                        b |= (0x80 >> bit)
                bitmaps.append(b)

    glyphs.append({
        'ch': ch,
        'bitmap_index': bitmap_index,
        'adv_w': adv_w,
        'box_w': box_w,
        'box_h': box_h,
        'ofs_x': ofs_x,
        'ofs_y': ofs_y
    })

print(f"Generated {len(glyphs)} glyphs, total bitmap bytes: {len(bitmaps)}")

# 生成 C 代码
c_code = f"""// Generated 96px LVGL Font for Huge Parking Numbers
#include "lvgl.h"

static const uint8_t g_huge_font_bitmaps[] = {{
"""
for i, b in enumerate(bitmaps):
    c_code += f"0x{b:02x}, "
    if (i + 1) % 16 == 0:
        c_code += "\n"
c_code += "\n};\n\n"

c_code += "static const lv_font_fmt_txt_glyph_dsc_t g_huge_glyph_dsc[] = {\n"
c_code += "    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0},\n" # 占位 0
for g in glyphs:
    c_code += f"    {{.bitmap_index = {g['bitmap_index']}, .adv_w = {g['adv_w'] * 16}, .box_w = {g['box_w']}, .box_h = {g['box_h']}, .ofs_x = {g['ofs_x']}, .ofs_y = {g['ofs_y']}}},\n"
c_code += "};\n\n"

# unicode mapping
c_code += f"""static const lv_font_fmt_txt_cmap_t g_huge_cmaps[] = {{
    {{
        .range_start = 32,
        .range_length = 1,
        .glyph_id_start = 1,
        .unicode_list = NULL,
        .glyph_id_ofs_list = NULL,
        .list_length = 0,
        .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    }},
    {{
        .range_start = 45,
        .range_length = 1,
        .glyph_id_start = 12,
        .unicode_list = NULL,
        .glyph_id_ofs_list = NULL,
        .list_length = 0,
        .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    }},
    {{
        .range_start = 48,
        .range_length = 10,
        .glyph_id_start = 2,
        .unicode_list = NULL,
        .glyph_id_ofs_list = NULL,
        .list_length = 0,
        .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY
    }}
}};

static const lv_font_fmt_txt_dsc_t g_huge_font_dsc = {{
    .glyph_bitmap = g_huge_font_bitmaps,
    .glyph_dsc = g_huge_glyph_dsc,
    .cmaps = g_huge_cmaps,
    .cmap_num = 3,
    .bpp = 1,
    .kern_scale = 0,
    .kern_dsc = NULL,
    .kern_classes = 0,
    .bitmap_format = 0,
}};

const lv_font_t font_huge_number_96 = {{
    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,
    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,
    .line_height = {line_height},
    .base_line = {descent},
    .subpx = 0,
    .underline_position = -10,
    .underline_thickness = 4,
    .dsc = &g_huge_font_dsc,
    .fallback = NULL,
    .user_data = NULL
}};
"""

with open("tools/parking_app/main/font_huge_number_96.c", "w") as f:
    f.write(c_code)

print("Saved tools/parking_app/main/font_huge_number_96.c successfully!")
