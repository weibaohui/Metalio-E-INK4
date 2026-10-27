#!/usr/bin/env python3
"""生成微信读书应用图标（70x70 RGBA，纯黑形 + alpha 抗锯齿，背景透明）。
图形：翻开的书本（Book / WeRead 象征）。
"""
import struct
import zlib
from pathlib import Path

W = H = 70
SS = 4

def covered(x, y):
    # 书本主体区域：居中 x in [12, 58], y in [18, 52]
    # 中轴书脊 x = 35
    if not (12 <= x <= 58 and 18 <= y <= 54):
        return False

    # 中轴空隙（书脊缝）
    if 34.0 <= x <= 36.0 and y >= 20:
        return False

    # 左半页
    if 14 <= x < 34:
        # 页面弯曲轮廓
        curve_top = 22 - (34 - x) * 0.15
        curve_bot = 50 - (34 - x) * 0.15
        if curve_top <= y <= curve_bot:
            # 镂空页边留白，形成书本线条感
            if (17 <= x <= 31) and (curve_top + 4 <= y <= curve_bot - 4):
                # 内部横条文字线条
                if 28 <= y <= 30 or 34 <= y <= 36 or 40 <= y <= 42:
                    return True
                return False
            return True

    # 右半页
    if 36 < x <= 56:
        curve_top = 22 - (x - 36) * 0.15
        curve_bot = 50 - (x - 36) * 0.15
        if curve_top <= y <= curve_bot:
            if (39 <= x <= 53) and (curve_top + 4 <= y <= curve_bot - 4):
                if 28 <= y <= 30 or 34 <= y <= 36 or 40 <= y <= 42:
                    return True
                return False
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
