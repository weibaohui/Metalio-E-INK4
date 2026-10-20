#!/usr/bin/env python3
"""生成扫雷应用图标（70x70 RGBA，黑形 + alpha 抗锯齿遮罩）。

与 tools/game2048_app/icon.png 同规格：RGBA，形状为黑(0,0,0)，
alpha 承载 4x 超采样的覆盖率，背景全透明。
图案：一面小旗 + 右下角一颗雷，一眼认出"扫雷"。
"""
import struct
import zlib
from pathlib import Path

W = H = 70
SS = 4

FLAG_X = 20          # 旗杆左边界
FLAG_TOP = 12
POLE_W = 4
POLE_BOT = 56
CLOTH_TOP = 14
CLOTH_BOT = 32
CLOTH_RIGHT = 46     # 旗面右端（斜边）


def in_pole(x, y):
    return FLAG_X <= x < FLAG_X + POLE_W and FLAG_TOP <= y < POLE_BOT


def in_cloth(x, y):
    """旗面：上边平、下边斜的三角旗。"""
    if not (FLAG_X + POLE_W <= x < CLOTH_RIGHT):
        return False
    if not (CLOTH_TOP <= y < CLOTH_BOT):
        return False
    # 斜边：从左上到右下，用 (x - x0)/(x1 - x0) 决定该列允许的 y 上限
    t = (x - (FLAG_X + POLE_W)) / float(CLOTH_RIGHT - FLAG_X - POLE_W)
    limit = CLOTH_TOP + t * (CLOTH_BOT - CLOTH_TOP)
    return y <= limit


def in_mine(x, y):
    """右下角一颗雷：中心方块 + 四向短刺。"""
    cx, cy = 50, 48
    half = 9
    if abs(x - cx) <= 5 and abs(y - cy) <= 5:
        return True
    if abs(x - cx) <= 1 and abs(y - cy) <= half:
        return True
    if abs(y - cy) <= 1 and abs(x - cx) <= half:
        return True
    return False


def covered(x, y):
    return in_pole(x, y) or in_cloth(x, y) or in_mine(x, y)


def render():
    rows = []
    for y in range(H):
        row = bytearray()
        for x in range(W):
            cnt = 0
            for sy in range(SS):
                for sx in range(SS):
                    if covered(x + (sx + 0.5) / SS, y + (sy + 0.5) / SS):
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
