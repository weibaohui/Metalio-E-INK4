#!/usr/bin/env python3
"""生成中国象棋应用图标（70x70 RGBA，黑形 + alpha 抗锯齿遮罩）。

与 tools/sudoku_app/make_icon.py 同规格：RGBA，形状为黑(0,0,0)，
alpha 承载 4x 超采样的覆盖率，背景全透明。
图形：圆角外框 + 中间横断的棋盘线（河界）+ 九宫斜线点 + 一黑一白两枚棋子。
"""
import struct
import zlib
from pathlib import Path

W = H = 70
SS = 4

M = 7
R = 6
STROKE = 4
COLS = 4      # 4 条竖线
ROWS = 5      # 5 条横线
LINE = 3
STONE_R = 9
DOT_R = 1.6


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


def covered(x, y):
    size = W - 2 * M
    if rrect_stroke(x, y, M, size, R, STROKE):
        return True
    step = size / COLS
    # 竖线（4 条）
    for i in range(COLS):
        p = M + i * step
        if seg_stroke(x, y, p, M, p, M + size, LINE):
            return True
    # 横线（5 条，中间那条断开留河界）
    hstep = size / (ROWS - 1)
    for i in range(ROWS):
        p = M + i * hstep
        if i == ROWS // 2:
            # 河界：左右两段
            if seg_stroke(x, y, M, p, M + size * 0.36, p, LINE):
                return True
            if seg_stroke(x, y, M + size * 0.64, p, M + size, p, LINE):
                return True
        else:
            if seg_stroke(x, y, M, p, M + size, p, LINE):
                return True
    # 九宫斜线（点串）
    for side in (0, 1):
        r0 = M if side == 0 else M + size * 0.5
        r1 = r0 + size * 0.5
        c0, c1 = M + size / 2 - step / 2, M + size / 2 + step / 2
        for i in range(9):
            t = i / 8.0
            px = c0 + (c1 - c0) * t
            py = r0 + (r1 - r0) * t
            if disc(x, y, px, py, DOT_R):
                return True
            px2 = c1 - (c1 - c0) * t
            if disc(x, y, px2, py, DOT_R):
                return True
    # 两枚棋子：一实心(黑)一空心(白)
    if disc(x, y, M + size * 0.28, M + size * 0.26, STONE_R):
        return True
    if ring(x, y, M + size * 0.72, M + size * 0.74, STONE_R, 3):
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
