#!/usr/bin/env python3
"""生成五子棋应用图标（70x70 RGBA，黑形 + alpha 抗锯齿遮罩）。

与 tools/sudoku_app/make_icon.py 同规格：RGBA，形状为黑(0,0,0)，
alpha 承载 4x 超采样的覆盖率，背景全透明。
图形：圆角外框 + 3x3 格线 + 两颗黑子 + 一颗白子(空心圈)。
"""
import struct
import zlib
from pathlib import Path

W = H = 70
SS = 4  # 超采样倍数

M = 8          # 棋盘边距
R = 6          # 外框圆角
STROKE = 4     # 外框线宽
GRID = 3       # 3x3 格线
LINE = 3       # 格线宽
STONE_R = 8    # 棋子半径


def rrect_stroke(x, y, m, size, radius, width):
    x0, y0 = m, m
    x1, y1 = m + size, m + size
    half = width / 2.0
    cx = min(max(x, x0 + radius), x1 - radius)
    cy = min(max(y, y0 + radius), y1 - radius)
    d = ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5
    if d > radius + half:
        return False
    if x0 + half <= x <= x1 - half and y0 + half <= y <= y1 - half:
        return False
    return True


def seg_stroke(x, y, x0, y0, x1, y1, width):
    dx, dy = x1 - x0, y1 - y0
    L2 = dx * dx + dy * dy
    t = 0.0 if L2 == 0 else max(0.0, min(1.0, ((x - x0) * dx + (y - y0) * dy) / L2))
    px, py = x0 + t * dx, y0 + t * dy
    return ((x - px) ** 2 + (y - py) ** 2) ** 0.5 <= width / 2.0


def disc(x, y, cx, cy, r):
    return ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5 <= r


def ring(x, y, cx, cy, r, width):
    d = ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5
    return abs(d - r) <= width / 2.0


# 黑子（实心）与白子（空心圈）位置：格线交点上
BLACKS = [(0, 1), (1, 2)]
WHITES = [(2, 0)]


def covered(x, y):
    size = W - 2 * M
    if rrect_stroke(x, y, M, size, R, STROKE):
        return True
    step = size / GRID
    for i in range(1, GRID):
        p = M + i * step
        if seg_stroke(x, y, p, M, p, M + size, LINE):
            return True
        if seg_stroke(x, y, M, p, M + size, p, LINE):
            return True
    for (gr, gc) in BLACKS:
        cx = M + (gc + 0.5) * step
        cy = M + (gr + 0.5) * step
        if disc(x, y, cx, cy, STONE_R):
            return True
    for (gr, gc) in WHITES:
        cx = M + (gc + 0.5) * step
        cy = M + (gr + 0.5) * step
        if ring(x, y, cx, cy, STONE_R, 3):
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
