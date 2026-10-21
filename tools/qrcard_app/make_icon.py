#!/usr/bin/env python3
"""生成二维码名片应用图标（70x70 RGBA，黑形 + alpha 抗锯齿遮罩）。"""
import struct
import zlib
from pathlib import Path

W = H = 70
SS = 4

# 7x7 的模块图案（1=黑），带三个定位角
MODS = [
    "1111101",
    "1000101",
    "1010101",
    "1000101",
    "1111101",
    "0000001",
    "1011110",
]
M = 7
GS = 56
CELL = GS / M
X0 = (W - GS) / 2
Y0 = (W - GS) / 2


def covered(x, y):
    c = int((x - X0) // CELL)
    r = int((y - Y0) // CELL)
    if 0 <= r < M and 0 <= c < M:
        if MODS[r][c] == "1":
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
