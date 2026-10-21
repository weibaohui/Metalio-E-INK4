#!/usr/bin/env python3
# tools/poetry_pack/pack_poems.py —— 唐诗宋词 App 数据打包器
#
# 输入:
#   /tmp/poetry_corpus/{唐诗三百首,宋词三百首,千家诗,诗经}.json  (chinese-poetry 语料, 繁体)
#   知识卡库 课本里的古诗词/*.md + 遇见最美的宋词.md            (赏析层, 简体)
# 输出:
#   tools/poetry_app/data/poems.db           设备数据库(逐条 deflate)
#   tools/poetry_app/data/poetry_charset.txt 补字集(优先序: 唐宋千家全保 -> 诗经按频 -> 赏析)
#   tools/poetry_app/data/cut_chars.json     预算外被砍的字(仅供排查)
# 约束: font_data 分区 5MB 只余 ~257KB, 按 ~214B/字只能补 ~1190 字(30@2 一档),
#   因此含预算外生僻字的诗经篇目整首剔除(保证正文零缺字); 扩展区古字先经
#   zhcdict 反查换成繁体原字(BMP, MiSans 可栅格化)。
#
# db 格式 (little-endian):
#   Header 40B: magic 'PMDB' | ver u16 | hdr_len u16=40 | coll_count u32
#               | poem_count u32 | appr_count u32 | blob_off u64
#   CollRec 32B × n: name[28] utf8z | poem_start u16 | poem_count u16
#   PoemRec 16B × n: blob_off u32 | z_len u16 | raw_len u16
#                    | coll u8 | appr u8(0xFF=无) | pad[6]
#   ApprRec 16B × n: blob_off u32 | z_len u16 | raw_len u16
#                    | poem u16(0xFFFF=未挂接) | cat u8(0课内讲解 1独立讲解 2词牌故事) | pad[5]
#   Blob: zlib deflate 串联
#   诗文 blob 明文: title\0author\0dynasty\0kind\0body(\n 分行)
#   赏析 blob 明文: title\0text(\n 分段)
import json, os, re, struct, sys, zlib
from zhconv import convert

HERE = os.path.dirname(os.path.abspath(__file__))
CORPUS = os.environ.get("POETRY_CORPUS", "/tmp/poetry_corpus")
CARDS = "/Users/weibh/projects/dockerfiles/书籍/知识点卡片/知识卡片/文学小说"
OUT_DIR = os.path.join(HERE, "..", "poetry_app", "data")

UI_CHARS = ("每日一诗随机一首书目浏览收藏返回退出上一首下一首首页第页共"
            "唐诗宋词三百首千家诗经课本讲解牌故事作者朝代有赏析触屏点击查看"
            "已加入未收夹空首诗还没有任何换一首长按侧键也能翻翻到头了没更多"
            "打开中载失败数据损坏重新安装应用格式版本不符朗今日显示天缺字")


def J(x):
    """把语料里偶尔混进的 dict/list 元素安全拍平成字符串。"""
    if x is None:
        return ""
    if isinstance(x, str):
        return x
    if isinstance(x, (int, float)):
        return str(x)
    return "".join(J(i) for i in x)


REPLACE_MAP = {}


def clean(s):
    """转简体、去 markdown 痕迹、规范化空白。"""
    s = convert(s, "zh-cn")
    s = s.replace("\u3000", " ")
    for k, v in REPLACE_MAP.items():
        s = s.replace(k, v)
    return s


def strip_author(s):
    s = re.sub(r"[（\[〔]\s*(唐|宋|金|元|明|清|汉|魏晋|南北朝|先秦|近现代|当代)\s*[）\]〕]\s*", "", s)
    return s.strip()


def guess_dynasty(s):
    m = re.search(r"[（\[〔]\s*(唐|宋|金|元|明|清|汉|魏晋|南北朝|先秦|近现代|当代)\s*[）\]〕]", s)
    if m:
        return m.group(1)
    return ""


POEMS = []   # dict(title, author, dynasty, kind, lines[], coll, appr=0xFF)
APPRS = []   # dict(title, text, poem=0xFFFF, cat)


def load_tang():
    data = json.load(open(os.path.join(CORPUS, "唐诗三百首.json"), encoding="utf-8"))
    for po in data:
        tags = [J(t) for t in po.get("tags", [])]
        kind = ""
        for t in tags:
            if re.match(r"^[五六七]言(绝句|律诗|古诗|乐府)$|^[乐府杂言五七]言.*|词$", t):
                kind = t
                break
        if not kind and tags:
            kind = tags[-1]
        POEMS.append(dict(title=clean(J(po["title"])), author=clean(J(po["author"])),
                          dynasty="唐", kind=clean(kind), appr=0xFF,
                          lines=[clean(J(x)) for x in po["paragraphs"] if clean(J(x))],
                          coll=0))


def load_song():
    data = json.load(open(os.path.join(CORPUS, "宋词三百首.json"), encoding="utf-8"))
    for po in data:
        rhythmic = clean(J(po.get("rhythmic", ""))) or "词"
        body = [clean(J(x)) for x in po["paragraphs"] if clean(J(x))]
        first = re.sub(r"[，。、！？；：]", "", body[0])[:7] if body else ""
        POEMS.append(dict(title=rhythmic, author=clean(J(po.get("author", ""))),
                          dynasty="宋", kind="词", appr=0xFF,
                          lines=body, coll=1, sub=first))


def load_qian():
    data = json.load(open(os.path.join(CORPUS, "千家诗.json"), encoding="utf-8"))
    for g in data["content"]:
        for po in g["content"]:
            dyn = guess_dynasty(J(po["author"]))
            POEMS.append(dict(title=clean(J(po["chapter"])), author=clean(strip_author(J(po["author"]))),
                              dynasty=dyn, kind=clean(J(g.get("type", ""))), appr=0xFF,
                              lines=[clean(J(x)) for x in po["paragraphs"] if clean(J(x))],
                              coll=2))


def load_shijing():
    data = json.load(open(os.path.join(CORPUS, "诗经.json"), encoding="utf-8"))
    for po in data:
        chap = clean(J(po.get("chapter", "")))
        sect = clean(J(po.get("section", "")))
        src = f"{chap}·{sect}" if sect and sect != chap else chap
        POEMS.append(dict(title=clean(J(po["title"])), author=src, dynasty="先秦", kind="诗经", appr=0xFF,
                          lines=[clean(J(x)) for x in po["content"] if clean(J(x))],
                          coll=3))


def parse_lesson_md(path):
    txt = open(path, encoding="utf-8").read()
    txt = re.sub(r"^---\n.*?\n---\n", "", txt, flags=re.S)
    txt = txt.replace("\r\n", "\n")
    # 标题: 一级标题去掉书名号说明
    m = re.match(r"#\s*(.+)", txt)
    title = m.group(1).strip() if m else os.path.basename(path).removesuffix(".md")
    # 段落: 去掉二级标题井号, 保留引用
    lines = []
    for ln in txt.split("\n"):
        ln = ln.strip()
        if not ln or ln.startswith("#"):
            continue
        lines.append(ln)
    return clean(title), "\n".join(lines)


def load_appr_lessons():
    d = os.path.join(CARDS, "课本里的古诗词")
    for fn in sorted(os.listdir(d)):
        if not fn.endswith(".md"):
            continue
        title, text = parse_lesson_md(os.path.join(d, fn))
        m = re.match(r"^\d+[-.、]?(.+?)[：:]", fn)
        poem_hint = clean(m.group(1)) if m else title.split("：")[0].split("：")[0]
        poem_hint = re.sub(r"[·•].*$", "", poem_hint).strip()
        APPRS.append(dict(title=title, text=text, cat=0, poem=0xFFFF, hint=poem_hint))


def load_appr_cipai():
    path = os.path.join(CARDS, "遇见最美的宋词 - 赵小峦 _ 行云.md")
    txt = open(path, encoding="utf-8").read().replace("\r\n", "\n")
    secs = re.split(r"^###\s+", txt, flags=re.M)
    for s in secs[1:]:
        lines = [l.strip() for l in s.strip().split("\n") if l.strip()]
        title = clean(lines[0].lstrip("#").strip())
        text = "\n".join(l for l in lines[1:] if not l.startswith("#"))
        m = re.search(r"《(.+?)》", title)
        APPRS.append(dict(title=title, text=clean(text), cat=2, poem=0xFFFF,
                          hint=clean(m.group(1)) if m else ""))


def link_apprs():
    """标题/首句模糊挂接赏析到诗。"""
    by_title = {}
    for i, p in enumerate(POEMS):
        by_title.setdefault(p["title"], i)
    titles = list(by_title.keys())
    for a in APPRS:
        hint = a.pop("hint", "")
        if not hint:
            continue
        idx = by_title.get(hint)
        if idx is None:  # 前缀匹配
            cands = [t for t in titles if t.startswith(hint) or hint.startswith(t)]
            if len(cands) == 1:
                idx = by_title[cands[0]]
        if idx is None and len(hint) >= 4:  # 诗句反查: 课本讲解引文里找正文含 hint 片段的诗
            frag = hint
            for t in titles:
                if frag in "".join(POEMS[by_title[t]]["lines"]).replace("，", "").replace("。", ""):
                    idx = by_title[t]
                    break
        if idx is not None:
            a["poem"] = idx
            if POEMS[idx]["appr"] == 0xFF:
                POEMS[idx]["appr"] = APPRS.index(a)


def poem_plain(p):
    body = "\n".join(p["lines"])
    sub = p.get("sub", "")
    author = p["author"] + (f"《{sub}》" if sub else "")
    parts = [p["title"], author, p["dynasty"], p["kind"], body]
    return "\x00".join(parts).encode("utf-8")


def appr_plain(a):
    return f"{a['title']}\x00{a['text']}".encode("utf-8")


def load_ext_char_map():
    """zhcdict 反查: 扩展区字(无字形) -> 繁体原字(BMP, MiSans 可栅格化)。"""
    import zhconv
    dd = json.load(open(os.path.join(os.path.dirname(zhconv.__file__), "zhcdict.json"),
                        encoding="utf-8"))
    rev = {}
    for k, v in dd["zh2Hans"].items():
        if len(k) == 1 and len(v) == 1 and ord(v) > 0x20000 and k != v:
            rev.setdefault(v, []).append(k)
    return rev


def pack_budget_charset(have, budget_bytes=250_000, est_per_glyph=214):
    """字数预算内装哪些字: 唐/宋/千家全保 -> 诗经按字频 -> 赏析独有。
    返回 (charset 优先序, 被剔除的诗下标集)。"""
    poem_text = appr_text = ""
    per_poem = []
    for i, p in enumerate(POEMS):
        t = p["title"] + p["author"] + p["dynasty"] + p["kind"] + "".join(p["lines"])
        per_poem.append(t)
        poem_text += t
    for a in APPRS:
        appr_text += a["title"] + a["text"]
    allch = set(poem_text + appr_text + UI_CHARS)
    # 全程确定性排序(码位), 避免 set 迭代序让每次打包剔除不同的诗
    missing = sorted(c for c in allch if ord(c) > 0x2000 and ord(c) not in have)

    freq = {}
    for c in poem_text:
        freq[c] = freq.get(c, 0) + 1
    core_text = "".join(per_poem[i] for i, p in enumerate(POEMS) if p["coll"] < 3)
    must = [c for c in missing if c in core_text]
    sj = [c for c in missing if c not in must and c in poem_text]
    sj.sort(key=lambda c: (-freq.get(c, 0), ord(c)))
    appr_only = [c for c in missing if c not in poem_text]
    appr_only.sort(key=lambda c: (-freq.get(c, 0), ord(c)))
    cap = budget_bytes // est_per_glyph
    keep = must + sj[:max(0, cap - len(must))] + appr_only
    keep_set = set(keep)
    cut = set(missing) - keep_set
    # 剔除含被砍字的诗(按集序保唐宋千, 诗经牺牲)
    drop = set()
    for i, t in enumerate(per_poem):
        if cut & set(t):
            drop.add(i)
    return keep, drop, cut


def main():
    rm = os.path.join(OUT_DIR, "char_replace.json")
    if os.path.isfile(rm):
        REPLACE_MAP.update(json.load(open(rm, encoding="utf-8")))
        print(f"替换表 {len(REPLACE_MAP)} 项")
    load_tang()
    load_song()
    load_qian()
    load_shijing()
    load_appr_lessons()
    load_appr_cipai()
    # 挂接顺序: 先课内讲解(有明确诗题), 再词牌故事(词牌名即 title 匹配)
    for a in APPRS:
        if a["cat"] == 2:
            hint = a["title"].split("》")[0].replace("《", "").strip()
            hint = re.sub(r"^[0-9.\s]+", "", hint)   # 去掉「5.」这类序号
            a["hint"] = hint
    link_apprs()

    # 扩展区古字换繁体原字(设备可渲染), 再做字数预算过滤
    rev = load_ext_char_map()
    def sub_ext(txt):
        for c, cands in rev.items():
            if c in txt:
                txt = txt.replace(c, cands[0])
        return txt
    n_sub = 0
    for p in POEMS:
        t2 = sub_ext(p["title"]); l2 = [sub_ext(x) for x in p["lines"]]
        if t2 != p["title"] or l2 != p["lines"]:
            n_sub += 1
            p["title"], p["lines"] = t2, l2
    for a in APPRS:
        a["text"] = sub_ext(a["text"])
    print(f"扩展区古字换繁体: 涉及 {n_sub} 首, 映射 {len(rev)} 字")

    # 字库缺字集与预算内取舍
    fontpack = "/Users/weibh/Desktop/Metalio-E-INK4/tools/fontpack/fonts_misans_25_30.fontpack"
    have = set()
    with open(fontpack, "rb") as f:
        _, _, _, total, idx_off, _, _ = struct.unpack("<4sHHIQQI", f.read(32))
        f.seek(idx_off)
        for _ in range(total):
            key, _, _, _ = struct.unpack("<QIHH", f.read(16))
            if ((key >> 16) & 0xFFFF) == 30 and (key & 0xFFFF) == 2:
                have.add(key >> 32)
    charset, drop, cut = pack_budget_charset(have)

    # 剔除后重建诗表与集合表
    POEMS[:] = [p for i, p in enumerate(POEMS) if i not in drop]
    coll_names = ["唐诗三百首", "宋词三百首", "千家诗", "诗经"]
    starts, counts = [], []
    for c in range(4):
        idxs = [i for i, p in enumerate(POEMS) if p["coll"] == c]
        starts.append(idxs[0] if idxs else 0)
        counts.append(len(idxs))
    print(f"预算过滤: 剔除 {len(drop)} 首, 保留 {len(POEMS)} 首")

    blobs = bytearray()
    precs, arecs = [], []
    for p in POEMS:
        raw = poem_plain(p)
        z = zlib.compress(raw, 9)
        precs.append(dict(off=len(blobs), z=len(z), raw=len(raw),
                          coll=p["coll"], appr=p["appr"]))
        blobs += z
    for a in APPRS:
        raw = appr_plain(a)
        z = zlib.compress(raw, 9)
        arecs.append(dict(off=len(blobs), z=len(z), raw=len(raw),
                          poem=a["poem"], cat=a["cat"]))
        blobs += z

    n_coll = 4
    header = struct.pack("<4sHHIIIQ12x", b"PMDB", 1, 40, n_coll,
                         len(POEMS), len(APPRS),
                         40 + 32 * n_coll + 16 * len(POEMS) + 16 * len(APPRS))
    coll_tbl = b""
    for c in range(4):
        nm = coll_names[c].encode("utf-8")[:27]
        coll_tbl += nm.ljust(28, b"\x00") + struct.pack("<HH", starts[c], counts[c])

    os.makedirs(OUT_DIR, exist_ok=True)
    with open(os.path.join(OUT_DIR, "poems.db"), "wb") as f:
        f.write(header)
        f.write(coll_tbl)
        for r in precs:
            f.write(struct.pack("<IHHBB6x", r["off"], r["z"], r["raw"],
                                r["coll"], r["appr"]))
        for r in arecs:
            f.write(struct.pack("<IHHHB5x", r["off"], r["z"], r["raw"],
                                r["poem"], r["cat"]))
        f.write(blobs)

    with open(os.path.join(OUT_DIR, "poetry_charset.txt"), "w", encoding="utf-8") as f:
        f.write("".join(charset))
    if cut:
        json.dump({c: "\u25a1" for c in cut}, open(os.path.join(OUT_DIR, "cut_chars.json"), "w", encoding="utf-8"), ensure_ascii=False)

    linked = sum(1 for a in APPRS if a["poem"] != 0xFFFF)
    print(f"诗 {len(POEMS)} 首 (唐{counts[0]} 宋{counts[1]} 千家{counts[2]} 诗经{counts[3]})  "
          f"赏析 {len(APPRS)} 篇 (挂接 {linked})  poems.db {os.path.getsize(os.path.join(OUT_DIR,'poems.db'))} B  "
          f"补字 {len(charset)}")


if __name__ == "__main__":
    sys.exit(main())
