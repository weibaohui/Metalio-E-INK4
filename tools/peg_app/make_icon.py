#!/usr/bin/env python3
"""生成孔明棋应用图标（70x70 RGBA，黑形 + alpha 抗锯齿遮罩）。

图形：十字形盘面上的若干实心棋子（黑点）+ 一个空心圈（洞）。
"""
import struct
import zlib
from pathlib import Path

W = H = 70
SS = 4

# 7x7 盘面（英文十字）
MAP = [
    0, 0, 1, 1, 1, 0, 0,
    0, 0, 1, 1, 1, 0, 0,
    1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1,
    0, 0, 1, 1, 1, 0, 0,
    0, 0, 1, 1, 1, 0, 0,
]
# 有棋子的格子（中心是空洞）
PEG = {(1, 2), (2, 2), (4, 1), (4, 3), (3, 0), (2, 4), (5, 4), (1, 5), (3, 5), (4, 5)}

GX, GY = 12, 8
GS = 46
CELL = GS / 7
R = 4.2


def covered(x, y):
    c = int((x - GX) // CELL)
    r = int((y - GY) // CELL)
    if not (0 <= r < 7 and 0 <= c < 7) or not MAP[r * 7 + c]:
        return False
    cx = GX + (c + 0.5) * CELL
    cy = GY + (r + 0.5) * CELL
    d = ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5
    if (c, r) in PEG:
        return d <= R
    # 洞：空心圈
    return abs(d - R) <= 1.0


def render():
    rows = []
    for y in range(H):
        row = bytearray()
        for x in range(W):
            cnt = 0
            for sy in range(SS):
                for sx in range(SS):
                    px = x + (sx + 0.5) / SS
                    py = y + (sy + 0.5) / SS
                    if covered(px, py):
                        cnt += 1
            a = round(cnt * 255 / (SS * SS))
            row += bytes((0, 0, 0, a))
        rows.append(bytes(row))
    return rows


def write_png(path, rows):
    raw = b"".join(b"\x00" + r for r in rows)

    def chunk(t, d):
        c = t + d
        return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    Path(path).write_bytes(png)
    return len(png)


if __name__ == "__main__":
    out = Path(__file__).with_name("icon.png")
    n = write_png(out, render())
    print(f"生成 {out} ({n} bytes)")
