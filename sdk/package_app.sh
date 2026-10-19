#!/usr/bin/env bash
# sdk/package_app.sh —— 动态应用编译 + 组装 + 打包 + 可选 WiFi 直推设备
#
# 用法:
#   ./package_app.sh <app_dir> [--upload <device_ip>] [--pin <pin>]
#
# <app_dir> 结构约定(以 examples/hello 为模板):
#   <app_dir>/main/<name>_main.c   # 应用源码
#   <app_dir>/manifest.json        # 应用元数据
#   <app_dir>/icon.png             # 70x70 图标(可选)
#
# 产物:
#   <app_dir>/bin/<name>.app/      # Bundle 目录
#   <app_dir>/bin/<name>.app.zip   # 可分发安装包
set -euo pipefail

SDK_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ $# -lt 1 ]; then
    echo "用法: $0 <app_dir> [--upload <device_ip>] [--pin <pin>]"
    exit 1
fi

APP_DIR="$(cd "$1" && pwd)"
shift

UPLOAD_IP=""
PIN=""
while [[ $# -gt 0 ]]; do
    case "$1" in
        --upload) UPLOAD_IP="$2"; shift 2 ;;
        --pin)    PIN="$2"; shift 2 ;;
        *) echo "未知参数: $1"; exit 1 ;;
    esac
done

APP_NAME="$(basename "$APP_DIR" | sed 's/_app$//')"
SRC=""
if [ -f "$APP_DIR/main/${APP_NAME}_main.c" ]; then
    SRC="$APP_DIR/main/${APP_NAME}_main.c"
else
    SRC="$(find "$APP_DIR/main" -name "*.c" | head -n 1 || true)"
fi
[ -n "$SRC" ] && [ -f "$SRC" ] || { echo "错误: 未在 $APP_DIR/main 下找到入口 C 源码"; exit 1; }

echo "=============================================="
echo " 应用: $APP_NAME    源码: $SRC"
echo "=============================================="

# 1. 编译(构建产物落在 APP_DIR/build/)
(cd "$APP_DIR" && "$SDK_ROOT/build_app.sh" "$SRC" "$APP_NAME")

SO_FILE="$APP_DIR/build/${APP_NAME}.so"
[ -f "$SO_FILE" ] || { echo "错误: 构建产物不存在: $SO_FILE"; exit 1; }

# 2. 组装 .app Bundle
OUT_DIR="$APP_DIR/bin"
BUNDLE_DIR="$OUT_DIR/${APP_NAME}.app"
rm -rf "$BUNDLE_DIR"
mkdir -p "$BUNDLE_DIR/lib"

# manifest:读取 id 作为 .so 唯一名。esp_dlfcn 按基础文件名缓存已加载模块,
# 所有应用都叫 lib/app.so 时,第二个应用 dlopen 会拿到第一个应用的缓存模块
# (表现为"启动 A 却显示 B 的界面"),因此 .so 必须以应用 id 命名。
APP_ID="$APP_NAME"
if [ -f "$APP_DIR/manifest.json" ]; then
    APP_ID="$(python3 -c "
import json,sys
try:
    print(json.load(open('$APP_DIR/manifest.json')).get('id') or '$APP_NAME')
except Exception:
    print('$APP_NAME')
" 2>/dev/null || echo "$APP_NAME")"
fi
SO_NAME="$APP_ID"

cp "$SO_FILE" "$BUNDLE_DIR/lib/${SO_NAME}.so"

if [ -f "$APP_DIR/manifest.json" ]; then
    python3 - "$APP_DIR/manifest.json" "$BUNDLE_DIR/manifest.json" "$SO_NAME" <<'PYEOF'
import json, sys
src, dst, so_name = sys.argv[1], sys.argv[2], sys.argv[3]
m = json.load(open(src))
m["executable"] = f"lib/{so_name}.so"   # 唯一 .so 名(防 dlfcn 按文件名缓存串台)
json.dump(m, open(dst, "w"), indent=2, ensure_ascii=False)
PYEOF
else
    cat > "$BUNDLE_DIR/manifest.json" <<EOF
{
  "id": "${SO_NAME}",
  "name": "${APP_NAME}",
  "icon": "icon.png",
  "executable": "lib/${SO_NAME}.so",
  "abi": 1,
  "version": "1.0.0"
}
EOF
    echo "(已生成默认 manifest.json,请按需修改)"
fi
[ -f "$APP_DIR/icon.png" ] && cp "$APP_DIR/icon.png" "$BUNDLE_DIR/icon.png"

echo "==> Bundle 准备就绪: $BUNDLE_DIR (lib/${SO_NAME}.so)"

# 3. 打包 .app.zip
APP_ZIP="$OUT_DIR/${APP_NAME}.app.zip"
rm -f "$APP_ZIP"
(cd "$BUNDLE_DIR" && zip -r -q "$APP_ZIP" .)
echo "==> 安装包已生成: $APP_ZIP ($(stat -f%z "$APP_ZIP" 2>/dev/null || wc -c < "$APP_ZIP") 字节)"

# 4. 可选: WiFi 直推设备安装
if [ -n "$UPLOAD_IP" ]; then
    echo "==> 直推设备 http://${UPLOAD_IP}:8080/app/install ..."
    AUTH=()
    [ -n "$PIN" ] && AUTH=(-H "Authorization: Bearer $PIN")
    curl -s -X POST "http://${UPLOAD_IP}:8080/app/install" "${AUTH[@]}" \
        --data-binary "@$APP_ZIP"
    echo ""
fi
