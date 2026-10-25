#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""推箱子 1bpp 显示方案 v2：按真实实现的"格子(tile)"思路重画。

参考 https://www.sokobanonline.com / GameFromZero Day23 的通用画法：
  每个格子是一个独立的圆角 tile（墙=深色 tile，地板=浅色 tile，目标=染色 tile），
  箱子和小人是画在 tile 上的物件。
1bpp 没有颜色，就用"填充 + 形状"分层：
  墙   实心黑 tile（留白缝 → 一格一格，不再是一坨黑）
  地板 白底 + 淡格线（格子结构可见）
  箱子 实心黑方块（比墙 tile 小）+ 中心白孔
  目标 粗黑靶心（环 + 点）；箱子压上去转白
  人   实心圆 + 白描边

产出 docs/mockups/sok-style-v2.png
"""
import os

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FONT_DIR = os.path.join(ROOT, "tools", "ttf-fonts", "MiSans")
OUT = os.path.join(ROOT, "docs", "mockups", "sok-style-v2.png")
SAVE_TXT = "/tmp/sok_now.txt"

GW = GH = 10
CELL = 44
BOARD_X = (480 - GW * CELL) // 2
BOARD_Y = 104


def font(name, size):
    return ImageFont.truetype(os.path.join(FONT_DIR, name), size)


F_TITLE = font("MiSans-Medium.ttf", 30)
F_UI = font("MiSans-Regular.ttf", 25)
F_UI_B = font("MiSans-Semibold.ttf", 25)
F_LABEL = font("MiSans-Medium.ttf", 21)


def load_board(path):
    lines = open(path).read().split("\n")
    hexline = lines[1].strip()
    wall = [[0] * GW for _ in range(GH)]
    goal = [[0] * GW for _ in range(GH)]
    box = [[0] * GW for _ in range(GH)]
    for i, ch in enumerate(hexline):
        v = int(ch, 16)
        y, x = divmod(i, GW)
        wall[y][x] = v & 1
        goal[y][x] = (v >> 1) & 1
        box[y][x] = (v >> 2) & 1
    return wall, goal, box, 1, 6


def new_canvas():
    img = Image.new("L", (480, 800), 255)
    d = ImageDraw.Draw(img)
    d.text((16, 48), "推箱子", font=F_TITLE, fill=0)
    d.text((464, 56), "第 2 关 · 步 0", font=F_UI, fill=0, anchor="ra")
    return img


def draw_board(img, wall, goal, box, px, py, style):
    d = ImageDraw.Draw(img)
    grid = 2 if style == "D3" else 1
    for y in range(GH):
        for x in range(GW):
            ox = BOARD_X + x * CELL
            oy = BOARD_Y + y * CELL
            has_box = box[y][x]
            on_goal = goal[y][x]

            if style == "A":
                # 当前实现：无格线 + 满格实心墙
                if wall[y][x]:
                    d.rectangle([ox, oy, ox + CELL - 1, oy + CELL - 1], fill=0)
            elif style in ("D1", "D2", "D3"):
                # 地板：白底 + 淡格线（格子结构可见）
                d.rectangle([ox, oy, ox + CELL - 1, oy + CELL - 1], outline=0, width=grid)

            if wall[y][x]:
                if style == "D1":
                    # 砖墙：实心黑块留 2px 白缝 → 一格一格，不是一坨黑
                    d.rounded_rectangle([ox + 2, oy + 2, ox + CELL - 3, oy + CELL - 3],
                                        radius=6, fill=0)
                elif style == "D2":
                    # 浅墙：3px 黑框（白心）→ 整体很轻
                    d.rectangle([ox + 2, oy + 2, ox + CELL - 3, oy + CELL - 3], outline=0, width=3)
                elif style == "D3":
                    d.rectangle([ox, oy, ox + CELL - 1, oy + CELL - 1], fill=0)

            if has_box:
                # 箱子：实心黑方块（比墙 tile 小）+ 中心白孔
                if style in ("D1", "D2", "D3"):
                    d.rectangle([ox + 6, oy + 6, ox + CELL - 7, oy + CELL - 7], fill=0)
                    d.rectangle([ox + 17, oy + 17, ox + 26, oy + 26], fill=255)
                else:
                    d.rounded_rectangle([ox + 5, oy + 5, ox + CELL - 6, oy + CELL - 6],
                                        radius=7, fill=0)
                    d.rectangle([ox + 17, oy + 17, ox + 26, oy + 26], fill=255)

            if on_goal:
                if has_box:
                    d.ellipse([ox + 13, oy + 13, ox + 30, oy + 30], outline=255, width=3)
                    d.ellipse([ox + 19, oy + 19, ox + 24, oy + 24], fill=255)
                else:
                    d.ellipse([ox + 13, oy + 13, ox + 30, oy + 30], outline=0, width=3)
                    d.ellipse([ox + 19, oy + 19, ox + 24, oy + 24], fill=0)

    ox = BOARD_X + px * CELL
    oy = BOARD_Y + py * CELL
    d.ellipse([ox + 8, oy + 8, ox + 35, oy + 35], fill=255, outline=255, width=3)
    d.ellipse([ox + 11, oy + 11, ox + 32, oy + 32], fill=0)


def chrome(img):
    d = ImageDraw.Draw(img)
    d.text((16, 560), "滑动屏幕或按方向键推箱子", font=F_UI, fill=0)
    for (x, y, t) in [(70, 600, "↑"), (10, 660, "←"), (70, 660, "↓"), (130, 660, "→")]:
        d.rectangle([x, y, x + 60, y + 60], outline=0, width=2)
        d.text((x + 30, y + 30), t, font=F_UI_B, fill=0, anchor="mm")
    for (x, y, t) in [(200, 600, "悔棋"), (335, 600, "重开"), (200, 670, "下一关")]:
        w = 125 if t != "下一关" else 260
        d.rectangle([x, y, x + w, y + 60], outline=0, width=2)
        d.text((x + w // 2, y + 30), t, font=F_UI_B, fill=0, anchor="mm")


def main():
    wall, goal, box, px, py = load_board(SAVE_TXT)
    styles = [
        ("A", "A（当前）无格线 + 满格实心墙"),
        ("D1", "D1 格线 + 砖墙(留白缝) + 实心箱"),
        ("D2", "D2 格线 + 浅墙(3px框) + 实心箱"),
        ("D3", "D3 粗格线 + 满格实心墙"),
    ]
    sheet = Image.new("L", (480 * 2, 830 * 2), 255)
    for i, (style, label) in enumerate(styles):
        img = new_canvas()
        draw_board(img, wall, goal, box, px, py, style)
        chrome(img)
        d = ImageDraw.Draw(sheet)
        d.text((480 * (i % 2) + 16, 830 * (i // 2) + 8), label, font=F_LABEL, fill=0)
        sheet.paste(img, (480 * (i % 2), 830 * (i // 2) + 30))
    sheet.convert("1").save(OUT)
    print("wrote", OUT)


if __name__ == "__main__":
    main()
