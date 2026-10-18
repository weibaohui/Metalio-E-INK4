#!/usr/bin/env bash
# scripts/gen_sdk.sh —— 组装 Metalio 动态应用 SDK 发布包
#
# 用法: ./scripts/gen_sdk.sh [版本号]   # 缺省取 git describe
# 产物: dist/metalio-sdk-<version>.zip
#
# 包内容: 头文件(metalio/fontpack/lvgl) + 固件配置 + 符号清单 + 编译/打包脚本
#        + 示例 + CHANGELOG。组装后自动跑冒烟测试(用包内脚本编译示例应用),
#        保证发布包自洽可用。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VER="${1:-$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || echo dev)}"
STAGE="$ROOT/dist/sdk-stage/metalio-sdk"
LVGL_SRC="$ROOT/managed_components/lvgl__lvgl"

echo "==> 组装 Metalio SDK v$VER"
rm -rf "$ROOT/dist/sdk-stage"
mkdir -p "$STAGE/include/metalio" "$STAGE/include/fontpack" "$STAGE/include/lvgl" "$STAGE/examples"

# 1. 契约与服务头(SDK 唯一事实源)
cp "$ROOT/sdk/include/metalio/"*.h "$STAGE/include/metalio/"
cp "$ROOT/sdk/include/fontpack/fontpack_lvgl.h" "$STAGE/include/fontpack/"
# 1b. ESP-IDF 公共头(应用合法使用:esp_random 硬件随机数等)
cp "$ROOT/sdk/include/esp_random.h" "$STAGE/include/"

# 2. 旧式引用转发头(存量应用 #include "dynamic_app_abi.h" 等也能编译)
cat > "$STAGE/include/dynamic_app_abi.h" <<'EOF'
#pragma once
#include "metalio/abi.h"
EOF
cat > "$STAGE/include/dynamic_app_shims.h" <<'EOF'
#pragma once
#include "metalio/services.h"
EOF

# 3. LVGL 头文件(含目录结构)与固件真实构建配置
bash "$ROOT/scripts/sync_sdk_lvgl.sh"
cp -r "$ROOT/sdk/include/lvgl" "$STAGE/include/"

# 4. 符号清单与变更记录
if [ ! -f "$ROOT/sdk/symbols.txt" ]; then
    echo "==> symbols.txt 不存在,先运行 gen_symbols.sh"
    "$ROOT/scripts/gen_symbols.sh"
fi
cp "$ROOT/sdk/symbols.txt" "$STAGE/symbols.txt"
cp "$ROOT/sdk/CHANGELOG.md" "$STAGE/CHANGELOG.md"

# 5. 编译/打包脚本与示例
cp "$ROOT/sdk/build_app.sh" "$ROOT/sdk/package_app.sh" "$STAGE/"
chmod +x "$STAGE/build_app.sh" "$STAGE/package_app.sh"
cp -r "$ROOT/sdk/examples/hello" "$STAGE/examples/"
[ -f "$ROOT/tools/hello_app/icon.png" ] && cp "$ROOT/tools/hello_app/icon.png" "$STAGE/examples/hello/icon.png"

# 6. README
cat > "$STAGE/README.md" <<EOF
# Metalio 动态应用 SDK v$VER

为 Metalio E-INK 墨水屏设备开发第三方动态应用(.so),无需固件源码。

## 快速开始

\`\`\`bash
# 1. 用示例模板开始你的应用
cp -r examples/hello my_app
# 2. 编辑 my_app/main/hello_main.c 与 manifest.json
# 3. 编译 + 打包(产物: my_app/bin/hello.app.zip)
./package_app.sh my_app
# 4. WiFi 直推设备安装(设备与电脑同局域网)
./package_app.sh my_app --upload <device_ip> --pin <pin>
\`\`\`

## 包结构
- include/metalio/abi.h      应用入口契约(metalio_app_entry)
- include/metalio/services.h 系统能力垫片(约 60 个服务 API)
- include/lvgl/              LVGL 图形库头文件 + 固件构建配置
- include/fontpack/          字库接口
- symbols.txt                固件导出符号清单(你只能调用其中的函数)
- build_app.sh               .c → .so 独立编译器
- package_app.sh             编译 + 组装 + 打包 + 直推设备
- examples/hello             最小示例应用
- CHANGELOG.md               ABI 变更记录

## 规则
- 只允许调用 symbols.txt 中的符号(其他符号在安装时加载失败,dlerror 会点名)
- ABI 稳定政策: 签名只增不改;破坏性变更递增 METALIO_DYNAPP_ABI_VERSION
- 应用退出时必须自清定时器/任务(退出即 dlclose,悬空即崩溃)
EOF

# 7. 冒烟测试: 用发布包内的脚本编译示例应用(证明包自洽)
echo "==> 冒烟测试: 用包内脚本编译 examples/hello"
SMOKE="$ROOT/dist/sdk-smoke"
rm -rf "$SMOKE"
mkdir -p "$SMOKE"
cp -r "$STAGE" "$SMOKE/"
(cd "$SMOKE/metalio-sdk" && ./package_app.sh examples/hello)
echo "==> 冒烟测试通过(示例应用已用发布包独立编译成功)"

# 8. 打包
mkdir -p "$ROOT/dist"
rm -f "$ROOT/dist/metalio-sdk-$VER.zip"
(cd "$ROOT/dist/sdk-stage" && zip -r -q "$ROOT/dist/metalio-sdk-$VER.zip" metalio-sdk)
echo ""
echo "✅ SDK 包已生成: dist/metalio-sdk-$VER.zip ($(stat -f%z "$ROOT/dist/metalio-sdk-$VER.zip" 2>/dev/null || wc -c < "$ROOT/dist/metalio-sdk-$VER.zip") 字节)"
