#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""推箱子：黑白版 vs 网点灰版（1:1 等比，模拟真机 480x800 / 1bpp）。

背景：1bpp 墨水屏物理只有黑白两色，"灰"只能靠 **Bayer 有序抖动（网点）** 模拟。
做法与固件首页的卡片灰底一致（main/display/screen/home_screen/home_screen.cc
的 GrayDitherImg()）：4x4 Bayer 矩阵，每个格放大成 2x2 像素块，生成 8x8 的
L8 点阵图，用 bg_image_src + bg_image_tiled 平铺。

本脚本严格复刻 tools/sok_app/main/sok_gray.h + sok_main.c 的几何与图案，
所以出来的图就是真机该有的样子（点阵相位以每个对象的左上角为原点，
和 LVGL 的平铺起点一致）。

产出：
  docs/mockups/sok-gray-preview.png   灰度版单图（480x800，可当截图预览）
  docs/mockups/sok-gray-compare.png   黑白 vs 灰度 并排对比
"""
import os

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FONT_DIR = os.path.join(ROOT, "tools", "ttf-fonts", "MiSans")
OUT_DIR = os.path.join(ROOT, "docs", "mockups")
SAVE_TXT = "/tmp/sok_now.txt"

GW = GH = 10
CELL = 44
BOARD_X = (480 - GW * CELL) // 2
BOARD_Y = 104
CRATE = 32
CRATE_OFF = 6

# ---- 网点参数（必须和 sok_gray.h 一致）----
BAYER = [
    [0, 8, 2, 10],
    [12, 4, 14, 6],
    [3, 11, 1, 9],
    [15, 7, 13, 5],
]
SCALE = 2
TILE = 4 * SCALE
GRAY_LIGHT = 2   # 阈值 -> 黑点占比 threshold/16
GRAY_WALL = 4
GRAY_GOAL = 8


def font(name, size):
    return ImageFont.truetype(os.path.join(FONT_DIR, name), size)


F_TITLE = font("MiSans-Medium.ttf", 30)
F_UI = font("MiSans-Regular.ttf", 25)


def load_board(path):
    lines = open(path).read().split("\n")
    head = lines[0].split()
    magic = head[0]
    level, seed, moves, best, solved, px, py = (int(v) for v in head[1:8])
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
    return dict(level=level, seed=seed, moves=moves, best=best, solved=solved,
                px=px, py=py, wall=wall, goal=goal, box=box)


def dither_mask(w, h, threshold):
    """对象内的黑白点阵。**相位锚点是对象自己的左上角**——
    LVGL 平铺 bg_image 时从对象原点开始铺（lv_draw_image_tiled_helper），
    所以每个砖/每格的网点相位都不同，这正是真机的样子。"""
    out = []
    for yy in range(h):
        row = []
        for xx in range(w):
            bx = (xx // SCALE) & 3
            by = (yy // SCALE) & 3
            row.append(BAYER[by][bx] < threshold)
        out.append(row)
    return out


def draw_dither_rect(d, ox, oy, w, h, threshold, radius=0):
    """把网点灰画成一块区域。ox/oy = 对象的绝对左上角（相位原点）。"""
    mask = dither_mask(w, h, threshold)
    for yy in range(h):
        for xx in range(w):
            if not mask[yy][xx]:
                continue
            px, py = ox + xx, oy + yy
            if radius:
                # 圆角判定：只画离四个角中心足够远的像素
                cx = min(max(px, ox + radius), ox + w - 1 - radius)
                cy = min(max(py, oy + radius), oy + h - 1 - radius)
                if (px - cx) ** 2 + (py - cy) ** 2 > radius * radius:
                    continue
            d.point((px, py), fill=0)


def render(b, gray):
    """渲染一整屏。gray=False -> 纯黑白；gray=True -> 墙/目标用网点灰。"""
    img = Image.new("L", (480, 800), 255)
    d = ImageDraw.Draw(img)

    # 状态栏
    d.text((16, 12), "WiFi", font=font("MiSans-Regular.ttf", 18), fill=0)
    d.text((200, 10), "22:23", font=font("MiSans-Medium.ttf", 22), fill=0)
    d.text((356, 10), "100%", font=font("MiSans-Medium.ttf", 22), fill=0)

    # 标题 + 信息行
    d.text((16, 48), "推箱子", font=F_TITLE, fill=0)
    info = "第 %d 关 · 步 %d" % (b["level"], b["moves"])
    if b["best"] > 0:
        info += " · 最佳 %d" % b["best"]
    d.text((300, 56), info, font=F_UI, fill=0)

    # 棋盘
    for y in range(GH):
        for x in range(GW):
            cx = BOARD_X + x * CELL
            cy = BOARD_Y + y * CELL
            is_wall = b["wall"][y][x]
            is_goal = b["goal"][y][x]
            is_box = b["box"][y][x]

            # 格子底色：普通地板 = 白；目标格 = 中灰网点（灰度模式）。
            # 点阵铺满整格（bg_image 画在对象整个区域），1px 格线随后盖在上面。
            if gray and is_goal and not is_wall:
                draw_dither_rect(d, cx, cy, CELL, CELL, GRAY_GOAL)

            # 1px 格线
            d.rectangle([cx, cy, cx + CELL - 1, cy + CELL - 1], outline=0)

            if is_wall:
                # 墙砖：cell 的子对象，局部坐标 (2,2)；cell 有 1px 边框，
                # LVGL 子对象定位在**内容区**内 → 绝对位置 = cell + 1 + 2 = cell + 3。
                if gray:
                    draw_dither_rect(d, cx + 3, cy + 3, CELL - 4, CELL - 4, GRAY_WALL, radius=6)
                else:
                    d.rounded_rectangle([cx + 3, cy + 3, cx + CELL - 4, cy + CELL - 4],
                                        radius=6, fill=0)

            if is_goal and not is_box:
                # 靶心：灰度模式=白环(4px)+黑心(8px)（网点底上白环才立得住）；
                #        黑白模式=黑环(3px)+黑心(8px)
                if gray:
                    d.ellipse([cx + 11, cy + 11, cx + 32, cy + 32], outline=255, width=4)
                else:
                    d.ellipse([cx + 12, cy + 12, cx + 31, cy + 31], outline=0, width=3)
                d.ellipse([cx + 18, cy + 18, cx + 25, cy + 25], fill=0)

            if is_box:
                on_goal = bool(is_goal)
                bx, by = cx + CRATE_OFF, cy + CRATE_OFF
                # 箱子 32x32 + 2px 黑边框；横带是**子对象**，同样定位在内容区内
                # → 绝对位置 = crate + 2（边框宽度），y 再 + 2 + k*9。
                d.rectangle([bx, by, bx + CRATE - 1, by + CRATE - 1],
                            fill=255 if on_goal else 0, outline=0, width=2)
                for k in range(3):
                    sy = by + 2 + 2 + k * 9
                    d.rectangle([bx + 2, sy, bx + 29, sy + 3],
                                fill=0 if on_goal else 255)

    # 人：一个小人（圆头 + 身子 + 伸开的手臂 + 两条腿）
    # 几何必须和 sok_main.c 的 mk_person_part()/mk_person_white() 一致（可爱版）：
    # 头 圆13 @(10,2)、呆毛 2x3 @(15,0)、眼 2x2 白 @(13,7)/(18,7)、嘴 4x1 白 @(14,11)、
    # 身 10x8 radius4 @(11,15)、臂 18x3 @(7,17)、腿 4x8 @(11,23)/(17,23)
    # ⚠ 顺序也要一致：五官（白）必须画在头之后才会盖住黑头。
    def person(ox, oy):
        d.ellipse([ox + 10, oy + 2, ox + 22, oy + 14], fill=0)            # 头
        d.rectangle([ox + 15, oy + 0, ox + 16, oy + 2], fill=0)            # 呆毛
        d.rectangle([ox + 13, oy + 7, ox + 14, oy + 8], fill=255)          # 左眼
        d.rectangle([ox + 18, oy + 7, ox + 19, oy + 8], fill=255)          # 右眼
        d.rectangle([ox + 14, oy + 11, ox + 17, oy + 11], fill=255)        # 笑嘴
        d.rounded_rectangle([ox + 11, oy + 15, ox + 20, oy + 22],
                            radius=4, fill=0)                              # 身子
        d.rectangle([ox + 7, oy + 17, ox + 24, oy + 19], fill=0)           # 手臂
        d.rectangle([ox + 11, oy + 23, ox + 14, oy + 30], fill=0)          # 左腿
        d.rectangle([ox + 17, oy + 23, ox + 20, oy + 30], fill=0)          # 右腿

    pxx, pyy = BOARD_X + b["px"] * CELL + CRATE_OFF, BOARD_Y + b["py"] * CELL + CRATE_OFF
    person(pxx, pyy)

    # 状态行
    d.text((16, 560), "滑动屏幕或按方向键推箱子", font=F_UI, fill=0)

    # 方向键
    def btn(x, y, w, h, text, fill=False):
        d.rounded_rectangle([x, y, x + w - 1, y + h - 1], radius=8,
                            fill=0 if fill else 255, outline=0, width=2)
        if fill:
            d.text((x + w / 2, y + h / 2), text, font=F_UI, fill=255, anchor="mm")
        else:
            d.text((x + w / 2, y + h / 2), text, font=F_UI, fill=0, anchor="mm")

    btn(70, 600, 60, 60, "↑")
    btn(10, 660, 60, 60, "←")
    btn(70, 660, 60, 60, "↓")
    btn(130, 660, 60, 60, "→")
    btn(200, 600, 125, 60, "悔棋")
    btn(335, 600, 125, 60, "重开")
    btn(200, 670, 260, 60, "下一关")
    # 灰度开关：开=黑底白字，关=白底黑字
    btn(16, 740, 160, 48, "灰度", fill=gray)

    return img


def main():
    b = load_board(SAVE_TXT)
    mono = render(b, gray=False)
    gray = render(b, gray=True)

    gray.save(os.path.join(OUT_DIR, "sok-gray-preview.png"))

    # 并排对比
    W, H = 480, 800
    cmp_img = Image.new("L", (W * 2 + 60, H + 70), 255)
    cd = ImageDraw.Draw(cmp_img)
    cmp_img.paste(mono, (30, 50))
    cmp_img.paste(gray, (W + 60, 50))
    cd.text((30, 12), "黑白版（现在）", font=F_TITLE, fill=0)
    cd.text((W + 60, 12), "网点灰版（新）", font=F_TITLE, fill=0)
    cd.rectangle([30, 50, 30 + W - 1, 50 + H - 1], outline=0)
    cd.rectangle([W + 60, 50, W + 60 + W - 1, 50 + H - 1], outline=0)
    cmp_img.save(os.path.join(OUT_DIR, "sok-gray-compare.png"))
    print("✓ sok-gray-preview.png / sok-gray-compare.png 已生成")


if __name__ == "__main__":
    main()
