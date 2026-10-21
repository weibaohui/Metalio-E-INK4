#!/usr/bin/env python3
"""生成生命游戏应用图标（70x70 RGBA，黑形 + alpha 抗锯齿遮罩）。

与 tools/sudoku_app/make_icon.py 同规格：RGBA，形状为黑(0,0,0)，
alpha 承载 4x 超采样的覆盖率，背景全透明。
图形：6x6 网格 + 一个滑翔机图案（生命游戏最经典的图案）。
"""
import struct
import zlib
from pathlib import Path

W = H = 70
SS = 4

M = 8
CELL = 9          # 6 格 = 54
N = 6
GLIDER = {        # (col,row) 活格子
    (1, 0), (2, 1), (0, 2), (1, 2), (2, 2),
}


def covered(x, y):
    # 网格线
    gx = M + (x - M) % CELL
    gy = M + (y - M) % CELL
    if M <= x <= M + CELL * N and M <= y <= M + CELL * N:
        if abs(x - gx) <= 0.5 or abs(y - gy) <= 0.5:
            return True
    # 滑翔机的活格子（实心黑方块）
    for (c, r) in GLIDER:
        x0 = M + c * CELL
        y0 = M + r * CELL
        if x0 <= x <= x0 + CELL and y0 <= y <= y0 + CELL:
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
