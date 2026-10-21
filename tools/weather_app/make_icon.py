#!/usr/bin/env python3
"""生成天气应用图标（70x70 RGBA，黑形 + alpha 抗锯齿遮罩）。

与 tools/pomo_app/make_icon.py 同规格：RGBA，形状为黑(0,0,0)，
alpha 承载 4x 超采样的覆盖率，背景全透明。
图形：右上角一轮太阳（外圈是阳光射线），左下一朵云，云下三滴雨。
"""
import struct
import zlib
from pathlib import Path

W = H = 70
SS = 4  # 超采样倍数

SUN_CX, SUN_CY, SUN_R = 51, 18, 13
CLOUD = [  # (cx, cy, r)
    (19, 45, 11),
    (32, 39, 13),
    (44, 45, 10),
]
CLOUD_BASE = (9, 47, 46, 58)  # x0, y0, x1, y1
DROPS = [(15, 65, 3), (28, 68, 3), (41, 65, 3)]
RAYS = [  # (x0, y0, x1, y1, 半宽)
    (51, 1, 51, 4, 1.2),
    (36, 18, 39, 18, 1.2),
    (63, 18, 67, 18, 1.2),
    (42, 3, 45, 6, 1.2),
    (57, 3, 60, 6, 1.2),
    (62, 29, 65, 32, 1.2),
]


def disc(x, y, cx, cy, r):
    return ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5 <= r


def seg(x, y, x0, y0, x1, y1, w):
    dx, dy = x1 - x0, y1 - y0
    L2 = dx * dx + dy * dy
    t = 0.0 if L2 == 0 else max(0.0, min(1.0, ((x - x0) * dx + (y - y0) * dy) / L2))
    px, py = x0 + t * dx, y0 + t * dy
    return ((x - px) ** 2 + (y - py) ** 2) ** 0.5 <= w / 2.0


def rrect(x, y, x0, y0, x1, y1, r):
    if not (x0 <= x <= x1 and y0 <= y <= y1):
        return False
    # 四个角的圆角区
    for cx, cy in ((x0 + r, y0 + r), (x1 - r, y0 + r), (x0 + r, y1 - r), (x1 - r, y1 - r)):
        if (x < x0 + r or x > x1 - r) and (y < y0 + r or y > y1 - r):
            if disc(x, y, cx, cy, r):
                return True
    return True


def covered(x, y):
    for (x0, y0, x1, y1, w) in RAYS:
        if seg(x, y, x0, y0, x1, y1, w):
            return True
    if disc(x, y, SUN_CX, SUN_CY, SUN_R):
        return True
    for (cx, cy, r) in CLOUD:
        if disc(x, y, cx, cy, r):
            return True
    if rrect(x, y, *CLOUD_BASE, 6):
        return True
    for (cx, cy, r) in DROPS:
        if disc(x, y, cx, cy, r):
            return True
    return False


def build():
    rows = []
    for py in range(H):
        row = bytearray()
        for px in range(W):
            hits = 0
            for sy in range(SS):
                for sx in range(SS):
                    x = px + (sx + 0.5) / SS
                    y = py + (sy + 0.5) / SS
                    if covered(x, y):
                        hits += 1
            a = round(255 * hits / (SS * SS))
            row += bytes((0, 0, 0, a))
        rows.append(bytes(row))
    return b"".join(b"\x00" + r for r in rows)


def chunk(tag, data):
    return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)


def main():
    raw = build()
    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", W, H, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    out = Path(__file__).parent / "icon.png"
    out.write_bytes(png)
    print(f"wrote {out} ({len(png)} bytes)")


if __name__ == "__main__":
    main()
