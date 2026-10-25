#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""挪车电话 App —— 1bit 墨水屏效果图生成器（480x800）。

不是运行时代码，只是设计评审用的静态效果图：
模拟 SSD1677 面板的 480x800 / 1bpp 观感（有序抖动做灰阶），
用 MiSans 字形渲染界面元素，方便在开发前先看效果。

用法:
    python3 docs/mockups/make_parkphone_mockup.py
产出:
    docs/mockups/parkphone-main.png     显示页
    docs/mockups/parkphone-config.png   配置页
"""
import os
from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FONT_DIR = os.path.join(ROOT, "tools", "ttf-fonts", "MiSans")
OUT_DIR = os.path.join(ROOT, "docs", "mockups")

W, H = 480, 800
STATUS_H = 48

# 4x4 Bayer 矩阵（与固件 epd-bayer-dither 同一套阈值取向）
BAYER = [
    [0, 8, 2, 10],
    [12, 4, 14, 6],
    [3, 11, 1, 9],
    [15, 7, 13, 5],
]


def font(name, size):
    return ImageFont.truetype(os.path.join(FONT_DIR, name), size)


F_DIGIT = font("MiSans-Semibold.ttf", 62)
F_DIGIT_SMALL = font("MiSans-Semibold.ttf", 26)
F_TITLE = font("MiSans-Medium.ttf", 30)
F_UI = font("MiSans-Regular.ttf", 25)
F_UI_BOLD = font("MiSans-Semibold.ttf", 25)
F_SMALL = font("MiSans-Regular.ttf", 20)
F_CELL_DIGIT = font("MiSans-Semibold.ttf", 50)
F_GHOST = font("MiSans-Regular.ttf", 20)


def new_canvas():
    return Image.new("L", (W, H), 255)


def dither(mask, darkness=1.0):
    """把灰度遮罩按 Bayer 阈值抖动成 0/255（darkness: 1=纯黑, 0.5=半灰）。"""
    w, h = mask.size
    px = mask.load()
    out = Image.new("L", (w, h), 255)
    op = out.load()
    for y in range(h):
        brow = BAYER[y & 3]
        for x in range(w):
            p = (255 - px[x, y]) / 255.0 * darkness
            t = (brow[x & 3] + 0.5) / 16.0
            if p > t:
                op[x, y] = 0
    return out


def draw_text(canvas, xy, text, fnt, darkness=1.0, anchor="la", color=0):
    """在 canvas 上画抖动文字（只覆盖指定颜色像素，不擦掉已画内容）。"""
    tmp = Image.new("L", canvas.size, 255)
    ImageDraw.Draw(tmp).text(xy, text, font=fnt, fill=0, anchor=anchor)
    mask = dither(tmp, darkness).point(lambda v: 255 if v == 0 else 0)
    canvas.paste(color, (0, 0), mask)
    return mask


def rect(canvas, box, width=2, fill=False):
    d = ImageDraw.Draw(canvas)
    if fill:
        d.rectangle(box, fill=0)
    else:
        d.rectangle(box, outline=0, width=width)


def line(canvas, x1, y1, x2, y2, width=2):
    ImageDraw.Draw(canvas).line([x1, y1, x2, y2], fill=0, width=width)


def gear(canvas, cx, cy, r):
    """画一个简单的齿轮图标。"""
    import math

    d = ImageDraw.Draw(canvas)
    for i in range(8):
        a = i * math.pi / 4
        tx, ty = cx + math.cos(a) * (r + 4), cy + math.sin(a) * (r + 4)
        d.ellipse([tx - 3, ty - 3, tx + 3, ty + 3], fill=0)
    d.ellipse([cx - r, cy - r, cx + r, cy + r], outline=0, width=2)
    d.ellipse([cx - r // 3, cy - r // 3, cx + r // 3, cy + r // 3], fill=0)


def triangle(canvas, cx, cy, size, up=True):
    d = ImageDraw.Draw(canvas)
    if up:
        d.polygon([(cx, cy - size), (cx - size, cy + size), (cx + size, cy + size)], fill=0)
    else:
        d.polygon([(cx, cy + size), (cx - size, cy - size), (cx + size, cy - size)], fill=0)


def status_bar(canvas, right="82%"):
    """模拟固件状态栏：时间 / 电量 / Wi-Fi。"""
    draw_text(canvas, (16, 8), "10:57", F_UI, 1.0)
    # 右上角：Wi-Fi + 电池
    d = ImageDraw.Draw(canvas)
    for i in range(3):
        d.arc([416 - i * 5, 8 + i * 5, 424 + i * 5, 20 + i * 5], 200, 340, fill=0, width=2)
    d.ellipse([418, 24, 422, 28], fill=0)
    rect(canvas, [432, 12, 456, 28], width=2)
    d.rectangle([456, 16, 460, 24], fill=0)
    line(canvas, 436, 24, 452, 24, width=5)
    line(canvas, 0, STATUS_H, W, STATUS_H, width=1)


def draw_digits(canvas, digits, y, size=62, x0=20, x1=460, gap_min=2):
    """把 11 位数字均匀铺在 [x0, x1] 之间，返回每个数字的 (x, y, w) 盒。"""
    widths = []
    for ch in digits:
        bb = ImageDraw.Draw(Image.new("L", (1, 1))).textbbox((0, 0), ch, font=F_DIGIT)
        widths.append(bb[2] - bb[0])
    total = sum(widths)
    n = len(digits)
    space = (x1 - x0 - total) / (n - 1) if n > 1 else 0
    space = max(space, gap_min)
    boxes = []
    x = x0
    for i, ch in enumerate(digits):
        draw_text(canvas, (x, y), ch, F_DIGIT, 1.0, anchor="lt")
        bb = ImageDraw.Draw(Image.new("L", (1, 1))).textbbox((0, 0), ch, font=F_DIGIT)
        boxes.append((x, y, bb[2] - bb[0]))
        x += widths[i] + space
    return boxes


def make_main():
    canvas = new_canvas()
    status_bar(canvas)

    # 标题
    draw_text(canvas, (W // 2, 92), "挪车请拨打", F_TITLE, 0.55, anchor="mt")

    # 号码：11 位横排
    digits = "13800000000"
    draw_digits(canvas, digits, 215)

    # 下方提示
    draw_text(canvas, (W // 2, 325), "本人手机号 · 可直接拨打", F_UI, 0.45, anchor="mt")

    # 分隔线
    line(canvas, 40, 600, 440, 600, width=2)

    # 底栏：配置 + 3 个槽位
    btn_y, btn_h = 646, 58
    rect(canvas, [16, btn_y, 136, btn_y + btn_h], width=2)
    gear(canvas, 44, btn_y + btn_h // 2, 11)
    draw_text(canvas, (66, btn_y + btn_h // 2), "配置", F_UI_BOLD, 1.0, anchor="lm")

    slots = [("槽 1", True), ("槽 2", False), ("槽 3", False)]
    x = 152
    for label, active in slots:
        rect(canvas, [x, btn_y, x + 92, btn_y + btn_h], width=2, fill=active)
        draw_text(canvas, (x + 46, btn_y + btn_h // 2), label, F_UI_BOLD,
                  1.0, anchor="mm", color=255 if active else 0)
        x += 100

    draw_text(canvas, (W // 2, 740), "点槽位切换 · 点配置改号", F_UI, 0.45, anchor="mt")
    return canvas


def make_config():
    canvas = new_canvas()
    status_bar(canvas)

    # 顶栏
    rect(canvas, [12, 60, 108, 104], width=2)
    line(canvas, 66, 82, 44, 82, width=3)
    line(canvas, 44, 82, 58, 68, width=3)
    line(canvas, 44, 82, 58, 96, width=3)
    draw_text(canvas, (W // 2, 82), "配置号码", F_TITLE, 1.0, anchor="mm")

    for i, x in enumerate([312, 356, 400]):
        rect(canvas, [x, 62, x + 40, 102], width=2, fill=(i == 0))
        draw_text(canvas, (x + 20, 82), str(i + 1), F_UI_BOLD, 1.0, anchor="mm",
                  color=255 if i == 0 else 0)

    draw_text(canvas, (W // 2, 126), "按住格子上下滑动改号", F_UI, 0.45, anchor="mt")

    # 11 个格子
    digits = "13800000000"
    cell_w, gap, cell_h = 40, 2, 118
    x0 = 10
    top = 158
    sel = 3
    for i, ch in enumerate(digits):
        x = x0 + i * (cell_w + gap)
        active = (i == sel)
        rect(canvas, [x, top, x + cell_w, top + cell_h], width=2 if active else 1)
        # 相邻数字（幽灵提示）
        draw_text(canvas, (x + cell_w // 2, top + 8), str((int(ch) - 1) % 10), F_GHOST, 0.28, anchor="mt")
        draw_text(canvas, (x + cell_w // 2, top + cell_h - 26), str((int(ch) + 1) % 10), F_GHOST, 0.28, anchor="mt")
        # 当前数字
        draw_text(canvas, (x + cell_w // 2, top + 34), ch, F_CELL_DIGIT, 1.0, anchor="mt")
        if active:
            triangle(canvas, x + cell_w // 2, top + 6, 4, up=True)
            triangle(canvas, x + cell_w // 2, top + cell_h - 6, 4, up=False)

    # 预览整串
    draw_text(canvas, (W // 2, 300), "当前：13800000000", F_UI, 0.5, anchor="mt")
    draw_text(canvas, (W // 2, 342), "正在改第 4 位", F_UI, 0.45, anchor="mt")

    # 底部按钮
    btn_y, btn_h = 640, 58
    buttons = [("保存", 16), ("清空", 172), ("取消", 328)]
    for label, x in buttons:
        rect(canvas, [x, btn_y, x + 136, btn_y + btn_h], width=2)
        draw_text(canvas, (x + 68, btn_y + btn_h // 2), label, F_UI_BOLD, 1.0, anchor="mm")

    draw_text(canvas, (W // 2, 730), "保存后立即生效", F_UI, 0.45, anchor="mt")
    return canvas


if __name__ == "__main__":
    os.makedirs(OUT_DIR, exist_ok=True)
    for name, img in (("parkphone-main.png", make_main()), ("parkphone-config.png", make_config())):
        path = os.path.join(OUT_DIR, name)
        img.convert("1").save(path)
        print("wrote", path)
