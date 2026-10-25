#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""箱子加条纹的几种画法（1bpp 墨水屏）。

背景：墙是"带白缝的圆角黑砖"，箱子是"实心黑方块"——两者都是黑块，
容易混。加条纹/标记让箱子一眼看出"这是能推的箱子"。

注意：真斜线在 1bpp 上要么糊、要么要重刷固件（lv_obj_set_style_line_width 没导出），
所以斜纹用**阶梯像素块**画（纯矩形，像素风，1bpp 下反而最清晰）。

产出 docs/mockups/sok-crate-stripes.png
"""
import os

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FONT_DIR = os.path.join(ROOT, "tools", "ttf-fonts", "MiSans")
OUT = os.path.join(ROOT, "docs", "mockups", "sok-crate-stripes.png")
SAVE_TXT = "/tmp/sok_now.txt"

GW = GH = 10
CELL = 44
BOARD_X = (480 - GW * CELL) // 2
BOARD_Y = 104
CRATE = 32          # 箱子方块边长
CRATE_OFF = 6       # 在格子里居中的偏移


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


def diagonal_band(d, x0, y0, size, width, offset):
    """在 size×size 的方块里画一条 45° 白色斜带（阶梯像素块，1bpp 友好）。"""
    step = width
    i = -size
    while i < size:
        x = x0 + max(0, i)
        y = y0 + max(0, -i) if i < 0 else y0 + i
        if i < 0:
            # 斜带进入方块的上边
            y = y0
            x = x0 + i + size
            d.rectangle([x, y, x + width - 1, y + width - 1], fill=255)
        else:
            d.rectangle([x, y, x + width - 1, y + width - 1], fill=255)
        i += step
    return


def draw_crate(d, ox, oy, style):
    """在 (ox,oy) 画一个 32×32 的箱子，style 决定条纹样式。"""
    x0, y0 = ox + CRATE_OFF, oy + CRATE_OFF
    d.rectangle([x0, y0, x0 + CRATE - 1, y0 + CRATE - 1], fill=0)
    if style == "S0":  # 现状：中心白方孔
        d.rectangle([x0 + 11, y0 + 11, x0 + 20, y0 + 20], fill=255)
    elif style == "S1":  # 一条斜带
        diagonal_band(d, x0, y0, CRATE, 5, 0)
    elif style == "S2":  # 两条斜带
        diagonal_band(d, x0, y0, CRATE, 4, 0)
        diagonal_band(d, x0, y0, CRATE, 4, 11)
    elif style == "S3":  # 三条横带（木板箱）
        for k in (4, 13, 22):
            d.rectangle([x0, y0 + k, x0 + CRATE - 1, y0 + k + 4], fill=255)
    elif style == "S5":  # 反色 = 已归位（白底黑条 + 黑框）
        d.rectangle([x0, y0, x0 + CRATE - 1, y0 + CRATE - 1], fill=255, outline=0, width=2)
        for k in (4, 13, 22):
            d.rectangle([x0 + 2, y0 + k, x0 + CRATE - 3, y0 + k + 4], fill=0)
    elif style == "S4":  # 白色十字
        d.rectangle([x0 + 13, y0, x0 + 18, y0 + CRATE - 1], fill=255)
        d.rectangle([x0, y0 + 13, x0 + CRATE - 1, y0 + 18], fill=255)


def draw_board(img, wall, goal, box, px, py, style):
    d = ImageDraw.Draw(img)
    for y in range(GH):
        for x in range(GW):
            ox = BOARD_X + x * CELL
            oy = BOARD_Y + y * CELL
            d.rectangle([ox, oy, ox + CELL - 1, oy + CELL - 1], outline=0, width=1)
            if wall[y][x]:
                d.rounded_rectangle([ox + 2, oy + 2, ox + CELL - 3, oy + CELL - 3], radius=6, fill=0)
            if box[y][x]:
                draw_crate(d, ox, oy, style)
            if goal[y][x]:
                if box[y][x]:
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
    d.text((16, 48), "推箱子", font=F_TITLE, fill=0)
    d.text((464, 56), "第 2 关 · 步 0", font=F_UI, fill=0, anchor="ra")
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
        ("S0", "S0 旧：白方孔"),
        ("S1", "S1 斜带"),
        ("S2", "S2 双斜带"),
        ("S3", "S3 三条横带 ★"),
        ("S5", "S5 反色=已归位"),
    ]
    # 上排：每个方案里放大一个箱子（4 倍）看细节；下排：整盘
    sheet = Image.new("L", (5 * 200 + 40, 300 + 830 + 60), 255)
    d = ImageDraw.Draw(sheet)
    for i, (style, label) in enumerate(styles):
        x = 20 + i * 200
        d.text((x, 8), label, font=F_LABEL, fill=0)
        cell = Image.new("L", (CELL, CELL), 255)
        cd = ImageDraw.Draw(cell)
        cd.rectangle([0, 0, CELL - 1, CELL - 1], outline=0, width=1)
        draw_crate(cd, 0, 0, style)
        sheet.paste(cell.resize((180, 180), Image.NEAREST), (x, 40))
    for i, (style, label) in enumerate(styles):
        img = Image.new("L", (480, 800), 255)
        draw_board(img, wall, goal, box, px, py, style)
        chrome(img)
        sheet.paste(img, (20 + i * 0, 320 + i * 0)) if False else None
    # 整盘只放推荐的两个
    for i, style in enumerate(["S3", "S5"]):
        img = Image.new("L", (480, 800), 255)
        if style == "S5":
            # 演示：把第一个箱子画成"已归位"（反色）
            box2 = [row[:] for row in box]
            for y in range(GH):
                for x in range(GW):
                    if box2[y][x]:
                        box2[y][x] = 0
                        box2[y][x] = 2
                        break
                else:
                    continue
                break
            for y in range(GH):
                for x in range(GW):
                    if box2[y][x] == 2:
                        box2[y][x] = 1
            draw_board(img, wall, goal, box2, px, py, style)
        else:
            draw_board(img, wall, goal, box, px, py, style)
        chrome(img)
        d.text((40 + i * 500, 300), f"整盘效果：{style}", font=F_LABEL, fill=0)
        sheet.paste(img, (20 + i * 500, 330))
    sheet.convert("1").save(OUT)
    print("wrote", OUT)


if __name__ == "__main__":
    main()
