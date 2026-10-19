#!/usr/bin/env bash
# sdk/build_app.sh —— Metalio 动态应用独立编译器
#
# 不依赖固件源码:仅需 xtensa-esp32s3 工具链(ESP-IDF 自带) + 本 SDK 包。
# 用法:
#   ./build_app.sh <src.c> <out_name>
#   XTENSA_GCC=/path/to/xtensa-esp32s3-elf-gcc ./build_app.sh main/my_main.c myapp
#
# 产物: <当前目录>/build/<out_name>.so
# 同时做静态符号校验: 若 .so 引用了固件导出清单(symbols.txt)之外的符号,给出警告
# (那些符号在设备安装时会导致 App load failed)。
set -euo pipefail

SDK_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ $# -lt 2 ]; then
    echo "用法: $0 <src.c> <out_name>"
    echo "示例: $0 main/hello_main.c hello"
    exit 1
fi
SRC="$1"
OUT="$2"
[ -f "$SRC" ] || { echo "错误: 源文件不存在: $SRC"; exit 1; }

# ---- 定位工具链 ----
GCC="${XTENSA_GCC:-}"
if [ -z "$GCC" ]; then
    GCC="$(command -v xtensa-esp32s3-elf-gcc 2>/dev/null || true)"
fi
if [ -z "$GCC" ]; then
    # 标准 ESP-IDF 安装位置(取最新版本)
    GCC="$(ls -d "$HOME"/.espressif/tools/xtensa-esp-elf/*/*/bin/xtensa-esp32s3-elf-gcc 2>/dev/null | sort | tail -1 || true)"
fi
if [ -z "$GCC" ] || [ ! -x "$GCC" ]; then
    echo "错误: 找不到 xtensa-esp32s3-elf-gcc"
    echo "请安装 ESP-IDF 工具链(https://docs.espressif.com) 或设 XTENSA_GCC 环境变量"
    exit 1
fi
BIN_DIR="$(dirname "$GCC")"
STRIP="$BIN_DIR/xtensa-esp32s3-elf-strip"
READELF="$BIN_DIR/xtensa-esp32s3-elf-readelf"

# 仓库内直接使用时,LVGL 头文件可能尚未物化:自动同步一次(发布包内则已就位)
if [ ! -f "$SDK_ROOT/include/lvgl/sdkconfig.h" ] && [ -f "$SDK_ROOT/../scripts/sync_sdk_lvgl.sh" ]; then
    bash "$SDK_ROOT/../scripts/sync_sdk_lvgl.sh"
fi
if [ ! -f "$SDK_ROOT/include/lvgl/sdkconfig.h" ]; then
    echo "错误: SDK 缺 LVGL 配置($SDK_ROOT/include/lvgl/sdkconfig.h)"
    echo "若从 git 克隆,请先运行 scripts/gen_sdk.sh 完成物化"
    exit 1
fi

# ---- 固化编译参数(与固件构建一致;SDK 独立可复现) ----
CFLAGS=(
    -std=gnu17
    -Os
    -fPIC
    # 与固件/toolchain 响应文件一致的 ABI 关键参数——缺失 -mlongcalls 会导致
    # 应用内短调用编码跳不到固件函数,运行即崩溃(实测)
    -mlongcalls
    -fno-builtin-memcpy
    -fno-builtin-memset
    -fno-builtin-bzero
    -mdisable-hardware-atomics
    -ffunction-sections
    -fdata-sections
    -DESP_PLATFORM
    -D_GNU_SOURCE
    -DLV_CONF_INCLUDE_SIMPLE
    -DLV_LVGL_H_INCLUDE_SIMPLE
    -DLVGL_PORT_PPA_ALIGNMENT=128
    -I"$SDK_ROOT/include"
    -I"$SDK_ROOT/include/lvgl"
    -I"$SDK_ROOT/include/lvgl/src"
    -I"$SDK_ROOT/include/fontpack"
    -include "$SDK_ROOT/include/lvgl/sdkconfig.h"
)

# elf_loader 官方 .so 链接参数(未定义符号运行期由固件符号表解析)
LDFLAGS=(
    -shared -fPIC -static-libgcc -nostdlib -nostartfiles
    -fdata-sections -ffunction-sections -Wl,--gc-sections -fvisibility=hidden
    -Wl,--strip-all -Wl,--strip-debug -Wl,--strip-discarded
)

STRIP_SECTIONS=(
    --strip-unneeded --remove-section=.comment --remove-section=.got.loc
    --remove-section=.dynamic --remove-section=.xt.lit --remove-section=.xt.prop
    --remove-section=.xtensa.info
)

BUILD_DIR="$PWD/build"
mkdir -p "$BUILD_DIR"

echo "[1/3] 编译 $(basename "$SRC")"
"$GCC" -c "${CFLAGS[@]}" "$SRC" -o "$BUILD_DIR/${OUT}.o"

echo "[2/3] 链接 ${OUT}.so"
"$GCC" "${LDFLAGS[@]}" -o "$BUILD_DIR/${OUT}.so" "$BUILD_DIR/${OUT}.o"

echo "[3/3] strip"
"$STRIP" "${STRIP_SECTIONS[@]}" "$BUILD_DIR/${OUT}.so"

ls -la "$BUILD_DIR/${OUT}.so"

# ---- 静态符号校验(警告级别):未定义符号应全部在固件导出清单内 ----
if [ -f "$SDK_ROOT/symbols.txt" ] && [ -x "$READELF" ]; then
    UNDEF="$("$READELF" -W --dyn-syms "$BUILD_DIR/${OUT}.so" 2>/dev/null | awk '$7=="UND" {print $8}' | sort -u)"
    EXTRA=""
    for s in $UNDEF; do
        grep -qx "$s" "$SDK_ROOT/symbols.txt" || EXTRA="$EXTRA $s"
    done
    if [ -n "$EXTRA" ]; then
        echo ""
        echo "⚠️  警告: 以下符号不在固件导出清单(symbols.txt)中,设备安装时会加载失败:"
        for s in $EXTRA; do echo "     $s"; done
        echo "  → 请改用清单内 API,或联系固件方在导出表中补充后重编固件"
    else
        echo "✓ 符号校验通过: 全部外部引用均在固件导出清单内"
    fi
fi

# ---- printf 格式检查:固件 libc 为 newlib NANO 格式,%lld/%llu 不支持 ----
if grep -nE '%[0-9.]*ll[diouxX]' "$SRC" >/dev/null 2>&1; then
    echo ""
    echo "⚠️  警告: 源码使用了 long-long printf 格式(%lld/%llu 等):"
    grep -nE '%[0-9.]*ll[diouxX]' "$SRC" | head -5
    echo "  → 固件 libc 是 newlib NANO 格式,不支持 %lld/%llu,运行时会输出乱码"
    echo "    (如 \"0ld\")。请改用 32 位 long/%ld,或自行拼字符串"
fi

echo "OK: $BUILD_DIR/${OUT}.so"
