#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""推箱子 App 的 1bpp 显示方案对比图。

用真机上那一关的真实棋盘（apps/sok.app/data/sok.txt），把 4 种视觉方案
按 480x800 / 1bit 渲染出来并排对比：

  现状   1px 格线 + 空心箱框 + 10px 目标点        （用户反馈：箱子和背景分不开）
  A      无格线 + 墙实心 + 实心箱(白孔) + 靶心目标 + 白圈人   ← 推荐
  B      墙改 3px 细框(轻) + 实心箱(重)          ← 对比最强，但墙可能像箱子
  C      全实心 + 箱子上白色十字

用法: python3 docs/mockups/make_sok_mockup.py
产出: docs/mockups/sok-style-compare.png
"""
import os
import sys

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FONT_DIR = os.path.join(ROOT, "tools", "ttf-fonts", "MiSans")
OUT = os.path.join(ROOT, "docs", "mockups", "sok-style-compare.png")
SAVE_TXT = "/tmp/sok_dev.txt"

GW = GH = 10
CELL = 44
BOARD_X = (480 - GW * CELL) // 2
BOARD_Y = 104


def font(name, size):
    return ImageFont.truetype(os.path.join(FONT_DIR, name), size)


F_TITLE = font("MiSans-Medium.ttf", 30)
F_UI = font("MiSans-Regular.ttf", 25)
F_UI_B = font("MiSans-Semibold.ttf", 25)
F_LABEL = font("MiSans-Medium.ttf", 22)


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
    px, py = 5, 1
    return wall, goal, box, px, py


def new_canvas():
    img = Image.new("L", (480, 800), 255)
    d = ImageDraw.Draw(img)
    d.text((16, 48), "推箱子", font=F_TITLE, fill=0)
    d.text((464, 56), "第 1 关 · 步 3", font=F_UI, fill=0, anchor="ra")
    return img


def draw_board(img, wall, goal, box, px, py, style):
    d = ImageDraw.Draw(img)
    for y in range(GH):
        for x in range(GW):
            ox = BOARD_X + x * CELL
            oy = BOARD_Y + y * CELL
            has_box = box[y][x]
            on_goal = goal[y][x]

            if style == "old":
                # 现状：地板 1px 格线
                d.rectangle([ox, oy, ox + CELL - 1, oy + CELL - 1], outline=0, width=1)
                if wall[y][x]:
                    d.rectangle([ox, oy, ox + CELL - 1, oy + CELL - 1], fill=0)
                if has_box:
                    d.rectangle([ox + 7, oy + 7, ox + CELL - 8, oy + CELL - 8], outline=0, width=3)
                if on_goal:
                    d.ellipse([ox + 17, oy + 17, ox + 26, oy + 26], fill=0)
                continue

            if style in ("A", "C"):
                if wall[y][x]:
                    d.rectangle([ox, oy, ox + CELL - 1, oy + CELL - 1], fill=0)
            elif style == "B":
                # 轻墙：3px 框
                if wall[y][x]:
                    d.rectangle([ox + 1, oy + 1, ox + CELL - 2, oy + CELL - 2], outline=0, width=3)

            if style == "C" and has_box:
                # 实心箱 + 白色十字
                d.rectangle([ox + 3, oy + 3, ox + CELL - 4, oy + CELL - 4], fill=0)
                d.rectangle([ox + 12, oy + 21, ox + CELL - 13, oy + 25], fill=255)
                d.rectangle([ox + 21, oy + 12, ox + 25, oy + CELL - 13], fill=255)
            elif style in ("A", "B") and has_box:
                # 实心箱：38x38 黑方块 + 3px 白描边（贴着墙也不粘连）+ 中心白方孔
                d.rectangle([ox + 3, oy + 3, ox + CELL - 4, oy + CELL - 4], outline=255, width=3)
                d.rectangle([ox + 5, oy + 5, ox + CELL - 6, oy + CELL - 6], fill=0)
                d.rectangle([ox + 16, oy + 16, ox + 27, oy + 27], fill=255)

            if on_goal:
                if has_box:
                    # 箱子压在目标上：白靶心画在黑箱上
                    d.ellipse([ox + 12, oy + 12, ox + 31, oy + 31], outline=255, width=3)
                    d.ellipse([ox + 19, oy + 19, ox + 24, oy + 24], fill=255)
                else:
                    d.ellipse([ox + 12, oy + 12, ox + 31, oy + 31], outline=0, width=3)
                    d.ellipse([ox + 19, oy + 19, ox + 24, oy + 24], fill=0)

    # 人：实心圆 + 白描边
    ox = BOARD_X + px * CELL
    oy = BOARD_Y + py * CELL
    d.ellipse([ox + 7, oy + 7, ox + 36, oy + 36], fill=255, outline=255, width=3)
    d.ellipse([ox + 10, oy + 10, ox + 33, oy + 33], fill=0)


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
        ("old", "现状：1px 格线 + 空心箱框"),
        ("A", "A：无格线 + 实心箱(白孔) + 靶心"),
        ("B", "B：轻墙(3px 框) + 实心箱"),
        ("C", "C：实心箱 + 白色十字"),
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
