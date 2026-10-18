#!/usr/bin/env bash
# tools/package_app.sh —— 动态应用一键构建、打包与部署工具
#
# 架构规范：
#   构建产物输出到 bin/ 目录（已被 .gitignore 排除，绝不提交进仓库）：
#   <app_dir>/bin/<app_name>.app/
#   ├── manifest.json
#   ├── icon.png
#   └── bin/
#       └── app.so
#
# 用法:
#   ./tools/package_app.sh <app_dir> [--upload <device_ip>] [--pin <pin>]
#
# 示例:
#   # 仅本地构建出 .app Bundle 包（输出到 tools/hello_app/bin/hello.app）
#   ./tools/package_app.sh tools/hello_app
#
#   # 构建并直接通过 WiFi 部署到设备
#   ./tools/package_app.sh tools/hello_app --upload 192.168.0.84 --pin 444108
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
BUILD_DIR="$FW_ROOT/build"
CC_JSON="$BUILD_DIR/compile_commands.json"

if [ $# -lt 1 ]; then
    echo "用法: $0 <app_dir> [--upload <device_ip>] [--pin <pin>]"
    echo "示例: $0 tools/hello_app --upload 192.168.0.84 --pin 444108"
    exit 1
fi

APP_DIR="$(cd "$1" && pwd)"
shift

UPLOAD_IP=""
PIN=""

while [[ $# -gt 0 ]]; do
    case "$1" in
        --upload)
            UPLOAD_IP="$2"
            shift 2
            ;;
        --pin)
            PIN="$2"
            shift 2
            ;;
        *)
            echo "未知参数: $1"
            exit 1
            ;;
    esac
done

if [ ! -f "$CC_JSON" ]; then
    echo "错误: 未找到 $CC_JSON，请先在固件根目录执行 idf.py build"
    exit 1
fi

APP_NAME="$(basename "$APP_DIR" | sed 's/_app$//')"
SRC_FILE=""
if [ -f "$APP_DIR/main/${APP_NAME}_main.c" ]; then
    SRC_FILE="$APP_DIR/main/${APP_NAME}_main.c"
else
    SRC_FILE="$(find "$APP_DIR/main" -name "*.c" | head -n 1 || true)"
fi

if [ -z "$SRC_FILE" ] || [ ! -f "$SRC_FILE" ]; then
    echo "错误: 未在 $APP_DIR/main 下找到入口 C 源码"
    exit 1
fi

echo "=============================================="
echo " 正在构建应用: $APP_NAME"
echo " 源码文件: $SRC_FILE"
echo "=============================================="

# 1. 编译 .so
bash "$FW_ROOT/tools/hello_app/build_so.sh" "$SRC_FILE" "$APP_NAME"

SO_FILE="$BUILD_DIR/${APP_NAME}.so"
if [ ! -f "$SO_FILE" ]; then
    echo "错误: 构建输出文件不存在: $SO_FILE"
    exit 1
fi

# 2. 组装 .app Bundle 输出到 bin/ 目录（排除于 git 之外）
BUNDLE_DIR="$APP_DIR/bin/${APP_NAME}.app"
rm -rf "$BUNDLE_DIR"
mkdir -p "$BUNDLE_DIR/lib"
cp "$SO_FILE" "$BUNDLE_DIR/lib/app.so"

# 复制 manifest.json
if [ -f "$APP_DIR/manifest.json" ]; then
    cp "$APP_DIR/manifest.json" "$BUNDLE_DIR/manifest.json"
elif [ ! -f "$BUNDLE_DIR/manifest.json" ]; then
    cat <<EOF > "$BUNDLE_DIR/manifest.json"
{
  "id": "${APP_NAME}",
  "name": "${APP_NAME}",
  "icon": "icon.png",
  "executable": "lib/app.so",
  "abi": 1,
  "version": "1.0.0"
}
EOF
    echo "已在构建输出中生成默认 manifest.json"
fi

# 复制 icon.png（若存在）
if [ -f "$APP_DIR/icon.png" ]; then
    cp "$APP_DIR/icon.png" "$BUNDLE_DIR/icon.png"
fi

# 复制说明文档 README.md（若存在）
if [ -f "$APP_DIR/README.md" ]; then
    cp "$APP_DIR/README.md" "$BUNDLE_DIR/README.md"
fi

# 复制示例配置文件（如 stations.example.json 等）
for ex in "$APP_DIR"/*.example.*; do
    if [ -f "$ex" ]; then
        cp "$ex" "$BUNDLE_DIR/"
    fi
done

echo "==> Bundle 准备就绪: $BUNDLE_DIR"
find "$BUNDLE_DIR" -type f

# 3. 如果指定了上传 IP，则通过 HTTP MCP API 直推
if [ -n "$UPLOAD_IP" ]; then
    echo ""
    echo "=============================================="
    echo " 正在通过 WiFi 部署到设备 http://${UPLOAD_IP}:8080 ..."
    echo "=============================================="

    AUTH_HEADER=()
    if [ -n "$PIN" ]; then
        AUTH_HEADER=(-H "Authorization: Bearer $PIN")
    fi

    # 上传 manifest.json
    echo "-> 上传 manifest.json..."
    curl -s -f -X POST "http://${UPLOAD_IP}:8080/fs/upload?path=apps/${APP_NAME}.app" \
        -H "X-Filename: manifest.json" \
        "${AUTH_HEADER[@]}" \
        --data-binary "@$BUNDLE_DIR/manifest.json"
    echo ""

    # 上传 icon.png（若存在）
    if [ -f "$BUNDLE_DIR/icon.png" ]; then
        echo "-> 上传 icon.png..."
        curl -s -f -X POST "http://${UPLOAD_IP}:8080/fs/upload?path=apps/${APP_NAME}.app" \
            -H "X-Filename: icon.png" \
            "${AUTH_HEADER[@]}" \
            --data-binary "@$BUNDLE_DIR/icon.png"
        echo ""
    fi

    # 上传 README.md（若存在）
    if [ -f "$BUNDLE_DIR/README.md" ]; then
        echo "-> 上传 README.md..."
        curl -s -f -X POST "http://${UPLOAD_IP}:8080/fs/upload?path=apps/${APP_NAME}.app" \
            -H "X-Filename: README.md" \
            "${AUTH_HEADER[@]}" \
            --data-binary "@$BUNDLE_DIR/README.md"
        echo ""
    fi

    # 上传示例文件（若存在）
    for ex in "$BUNDLE_DIR"/*.example.*; do
        if [ -f "$ex" ]; then
            ex_name="$(basename "$ex")"
            echo "-> 上传 $ex_name..."
            curl -s -f -X POST "http://${UPLOAD_IP}:8080/fs/upload?path=apps/${APP_NAME}.app" \
                -H "X-Filename: $ex_name" \
                "${AUTH_HEADER[@]}" \
                --data-binary "@$ex"
            echo ""
        fi
    done

    # 上传 lib/app.so
    echo "-> 上传 lib/app.so..."
    curl -s -f -X POST "http://${UPLOAD_IP}:8080/fs/upload?path=apps/${APP_NAME}.app/lib" \
        -H "X-Filename: app.so" \
        "${AUTH_HEADER[@]}" \
        --data-binary "@$BUNDLE_DIR/lib/app.so"
    echo ""

    echo "==> 部署成功！应用已在桌面热更新注册完毕。"
fi
