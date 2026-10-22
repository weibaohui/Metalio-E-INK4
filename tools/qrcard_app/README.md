# 电子名片 动态应用

一张能递出去的名片：**身份区 + 多个码入口**。

![界面](icon.png)

## 界面

```
┌──────────────────────────────┐
│ 电子名片                      │
│  (头像)  神经蛙               │
│          产品经理              │
│          示例科技 · EMP-1024   │
│ ──────────────────────────── │
│ 13800000000 · frog@example.com│
│ 今天也在搬砖                   │
│  微信  企微  飞书  群聊  主页  微信码 │  ← 点标签换码
│  ┌───────────────┐            │
│  │               │            │
│  │   大二维码      │            │
│  │               │            │
│  └───────────────┘            │
│  微信 · 扫码打开链接            │
│  [重载] [怎么改]        Metalio │
└──────────────────────────────┘
```

- **标签行**点一下就换码（微信 / 企微 / 飞书 / 群聊 / 主页 / 任意自定义）
- 码外面套一圈细边框，看起来是「一张卡」而不是浮在白底上
- 一屏只放一个码，因为 280px 才扫得稳

## 设计参考

- **企业微信「对外名片」**：头像/姓名/企业/职务/联系方式，再挂 6~7 个自定义对外信息
  （文字 / 链接 / 小程序三种形式）——对应到墨水屏就是**多个二维码入口**
- **二维彩虹「社交媒体二维码」**：把公众号/官网/各渠道链接汇到一页，点哪个跳哪个
- 一屏只放一个码、标签行切换，是为了「递出去就能扫」，不是为了塞下所有信息

## 两种码

| 类型 | 配置写法 | 渲染 |
|---|---|---|
| 文本码 | `微信\|mywechatid` | LVGL 自带 `lv_qrcode` 生成 |
| 图片码 | `微信码\|wechat_qr.png` | 解码成位图显示（微信个人码、收款码这类没有 URL 的码） |

图片名以 `.png/.jpg/.jpeg/.bmp/.webp` 结尾就当图片码，其余当文本码。

## 配置

`profile.txt` 放 app 自己的 `data/` 目录（注意是 `.app`，点号）：

```ini
[card]
name=神经蛙
title=产品经理
company=示例科技
dept=产品部
id=EMP-1024
phone=13800000000
email=frog@example.com
url=https://folotoy.com
status=今天也在搬砖
avatar=avatar.png

[qr]
微信|神经蛙
企微|https://work.weixin.qq.com/qr/demo
飞书|https://feishu.cn/qr/demo
群聊|https://example.com/group
主页|https://folotoy.com
微信码|wechat_qr.png
```

- `[card]` 段写身份资料；`[qr]` 段每行一个码：**标签|内容**
- 不写 `[qr]` 段时，自动用 `url`（或只含姓名/电话/邮箱的精简 vCard）当唯一那个码
- 标签建议 2~3 个字，太长会被裁

## 怎么传上去

```bash
curl -X POST \
  -H "Authorization: Bearer <PIN>" \
  -H "X-Filename: profile.txt" \
  --data-binary @profile.txt \
  "http://<设备IP>:8080/fs/upload?path=apps/qrcard.app/data"

curl -X POST \
  -H "Authorization: Bearer <PIN>" \
  -H "X-Filename: avatar.png" \
  --data-binary @avatar.png \
  "http://<设备IP>:8080/fs/upload?path=apps/qrcard.app/data"
```

改完点「重载」生效（不用重启设备）。

## 按键

| 键 | 动作 |
|---|---|
| 触摸标签 | 切换到那个码 |
| 硬件上/下键 | 上一码 / 下一码 |
| 硬件 Home 键 | 返回桌面 |
