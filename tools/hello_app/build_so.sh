#!/bin/bash
# tools/hello_app/build_so.sh —— Hello 动态应用 .so 构建
#
# 从固件 build/compile_commands.json 提取 main 组件的真实编译参数
# （-I 头路径 + -D 定义与固件完全一致，保证 ABI 一致），编译 tools/hello_app/main/hello_main.c，
# 再按官方 elf_loader 的 .so 链接参数出 hello.so（-shared -fPIC -nostdlib，
# 未定义符号运行期由固件符号表解析）。
#
# 前提：固件已 idf.py build（build/compile_commands.json 存在）。
# 产物：build/hello.so → 拷入 SD 卡 /sdcard/metalio/e-ink/apps/
set -e

FW_ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$FW_ROOT/build"
CC_JSON="$BUILD/compile_commands.json"

[ -f "$CC_JSON" ] || { echo "ERROR: $CC_JSON 不存在，先 idf.py build 固件"; exit 1; }

TOOLCHAIN_BIN="$(dirname "$(python3 -c "
import json
db = json.load(open('$CC_JSON'))
cmd = [e['command'] for e in db if '/main/' in e['file']][0]
print(cmd.split()[0])
")")"

# 提取 main 组件编译命令里的 -I / -D / -include 参数（跳过 -o/-c 与源文件本身）
SO_CFLAGS="$(python3 - "$CC_JSON" << 'PYEOF'
import json, shlex, sys
db = json.load(open(sys.argv[1]))
entries = [e for e in db if '/main/' in e['file']]
cmd = [e for e in entries if e['file'].endswith('dynamic_app_loader.cc')][0]['command']
parts = shlex.split(cmd)
out = []
skip_next = False
for p in parts:
    if skip_next:
        out.append(p); skip_next = False; continue
    if p in ('-o', '-c'):
        continue
    if p == '-include':
        out.append(p); skip_next = True; continue
    if p.startswith('-I') or p.startswith('-D'):
        out.append(p)
    elif p.startswith('@'):
        out.append(p)
print(' '.join(out))
PYEOF
)"
# ABI 头目录：compile_commands.json 里靠同目录引用解析，不在 -I 列表，显式补上
SO_CFLAGS="$SO_CFLAGS -I$FW_ROOT/main/services/dynamic_apps"

# elf_loader 的 .so 链接参数（elf_loader.cmake project_so 同款）
SO_LINK="-shared -fPIC -static-libgcc -nostdlib -nostartfiles \
-fdata-sections -ffunction-sections -Wl,--gc-sections -fvisibility=hidden \
-Wl,--strip-all -Wl,--strip-debug -Wl,--strip-discarded"

SO_STRIP="--strip-unneeded --remove-section=.comment --remove-section=.got.loc \
--remove-section=.dynamic --remove-section=.xt.lit --remove-section=.xt.prop \
--remove-section=.xtensa.info"

CC="${TOOLCHAIN_BIN}/xtensa-esp32s3-elf-gcc"
STRIP="${TOOLCHAIN_BIN}/xtensa-esp32s3-elf-strip"

echo "[1/3] compile hello_main.c"
"$CC" -c -fPIC -std=gnu17 $SO_CFLAGS "$FW_ROOT/tools/hello_app/main/hello_main.c" -o "$BUILD/hello.o"

echo "[2/3] link hello.so"
"$CC" $SO_LINK -o "$BUILD/hello.so" "$BUILD/hello.o"

echo "[3/3] strip"
"$STRIP" $SO_STRIP "$BUILD/hello.so"

ls -la "$BUILD/hello.so"
echo "OK: $BUILD/hello.so → 拷入 SD 卡 /sdcard/metalio/e-ink/apps/"
