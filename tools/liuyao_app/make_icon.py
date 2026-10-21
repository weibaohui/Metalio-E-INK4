#!/usr/bin/env python3
"""生成六爻占卜应用图标（70x70 RGBA，黑形 + alpha 抗锯齿遮罩）。

与 tools/ntbk_app/make_icon.py 同规格：RGBA，形状为黑(0,0,0)，
alpha 承载 4x 超采样的覆盖率，背景全透明。
图形：六爻卦象（下三爻上三爻，实线为阳、断线为阴），中间留白分隔上下卦。
"""
import struct
import zlib
from pathlib import Path

W = H = 70
SS = 4

X0, X1 = 12, 58          # 卦象左右边界
LINE_H = 4.0             # 爻线粗
ROW_H = 8.0              # 爻间距
GAP = 3.0                # 断线中间的缝
Y0 = 14.0                # 第一爻（最上一爻）顶边

# 自下而上：阳、阴、阳 | 阴、阳、阴
YAO = [1, 0, 1, 0, 1, 0]  # 1=实线(阳) 0=断线(阴)


def covered(x, y):
    for i, yang in enumerate(YAO):
        # i=0 是最下一爻
        row = 5 - i
        top = Y0 + row * ROW_H
        bot = top + LINE_H
        if not (top <= y < bot):
            continue
        if yang:
            if X0 <= x < X1:
                return True
        else:
            mid = (X0 + X1) / 2
            if X0 <= x < mid - GAP / 2 or mid + GAP / 2 <= x < X1:
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
