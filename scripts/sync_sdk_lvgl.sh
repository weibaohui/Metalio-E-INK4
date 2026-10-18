#!/usr/bin/env bash
# scripts/sync_sdk_lvgl.sh —— 把 LVGL 头文件与固件构建配置物化到 sdk/include/lvgl/
#
# 仓库内直接用 sdk/build_app.sh 编译应用时需要这些头;
# scripts/gen_sdk.sh 组装发布包时也调用本脚本。
# 内容物生成自固件构建产物,已列入 .gitignore,勿手工提交。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LVGL_SRC="$ROOT/managed_components/lvgl__lvgl"
DST="$ROOT/sdk/include/lvgl"

if [ ! -d "$LVGL_SRC" ]; then
    echo "错误: 找不到 LVGL 组件: $LVGL_SRC" >&2
    exit 1
fi
if [ ! -f "$ROOT/build/config/sdkconfig.h" ]; then
    echo "错误: 找不到 $ROOT/build/config/sdkconfig.h,请先 idf.py build 固件" >&2
    exit 1
fi

mkdir -p "$DST/src"
cp "$LVGL_SRC/lvgl.h" "$DST/" 2>/dev/null || true
cp "$LVGL_SRC/lv_version.h" "$DST/" 2>/dev/null || true
(cd "$LVGL_SRC/src" && find . -name "*.h" | tar cf - -T - | (cd "$DST/src/" && tar xf -))
cp "$ROOT/build/config/sdkconfig.h" "$DST/sdkconfig.h"
IDF_DIR="${IDF_PATH:-$HOME/esp/esp-idf}"
cp "$IDF_DIR/components/esp_common/include/esp_attr.h" "$DST/esp_attr.h"
echo "已物化 SDK LVGL 头文件与配置到 sdk/include/lvgl/"
