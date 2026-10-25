#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""真机截图 vs 设备存档：逐格像素级比对。

用法：python3 docs/mockups/verify_sok_render.py <截图.png> <存档.txt> [灰度0|1]

以**设备存档**（sok.txt，唯一权威状态）为基准，用与 App 完全相同的几何规则
重画一版棋盘，再和真机截图逐格做像素 diff，输出对不上的格子。

比"分类统计"可靠得多：黑箱（整格黑占比 44%）和目标地砖（50%）在统计上几乎
分不开，但像素 diff 能精确指出差异位置。
"""
import importlib.util
import os
import sys

from PIL import Image, ImageChops

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
spec = importlib.util.spec_from_file_location("mk", os.path.join(ROOT, "docs", "mockups", "make_sok_gray.py"))
mk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mk)

GW = GH = 10
CELL = 44
BOARD_X = (480 - GW * CELL) // 2
BOARD_Y = 104


def load_save(path):
    lines = open(path).read().split("\n")
    head = lines[0].split()
    hx = lines[1].strip()
    wall = [[0] * GW for _ in range(GH)]
    goal = [[0] * GW for _ in range(GH)]
    box = [[0] * GW for _ in range(GH)]
    for i, ch in enumerate(hx):
        v = int(ch, 16)
        y, x = divmod(i, GW)
        wall[y][x] = v & 1
        goal[y][x] = (v >> 1) & 1
        box[y][x] = (v >> 2) & 1
    # SOK3 头：magic level seed moves best solved px py undo_n gray
    return dict(level=int(head[1]), seed=int(head[2]), moves=int(head[3]),
                best=int(head[4]), solved=int(head[5]),
                px=int(head[6]), py=int(head[7]),
                undo_n=int(head[8]), gray=int(head[9]),
                wall=wall, goal=goal, box=box)


def main():
    shot, save = sys.argv[1], sys.argv[2]
    gray_override = int(sys.argv[3]) if len(sys.argv) > 3 else None
    dev = Image.open(shot).convert("L")
    b = load_save(save)
    gray = b["gray"] if gray_override is None else gray_override
    print(f"存档：第 {b['level']} 关 · 步 {b['moves']} · 玩家({b['px']},{b['py']}) · "
          f"悔棋栈 {b['undo_n']} · 灰度={b['gray']}（比对用灰度={gray}）")

    ref = mk.render(b, gray=bool(gray))
    ref_px = ref.load()

    bad = 0
    for gy in range(GH):
        marks = ""
        for gx in range(GW):
            x0, y0 = BOARD_X + gx * CELL, BOARD_Y + gy * CELL
            diff = 0
            for y in range(y0, y0 + CELL):
                for x in range(x0, x0 + CELL):
                    if abs(dev.load()[x, y] - ref_px[x, y]) > 96:
                        diff += 1
            pct = 100.0 * diff / (CELL * CELL)
            marks += "!" if pct > 4 else "."
            if pct > 4:
                bad += 1
                w, g, bx = b["wall"][gy][gx], b["goal"][gy][gx], b["box"][gy][gx]
                kind = "墙" if w else ("箱+目标" if bx and g else ("箱" if bx else ("目标" if g else "空")))
                print(f"  ✗ 格({gx},{gy}) {kind}: {pct:.1f}% 像素不同")
        print(f"row{gy}: {marks}")
    print(f"\n不一致格子数：{bad}")
    return 0 if bad == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
