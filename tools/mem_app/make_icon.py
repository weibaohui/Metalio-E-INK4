#!/usr/bin/env python3
"""生成记忆翻牌应用图标（70x70 RGBA，黑形 + alpha 抗锯齿遮罩）。

与 tools/sudoku_app/make_icon.py 同规格：RGBA，形状为黑(0,0,0)，
alpha 承载 4x 超采样的覆盖率，背景全透明。
图形：2x2 四张牌 —— 三张是黑牌背，右下那张翻开了（黑边白心 + 中心黑桃点）。
"""
import struct
import zlib
from pathlib import Path

W = H = 70
SS = 4

CARDS = [(8, 8, 26, 36), (36, 8, 26, 36), (8, 40, 26, 36), (36, 40, 26, 36)]
R = 4


def rrect(x, y, x0, y0, w, h, r):
    if x < x0 or x > x0 + w or y < y0 or y > y0 + h:
        return False
    cx = min(max(x, x0 + r), x0 + w - r)
    cy = min(max(y, y0 + r), y0 + h - r)
    return ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5 <= r


def rrect_border(x, y, x0, y0, w, h, r, t):
    if not rrect(x, y, x0, y0, w, h, r):
        return False
    return not rrect(x, y, x0 + t, y0 + t, w - 2 * t, h - 2 * t, max(r - t, 0.5))


def disc(x, y, cx, cy, r):
    return ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5 <= r


def covered(x, y):
    # 三张牌背：实心黑
    for (x0, y0, w, h) in CARDS[:3]:
        if rrect(x, y, x0, y0, w, h, R):
            return True
    # 翻开的那张：黑边框 + 中心一个黑点（牌面符号）
    x0, y0, w, h = CARDS[3]
    if rrect_border(x, y, x0, y0, w, h, R, 2.5):
        return True
    if rrect(x, y, x0, y0, w, h, R):
        if disc(x, y, x0 + w / 2, y0 + h / 2, 5.0):
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
