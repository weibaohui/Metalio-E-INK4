#!/usr/bin/env python3
"""生成数回应用图标（70x70 RGBA，黑形 + alpha 抗锯齿遮罩）。

图形：一条闭合的方框回路 + 回路上的几个提示小方块。
"""
import struct
import zlib
from pathlib import Path

W = H = 70
SS = 4

# 回路的顶点（格点坐标）
PATH = [(0, 0), (0, 1), (0, 2), (1, 2), (1, 3), (2, 3), (2, 2),
        (3, 2), (3, 1), (4, 1), (4, 0), (3, 0), (3, -1), (2, -1),
        (2, 0), (1, 0), (1, -1), (0, -1)]
S = 13.0
OX, OY = 12, 26
T = 4.0  # 线宽


def seg(x, y, x0, y0, x1, y1, w):
    dx, dy = x1 - x0, y1 - y0
    L2 = dx * dx + dy * dy
    t = 0.0 if L2 == 0 else max(0.0, min(1.0, ((x - x0) * dx + (y - y0) * dy) / L2))
    px, py = x0 + t * dx, y0 + t * dy
    return ((x - px) ** 2 + (y - py) ** 2) ** 0.5 <= w / 2.0


def covered(x, y):
    for i in range(len(PATH)):
        a = PATH[i]
        b = PATH[(i + 1) % len(PATH)]
        x0, y0 = OX + a[0] * S, OY + a[1] * S
        x1, y1 = OX + b[0] * S, OY + b[1] * S
        if seg(x, y, x0, y0, x1, y1, T):
            return True
    # 回路内外的提示小方块
    for (gx, gy) in ((1, 0), (2, 1), (1, 2)):
        cx, cy = OX + gx * S, OY + gy * S
        if abs(x - cx) <= 3 and abs(y - cy) <= 3:
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
