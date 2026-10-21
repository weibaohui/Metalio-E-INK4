#!/usr/bin/env python3
"""知识点卡片 → 设备端 cards.db 打包器 v2。

支持两种卡片格式：
  A. 经典语句卡：英文 frontmatter(title/book/chapter)+ 节 原文/出处/释义/解读/关联
  B. 知识点卡：中文 frontmatter(书名/章节/作者/类型/标签)+ H1 标题 + 开篇核心段 +
     节 详细展开/原文摘录/启发与应用/关联

v2 变化：
  - 分组两级化：collection(顶层目录) → book(直接父目录)，适配 4.4 万卡规模
  - 按卡 zlib deflate 压缩（设备端经 dynapp_zlib_inflate 解压）
  - 开篇核心段(第一个 ## 之前的内容)以 \\x03 标记渲染为强调块

用法: pack_cards.py <卡片库根目录> <输出 cards.db>
"""
import os
import re
import sys
import struct
import zlib
from collections import OrderedDict

QUOTE_SECTIONS = {"原文", "要点", "原文摘录", "全书一句话"}
MAX_BODY_RAW = 96 * 1024
ALIAS = {"书名": "book", "章节": "chapter", "来源": "source"}


def parse_card(path: str):
    with open(path, "r", encoding="utf-8-sig") as f:
        text = f.read()
    fm = {}
    body = text
    if text.startswith("---"):
        end = text.find("\n---", 3)
        if end > 0:
            for line in text[4:end].splitlines():
                m = re.match(r"^([^:\s][^:]*):\s*(.*)$", line)
                if m:
                    key = ALIAS.get(m.group(1).strip(), m.group(1).strip())
                    fm[key] = m.group(2).strip()
            body = text[end + 4:].lstrip("\r\n")
    if "book" not in fm:
        # 格式 C:全书知识卡(无 frontmatter,H1 各式各样)
        m1 = re.match(r"#\s*(.+?)\s*$", body)
        h1 = m1.group(1) if m1 else ""
        mb = re.search(r"《(.+?)》", h1)
        fm["book"] = mb.group(1) if mb else os.path.basename(os.path.dirname(path))
        tcore = re.sub(r"^《(.+?)》", "", h1)
        tcore = re.sub(r"(知识卡|读书笔记|知识要点|要点卡|知识手册)\s*$", "", tcore)
        fm["title"] = tcore.strip(" -—") or os.path.splitext(os.path.basename(path))[0]
        mmeta = re.search(r"^>\s*(?:分类：.+)$", body, re.M)
        if mmeta:
            fm["chapter"] = re.sub(r"\|\s*主题：.*$", "", mmeta.group(0)[2:].strip())
            body = re.sub(r"^>\s*(?:分类：.+)[ \t]*\r?\n?", "", body, count=1, flags=re.M)

    # H1 标题(无 title 键时用)
    m1 = re.search(r"^#\s+(.+?)\s*$", body, re.M)
    title = fm.get("title") or (m1.group(1) if m1 else "")
    if not title:
        return None

    # 开篇核心段(第一个 ## 之前的非空内容)
    pre_end = body.find("\n## ")
    core = body[:pre_end] if pre_end >= 0 else body
    core = re.sub(r"^#\s+.+?\s*$", "", core, count=1, flags=re.M)
    core_lines = [re.sub(r"\*\*", "", clean(l)) for l in core.splitlines() if clean(l)]
    core_text = "\n".join(core_lines).strip()

    sections = []
    cur_name, cur_lines = None, []
    for line in body.splitlines():
        m = re.match(r"^##+\s*(.+?)\s*$", line)
        if m:
            if cur_name is not None:
                sections.append((cur_name, cur_lines))
            cur_name, cur_lines = m.group(1), []
        elif cur_name is not None:
            cur_lines.append(line)
    if cur_name is not None:
        sections.append((cur_name, cur_lines))
    return fm, title, core_text, sections


def clean(line: str) -> str:
    line = re.sub(r"^>\s?", "", line)
    return line.replace("**", "").rstrip()


def extract_links(lines):
    out = []
    for line in lines:
        for m in re.finditer(r"\[\[([^\]\[]+)\]\]", line):
            out.append(m.group(1).strip())
    return out


def main(root: str, out_path: str):
    cards = []
    for dirpath, _dirnames, filenames in os.walk(root):
        for fn in sorted(filenames):
            if not fn.endswith(".md") or fn in ("README.md", "进度.md"):
                continue
            path = os.path.join(dirpath, fn)
            rel_id = os.path.relpath(path, root)[:-3].replace("\\", "/")
            parsed = parse_card(path)
            if parsed is None:
                continue
            fm, title, core_text, sections = parsed
            collection = os.path.relpath(dirpath, root).split(os.sep)[0]
            group = os.path.basename(os.path.normpath(dirpath))
            book = fm.get("book") or group
            chapter = fm.get("chapter", "")
            cards.append({
                "id": rel_id, "dir": os.path.relpath(dirpath, root),
                "collection": collection, "group": group,
                "book": book, "title": title, "chapter": chapter,
                "core": core_text, "sections": sections, "raw_links": [],
            })
            for name, lines in sections:
                if name == "关联":
                    cards[-1]["raw_links"] = extract_links(lines)
    if not cards:
        print("no cards found"); sys.exit(1)

    id2idx = {c["id"]: i for i, c in enumerate(cards)}
    base_map = {}
    for i, c in enumerate(cards):
        base_map.setdefault(c["id"].split("/")[-1], []).append(i)

    # 分组:collection → group → 卡(文件名序)
    colls = OrderedDict()
    for c in sorted(cards, key=lambda c: (c["collection"], c["group"], c["id"])):
        colls.setdefault(c["collection"], OrderedDict()).setdefault(
            c["group"], []).append(c)
    coll_names = sorted(colls.keys())

    ordered = []
    for cn in coll_names:
        for gn in colls[cn]:
            ordered += colls[cn][gn]
    idx_of = {c["id"]: i for i, c in enumerate(ordered)}
    book_recs_map = OrderedDict()   # (coll_idx, group) → rec
    for cn in coll_names:
        for gn in colls[cn]:
            first = idx_of[colls[cn][gn][0]["id"]]
            book_recs_map[(cn, gn)] = {
                "name_off": None, "first": first, "count": len(colls[cn][gn]),
                "coll": coll_names.index(cn),
            }

    # ---- blobs ----
    strings = bytearray()
    def add_str(s):
        nonlocal strings
        off = len(strings)
        strings += s.encode("utf-8") + b"\x00"
        return off

    body = bytearray()
    links = bytearray()
    card_recs = []
    resolved_total = 0
    for c in ordered:
        cbody = bytearray()
        if c["core"]:
            cbody += b"\x03" + "核心".encode("utf-8") + c["core"].encode("utf-8") + b"\n"
        first = True
        for name, lines in c["sections"]:
            if name == "关联":
                continue
            marker = b"\x02" if (first and name in QUOTE_SECTIONS) else b"\x01"
            first = False
            cbody += marker + name.encode("utf-8") + b"\n"
            for ln in lines:
                ln = clean(ln)
                if ln:
                    cbody += ln.encode("utf-8") + b"\n"
        raw = bytes(cbody)
        comp = zlib.compress(raw, 6)
        body_off = len(body)
        body += comp
        link_ids, seen = [], set()
        for raw_l in c["raw_links"]:
            target = None
            if raw_l in id2idx:
                target = id2idx[raw_l]
            else:
                cand_dir = [i for i, cc in enumerate(cards)
                            if cc["dir"] == c["dir"] and cc["id"].split("/")[-1] == raw_l]
                if len(cand_dir) == 1:
                    target = idx_of[cards[cand_dir[0]]["id"]]
                else:
                    cand_base = base_map.get(raw_l, [])
                    if len(cand_base) == 1:
                        target = idx_of[cards[cand_base[0]]["id"]]
            if target is None or target == idx_of[c["id"]] or target in seen:
                continue
            seen.add(target)
            link_ids.append(idx_of[cards[target]["id"]])
        resolved_total += len(link_ids)
        links_off = len(links)
        for t in link_ids:
            links += struct.pack("<I", t)
        card_recs.append({
            "title_off": add_str(c["title"]),
            "chapter_off": add_str(c["chapter"]),
            "body_off": body_off, "body_len": len(comp), "raw_len": len(raw),
            "links_off": links_off, "link_count": len(link_ids),
            "book_id": 0,   # 占位,书表确定后回填
            "coll": coll_names.index(c["collection"]),
            "group_key": (c["collection"], c["group"]),
        })

    # ---- 书表(组)与回填 book_id ----
    book_recs = []
    book_key2id = {}
    for (cn, gn), info in book_recs_map.items():
        coll_idx = info["coll"]
        key = (coll_idx, gn)
        if key not in book_key2id:
            book_key2id[key] = len(book_recs)
            book_recs.append({"name_off": None, "first": info["first"],
                              "count": info["count"], "coll": coll_idx,
                              "name": gn})
        info["book_slot"] = book_key2id[key]
    for i, c in enumerate(ordered):
        slot = book_recs_map[(c["collection"], c["group"])]["book_slot"]
        card_recs[i]["book_id"] = slot
    for r in book_recs:
        r["name_off"] = add_str(r["name"])

    # ---- collection 表 ----
    coll_recs = [{"name_off": add_str(cn)} for cn in coll_names]

    # ---- 布局(v2 header 64B) ----
    header_size = 64
    card_table_off = header_size
    card_table_size = 32 * len(ordered)
    book_table_off = card_table_off + card_table_size
    book_table_size = 16 * len(book_recs)
    coll_table_off = book_table_off + book_table_size
    coll_table_size = 8 * len(coll_recs)
    strings_off = coll_table_off + coll_table_size
    links_off = strings_off + len(strings)
    body_off = links_off + len(links)
    total = body_off + len(body)

    out = bytearray(total)
    out[0:4] = b"KNOW"
    struct.pack_into("<HH", out, 4, 2, header_size)
    struct.pack_into("<II", out, 8, len(ordered), len(book_recs))
    struct.pack_into("<II", out, 16, card_table_off, book_table_off)
    struct.pack_into("<II", out, 24, coll_table_off, len(coll_recs))
    struct.pack_into("<IIII", out, 32, strings_off, links_off, body_off, total)
    for i, r in enumerate(card_recs):
        o = card_table_off + 32 * i
        struct.pack_into("<IIIII", out, o, r["title_off"], r["chapter_off"],
                         r["body_off"], r["body_len"], r["raw_len"])
        struct.pack_into("<IIHHH", out, o + 20, r["links_off"],
                         r["link_count"], r["book_id"], r["coll"], 0)
    for i, r in enumerate(book_recs):
        o = book_table_off + 16 * i
        struct.pack_into("<IIIH", out, o, r["name_off"], r["first"],
                         r["count"], r["coll"])
    for i, r in enumerate(coll_recs):
        struct.pack_into("<I", out, coll_table_off + 8 * i, r["name_off"])
    out[strings_off:strings_off + len(strings)] = strings
    out[links_off:links_off + len(links)] = links
    out[body_off:body_off + len(body)] = body

    with open(out_path, "wb") as f:
        f.write(out)

    comp_total = sum(1 for _ in card_recs)
    print(f"卡片: {len(ordered)} | collection: {len(coll_recs)} | 书(组): {len(book_recs)}"
          f" | 关联解析: {resolved_total}")
    print(f"db: {total/1024/1024:.1f} MB")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__); sys.exit(1)
    main(sys.argv[1], sys.argv[2])
