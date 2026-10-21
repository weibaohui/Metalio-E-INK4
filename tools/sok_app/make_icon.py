#!/usr/bin/env python3
"""生成推箱子应用图标（70x70 RGBA，黑形 + alpha 抗锯齿遮罩）。

与 tools/sudoku_app/make_icon.py 同规格：RGBA，形状为黑(0,0,0)，
alpha 承载 4x 超采样的覆盖率，背景全透明。
图形：一个人（实心圆）正在推一个箱子（空心方框），脚下是格子。
"""
import struct
import zlib
from pathlib import Path

W = H = 70
SS = 4

# 格子区域
GX, GY, GS = 8, 14, 54   # 格子区左上角与边长
CELL = 18               # 3x3 格


def rrect_border(x, y, x0, y0, w, h, t):
    if x < x0 or x > x0 + w or y < y0 or y > y0 + h:
        return False
    return not (x0 + t <= x <= x0 + w - t and y0 + t <= y <= y0 + h - t)


def disc(x, y, cx, cy, r):
    return ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5 <= r


def covered(x, y):
    # 格子线
    if GX <= x <= GX + GS and GY <= y <= GY + GS:
        step = GS / 3.0
        for i in range(4):
            p = GX + i * step
            if abs(x - p) <= 1.0 or abs(y - p) <= 1.0:
                return True
    # 箱子（空心方框）：中格
    bx, by = GX + CELL, GY + CELL
    if rrect_border(x, y, bx + 1, by + 1, CELL - 2, CELL - 2, 2.5):
        return True
    # 人（实心圆）：左格
    px, py = GX + CELL * 0.5, GY + CELL * 1.5
    if disc(x, y, px, py, 6.5):
        return True
    return False


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
