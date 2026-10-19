#!/usr/bin/env bash
# tools/package_app.sh —— 动态应用编译、打包与部署入口(兼容旧 CLI)
#
# 自 v2.0.57 起全部动态应用统一走 Metalio SDK 构建(sdk/build_app.sh,固化编译
# 参数、脱离固件源码),本脚本仅转发到 sdk/package_app.sh —— CLI 与产物完全一致:
#
#   ./tools/package_app.sh <app_dir> [--upload <device_ip>] [--pin <pin>]
#
# 产物:
#   <app_dir>/bin/<name>.app/      Bundle 目录
#   <app_dir>/bin/<name>.app.zip   可分发安装包
set -euo pipefail

SDK_PKG="$(cd "$(dirname "${BASH_SOURCE[0]}")/../sdk" && pwd)/package_app.sh"

if [ ! -f "$SDK_PKG" ]; then
    echo "错误: 找不到 SDK 打包脚本: $SDK_PKG" >&2
    exit 1
fi

exec bash "$SDK_PKG" "$@"
