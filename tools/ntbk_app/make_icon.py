#!/usr/bin/env python3
"""生成夜市卜卦应用图标（70x70 RGBA，黑形 + alpha 抗锯齿遮罩）。

与 tools/sudoku_app/make_icon.py 同规格：RGBA，形状为黑(0,0,0)，
alpha 承载 4x 超采样的覆盖率，背景全透明。
图形：外方内圆的铜钱（方孔圆钱），下方一条摊台横线。
"""
import struct
import zlib
from pathlib import Path

W = H = 70
SS = 4

CX, CY = 35.0, 30.0
R = 20.0        # 钱币外圆
HOLE = 11.0      # 方孔半边长
BAR_Y0, BAR_Y1 = 55.0, 60.0


def disc(x, y, cx, cy, r):
    return ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5 <= r


def square(x, y, cx, cy, half):
    return abs(x - cx) <= half and abs(y - cy) <= half


def covered(x, y):
    # 钱币：圆环（挖掉方孔）
    if disc(x, y, CX, CY, R) and not square(x, y, CX, CY, HOLE):
        return True
    # 摊台横线
    if BAR_Y0 <= y <= BAR_Y1 and 12 <= x <= 58:
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
