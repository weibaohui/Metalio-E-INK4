#!/usr/bin/env python3
"""生成收款码应用图标（70x70 RGBA，黑形 + alpha 抗锯齿遮罩）。

与 tools/pomo_app/make_icon.py 同规格：RGBA，形状为黑(0,0,0)，
alpha 承载 4x 超采样的覆盖率，背景全透明。
图形：一个二维码块（三个定位角 + 若干数据点），中间压一个 ¥。
"""
import struct
import zlib
from pathlib import Path

W = H = 70
SS = 4

# 二维码外框
Q_X0, Q_Y0, Q_X1, Q_Y1 = 6, 6, 64, 64
# 三个定位角（左上/右上/左下）——外圈方框 + 内圈实心
FINDERS = [(10, 10), (44, 10), (10, 44)]
FINDER_OUT = 17  # 外框边长
FINDER_IN = 7   # 内芯边长
# 中间的数据点（避开 ¥ 字）
DOTS = [
    (28, 12), (34, 12), (40, 12), (28, 18), (34, 18),
    (28, 24), (34, 24), (28, 30), (40, 30),
    (12, 58), (18, 58), (24, 58), (30, 58), (36, 58),
    (42, 58), (48, 58), (54, 58), (12, 52), (18, 52),
    (24, 52), (30, 52), (36, 52), (42, 52), (48, 52), (54, 52),
    (48, 44), (54, 44), (48, 50), (54, 50), (44, 38), (50, 38),
]


def in_rect(x, y, x0, y0, x1, y1):
    return x0 <= x <= x1 and y0 <= y <= y1


def disc(x, y, cx, cy, r):
    return ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5 <= r


def seg(x, y, x0, y0, x1, y1, w):
    dx, dy = x1 - x0, y1 - y0
    L2 = dx * dx + dy * dy
    t = 0.0 if L2 == 0 else max(0.0, min(1.0, ((x - x0) * dx + (y - y0) * dy) / L2))
    px, py = x0 + t * dx, y0 + t * dy
    return ((x - px) ** 2 + (y - py) ** 2) ** 0.5 <= w / 2.0


def yuan(x, y):
    """¥ 符号：两条斜线 + 两横 + 一竖。"""
    cx = 35
    # 两条斜线（V 形）
    if seg(x, y, 26, 26, 34, 37, 3.2):
        return True
    if seg(x, y, 44, 26, 36, 37, 3.2):
        return True
    # 两横
    if seg(x, y, 27, 38, 43, 38, 3.0):
        return True
    if seg(x, y, 28, 44, 42, 44, 3.0):
        return True
    # 一竖
    if seg(x, y, cx, 37, cx, 52, 3.2):
        return True
    return False


def covered(x, y):
    # 外框（4px 粗）
    if in_rect(x, y, Q_X0, Q_Y0, Q_X1, Q_Y0 + 3):
        return True
    if in_rect(x, y, Q_X0, Q_Y1 - 3, Q_X1, Q_Y1):
        return True
    if in_rect(x, y, Q_X0, Q_Y0, Q_X0 + 3, Q_Y1):
        return True
    if in_rect(x, y, Q_X1 - 3, Q_Y0, Q_X1, Q_Y1):
        return True
    # 定位角
    for fx, fy in FINDERS:
        if in_rect(x, y, fx, fy, fx + FINDER_OUT - 1, fy + FINDER_OUT - 1):
            # 挖空中间一圈，只留外圈和内芯
            if in_rect(x, y, fx + 5, fy + 5, fx + FINDER_OUT - 6, fy + FINDER_OUT - 6):
                continue
            return True
    # 数据点
    for dx, dy in DOTS:
        if in_rect(x, y, dx, dy, dx + 4, dy + 4):
            return True
    # ¥
    if yuan(x, y):
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
