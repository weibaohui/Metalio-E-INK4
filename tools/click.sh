#!/usr/bin/env bash
# tools/click.sh —— 墨水屏设备屏幕虚拟触控点击自动化脚本
#
# 功能：
#   向真机发送屏幕点击或长按指令，触发与真实手指按压完全一致的
#   LVGL 命中测试、马达震动、按钮按下视觉态及回调响应。
#
# 用法：
#   ./tools/click.sh <x> <y> [duration_ms] [--ip <device_ip>] [--pin <pin>]
#
# 示例：
#   ./tools/click.sh 240 300            # 点击屏幕中间位置 (默认按压 60ms)
#   ./tools/click.sh 80 900             # 点击底部 vk_home 触控虚拟按键
#   ./tools/click.sh 80 900 600         # 长按 vk_home 虚拟按键 600ms
#
set -euo pipefail

DEFAULT_IP="192.168.0.84"
DEFAULT_PORT="8080"
DEFAULT_PIN="444108"

if [ $# -lt 2 ]; then
    echo "用法: $0 <x> <y> [duration_ms] [--ip <ip>] [--pin <pin>]"
    echo "说明: 注入坐标 = 当前屏幕 LVGL 逻辑坐标。桌面页 800x480 横屏,与截图像素 1:1;"
    echo "      动态应用页为 480x800 竖屏逻辑(其内容在截图里旋转了 90°),须按应用源码布局坐标注入,"
    echo "      从截图像素反推: app_x = 479 - 截图行, app_y = 截图列。盖板虚拟按键请用 /input/key"
    exit 1
fi

X="$1"
Y="$2"
shift 2 || true

DUR=60
if [[ $# -gt 0 && ! "$1" =~ ^-- ]]; then
    DUR="$1"
    shift
fi

DEVICE_IP="$DEFAULT_IP"
PIN="$DEFAULT_PIN"

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

echo "--> 正在向 http://${DEVICE_IP}:${DEFAULT_PORT}/input/click 发送点击: x=${X}, y=${Y}, duration=${DUR}ms..."
curl -s -S -f -X POST \
    -H "Authorization: Bearer ${PIN}" \
    "http://${DEVICE_IP}:${DEFAULT_PORT}/input/click?x=${X}&y=${Y}&duration_ms=${DUR}"
echo ""
