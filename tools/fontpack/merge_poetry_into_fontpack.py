#!/usr/bin/env python3
"""把诗词语料缺字合并进 EFNT fontpack（30@2 一档，预算内尽量补全）。

唐诗宋词 App 的诗题/作者/正文/赏析统一用 fontpack_lv_font_get(30, 2) 渲染，
因此只补 30@2；语料缺字集来自 tools/poetry_pack/pack_poems.py 输出的
poetry_charset.txt（按语料出现频次排序可选）。

font_data 分区 5MB、原包 ~4.98MB，余量 ~257KB，1293 字 × ~197B ≈ 255KB，
恰好卡线；--budget-bytes 超限时按「频次高优先」截断并把截掉的字打到 stderr。

示例：
  .venv/bin/python merge_poetry_into_fontpack.py \
    --base fonts/MiSans-Mixed/fonts_misans_25_30.fontpack \
    --chars ../poetry_app/data/poetry_charset.txt \
    -o fonts/MiSans-Mixed/fonts_misans_25_30.fontpack --verify
"""

from __future__ import annotations

import argparse
import struct
import time
from pathlib import Path

import freetype

from build_fontpack import (
    GLYPH_META_SIZE,
    GlyphItem,
    make_key,
    render_glyph,
    verify_fontpack,
    write_fontpack,
)
from merge_math_into_fontpack import load_fontpack_items

ROOT = Path(__file__).resolve().parent
DEFAULT_MISANS = ROOT.parent / "ttf-fonts/MiSans/MiSans-Light.ttf"


def main() -> None:
    ap = argparse.ArgumentParser(description="Merge poetry charset into EFNT fontpack")
    ap.add_argument("--base", type=Path, required=True)
    ap.add_argument("--misans-ttf", type=Path, default=DEFAULT_MISANS)
    ap.add_argument("--fallback-ttf", type=Path, action="append", default=[],
                    help="MiSans 缺字时依次尝试的兜底字库(可多次指定, 如 Hiragino Sans GB)")
    ap.add_argument("--chars-file", type=Path, required=True)
    ap.add_argument("--size", type=int, default=30)
    ap.add_argument("--bpp", type=int, default=2, choices=(1, 2, 4))
    ap.add_argument("--budget-bytes", type=int, default=255_000,
                    help="新增字形总字节上限（超出按序截断）")
    ap.add_argument("-o", "--output", type=Path, required=True)
    ap.add_argument("--verify", action="store_true")
    args = ap.parse_args()

    text = args.chars_file.read_text(encoding="utf-8")
    cps = list(dict.fromkeys(ord(c) for c in text if not c.isspace()))  # 保持词频序
    print(f"poetry charset cps={len(cps)}(词频序) size={args.size} bpp={args.bpp} "
          f"budget={args.budget_bytes}B")
    print(f"  ttf={args.misans_ttf}")

    print(f"load {args.base} …")
    t0 = time.time()
    items = load_fontpack_items(args.base)
    print(f"  base items={len(items)} ({time.time() - t0:.1f}s)")

    faces = [freetype.Face(str(args.misans_ttf))]
    for fb in args.fallback_ttf:
        if not fb.is_file():
            raise SystemExit(f"fallback TTF not found: {fb}")
        faces.append(freetype.Face(str(fb)))
    for f in faces:
        f.set_pixel_sizes(0, args.size)
    print(f"  fallback={[str(f) for f in args.fallback_ttf]}")
    existing = {make_key(it.codepoint, it.size, it.bpp) for it in items}
    fallback_used = 0

    added = skipped = missing = truncated = 0
    budget_left = args.budget_bytes
    t1 = time.time()
    for cp in cps:
        key = make_key(cp, args.size, args.bpp)
        if key in existing:
            skipped += 1
            continue
        if budget_left <= 0:
            truncated += 1
            print(f"  budget cut: U+{cp:04X} {chr(cp)}", file=__import__("sys").stderr)
            continue
        g = None
        for fi, face in enumerate(faces):
            if face.get_char_index(cp) == 0:
                continue
            g = render_glyph(face, cp, args.size, args.bpp)
            if g is not None:
                if fi > 0:
                    fallback_used += 1
                break
        if g is None:
            missing += 1
            print(f"  not in TTF: U+{cp:04X} {chr(cp)}", file=__import__("sys").stderr)
            continue
        items.append(g)
        existing.add(key)
        added += 1
        budget_left -= GLYPH_META_SIZE + len(g.bitmap) + 16

    print(f"merge poetry: +{added} (fallback {fallback_used}) skip_dup={skipped} "
          f"missing_glyph={missing} truncated={truncated} ({time.time() - t1:.1f}s) total={len(items)}")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    write_fontpack(items, args.output)
    if args.verify:
        # 抽检: 诗词高频补字(嗟/砧/谪/嵬) + 原有汉字/数学/Emoji 各一
        samples = [
            (ord("嗟"), args.size, args.bpp),
            (ord("砧"), args.size, args.bpp),
            (ord("嵬"), args.size, args.bpp),
            (0x4F60, 30, 2),   # 你 — 原汉字包仍在
            (0x2248, 28, 2),   # ≈ — 数学包仍在
        ]
        verify_fontpack(args.output, samples)
    print(f"wrote {args.output} ({args.output.stat().st_size} bytes) "
          f"+{args.output.stat().st_size - args.base.stat().st_size} vs base")


if __name__ == "__main__":
    main()
