#!/usr/bin/env python3
"""生成电子工牌应用图标（70x70 RGBA，黑形 + alpha 抗锯齿遮罩）。

图形：一张带挂绳孔的工牌卡，卡面上有头像圆圈和两行文字条。
"""
import struct
import zlib
from pathlib import Path

W = H = 70
SS = 4

# 工牌卡
CX0, CY0, CW, CH = 14, 12, 42, 52
R = 6
# 挂绳孔
HOLE_X, HOLE_Y, HOLE_R = 35, 20, 4


def rrect(x, y, x0, y0, w, h, r):
    if x < x0 or x > x0 + w or y < y0 or y > y0 + h:
        return False
    cx = min(max(x, x0 + r), x0 + w - r)
    cy = min(max(y, y0 + r), y0 + h - r)
    return ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5 <= r


def covered(x, y):
    # 卡框（描边）
    if rrect(x, y, CX0, CY0, CW, CH, R):
        inner = rrect(x, y, CX0 + 4, CY0 + 4, CW - 8, CH - 8, max(R - 4, 1))
        if not inner:
            return True
    # 挂绳孔（空心圆环）
    d = ((x - HOLE_X) ** 2 + (y - HOLE_Y) ** 2) ** 0.5
    if abs(d - HOLE_R) <= 1.2:
        return True
    # 头像圆圈
    d2 = ((x - 29) ** 2 + (y - 36) ** 2) ** 0.5
    if abs(d2 - 9) <= 1.6:
        return True
    # 文字条
    if 40 <= x <= 52 and 30 <= y <= 33:
        return True
    if 40 <= x <= 50 and 38 <= y <= 41:
        return True
    if 40 <= x <= 52 and 46 <= y <= 49:
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
