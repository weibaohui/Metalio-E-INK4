#!/bin/sh
# 智谱 GLM 用量看板 (Kindle KUAL / 本地纯中文渲染版)

FBINK=/mnt/us/libkh/bin/fbink
[ -x "$FBINK" ] || FBINK=/var/local/kmc/bin/fbink

FONT=/mnt/us/koreader/fonts/noto/NotoSansCJKsc-Regular.otf
LUAJIT=/mnt/us/koreader/luajit
export LD_LIBRARY_PATH=/mnt/us/koreader/libs

# 提示连接中
$FBINK -q -c -m -t regular=$FONT,px=28,top=350 '正在查询智谱 GLM 额度用量...'

API_KEY="YOUR_API_KEY_HERE"
ORG="YOUR_ORG_ID_HERE"
PROJ="YOUR_PROJ_ID_HERE"

KEY_ID=$(echo "$API_KEY" | cut -d'.' -f1)
SECRET=$(echo "$API_KEY" | cut -d'.' -f2)
NOW=$(date +%s)
EXP=$((NOW + 3600))

b64url() {
    openssl base64 -e -A 2>/dev/null | tr '+/' '-_' | tr -d '='
}

HEADER_B64=$(printf '{"alg":"HS256","sign_type":"SIGN"}' | b64url)
PAYLOAD_B64=$(printf '{"api_key":"%s","exp":%d,"timestamp":%d}' "$KEY_ID" "$EXP" "$NOW" | b64url)
SIGN_INPUT="${HEADER_B64}.${PAYLOAD_B64}"
SIGN_B64=$(printf '%s' "$SIGN_INPUT" | openssl dgst -sha256 -hmac "$SECRET" -binary 2>/dev/null | b64url)
API_JWT="${SIGN_INPUT}.${SIGN_B64}"

QUOTA=$(curl -s --max-time 10 \
  -H "Authorization: $API_JWT" \
  -H "bigmodel-organization: $ORG" \
  -H "bigmodel-project: $PROJ" \
  -H "Accept: application/json" \
  "https://open.bigmodel.cn/api/monitor/usage/quota/limit?type=2" 2>/dev/null)

RESETS=$(curl -s --max-time 10 \
  -H "Authorization: $API_JWT" \
  -H "bigmodel-organization: $ORG" \
  -H "bigmodel-project: $PROJ" \
  -H "Accept: application/json" \
  "https://open.bigmodel.cn/api/biz/customer-package-reset/list?targetType=TEAM" 2>/dev/null)

echo "$QUOTA" > /tmp/quota.json
echo "$RESETS" > /tmp/resets.json

$LUAJIT /mnt/us/extensions/glm_usage/bin/format_dashboard.lua
rc=$?

if [ $rc -ne 0 ]; then
    exit $rc
fi

$FBINK -q -c -t regular=$FONT,px=38,top=30,left=35,right=20 "$(cat /tmp/dashboard.txt)"

exit 0
