#!/usr/bin/env python3
"""生成数独应用图标（70x70 RGBA，黑形 + alpha 抗锯齿遮罩）。

与 tools/game2048_app/icon.png 同规格：RGBA，形状为黑(0,0,0)，
alpha 承载 4x 超采样的覆盖率，背景全透明。
"""
import struct
import zlib
from pathlib import Path

W = H = 70
SS = 4  # 超采样倍数

# ---- 几何定义（70x70 逻辑坐标）----
M = 8          # 棋盘边距
R = 6          # 外框圆角
STROKE = 4    # 外框线宽
GRID = 3      # 3x3 宫格 —— 数独的标志性结构
LINE = 3      # 宫格分隔线宽
BOLD = 3      # 备用（本图标各线等宽）
FILLED = {(0, 1), (2, 0), (2, 2)}  # 填了数的格子


def rrect_stroke(x, y, m, size, radius, width):
    """圆角矩形描边的内部判定（点在笔画内）。"""
    x0, y0 = m, m
    x1, y1 = m + size, m + size
    half = width / 2.0
    # 先判是否落在矩形主体（含圆角）外扩半宽的范围内
    cx = min(max(x, x0 + radius), x1 - radius)
    cy = min(max(y, y0 + radius), y1 - radius)
    d = ((x - cx) ** 2 + (y - cy) ** 2) ** 0.5
    if d > radius + half:
        return False
    # 落在外框内 → 若是边框带内则 True
    if x0 + half <= x <= x1 - half and y0 + half <= y <= y1 - half:
        return False  # 内部是空的
    return True


def seg_stroke(x, y, x0, y0, x1, y1, width):
    """线段描边判定。"""
    dx, dy = x1 - x0, y1 - y0
    L2 = dx * dx + dy * dy
    t = 0.0 if L2 == 0 else max(0.0, min(1.0, ((x - x0) * dx + (y - y0) * dy) / L2))
    px, py = x0 + t * dx, y0 + t * dy
    return ((x - px) ** 2 + (y - py) ** 2) ** 0.5 <= width / 2.0


def covered(x, y):
    size = W - 2 * M
    if rrect_stroke(x, y, M, size, R, STROKE):
        return True
    # 4x4 宫格分隔线：中间两条加粗（表达数独的 3x3 块结构）
    step = size / GRID
    for i in range(1, GRID):
        p = M + i * step
        w = BOLD if (i == GRID // 2) else LINE
        if seg_stroke(x, y, p, M, p, M + size, w):
            return True
        if seg_stroke(x, y, M, p, M + size, p, w):
            return True
    # 填了数的格子
    for (gr, gc) in FILLED:
        fx0 = M + gc * step + 3
        fy0 = M + gr * step + 3
        fx1 = M + (gc + 1) * step - 3
        fy1 = M + (gr + 1) * step - 3
        if fx0 <= x <= fx1 and fy0 <= y <= fy1:
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
