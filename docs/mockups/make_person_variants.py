#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""推箱子玩家小人：多版造型对比（1bpp / 32x32 画布，与真机几何 1:1）。

约束：墨水屏只有黑白两色 -> "可爱"只能靠**形状**（头身比、眼睛、圆角、呆毛），
没有灰度、没有斜线、没有颜色。所以每一版都是"黑块 + 白块"的拼法。

本脚本里每个 variant 的几何，就是将来要写进
tools/sok_app/main/sok_main.c 的 mk_person_part() 的坐标（同样是 32x32 容器）。

产出：docs/mockups/person-variants.png
"""
import os

from PIL import Image, ImageDraw, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
FONT_DIR = os.path.join(ROOT, "tools", "ttf-fonts", "MiSans")
OUT = os.path.join(ROOT, "docs", "mockups", "person-variants.png")

SIZE = 32          # 玩家容器大小（与真机一致）
CELL = 44          # 棋盘格大小
ZOOM = 7           # 放大倍数


def font(name, size):
    return ImageFont.truetype(os.path.join(FONT_DIR, name), size)


F_LABEL = font("MiSans-Medium.ttf", 20)
F_SUB = font("MiSans-Regular.ttf", 16)


# --------------------------------------------------------------------------
# 每个 variant 返回一串绘图指令：
#   ("rect", x, y, w, h, radius)   黑块，radius>0 用圆角
#   ("circle", x, y, d)            圆
#   ("white", x, y, w, h)          白块（眼睛/嘴，画在最后=画在最上层）
# --------------------------------------------------------------------------
def v_current():
    """A：现状（小黑点人，无脸）"""
    return [
        ("circle", 11, 1, 10),
        ("rect", 14, 11, 4, 11, 0),
        ("rect", 6, 13, 20, 3, 0),
        ("rect", 11, 21, 4, 8, 0),
        ("rect", 17, 21, 4, 8, 0),
    ]


def v_bighead_eyes():
    """B：大头 + 眼睛 + 细身子"""
    return [
        ("circle", 10, 2, 13),
        ("rect", 12, 15, 8, 9, 3),
        ("rect", 6, 18, 20, 3, 0),
        ("rect", 12, 24, 3, 7, 0),
        ("rect", 17, 24, 3, 7, 0),
        ("white", 13, 7, 2, 2),
        ("white", 18, 7, 2, 2),
    ]


def v_round_body():
    """C：大头 + 眼睛 + 圆胖身子 + 短腿"""
    return [
        ("circle", 10, 2, 13),
        ("rect", 11, 15, 10, 8, 5),
        ("rect", 5, 17, 22, 3, 0),
        ("rect", 11, 23, 4, 8, 0),
        ("rect", 17, 23, 4, 8, 0),
        ("white", 13, 7, 2, 2),
        ("white", 18, 7, 2, 2),
    ]


def v_ahoge():
    """D：C + 头顶呆毛 + 笑嘴"""
    return [
        ("rect", 15, 0, 2, 3, 0),        # 呆毛
        ("circle", 10, 2, 13),
        ("rect", 11, 15, 10, 8, 5),
        ("rect", 5, 17, 22, 3, 0),
        ("rect", 11, 23, 4, 8, 0),
        ("rect", 17, 23, 4, 8, 0),
        ("white", 13, 7, 2, 2),           # 左眼
        ("white", 18, 7, 2, 2),           # 右眼
        ("white", 14, 11, 4, 1),          # 嘴（一条白线）
    ]


def v_wave():
    """E：招手（肩部加连接件，手臂不贴到头上）"""
    return [
        ("circle", 7, 1, 12),
        ("rect", 9, 13, 10, 8, 5),
        ("rect", 3, 16, 7, 3, 0),         # 左臂平伸
        ("rect", 17, 12, 5, 3, 0),        # 肩连接
        ("rect", 20, 4, 3, 10, 0),        # 右臂举高
        ("circle", 19, 0, 5),             # 举起的手
        ("rect", 9, 21, 4, 8, 0),
        ("rect", 14, 21, 4, 8, 0),
        ("white", 10, 6, 2, 2),
        ("white", 13, 6, 2, 2),
    ]


# --------------------------------------------------------------------------
# 第二轮：细化
# --------------------------------------------------------------------------
def _base(hands=True, arm_len=6):
    """共用骨架：大头 + 3x3 大眼 + 圆胖身 + 短手（末端圆手）"""
    ops = [
        ("circle", 9, 3, 14),              # 头
        ("rect", 11, 17, 10, 8, 5),        # 圆身子
        ("rect", 5, 19, arm_len, 3, 0),    # 左臂
        ("rect", 21, 19, arm_len, 3, 0),   # 右臂
        ("rect", 11, 25, 4, 7, 0),         # 左腿
        ("rect", 17, 25, 4, 7, 0),         # 右腿
        ("white", 12, 8, 3, 3),            # 左眼
        ("white", 17, 8, 3, 3),            # 右眼
    ]
    if hands:
        ops += [("circle", 1, 17, 6), ("circle", 25, 17, 6)]
    return ops


def v_p1():
    """P1：大眼 + 圆手 + 呆毛 + 笑嘴"""
    ops = _base()
    ops += [
        ("rect", 15, 0, 2, 4, 0),         # 呆毛
        ("white", 14, 13, 4, 1),          # 笑嘴
    ]
    return ops


def v_p2():
    """P2：P1 去掉笑嘴"""
    ops = _base()
    ops += [("rect", 15, 0, 2, 4, 0)]
    return ops


def v_p3():
    """P3：P1 去掉呆毛（只留大眼 + 圆手）"""
    ops = _base()
    ops += [("white", 14, 13, 4, 1)]
    return ops


def v_p4():
    """P4：P1 去掉圆手（方手）"""
    ops = _base(hands=False)
    ops += [
        ("rect", 15, 0, 2, 4, 0),
        ("white", 14, 13, 4, 1),
    ]
    return ops


def v_final():
    """★ 最终版（已落地到 sok_main.c）：D 的底子 + 手臂缩短到 18"""
    return [
        ("circle", 10, 2, 13),
        ("rect", 15, 0, 2, 3, 0),         # 呆毛
        ("rect", 11, 15, 10, 8, 4),       # 身子（圆角 4）
        ("rect", 7, 17, 18, 3, 0),        # 手臂
        ("rect", 11, 23, 4, 8, 0),        # 左腿
        ("rect", 17, 23, 4, 8, 0),        # 右腿
        ("white", 13, 7, 2, 2),           # 左眼
        ("white", 18, 7, 2, 2),           # 右眼
        ("white", 14, 11, 4, 1),          # 笑嘴
    ]


VARIANTS = [
    ("A 现状", "无脸小黑人", v_current),
    ("B 大头+眼", "头 13px 眼睛 2x2", v_bighead_eyes),
    ("C 圆胖身", "身子 radius5 短腿", v_round_body),
    ("D 加呆毛", "C + 呆毛 + 笑嘴", v_ahoge),
    ("E 招手", "肩连接 举手", v_wave),
    ("★ 最终版", "呆毛+方眼+笑嘴+圆身 短臂", v_final),
    ("P1 大眼圆手", "3x3 眼 + 圆手 + 呆毛", v_p1),
    ("P3 无呆毛", "P1 去掉呆毛", v_p3),
    ("P4 方手", "P1 去掉圆手", v_p4),
]


def draw_person(d, ox, oy, ops):
    black = []
    white = []
    for op in ops:
        if op[0] == "rect":
            _, x, y, w, h, r = op
            if r > 0:
                black.append(("rect", x, y, w, h, r))
            else:
                black.append(("rect", x, y, w, h, 0))
        elif op[0] == "circle":
            _, x, y, d_ = op
            black.append(("circle", x, y, d_))
        else:
            white.append(op)

    for b in black:
        if b[0] == "rect":
            _, x, y, w, h, r = b
            box = [ox + x, oy + y, ox + x + w - 1, oy + y + h - 1]
            if r > 0:
                d.rounded_rectangle(box, radius=min(r, w // 2, h // 2), fill=0)
            else:
                d.rectangle(box, fill=0)
        else:
            _, x, y, d_ = b
            d.ellipse([ox + x, oy + y, ox + x + d_ - 1, oy + y + d_ - 1], fill=0)

    for wop in white:
        _, x, y, w, h = wop
        d.rectangle([ox + x, oy + y, ox + x + w - 1, oy + y + h - 1], fill=255)


def main():
    cols = 3
    n = len(VARIANTS)
    rows = (n + cols - 1) // cols
    pad = 18
    label_h = 30
    sub_h = 22
    tile = SIZE * ZOOM
    czoom = 5
    cell_tile = CELL * czoom

    W = cols * tile + (cols + 1) * pad
    row_h = label_h + tile + sub_h + cell_tile + pad
    H = rows * row_h + (rows + 1) * pad
    img = Image.new("L", (W, H), 255)
    d = ImageDraw.Draw(img)

    for i, (name, sub, fn) in enumerate(VARIANTS):
        r, c = divmod(i, cols)
        x0 = pad + c * (tile + pad)
        y0 = pad + r * row_h

        # --- 单独放大（32x32 画布 -> NEAREST 放大）---
        small = Image.new("L", (SIZE, SIZE), 255)
        sd = ImageDraw.Draw(small)
        draw_person(sd, 0, 0, fn())
        t = small.resize((tile, tile), Image.NEAREST)
        td = ImageDraw.Draw(t)
        # 画布边框，方便看出 32x32 的边界
        td.rectangle([0, 0, tile - 1, tile - 1], outline=180)
        img.paste(t, (x0, y0 + label_h))

        d.text((x0, y0 + 4), name, font=F_LABEL, fill=0)
        d.text((x0, y0 + label_h + tile + 4), sub, font=F_SUB, fill=60)

        # --- 放进棋盘格（44px）看实际观感 ---
        small_c = Image.new("L", (CELL, CELL), 255)
        cd = ImageDraw.Draw(small_c)
        cd.rectangle([0, 0, CELL - 1, CELL - 1], outline=200)
        cd.rectangle([1, 1, CELL - 2, CELL - 2], outline=230)
        draw_person(cd, 6, 6, fn())
        ct = small_c.resize((cell_tile, cell_tile), Image.NEAREST)
        img.paste(ct, (x0, y0 + label_h + tile + sub_h + pad))

    img.save(OUT)
    print("saved", OUT, img.size)


if __name__ == "__main__":
    main()
