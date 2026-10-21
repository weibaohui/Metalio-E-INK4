#!/usr/bin/env bash
# tools/poetry_pack/fetch_corpus.sh —— 下载 chinese-poetry 语料到 /tmp/poetry_corpus
#
# chinese-poetry 仓库的 raw.githubusercontent.com 直连常失败, 走 ghproxy 镜像。
# 打包器 pack_poems.py 另需本机知识卡库(赏析层), 路径写死在脚本顶部。
set -euo pipefail
OUT="${1:-/tmp/poetry_corpus}"
mkdir -p "$OUT"
B="https://ghproxy.net/https://raw.githubusercontent.com/chinese-poetry/chinese-poetry/master"
fetch() { # <repo-path> <dest>
    local code
    code=$(curl -sL -m 60 -o "$OUT/$2" -w "%{http_code}" "$B/$(python3 -c "import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))" "$1")")
    [ "$code" = "200" ] || { echo "下载失败 $1 ($code)" >&2; exit 1; }
    echo "$2 ($(wc -c <"$OUT/$2") bytes)"
}
fetch "全唐诗/唐诗三百首.json" 唐诗三百首.json
fetch "宋词/宋词三百首.json"   宋词三百首.json
fetch "蒙学/qianjiashi.json"  千家诗.json
fetch "诗经/shijing.json"     诗经.json
echo "完成: $OUT"
