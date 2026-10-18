#!/usr/bin/env bash
# tools/screenshot.sh —— 墨水屏设备一键远程截屏与正向旋转工具
#
# 功能：
#   1. 通过局域网 REST API 获取真机 1bpp 单色 BMP 截图
#   2. 顺时针旋转 90 度，还原为真机手持正向操作视角 (480x800 竖屏)
#
# 用法：
#   ./tools/screenshot.sh [output_path] [--ip <device_ip>] [--pin <pin>]
#
# 示例：
#   ./tools/screenshot.sh /tmp/screen.bmp
#   ./tools/screenshot.sh screenshot.png --ip 192.168.0.84 --pin 444108
#
set -euo pipefail

DEFAULT_IP="192.168.0.84"
DEFAULT_PORT="8080"
DEFAULT_PIN="444108"
OUTPUT_PATH="${1:-/tmp/device_screenshot_upright.bmp}"

DEVICE_IP="$DEFAULT_IP"
PIN="$DEFAULT_PIN"

shift || true
while [[ $# -gt 0 ]]; do
    case "$1" in
        --ip)
            DEVICE_IP="$2"
            shift 2
            ;;
        --pin)
            PIN="$2"
            shift 2
            ;;
        *)
            shift
            ;;
    esac
done

TMP_RAW="/tmp/metalio_raw_screen_$$.bmp"
trap 'rm -f "$TMP_RAW"' EXIT

echo "--> 正在从 http://${DEVICE_IP}:${DEFAULT_PORT}/screen/screenshot 获取截屏..."
curl -s -S -f -H "Authorization: Bearer ${PIN}" "http://${DEVICE_IP}:${DEFAULT_PORT}/screen/screenshot" -o "$TMP_RAW"

if [ ! -s "$TMP_RAW" ]; then
    echo "错误: 截屏下载失败或文件为空"
    exit 1
fi

echo "--> 正在将图片顺时针旋转 90 度以校准为真机竖屏视角 (480x800)..."
if command -v sips >/dev/null 2>&1; then
    # macOS 原生高性能图像工具
    sips -r 90 "$TMP_RAW" --out "$OUTPUT_PATH" >/dev/null
elif command -v python3 >/dev/null 2>&1; then
    python3 -c "
try:
    from PIL import Image
    im = Image.open('$TMP_RAW')
    im.transpose(Image.ROTATE_270).save('$OUTPUT_PATH')
except Exception as e:
    import sys
    print('请安装 Pillow 或在 macOS 下运行: pip install Pillow', file=sys.stderr)
    sys.exit(1)
"
elif command -v convert >/dev/null 2>&1; then
    # ImageMagick
    convert "$TMP_RAW" -rotate 90 "$OUTPUT_PATH"
else
    echo "警告: 未检测到 sips/PIL/convert 工具，输出原始横向 BMP"
    cp "$TMP_RAW" "$OUTPUT_PATH"
fi

echo "✓ 截屏成功保存至: $OUTPUT_PATH"
