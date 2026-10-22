# 共享件

| 文件 | 用途 |
|---|---|
| `badge_profile.h` | 电子工牌 / 二维码名片共用的资料解析（`profile.txt`） |
| `sevenseg.h` | 七段数码管（天气 App 用；番茄钟有一份自己内联的几何拷贝） |

## sevenseg.h

纯几何画七段数码管。**只画点亮的段**——灭掉的段完全不画：1bpp 上留一圈空心框或
细鬼影只会把字形搞脏，细鬼影在墨水屏上还会被残影放大成脏灰。

```c
SevenSeg ss;
sevenseg_init(&ss, parent, 3, x, y, dw, dh, stroke);
sevenseg_set_mask(&ss, 18, 0, 0x01);  // 显示 18，最高位留空
sevenseg_clear(&ss);                   // 全灭（还没数据时）
```

两个几何坑（从番茄钟那版拷过来的，别再踩）：

- **中段 g 是横杠**，不是竖条。竖条的版本会让 "5" 变成怪字形。
- **显示位序 = 数值位序的倒序**：显示位 d（从左往右）对应 `10^(digits-1-d)`。
  按 d 直接取 10 的幂次会让 "17" 显示成 "71"。

---

# 电子工牌 / 二维码名片 —— 资料配置说明

两个应用都从**自己 app 目录下的 `profile.txt`** 读资料，格式完全一样。
没有这个文件时用内置默认资料，开机即用。

## 目录在哪

| 应用 | 资料/头像目录 |
|---|---|
| 电子工牌 | `apps/badge.app/data/` |
| 二维码名片 | `apps/qrcard.app/data/` |

注意是 **`.app`（点）**，不是 `_app`（下划线）。

## profile.txt 格式

```ini
[card]
name=你的名字
title=职位
company=公司
dept=部门
id=工号
phone=13800000000
email=you@example.com
url=https://example.com
status=一句状态
avatar=avatar.png
qr=1

[card]
name=第二张名片
...
```

- `[card]` 开头一张新名片，可以写多段（工牌/名片都能切换）
- 不认识的 key 会被忽略
- `qr=0` 表示这张不显示二维码（仅电子工牌用）
- `avatar` 是相对 app data 目录的文件名

## 怎么传上去

设备的 HTTP 接口（端口 8080，Bearer token 是 PIN）：

```bash
curl -X POST \
  -H "Authorization: Bearer 444108" \
  -H "X-Filename: profile.txt" \
  --data-binary @profile.txt \
  "http://<设备IP>:8080/fs/upload?path=apps/badge.app/data"
```

头像同理，`X-Filename` 换成 `avatar.png`（PNG/JPG 都行，会自动灰度 + Bayer 抖动适配墨水屏）。

## 改完要点一下「重载」

动态应用的界面实例会被框架**缓存复用**（日志里是 `lifecycle: resume app xxx` 而不是 `create`），
所以改完 profile.txt 不会自动生效。两个 app 都有「重载」按钮，点一下就重新读文件 + 重新画头像。
（换 .so 也是同理：重装后界面还是旧的，得重启设备才会重建。）

## 二维码内容

- **二维码名片**：整张 vCard，手机扫了直接进通讯录（姓名/公司/职位/电话/邮箱/网址）
- **电子工牌**：优先放 `url`（短、扫了直接开主页）；没填 url 就放精简 vCard（姓名/电话/邮箱）。
  载荷越长二维码版本越高、每模块越小，190px 装不下就会糊，所以工牌上刻意用短载荷。
