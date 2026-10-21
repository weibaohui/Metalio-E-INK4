#!/usr/bin/env python3
"""生成数织应用图标（70x70 RGBA，黑形 + alpha 抗锯齿遮罩）。

与 tools/sudoku_app/make_icon.py 同规格：RGBA，形状为黑(0,0,0)，
alpha 承载 4x 超采样的覆盖率，背景全透明。
图形：5x5 格网 + 一部分格子被涂黑（数织的典型画面）+ 左侧的提示数字点。
"""
import struct
import zlib
from pathlib import Path

W = H = 70
SS = 4

GX, GY = 22, 10
GS = 44          # 格网边长
N = 5            # 5x5
CELL = GS / N
# 被涂黑的格子
FILLED = {(0, 0), (1, 0), (2, 0), (1, 1), (0, 2), (1, 2), (2, 2), (3, 3), (4, 3)}


def covered(x, y):
    # 格网线
    if GX <= x <= GX + GS and GY <= y <= GY + GS:
        for i in range(N + 1):
            p = GX + i * CELL
            if abs(x - p) <= 0.9 or abs(y - p) <= 0.9:
                return True
    # 涂黑的格子
    c = int((x - GX) // CELL) if x >= GX else -1
    r = int((y - GY) // CELL) if y >= GY else -1
    if 0 <= r < N and 0 <= c < N and (c, r) in FILLED:
        return True
    # 左侧的提示数字（三条短横线，像 "3 2 1" 的提示）
    for i, y0 in enumerate((16, 26, 36)):
        if 6 <= x <= 18 and abs(y - y0) <= 1.4:
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
