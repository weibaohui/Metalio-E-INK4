#!/usr/bin/env python3
"""生成纸牌接龙应用图标（70x70 RGBA，黑形 + alpha 抗锯齿遮罩）。

与 tools/sudoku_app/make_icon.py 同规格：RGBA，形状为黑(0,0,0)，
alpha 承载 4x 超采样的覆盖率，背景全透明。
图形：三张错位叠放的扑克牌 + 牌面上的黑桃点与方块点。
"""
import struct
import zlib
from pathlib import Path

W = H = 70
SS = 4


def rrect(x, y, x0, y0, w, h, r):
    """圆角矩形内部判定。"""
    if x < x0 or x > x0 + w or y < y0 or y > y0 + h:
        return False
    cx = min(max(x, x0 + r), x0 + w - r)
    cy = min(max(y, y0 + r), y0 + h - r)
    return ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5 <= r


def rrect_border(x, y, x0, y0, w, h, r, t):
    if not rrect(x, y, x0, y0, w, h, r):
        return False
    inner = rrect(x, y, x0 + t, y0 + t, w - 2 * t, h - 2 * t, max(r - t, 0.5))
    return not inner


def disc(x, y, cx, cy, r):
    return ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5 <= r


def diamond(x, y, cx, cy, r):
    """菱形（方块点）：|dx|+|dy| <= r"""
    return abs(x - cx) + abs(y - cy) <= r


# 三张牌：左上、中、右下错位
CARDS = [
    (10, 6, 26, 36, 5),    # x0, y0, w, h, 圆角
    (22, 16, 26, 36, 5),
    (34, 26, 26, 36, 5),
]


def covered(x, y):
    # 后面的牌先画，前面的牌盖在上面（所以从最后一张往前判）
    for (x0, y0, w, h, r) in reversed(CARDS):
        if rrect_border(x, y, x0, y0, w, h, r, 2.5):
            return True
    # 最前面那张牌上的两个花色点：一个圆(黑桃位) 一个菱形(方块位)
    x0, y0, w, h, r = CARDS[-1]
    if rrect(x, y, x0, y0, w, h, r):
        if disc(x, y, x0 + 8, y0 + 10, 4.2):
            return True
        if diamond(x, y, x0 + 18, y0 + 26, 5.0):
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
