#!/usr/bin/env python3
"""生成番茄钟应用图标（70x70 RGBA，黑形 + alpha 抗锯齿遮罩）。

与 tools/sudoku_app/make_icon.py 同规格：RGBA，形状为黑(0,0,0)，
alpha 承载 4x 超采样的覆盖率，背景全透明。
图形：一颗番茄（圆）+ 顶上的一片叶子（锯齿形）。
"""
import struct
import zlib
from pathlib import Path

W = H = 70
SS = 4

CX, CY = 35, 42      # 番茄圆心
R = 21               # 番茄半径


def disc(x, y, cx, cy, r):
    return ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5 <= r


def seg(x, y, x0, y0, x1, y1, w):
    dx, dy = x1 - x0, y1 - y0
    L2 = dx * dx + dy * dy
    t = 0.0 if L2 == 0 else max(0.0, min(1.0, ((x - x0) * dx + (y - y0) * dy) / L2))
    px, py = x0 + t * dx, y0 + t * dy
    return ((x - px) ** 2 + (y - py) ** 2) ** 0.5 <= w / 2.0


def covered(x, y):
    # 番茄果身
    if disc(x, y, CX, CY, R):
        return True
    # 叶子：三片小三角（用两条斜线围出的楔形近似）
    top = CY - R
    if y <= top + 2 and y >= top - 9:
        # 中心竖茎
        if abs(x - CX) <= 1.6:
            return True
        # 左右两片叶子
        for sgn in (-1, 1):
            bx = CX + sgn * 3
            if seg(x, y, bx, top, bx + sgn * 10, top - 6, 3.0):
                return True
            if seg(x, y, bx + sgn * 10, top - 6, bx + sgn * 13, top + 1, 3.0):
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
