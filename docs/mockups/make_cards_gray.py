#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""卡牌游戏：暗牌背「实心黑」 vs 「网点浅灰」对比（1:1 等比）。

背景：1bpp 墨水屏物理只有黑白两色，"灰"只能靠 **Bayer 有序抖动（网点）** 模拟
（tools/_shared/gray_dither.h，和推箱子 App 用的是同一套）。

两个卡牌游戏原来都把暗牌画成**实心黑牌**：
  - 记忆翻牌 mem_app：一屏 16 张 90×120 的黑牌 → 整屏死黑，还费墨
    （墨水屏大块纯黑局刷留灰影、刷新慢）
  - 纸牌接龙 sol_app：牌堆一压就是一大片黑

改成 25% 网点浅灰 + 黑框后：牌还是牌、堆还是堆，但不再是一片死黑，
翻开的牌（纯白/纯黑）也立刻跳出来。

产出：
  docs/mockups/cards-gray-mem.png   记忆翻牌 改前/改后
  docs/mockups/cards-gray-sol.png   纸牌接龙 改前/改后
"""
import os

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FONT_DIR = os.path.join(ROOT, "tools", "ttf-fonts", "MiSans")
OUT_DIR = os.path.join(ROOT, "docs", "mockups")

BAYER = [[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]]
SCALE = 2
TILE = 4 * SCALE
GRAY_CARD = 4  # 25% 黑


def font(name, size):
    return ImageFont.truetype(os.path.join(FONT_DIR, name), size)


F_TITLE = font("MiSans-Medium.ttf", 30)
F_UI = font("MiSans-Regular.ttf", 25)
F_LBL = font("MiSans-Medium.ttf", 24)


def dither_fill(d, ox, oy, w, h, threshold, radius=0):
    """网点灰。相位原点 = 区域左上角（和 LVGL 的平铺起点一致）。"""
    for yy in range(h):
        for xx in range(w):
            by = (yy // SCALE) & 3
            bx = (xx // SCALE) & 3
            if BAYER[by][bx] >= threshold:
                continue
            px, py = ox + xx, oy + yy
            if radius:
                cx = min(max(px, ox + radius), ox + w - 1 - radius)
                cy = min(max(py, oy + radius), oy + h - 1 - radius)
                if (px - cx) ** 2 + (py - cy) ** 2 > radius * radius:
                    continue
            d.point((px, py), fill=0)


def round_rect(d, box, radius, fill=None, outline=0, width=1):
    d.rounded_rectangle(box, radius=radius, fill=fill, outline=outline, width=width)


def suit(d, x, y, s, color, sym="♠"):
    """画一个扑克花色（MiSans 没有 ♠♥♦♣，这里用几何图形代替）。"""
    if sym == "♥":  # 心
        r = s * 0.28
        d.ellipse([x, y, x + r * 2, y + r * 2], fill=color)
        d.ellipse([x + r * 1.2, y, x + r * 3.2, y + r * 2], fill=color)
        d.polygon([(x + 0.1 * s, y + r * 1.4), (x + 0.9 * s, y + r * 1.4), (x + 0.5 * s, y + s)], fill=color)
    elif sym == "♦":  # 方块
        d.polygon([(x + 0.5 * s, y), (x + s, y + 0.5 * s), (x + 0.5 * s, y + s), (x, y + 0.5 * s)], fill=color)
    elif sym == "♠":  # 黑桃（倒过来的心 + 梗）
        r = s * 0.28
        d.ellipse([x, y + r * 1.2, x + r * 2, y + r * 3.2], fill=color)
        d.ellipse([x + r * 1.2, y + r * 1.2, x + r * 3.2, y + r * 3.2], fill=color)
        d.polygon([(x + 0.1 * s, y + 0.55 * s), (x + 0.9 * s, y + 0.55 * s), (x + 0.5 * s, y)], fill=color)
        d.rectangle([x + 0.44 * s, y + 0.6 * s, x + 0.56 * s, y + s], fill=color)
    else:  # ♣ 梅花
        r = s * 0.26
        d.ellipse([x + 0.5 * s - r, y, x + 0.5 * s + r, y + r * 2], fill=color)
        d.ellipse([x + 0.5 * s - r - r * 1.1, y + r * 0.8, x + 0.5 * s + r - r * 1.1, y + r * 2.8], fill=color)
        d.ellipse([x + 0.5 * s - r + r * 1.1, y + r * 0.8, x + 0.5 * s + r + r * 1.1, y + r * 2.8], fill=color)
        d.rectangle([x + 0.44 * s, y + 0.55 * s, x + 0.56 * s, y + s], fill=color)


# ================= 记忆翻牌 =================
def render_mem(gray):
    img = Image.new("L", (480, 800), 255)
    d = ImageDraw.Draw(img)
    d.text((16, 48), "记忆翻牌", font=F_TITLE, fill=0)
    d.text((250, 56), "翻牌配对 · 4×4", font=F_UI, fill=0)

    NCOL, CW, CH, GX, GY = 4, 90, 120, 10, 10
    grid_w = NCOL * CW + (NCOL - 1) * GX
    x0 = (480 - grid_w) // 2
    y0 = 96
    for r in range(4):
        for c in range(NCOL):
            x = x0 + c * (CW + GX)
            y = y0 + r * (CH + GY)
            if gray:
                dither_fill(d, x, y, CW, CH, GRAY_CARD, radius=8)
                round_rect(d, [x, y, x + CW - 1, y + CH - 1], 8, outline=0, width=2)
            else:
                round_rect(d, [x, y, x + CW - 1, y + CH - 1], 8, fill=0, outline=255, width=2)

    d.text((16, 660), "点两张翻牌，配对成功就消除", font=F_UI, fill=0)
    bw, by = 144, 700
    for i, t in enumerate(["新局", "难度", "帮助"]):
        x = 16 + i * (bw + 8)
        round_rect(d, [x, by, x + bw - 1, by + 55], 8, fill=255, outline=0, width=2)
        d.text((x + bw / 2, by + 28), t, font=F_UI, fill=0, anchor="mm")
    return img


# ================= 纸牌接龙 =================
def render_sol(gray):
    img = Image.new("L", (480, 800), 255)
    d = ImageDraw.Draw(img)
    d.text((16, 48), "纸牌接龙", font=F_TITLE, fill=0)
    d.text((300, 56), "第 12 步", font=F_UI, fill=0)

    CW, CH = 58, 82
    top_y = 88
    col_x0, col_dx = 16, 65

    def card_back(x, y):
        if gray:
            dither_fill(d, x, y, CW, CH, GRAY_CARD)
            d.rectangle([x, y, x + CW - 1, y + CH - 1], outline=0, width=1)
        else:
            d.rectangle([x, y, x + CW - 1, y + CH - 1], fill=0, outline=255, width=1)

    def card_face(x, y, rank, suit_sym, red):
        if red:
            d.rectangle([x, y, x + CW - 1, y + CH - 1], fill=0, outline=255, width=1)
            d.text((x + 5, y + 4), rank, font=font("MiSans-Regular.ttf", 18), fill=255)
            suit(d, x + CW - 20, y + CH - 26, 20, 255, suit_sym)
        else:
            d.rectangle([x, y, x + CW - 1, y + CH - 1], fill=255, outline=0, width=1)
            d.text((x + 5, y + 4), rank, font=font("MiSans-Regular.ttf", 18), fill=0)
            suit(d, x + CW - 20, y + CH - 26, 20, 0, suit_sym)

    # 左上：牌堆 + 废牌堆
    card_back(16, top_y)
    card_back(80, top_y)
    # 右上：4 个基础堆（空）
    for f in range(4):
        d.rectangle([220 + f * 62, top_y, 220 + f * 62 + CW - 1, top_y + CH - 1], outline=0, width=1)
    # 7 列：每列压着几张暗牌 + 顶上一张明牌
    GAP_DOWN = 20
    layout = [
        (3, "7", "♣", True), (2, "9", "♥", True), (4, "K", "♠", False),
        (1, "3", "♦", True), (0, "8", "♣", False), (2, "J", "♥", True),
        (1, "5", "♠", False),
    ]
    for c in range(7):
        x = col_x0 + c * col_dx
        d.rectangle([x, 186, x + CW - 1, 186 + CH - 1], outline=0, width=1)
        ndown, rank, suit_sym, red = layout[c]
        for k in range(ndown):
            card_back(x, 186 + k * GAP_DOWN)
        card_face(x, 186 + ndown * GAP_DOWN, rank, suit_sym, red)

    d.text((16, 686), "点牌选择，再点目标堆", font=F_UI, fill=0)
    return img


def compare(name, before, after, titles=("改前：暗牌=实心黑", "改后：暗牌=网点浅灰")):
    W, H = 480, 800
    out = Image.new("L", (W * 2 + 60, H + 70), 255)
    od = ImageDraw.Draw(out)
    out.paste(before, (30, 50))
    out.paste(after, (W + 60, 50))
    od.text((30, 12), titles[0], font=F_TITLE, fill=0)
    od.text((W + 60, 12), titles[1], font=F_TITLE, fill=0)
    od.rectangle([30, 50, 30 + W - 1, 50 + H - 1], outline=0)
    od.rectangle([W + 60, 50, W + 60 + W - 1, 50 + H - 1], outline=0)
    p = os.path.join(OUT_DIR, name)
    out.save(p)
    print("✓", name)

    # 顺带统计整屏黑色占比
    for tag, im in (("改前", before), ("改后", after)):
        px = im.load()
        n = b = 0
        for y in range(0, 800, 2):
            for x in range(0, 480, 2):
                n += 1
                if px[x, y] < 128:
                    b += 1
        print(f"   {tag} 整屏黑色占比 {100.0*b/n:.1f}%")


def main():
    compare("cards-gray-mem.png", render_mem(False), render_mem(True))
    compare("cards-gray-sol.png", render_sol(False), render_sol(True))


if __name__ == "__main__":
    main()
