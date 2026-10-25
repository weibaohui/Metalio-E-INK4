#!/usr/bin/env python3
"""生成挪车电话应用图标 (70x70 RGBA，抗锯齿黑形透明底)。
图形：外层圆角方框，中间一个醒目的车牌/电话听筒造型。
"""
import struct
import zlib
from pathlib import Path

W = H = 70
SS = 4

def rrect(x, y, x0, y0, w, h, r):
    if x < x0 or x > x0 + w or y < y0 or y > y0 + h:
        return False
    cx = min(max(x, x0 + r), x0 + w - r)
    cy = min(max(y, y0 + r), y0 + h - r)
    return ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5 <= r

def covered(x, y):
    # 外层大圆角矩形边框 (卡片外框)
    if rrect(x, y, 10, 10, 50, 50, 10):
        if not rrect(x, y, 14, 14, 42, 42, 7):
            return True
    
    # 顶部装饰横条
    if 20 <= x <= 50 and 18 <= y <= 21:
        return True

    # 居中显示大写字母 "P" (Parking) 带有电话感
    # P 的竖线
    if 24 <= x <= 29 and 26 <= y <= 48:
        return True
    # P 的上半圆环
    if rrect(x, y, 24, 26, 20, 15, 6):
        if not rrect(x, y, 29, 29, 11, 9, 3):
            return True

    return False

def make_png():
    rgba = bytearray()
    for y in range(H):
        for x in range(W):
            cnt = 0
            for sy in range(SS):
                for sx in range(SS):
                    px = x + (sx + 0.5) / SS
                    py = y + (sy + 0.5) / SS
                    if covered(px, py):
                        cnt += 1
            alpha = int(cnt * 255 / (SS * SS))
            rgba.extend([0, 0, 0, alpha])

    raw = bytearray()
    for y in range(H):
        raw.append(0)
        start = y * W * 4
        raw.extend(rgba[start:start + W * 4])

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)

    png = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", W, H, 8, 6, 0, 0, 0)
    png += chunk(b"IHDR", ihdr)
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    return png

if __name__ == "__main__":
    out = Path(__file__).parent / "icon.png"
    out.write_bytes(make_png())
    print(f"Generated {out}")
