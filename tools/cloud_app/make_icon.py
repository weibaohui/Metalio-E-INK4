#!/usr/bin/env python3
"""生成云工具应用图标（70x70 RGBA，黑形 + alpha 抗锯齿遮罩）。"""
import struct
import zlib
from pathlib import Path

W = H = 70
SS = 4


def disc(x, y, cx, cy, r):
    return ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5 <= r


def seg(x, y, x0, y0, x1, y1, w):
    dx, dy = x1 - x0, y1 - y0
    L2 = dx * dx + dy * dy
    t = 0.0 if L2 == 0 else max(0.0, min(1.0, ((x - x0) * dx + (y - y0) * dy) / L2))
    px, py = x0 + t * dx, y0 + t * dy
    return ((x - px) ** 2 + (y - py) ** 2) ** 0.5 <= w / 2.0


def covered(x, y):
    # 云朵（三个圆 + 底边）
    if disc(x, y, 26, 38, 12): return True
    if disc(x, y, 38, 34, 15): return True
    if disc(x, y, 50, 40, 10): return True
    if seg(x, y, 20, 46, 52, 46, 6): return True
    # 声波（右下角三道弧线）
    for r in (8, 14, 20):
        if abs(((x - 34) ** 2 + (y - 62) ** 2) ** 0.5 - r) <= 1.4 and x > 34 and y > 58:
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
