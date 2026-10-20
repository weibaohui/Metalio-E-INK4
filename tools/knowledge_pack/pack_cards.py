#!/usr/bin/env python3
"""知识点卡片 → 设备端 cards.db 打包器。

用法: pack_cards.py <卡片库根目录> <输出 cards.db>

输出格式(little-endian, 单文件随机访问):
  header 64B: magic"KNOW" u16 ver u16 hdr_size u32 card_count u32 book_count u32
              card_table_off u32 book_table_off u32 strings_off u32
              links_off u32 body_off u32 total_size u32 + 24B reserved
  card record 24B: title_off u32 chapter_off u32 body_off u32 body_len u32
                   links_off u32 link_count u16 book_id u16
  book record 12B: name_off u32 first_card u32 card_count u32
  strings: NUL 结尾 UTF-8 池
  links:   每卡 link_count × u32 目标卡号
  body:    每卡正文,分节标记 '\x01'(普通节)/'\x02'(引用节:原文/要点)
"""
import os
import re
import sys
import struct
from collections import OrderedDict

QUOTE_SECTIONS = {"原文", "要点"}
MAX_BODY = 16 * 1024


def parse_card(path: str):
    """返回 (frontmatter dict, sections [(name, text)])；非卡片返回 None。"""
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    if not text.startswith("---"):
        return None
    end = text.find("\n---", 3)
    if end < 0:
        return None
    fm_text = text[4:end]
    body = text[end + 4:].lstrip("\r\n")
    fm = {}
    for line in fm_text.splitlines():
        m = re.match(r"^(\w+):\s*(.*)$", line)
        if m:
            fm[m.group(1)] = m.group(2).strip()
    if "title" not in fm:
        return None
    sections = []
    cur_name, cur_lines = None, []
    for line in body.splitlines():
        m = re.match(r"^##\s*(.+?)\s*$", line)
        if m:
            if cur_name is not None:
                sections.append((cur_name, cur_lines))
            cur_name, cur_lines = m.group(1), []
        elif cur_name is not None:
            cur_lines.append(line)
    if cur_name is not None:
        sections.append((cur_name, cur_lines))
    return fm, sections


def clean_line(line: str) -> str:
    line = re.sub(r"^>\s?", "", line)
    line = line.replace("**", "")
    return line.rstrip()


def extract_links(lines):
    links = []
    for line in lines:
        for m in re.finditer(r"\[\[([^\]\[]+)\]\]", line):
            links.append(m.group(1).strip())
    return links


def main(root: str, out_path: str):
    cards = []   # dicts: id, book, title, chapter, sections, raw_links
    for dirpath, _dirnames, filenames in os.walk(root):
        for fn in sorted(filenames):
            if not fn.endswith(".md") or fn in ("README.md", "进度.md"):
                continue
            path = os.path.join(dirpath, fn)
            rel_id = os.path.relpath(path, root)[:-3].replace("\\", "/")
            parsed = parse_card(path)
            if parsed is None:
                continue
            fm, sections = parsed
            top_dir = os.path.relpath(dirpath, root).split(os.sep)[0]
            book = fm.get("book") or top_dir
            cards.append({
                "id": rel_id,
                "dir": os.path.relpath(dirpath, root),
                "book": book,
                "title": fm.get("title", rel_id),
                "chapter": fm.get("chapter", fm.get("category", "")),
                "sections": sections,
                "raw_links": [],
            })
            # 提取关联节的 wikilink
            for name, lines in sections:
                if name == "关联":
                    cards[-1]["raw_links"] = extract_links(lines)
    if not cards:
        print("no cards found"); sys.exit(1)

    id2idx = {c["id"]: i for i, c in enumerate(cards)}
    # 基名 → 卡号列表(同名可能多本)
    base_map = {}
    for i, c in enumerate(cards):
        base_map.setdefault(c["id"].split("/")[-1], []).append(i)

    # 书分组(按书名排序;卡按书内文件名排序)
    books = OrderedDict()
    for c in sorted(cards, key=lambda c: (c["book"], c["id"])):
        books.setdefault(c["book"], []).append(c)
    book_names = sorted(books.keys())
    book_id = {n: i for i, n in enumerate(book_names)}

    # 卡表顺序 = 书序展开
    ordered = []
    for bn in book_names:
        ordered += books[bn]
    idx_of = {c["id"]: i for i, c in enumerate(ordered)}

    # ---- 组装 blobs ----
    strings = bytearray()
    def add_str(s):
        nonlocal strings
        off = len(strings)
        strings += s.encode("utf-8") + b"\x00"
        return off

    body = bytearray()
    links = bytearray()
    card_recs = []
    resolved_total, unresolved = 0, []
    for c in ordered:
        # 正文:分节
        cbody = bytearray()
        first = True
        for name, lines in c["sections"]:
            if name == "关联":
                continue
            marker = b"\x02" if (first and name in QUOTE_SECTIONS) else b"\x01"
            first = False
            cbody += marker + name.encode("utf-8") + b"\n"
            for ln in lines:
                ln = clean_line(ln)
                if ln:
                    cbody += ln.encode("utf-8") + b"\n"
        if len(cbody) > MAX_BODY:
            cbody = cbody[:MAX_BODY] + "\n…(内容过长已截断)".encode("utf-8")
        body_off = len(body)
        body += cbody
        # 关联解析:全库精确 id → 同目录基名 → 全库唯一基名
        link_ids, seen = [], set()
        for raw in c["raw_links"]:
            target = None
            if raw in id2idx:
                target = id2idx[raw]
            else:
                cand_dir = [i for i, cc in enumerate(cards)
                            if cc["dir"] == c["dir"] and cc["id"].split("/")[-1] == raw]
                if len(cand_dir) == 1:
                    target = idx_of[cards[cand_dir[0]]["id"]]
                else:
                    cand_base = base_map.get(raw, [])
                    if len(cand_base) == 1:
                        target = idx_of[cards[cand_base[0]]["id"]]
            if target is None or target == idx_of[c["id"]] or target in seen:
                if target is None:
                    unresolved.append(f"{c['id']} → {raw}")
                continue
            seen.add(target)
            link_ids.append(target)
        links_off = len(links)
        for t in link_ids:
            links += struct.pack("<I", idx_of[cards[t]["id"]])
        resolved_total += len(link_ids)
        card_recs.append({
            "title_off": add_str(c["title"]),
            "chapter_off": add_str(c["chapter"]),
            "body_off": body_off, "body_len": len(cbody),
            "links_off": links_off, "link_count": len(link_ids),
            "book_id": book_id[c["book"]],
        })

    book_recs = []
    for bn in book_names:
        first = idx_of[books[bn][0]["id"]]
        cnt = len(books[bn])
        book_recs.append({"name_off": add_str(bn), "first": first, "count": cnt})

    # ---- 布局 ----
    header_size = 64
    card_table_off = header_size
    card_table_size = 24 * len(ordered)
    book_table_off = card_table_off + card_table_size
    book_table_size = 12 * len(book_recs)
    strings_off = book_table_off + book_table_size
    links_off = strings_off + len(strings)
    body_off = links_off + len(links)
    total = body_off + len(body)

    out = bytearray(total)
    out[0:4] = b"KNOW"
    struct.pack_into("<HH", out, 4, 1, header_size)
    struct.pack_into("<II", out, 8, len(ordered), len(book_recs))
    struct.pack_into("<IIIIII", out, 16, card_table_off, book_table_off,
                     strings_off, links_off, body_off, total)
    for i, r in enumerate(card_recs):
        o = card_table_off + 24 * i
        struct.pack_into("<IIIIIIHH", out, o,
                         r["title_off"], r["chapter_off"], r["body_off"],
                         r["body_len"], r["links_off"], r["link_count"],
                         r["book_id"], 0)
    for i, r in enumerate(book_recs):
        o = book_table_off + 12 * i
        struct.pack_into("<III", out, o, r["name_off"], r["first"], r["count"])
    out[strings_off:strings_off + len(strings)] = strings
    out[links_off:links_off + len(links)] = links
    out[body_off:body_off + len(body)] = body

    with open(out_path, "wb") as f:
        f.write(out)

    print(f"卡片: {len(ordered)} | 书: {len(book_recs)} | 关联解析: {resolved_total} 条"
          f" (未解析 {len(unresolved)})")
    print(f"db 大小: {total/1024/1024:.1f} MB (body {len(body)/1024/1024:.1f} MB)")
    if unresolved[:5]:
        print("未解析示例:", *unresolved[:5], sep="\n  ")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        print(__doc__); sys.exit(1)
    main(sys.argv[1], sys.argv[2])
