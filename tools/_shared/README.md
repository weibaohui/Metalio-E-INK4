# 共享件

| 文件 | 用途 |
|---|---|
| `card_kit.h` | 电子名片 / 电子工牌 共用的「名片套件」：资料解析、多码条目、文本码/图片码统一渲染 |
| `badge_profile.h` | 旧版资料解析（`profile.txt`），已被 `card_kit.h` 取代，新代码别用 |
| `sevenseg.h` | 七段数码管（天气 App 用；番茄钟有一份自己内联的几何拷贝） |

## card_kit.h

三个名片类应用共用：**一份资料 + 多个码入口**。

```c
CardSet set;
card_parse(text, &set);                       // 解析 profile.txt
const CardProfile* c = &set.cards[0];
CardQr q; get_qr(c, 0, &q);                    // 取第 0 个码
lv_obj_t* img = card_qr_render(parent, &q, 280); // 文本码→lv_qrcode，图片码→位图
```

配置文件里 `[qr]` 段每行一个码 `标签|内容`：内容是链接/微信号就生成文本码，
以 `.png/.jpg/.jpeg/.bmp/.webp` 结尾就当图片码（用户上传的微信个人码、收款码）。

配套渲染：头像（`card_avatar_render`，没图就画「圆圈 + 姓名首字」占位）、
vCard 拼接（`card_build_vcard`）、data 目录路径（`card_data_path`）。

> **图片码的内存**：位图缓冲用 `malloc`，而固件配了 `CONFIG_SPIRAM_USE_MALLOC`
> + `ALWAYSINTERNAL=16`，所以 16 字节以上的分配都走 PSRAM——几百 KB 的位图
> 也不占内部 RAM（内部 RAM 要留给 TLS 握手要的 14KB 连续块）。

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

# 三个名片类应用怎么配资料

| 应用 | 目录 | 配置文件 |
|---|---|---|
| 电子名片 | `apps/qrcard.app/data/` | `profile.txt` |
| 电子工牌 | `apps/badge.app/data/` | `profile.txt` |
| 收款码 | `apps/paycode.app/data/` | `pay.txt` |

注意是 **`.app`（点）**，不是 `_app`（下划线）。

各 app 的完整格式说明见它们自己的 README：
[电子名片](../qrcard_app/README.md) /
[电子工牌](../badge_app/README.md) /
[收款码](../paycode_app/README.md)。
