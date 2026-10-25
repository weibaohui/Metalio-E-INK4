import re
import struct

with open('tools/parking_app/main/font_misans_regular_160_2.c', 'r') as f:
    text = f.read()

# 提取 glyph_bitmap
bm_match = re.search(r'glyph_bitmap\[\]\s*=\s*\{([^}]+)\};', text, re.DOTALL)
hex_vals = re.findall(r'0x[0-9a-fA-F]+', bm_match.group(1))
bitmap = bytes([int(x, 16) for x in hex_vals])

# 提取 glyph_dsc
# {.bitmap_index = 0, .adv_w = 1623, .box_w = 83, .box_h = 124, .ofs_x = 9, .ofs_y = -3},
dsc_match = re.findall(r'\{\.bitmap_index\s*=\s*(\d+),\s*\.adv_w\s*=\s*(\d+),\s*\.box_w\s*=\s*(\d+),\s*\.box_h\s*=\s*(\d+),\s*\.ofs_x\s*=\s*(-?\d+),\s*\.ofs_y\s*=\s*(-?\d+)\}', text)
glyphs = []
for d in dsc_match:
    glyphs.append({
        'bitmap_index': int(d[0]),
        'adv_w': int(d[1]),
        'box_w': int(d[2]),
        'box_h': int(d[3]),
        'ofs_x': int(d[4]),
        'ofs_y': int(d[5]),
    })

# 480 x 800 画布 (1bpp: 60 bytes per row, 800 rows)
# 1 = 白, 0 = 黑
W, H = 480, 800
fb = bytearray([0xFF] * (W * H)) # 先用 1 字节表示 1 像素 (255白, 0黑)

def draw_glyph(gx, gy, ch):
    if not ('0' <= ch <= '9'):
        return
    gid = ord(ch) - ord('0') + 1
    g = glyphs[gid]
    box_w = g['box_w']
    box_h = g['box_h']
    ofs_x = g['ofs_x']
    ofs_y = g['ofs_y']
    b_idx = g['bitmap_index']
    
    # baseline 设为 124
    start_x = gx + ofs_x
    start_y = gy + (124 - box_h - ofs_y)
    
    bit_pos = 0
    for r in range(box_h):
        for c in range(box_w):
            byte_off = b_idx + (bit_pos >> 2)
            shift = 6 - ((bit_pos & 3) << 1)
            val = (bitmap[byte_off] >> shift) & 0x03
            bit_pos += 1
            if val > 0:
                px = start_x + c
                py = start_y + r
                if 0 <= px < W and 0 <= py < H:
                    fb[py * W + px] = 0

def draw_line(y_center, text_str):
    # 计算总宽度
    total_w = 0
    for ch in text_str:
        if '0' <= ch <= '9':
            gid = ord(ch) - ord('0') + 1
            total_w += glyphs[gid]['adv_w'] // 16
    start_x = (W - total_w) // 2
    cur_x = start_x
    y_top = y_center - 70
    for ch in text_str:
        if '0' <= ch <= '9':
            gid = ord(ch) - ord('0') + 1
            draw_glyph(cur_x, y_top, ch)
            cur_x += glyphs[gid]['adv_w'] // 16

# 绘制三行大字
draw_line(160, "138")
draw_line(380, "0000")
draw_line(600, "0000")

# 保存为 1bpp BMP
row_stride = (W + 7) // 8
pixel_bytes = row_stride * H
bmp_size = 14 + 40 + 8 + pixel_bytes

# Header
header = bytearray(62)
header[0:2] = b'BM'
struct.pack_into('<I', header, 2, bmp_size)
struct.pack_into('<I', header, 10, 62)
# DIB Header
struct.pack_into('<I', header, 14, 40)
struct.pack_into('<i', header, 18, W)
struct.pack_into('<i', header, 22, -H) # top-down
struct.pack_into('<H', header, 26, 1) # planes
struct.pack_into('<H', header, 28, 1) # 1bpp
struct.pack_into('<I', header, 34, pixel_bytes)
struct.pack_into('<i', header, 38, 2835)
struct.pack_into('<i', header, 42, 2835)
struct.pack_into('<I', header, 46, 2)
struct.pack_into('<I', header, 50, 2)
# Palette (0=Black, 1=White)
header[54:58] = b'\x00\x00\x00\x00'
header[58:62] = b'\xff\xff\xff\x00'

raw_1bpp = bytearray(pixel_bytes)
for y in range(H):
    for x in range(W):
        if fb[y * W + x] != 0: # 白
            raw_1bpp[y * row_stride + (x >> 3)] |= (1 << (7 - (x & 7)))

with open('/tmp/test_wallpaper.bmp', 'wb') as f:
    f.write(header)
    f.write(raw_1bpp)

print("BMP generated successfully at /tmp/test_wallpaper.bmp")
