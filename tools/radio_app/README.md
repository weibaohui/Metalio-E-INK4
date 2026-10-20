# 网络收音机 (Radio App) 使用与自定义电台配置说明

本应用已支持**外部自定义电台**读取。你可以自由添加任意电台名称与网络直播音频流地址（HTTP / HTTPS）。

---

## 1. 电台文件存放路径

收音机启动时会自动检查并加载以下路径的电台清单（按优先级自上而下检索）：

1. **公共推荐路径（最方便通过电脑/手机文件管理放入）：**
   - `/sdcard/metalio/e-ink/radio/stations.json`
   - `/sdcard/metalio/e-ink/radio/stations.m3u`
   - `/sdcard/metalio/e-ink/radio/stations.txt`
2. **应用私有数据目录：**
   - `/sdcard/metalio/e-ink/apps/radio.app/data/stations.json`
   - `/sdcard/metalio/e-ink/apps/radio.app/data/stations.m3u`
   - `/sdcard/metalio/e-ink/apps/radio.app/data/stations.txt`

> **提示**：如果以上路径均无文件，应用会在首次运行时自动在 `data/stations.json` 生成一份初始示范文件，方便直接修改。

---

## 2. 支持的文件格式与示例

支持以下任意一种格式（三选一即可）：

### 格式 A：JSON（推荐，结构最清晰规范）
文件名：`stations.json`
```json
[
  {
    "name": "中国之声",
    "url": "https://lhttp.qtfm.cn/live/5021381/64k.mp3"
  },
  {
    "name": "北京音乐广播",
    "url": "https://lhttp.qtfm.cn/live/332/64k.mp3"
  },
  {
    "name": "深圳飞扬971",
    "url": "http://lhttp.qingting.fm/live/4925/64k.mp3"
  },
  {
    "name": "Lofi Chill Radio",
    "url": "http://stream.lofi.cafe/stream"
  }
]
```

### 格式 B：M3U 播放列表（通用广播清单）
文件名：`stations.m3u`
```m3u
#EXTM3U
#EXTINF:-1,中国之声
https://lhttp.qtfm.cn/live/5021381/64k.mp3
#EXTINF:-1,北京音乐广播
https://lhttp.qtfm.cn/live/332/64k.mp3
#EXTINF:-1,Lofi Chill Radio
http://stream.lofi.cafe/stream
```

### 格式 C：TXT / CSV（纯文本，每行一个：名字,地址）
文件名：`stations.txt`
```text
中国之声,https://lhttp.qtfm.cn/live/5021381/64k.mp3
北京音乐广播,https://lhttp.qtfm.cn/live/332/64k.mp3
深圳飞扬971,http://lhttp.qingting.fm/live/4925/64k.mp3
```

---

## 3. 音频流规范与建议

- **音频编码**：支持标准 **MP3**、**AAC** 流，支持 ICY 元数据解析（实时抓取节目歌名）与分块传输。
- **推荐码率**：建议选择 **48kbps ~ 128kbps** 码率的流地址（墨水屏设备 Wi-Fi 功耗与带宽平衡最佳）。
- **协议**：支持 `http://` 与 `https://`（内部已包含 TLS 握手内存预检）。

---

## 4. 如何将文件上传到设备？

你可以使用以下任意一种方式放入文件：

1. **网页无线传（最方便）：**
   - 机器连接 Wi-Fi 后，在桌面打开 **WiFi 传输** 应用。
   - 用手机或电脑浏览器访问屏幕上显示的网址（如 `http://192.168.0.x/`；端口被占用时自动回退到 `8088`）。
   - 在网页的「文件管理」页签中，将 `stations.json` 上传到 `radio/` 目录。
2. **命令行 / curl 远程上传：**
   ```bash
   curl -X POST "http://<设备IP>:8080/fs/upload?path=radio" \
        -H "Authorization: Bearer <PIN码>" \
        -H "X-Filename: stations.json" \
        --data-binary @stations.json
   ```
3. **读卡器 / TF 卡拷贝：**
   - 取出 TF 卡插入电脑，直接在 `metalio/e-ink/radio/` 目录下创建或编辑 `stations.json`。

---

## 5. 界面功能与操作说明

- **范围切换按钮（右上角）**：
  - 点击可在 `自定义 (N)`、`内置 (339)`、`仅收藏` 三种模式间循环切换。
  - 检测到外部自定义电台时，默认进入 `自定义` 列表。
- **当前播放高亮**：
  - 正在播放的电台无论在哪个列表中，均会以 **加粗边框 + 粗体** 突出标出。
- **上一台 / 下一台 / 随机**：
  - 会在当前视图所在的电台列表内自动循环切台。
- **收藏夹（★）**：
  - 点击播放器卡片右下角的 `☆` / `★` 即可一键收藏或取消当前播放中的电台（自定义与内置电台皆可收藏）。
- **音量控制**：
  - 底部配有 `音量 -` / `音量 +`，直接作用于板载音频硬件。
