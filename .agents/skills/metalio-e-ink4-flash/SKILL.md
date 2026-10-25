---
name: metalio-e-ink4-flash
description: Build, flash, observe, debug, and develop services/UI/MCP for the Metalio E-Ink4 ESP32-S3 e-paper device on this Mac. Use for requests mentioning Metalio E-Ink4, this e-ink/e-paper device, its firmware, GLM usage app, local MCP service, burning its checkout, or on-device faults (hang/freeze/no-response/panic/100% CPU/no audio) needing OpenOCD+gdb live forensics or coredump analysis; not for generic ESP32 boards.
---

# Metalio E-Ink4 开发与烧录

本 skill 只针对本机桌面上的 **Metalio E-Ink4** 电子墨水屏设备。它是 ESP32-S3 / 16 MB Flash / 800×480 e-paper 设备，不是通用的合宙 CH343 开发板。

## 关键本机事实

- 默认 checkout：`/Users/weibh/Desktop/Metalio-E-INK4`
- 烧录串口：`/dev/cu.usbmodem11401`（USB Serial/JTAG；**名称随插的 USB 口变**,换口后 ls /dev/cu.usbmodem* 确认,别认死）
- 波特率：`460800`
- 芯片：`esp32s3`
- ESP-IDF：`$HOME/esp/esp-idf`
- IDF Python：`$HOME/.espressif/python_env/idf5.5_py3.13_env`
- ROM ELF：`$HOME/.espressif/tools/esp-rom-elfs/20241011/`
- Xtensa toolchain：`$HOME/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/bin`

不要把这个设备和 `esp32-s3-dev` skill 中合宙板子的 `/dev/cu.wchusbserial*` 规则混用。

## 首选流程

用户已经明确要求烧录时，运行：

```bash
bash "$HOME/.agent/skills/metalio-e-ink4-flash/scripts/flash.sh" \
  "$HOME/Desktop/Metalio-E-INK4"
```

脚本会：

1. 设置本机 ESP-IDF 环境变量；
2. 校验目标工程；
3. 增量执行 `idf.py build`；
4. 确认串口存在且未被占用；
5. 用 `idf.py flash` 按 `build/flash_args` 写入完整启动所需的镜像；
6. 等待 esptool 对每个分区做 hash 校验；
7. 抓取 20 秒串口日志并检查常见崩溃标记。

常用变体：

```bash
# 只编译
... scripts/flash.sh "$HOME/Desktop/Metalio-E-INK4" --build-only

# 只观察当前设备串口 20 秒
... scripts/flash.sh "$HOME/Desktop/Metalio-E-INK4" --log-only

# 打开 IDF monitor
... scripts/flash.sh "$HOME/Desktop/Metalio-E-INK4" --monitor

# 指定其他端口/观察时长
... scripts/flash.sh "$HOME/Desktop/Metalio-E-INK4" --port /dev/cu.usbmodem11401 --seconds 30
```

## 手动环境

如需手动执行，必须直接设置这些变量；不要依赖本机 `export.sh`：

```bash
export IDF_PATH="$HOME/esp/esp-idf"
export IDF_PYTHON_ENV_PATH="$HOME/.espressif/python_env/idf5.5_py3.13_env"
export ESP_ROM_ELF_DIR="$HOME/.espressif/tools/esp-rom-elfs/20241011/"
export PATH="$IDF_PYTHON_ENV_PATH/bin:$IDF_PATH/tools:$HOME/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/bin:/opt/homebrew/bin:/usr/bin:/bin"
```

然后在工程根目录：

```bash
idf.py build
idf.py -p /dev/cu.usbmodem11401 flash
```

## 安全边界

- 用户说“刷一下/烧录/试试”即已授权按当前 `build/flash_args` 写入。
- 不要默认整片擦除；whole-chip erase 是破坏性操作，必须单独获得明确授权。
- 不要只烧 `build/xiaozhi.bin` 到 `0x0`；应用镜像的正确偏移由 `flash_args` 决定。
- 若设备处于“虚拟 U 盘”模式，Serial/JTAG 会不可用；先让用户停用 U 盘/弹出 SD。
- 串口一次只能一个进程使用。烧录前确认没有 `idf.py monitor`、screen 或其他进程占用端口。
- 若端口不存在，让用户确认已开机、重新插线；仍失败时按住 BOOT 再插入 USB。不要盲目尝试其他串口。

## 报告要求

完成后分开报告：

- **Build**：是否成功，`build/xiaozhi.bin` 与 SHA256；
- **Flash**：端口、芯片/MAC、所有分区 hash 是否 verified；
- **Device log**：观察窗口内是否有 panic/abort/reboot 循环，屏幕、触摸、电池等实际结果；
- **Unverified**：没有实际看到的功能不要标 PASS，例如屏幕内容、语音、Wi-Fi、SD、按键必须由用户确认或日志明确证明。

## 应用与服务开发经验

### 固件架构约定

- 当前主线继续使用 CloudZao / Metalio 固件；不要为了 CrossMux 重写整个设备栈。CrossMux 的 web 配置、HAL、生命周期思想可以借鉴，但迁移 AI 助手、MCP、云协议、音频、LVGL 页面成本很高。
- 常驻能力应做成 `main/services/<service>/` 里的服务类：持有配置、NVS、HTTP、轮询、可观察状态，并自己注册 MCP 工具。LVGL 页面只做 observer/切换，不直接放业务和轮询。
- 新增服务源文件必须同时改 `main/CMakeLists.txt` 的 `SOURCES`；如果用 IDF 组件，还要加 `target_link_libraries(... idf::<component>)`。只加头文件路径不够。

### 本地 HTTP MCP 常驻服务

- 已实现的独立服务在 `main/services/local_mcp/`，监听：

```text
http://<device-ip>:8080/mcp
```

- 这个 MCP 不依赖 Xiaozhi WebSocket/MQTT。Xiaozhi 的 `type:"mcp"` 通道仍保留为兼容路径；新增工具会同时暴露给两个入口。
- Streamable HTTP 至少支持：
  - `POST /mcp` 处理 `initialize`、`tools/list`、`tools/call`；
  - `notifications/*` 立即返回 HTTP 202，不要让请求等 10 秒超时；
  - `GET /mcp` 明确返回 405，本实现不提供 SSE；
  - `OPTIONS /mcp` 返回 CORS/Allow 头。
- `McpServer::ReplyResult/ReplyError` 使用 `ResponseSink`，不要硬编码回包到 `Application::SendMcpMessage`。HTTP handler 传一个捕获字符串的 sink；Xiaozhi 协议传原有 sink。
- 工具回调会被 `Application::Schedule()` 放回主循环执行。HTTP handler 需要用 condition variable 等待 sink 回包，并设超时；不要在 HTTP server 栈里直接执行会碰 LVGL/外设的工具。
- IDF 5.5 的 `esp_http_server` 注意：
  - 写 `httpd_config_t config = HTTPD_DEFAULT_CONFIG();`，不要用 `auto config = ...`，宏内混合整型会让 CTAD 失败；
  - `httpd_req_get_hdr_value_len(req, field)` 直接返回 `size_t`，没有出参；
  - 如果本版头文件没有 `httpd_resp_set_err()`，用 `httpd_resp_set_status()` + `httpd_resp_set_type()` + `httpd_resp_sendstr()`。
- 默认当前固件 LAN 内未强制鉴权。若要加 token，可读取 NVS namespace `local_mcp` 的 `token`，然后要求 `Authorization: Bearer <token>`；改之前先明确用户是否接受局域网裸暴露。

### GLM 用量服务经验

- 相关代码在 `main/services/glm_usage/`。配置语义是：用户只提供 API Key 和一次性的网页 `jwt_token`；organization/project 由服务发现并持久化，不让用户手输。
- BigModel 监控接口不能直接把 `<id>.<secret>` 原样放 `Authorization`。设备端要用 API Key 生成短期 HS256 JWT：
  - header：`{"alg":"HS256","sign_type":"SIGN"}`；
  - payload：`api_key`、`exp`、`timestamp`；
  - secret 是 API Key 中点号后的 secret。
- `mbedtls_base64_encode()` 不要用 `dst=nullptr, dlen=0` 探测长度后把非零返回当失败；ESP-IDF 下会得到空串、HTTP 报“未收到 Authorization”。直接分配 `((len + 2) / 3) * 4 + 1` 缓冲区编码。
- 组织/项目发现接口：

```text
GET https://bigmodel.cn/api/biz/customer/getCustomerInfo
Authorization: Bearer <网页 JWT>
```

- 响应里的 `data.organizations[].projects[]` 可有多套。`projectType == 2` 是当前实测的团队 Coding Plan 上下文；自动选择时优先它，但所有 org/project 都要保存，UI 可切换。
- 持久化结构：
  - `glm/api_key`：API Key；
  - `glm/contexts_json`：全部 org/project/name/projectType/default；
  - `glm/active_context`：当前选中下标；
  - 网页 JWT 只放内存 `pending_web_token_`，发现成功或失败后都不落盘。
- 配额接口用团队头和 `?type=2`：

```text
Authorization: <API Key 派生 JWT>
bigmodel-organization: org-...
bigmodel-project: proj-...
```

- 重置次数来自 `customer-package-reset/list?targetType=TEAM`，统计 `fiveHourResets[]`、`weekResets[]` 中 available 项。偶发失败可重试 3 次，失败时保留旧值。
- MCP 工具保持少而直接：`configure(api_key,jwt_token)`、`get_config`、`get_usage`、`refresh`。不要把 organization/project 暴露成用户必填参数。

### LVGL / NVS / 任务栈红线

- 不要在 LVGL/PSRAM 调用栈里直接读 NVS 或触发 SPI flash。实测 `GlmUsageScreen::Create()` 调 `Start()` -> `Settings::GetString()` 会断言：

```text
assert failed: spi_flash_disable_interrupts_caches_and_other_cpu
cache_utils.c:127 (esp_task_stack_is_sane_cache_disabled())
```

- 服务启动、NVS load/persist 放在 `Application::Start()` 或专用 FreeRTOS worker；页面创建只读内存状态。
- UI 事件里不要直接持久化。点击切换 context 只更新内存 active index、发 refresh 事件，由 worker 完成 NVS 持久化和刷新。
- GLM worker 同时跑 HTTPS、JSON、多 context 快照，栈用 16 KB；过小可能在请求或解析阶段栈溢出。
- LVGL 动态页签/card 时，保存 count/active index，状态 timer 每秒同步；不要每次刷新全量重建控件。
- e-paper 数字和单位建议用 `fontpack_lv_font_get(30, 2)` 的 30 号字体；25 号 UI 字体的数字样式在小屏上易误读。

### 假死/Cache panic 三雷（2026-10 收音机页实测，OpenOCD 冻结现场定位）

**雷三:esp_lvgl_adapter 的 EPD/I1 显示桥对 flush/repair 区域零边界校验(已打补丁)。**
三次整机死机全部同源,都在 `managed_components/espressif__esp_lvgl_adapter/.../lvgl_bridge_v9.c`:

- `display_bridge_v9_prepare_mono`(~840 行):双重像素循环直接跑 `area->y1..y2`,异常几何时循环数十亿次 → 整机假死 90 秒(实测,PC 采样定位)。
- `copy_diff_repair_from_front_to_back`(~3010 行):`copy_w_px=(size_t)(rect.x2-rect.x1+1)` 对翻转/越界 rect 强转成天文数字 → memcpy 在 PSRAM 帧缓冲越界 → EXCCAUSE=7 存储异常。
- **panic 处理器自身随后卡死**(:136 `g_exc_frames[core_id]=frame` 附近)——无打印、无 coredump、USB 半死。这就是"死机却查无痕迹"的完整解释。

**补丁**:两处循环钳制到屏幕范围、空/翻转区域跳过,代码内 `PATCH(metalio)` 标记;仓库 `patches/README.md` 记录背景与验证(`grep -c "PATCH(metalio)" ...lvgl_bridge_v9.c` 应为 2)。`idf.py update-components` 会覆盖组件,覆盖后必须重放补丁。

**雷四:fontpack 规格缺口返回 NULL 字体 → lv_text 测量死循环(第三种假死形态)。**
`fontpack_lv_font_get(size, bpp)` 对字库没有的规格返回 NULL(且打一条 W "no glyphs for size=X bpp=Y")。任何控件把 NULL 字体直接 `lv_obj_set_style_text_font` 后,LVGL 布局测量 `lv_text_get_size_attributes(font=0x0)` 会在 LVGL worker 上死循环卡死 —— 整机假死,只能 JTAG 复位(实测:电源 APP 用 34 号字,字库只有 25/30/40 等零散规格,进屏即挂)。
**规矩**:`fontpack_lv_font_get` 的返回值**必须空守卫回退 `UiFont()`**,全工程无例外;新界面选字号前先确认字库有该规格;串口出现 `no glyphs for size=` 即有控件拿到了 NULL 字体,虽然守卫后不崩,但字号与设计不符,应改规格或补字库。

**雷一：把 NULL 控件喂给 LVGL = 无限重绘假死，不是崩溃。**
`MakeButton()` 返回的就是 label 本身，`lv_obj_get_child(MakeButton(...), 0)` 取的是"label 的第 0 个子对象"＝NULL。`lv_label_set_text(NULL, ...)` 不会立刻崩：NULL 偏移读出的垃圾坐标进入失效区域，LVGL 渲染器对天文数字区域逐像素混合（`display_bridge_v9_prepare_mono` + `lv_draw_sw_blend_to_i1` 的 `set_bit`），一次渲染几十秒，1s 状态 timer 再来一轮 → LVGL 锁被占死、EPD 永不刷新、整机无响应。特征：内核 1 100%、RAM 平稳、栈很浅、串口无 panic。
**规矩**：助手函数返回什么要看清；`SyncStateLabels` 这类批量刷新函数对每个 label 指针全量判空，不要只守卫第一个。

**雷二：任何 flash/NVS 写 × 核 1 LVGL 临界区 = `Cache disabled but cached memory region accessed` panic。**
flash 写会关全局 cache 并 IPC-stall 另一核；若核 1 正在临界区（LVGL 内部大量 `portENTER_CRITICAL`），stall 拦不住它，它继续执行 flash 常驻代码即炸。雷一的无限重绘让核 1 长期处于危险窗口，此时任何 flash 操作（音量 NVS、GLM 拉取、字体 mmap）都会引爆。
**规矩**：
- 高频路径（按键回调、MCP 工具）绝不直接写 NVS。音量类做"内存立即生效 + 去抖合并 + 投递 main 任务落盘"（见 `AudioCodec::SetOutputVolume`）。
- `iot_button` 的回调跑在 **esp_timer 任务**上——在回调里做 NVS/HTTP/长操作会阻塞全部软件定时器，一律 Schedule 到 main 或开内部栈短任务。
- `xTaskCreateWithCaps`（PSRAM 栈）创建的任务：flash/NVS/`esp_wifi_*` 一律投递 main 任务执行；自删必须用 `vTaskDeleteWithCaps` 配对（纯 `vTaskDelete` 泄漏 PSRAM 栈，IDF 对 NULL 自删有官方支持）。

### 墨水屏长文本滑动翻页(#47 模式,2026-10-13 落地)

- **LVGL indev 不可信**:本机实测 LVGL indev 对每手势只交付一个静止点
  (PRESSED→PRESSING#1(同坐标)→RELEASED,与轮询/INT 模式无关;而
  touch_feed 层的原始采样连续平滑——两层各挂日志一比即知)。凡自定义
  滚动 UX,绕过 indev,直接在 touch_feed 层做。
- **模式**:touch_feed 滑动状态机(按下记原点/移动记 last_y/抬手算
  dy)+ `SetSwipeScrollCb` 回调屏幕 + 屏内 `s_read_offset` 绝对阅读
  偏移 + `lv_obj_scroll_to_y` 绝对定位。一次手势一次墨水屏刷新。
  盖板键 vk_prev/vk_next 映射上/下翻页。
- **方向/歧义规避**:绝对偏移语义(0=文字开头,最大=内容高-视口高),
  上一页→偏移减小,方向由定义保证;相对 scroll_by 在 SCROLLABLE 已清
  的容器上会计账错乱(负向被吞、余量失真、滚进空白回不来)。
- **触摸初始化两件套**:开机 RST 脉冲唤醒休眠残留(待机写入的 0xA5=0x03
  跨 ESP 复位存活,芯片睡着的 boot 触摸全死且无任何报错日志)+ init
  重试 5×150ms(冷启动 I2C probe 竞争)。
- **诊断方法论**:管线分层各挂日志(原始采样→快照→indev→滚动执行),
  一次滑动定位断点在哪一层。这次三层全挂后发现断点在 indev 层,
  原始层完好——两小时盲猜 vs 三分钟分层日志。
- 遗留坑:屏上按钮翻页被用户否决(只留滑动+盖板键);GPT 并发编辑
  同文件时构建错误行号会漂移,先 git status 确认归属再修。

### AI 对话流式 TTS(2026-10-12 落地,模块 minimax_bidi_tts)

- 架构:WebSocket 双向流式 `wss://api.minimax.cn/ws/v1/t2a_v2_bidi`。LLM 边出字边发
  `task_continue`(服务端自动攒句),音频以 hex PCM 16k 分片(4-16KB)`task_continued`
  回来即播;`task_cancel` 原生打断。**分片即播从根上消除了整段 JSON 落地的 MB 级 OOM**
  (4be6684 案:440 字回复 ~1.7MB 响应,四份副本并存打爆 PSRAM → bad_alloc 未捕获 → abort)。
- 管线拓扑:管线任务持 `MiniMaxBidiTts` 会话,独立 `aichat_play` 任务(PSRAM 栈)
  `PopAudioPcm(960, 40ms)` 轮询 → `codec->OutputData`;PA 用 `PowerNeed::AiChatTts`
  在建连时 Acquire、三个完成路径 Release。
- 发送侧纪律:**智能断句缓冲**——按标点/长度分批发送,防逐字暴洪产生微帧;UTF-8 边界
  截断安全;`task_flushed` 事件确认服务端音频全部下发,播报完成等待用它,不盲等。
- 错误分级:WSS 建连失败自动降级 HTTP TTS(t2a_v2 整段式);OOM/异常双层捕获
  (Synthesize 层 + 管线层),报"TTS 内存不足(回复过长)"而非 abort。
- 会话拆除的服务端关连会产生 `mbedtls_ssl_fetch_input error=76 / -0x004C` 噪音,
  属正常,别当故障。

### 编译陷阱(GCC14 / xtensa libstdc++,2026-10-12 三连实测)

1. **`#include` 绝不能写在 namespace 里面**:`minimax_speech.h` 把
   `#include <functional>` 放进了 `namespace minimax_speech {}`,functional 传递
   包含的所有标准头都被裹进 `minimax_speech::std`,全文件 `std::mutex/string/vector`
   报 "is not a member of 'minimax_speech::std'" 30+ 连环错,且污染传递给所有包含方。
   症状识别:诊断里出现 `minimax_speech::std` 字样 = 某头文件的 include 落在命名空间内。
2. **`<format>`/unicode 相关模板 codegen 需要 `<array>`**:GCC14 xtensa 的 libstdc++
   在 codegen 阶段实例化 unicode.h 时 `std::array` 未定义(头内未自含 `<array>`)。
   `-fsyntax-only` 能过、真正 `-c` 编译才炸——**不要用语法检查判断这类错误**。
   涉及 <format> 的 TU 显式 `#include <array>`;这是持久要求,删了就复发。
3. **组件抽离必须补 REQUIRES**:`components/reader_core` 用 `<ff.h>` 要
   `REQUIRES fatfs`;主组件 INCLUDE_DIRS 重组时按报错逐个补回子目录
   (settings_screen/settings_test/backup_screen 等)。
4. **ml307 WebSocket API**:`SetHeader(key, value)` 两参都是 `const char*`,传
   `std::string` 表达式要 `.c_str()`;`SetReceiveBufferSize(size_t)`。

### SpawnPsrWorker 任务返回纪律(再强调)

SpawnPsrWorker 创建的 PSRAM 栈任务:**任务函数直接 `return`**,包装层自动
`vTaskDeleteWithCaps` 自删。裸 `vTaskDelete(nullptr)` 会跳过配对释放——
**每次泄漏整个 PSRAM 栈**(实测 4KB/次,流式播报每对话一轮漏一块)。

### i18n 文案流程(生成物勿手改)

- 源头:`assets/locales/{zh-CN,en-US}/language.json` 的 `strings` 字段。
- 生成:`scripts/gen_lang.py` 产出 `lang_config.h`(gitignored)与
  `lang_tables.cc`(入库)。CMake custom command 挂了依赖,改 json 即自动重生成。
- **手改生成物 = fresh clone/worktree 全量编译必挂**(Lang::Strings 缺成员),
  工作区因生成物残留侥幸通过——最阴险。新文案:写两份 json → 跑 gen_lang.py
  → 提交三个文件(json 2 份 + 重生成产物)。

### 音频 / 收音机服务经验

- 音频架构：板载 codec 是 `BTAudioCodecDuplex`（I2S 从机 32bit 立体声，16 kHz），`Write()` 内部有 `data_if_mutex_`。`AudioService::Stop()` **不 disable I2S 通道**，通道一旦 `Start()` 使能就全程保持——收音机等"只开输出"路径依赖这一点。
- `AudioCodec::Start()` 里 `i2s_channel_enable` 对已使能通道返回 `ESP_ERR_INVALID_STATE`，配合 `ESP_ERROR_CHECK` 会直接 abort；必须用 `started_` 守卫幂等化。
- `BTAudioCodec::Write` 不要用 `ESP_ERROR_CHECK(i2s_channel_write(..., portMAX_DELAY))`：任何非 OK 返回（通道被 disable）都 abort 整机，DMA 停摆时永久阻塞占死 `data_if_mutex_` → LVGL/MCP 全部拿不到锁假死。改有限超时 + 出错记日志丢块。
- `AudioService` 的 opus 编解码器按需起停时，**Stop 必须等任务真正退出再 reset 对象**：`OpusCodecTask` 在 `lock.unlock()` 后裸用 `opus_decoder_->Decode()`，`vTaskDelay(50ms)` 软等待不构成同步点，直接 `reset()` 是 use-after-free。用"任务退出前置 atomic 标志、Stop 轮询该标志（带超时）"的握手（见 `opus_task_exited_`）。
- 收音机播放链：`appfw_stream`（ICY/HLS/http 分身回退）→ MP3 simple decoder → 下混 → 重采样 16k → 小片（960 样本）直写 codec；播放期间 `KeepOutputAlive()` 防电源状态机关输出。PSRAM 栈任务禁止 NVS/flash/`esp_wifi_set_ps`，一律 `Application::Schedule` 到 main（12 KB 内部栈）。
- 内部 RAM 是全系统瓶颈：开机稳态 ~50 KB，一次 TLS 握手要 ~40 KB 连续内存。切台/开 https 流时旧流未关+新握手并发会把 lwIP 饿死（网络 100% 丢包、MCP 无响应、CPU 还在跑——看起来像死机）。开机就做 TLS（如 GLM 启动即拉取）会把剩余内存打到 9 KB；服务按需启停（PR #4 模式）能稳住 49 KB。

### PSRAM / 内部内存策略

- 配置已全开：`SPIRAM_USE_MALLOC=y` + `MALLOC_ALWAYSINTERNAL=16` → **>16 字节的 malloc/new 优先落 PSRAM**，std 容器/HTTP/TLS 动态缓冲基本自动进 PSRAM。写代码时不要画蛇添足地全部显式 `MALLOC_CAP_SPIRAM`；显式写只用于"必须进 PSRAM 的大块"（解码缓冲、任务栈）。
- 已经在 PSRAM 的：LVGL 绘制缓冲 + EPD 双帧缓冲（`lv_adapter_display.cc`）、LVGL/audio/radio 任务栈、收音机解码缓冲。
- **搬不动的内部 RAM 钉子户**：WiFi 射频/DMA 缓冲（几十 KB）、main 任务 12KB 栈（Schedule 路径含 NVS/flash，PSRAM 栈必崩）、esp_timer/IDLE/中断栈、外设 DMA 描述符。
- 还能挤的：`touch_feed` 4KB 栈迁 PSRAM；`CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM` 等静态缓冲调优（每砍 1 个 RX 省 ~1.6KB，弱网代价）；lwIP mbox 尺寸。
- 内部 RAM 基线 ~49KB（PR #4 按需启停后）。**"剩余"不等于"可用"**：TLS 要的是最大连续块，调优前先在系统监控里加 `heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)` 打印看碎片化。
- 旧注释里"一次 TLS 握手要 40KB 内部连续内存"写于 `MBEDTLS_DYNAMIC_BUFFER` 之前，动态缓冲现在走 malloc（大概率已落 PSRAM）——引用前先实测复核。
- **实测复核结果(2026-10-09)**:日历 ICS 的 TLS 预检阈值 30KB 在开机碎片化下永远拦截(44KB 空闲但最大块 <30KB),降到 12K 后握手一次成功(359 条节假日入库)——证实动态缓冲已 PSRAM 化,内部 12KB 余量即够。教训:防护性预检阈值若高于实际需求,就从保护变成永久禁止;放宽是安全的,前提是失败路径干净(esp-tls 失败只报错不死机,PR7/PR21 两处已验证)。

### 冻结/崩溃现场调试（OpenOCD + gdb + coredump）

本机 `/dev/cu.usbmodem11401` 走的是 **S3 原生 USB-Serial-JTAG**，OpenOCD 可直接 attach，不需要外部 JTAG：

```bash
# 1. attach（会短暂复位核；esptool 和它可共存，但 esptool 的 hard_reset 会清掉冻结现场）
~/.espressif/tools/openocd-esp32/v0.12.0-esp32-20260424/openocd-esp32/bin/openocd \
  -f board/esp32s3-builtin.cfg   # telnet 4444 / gdb server 3333

# 2. 死循环假死：高频采样核1 PC 找循环体（比单次 halt 可靠得多，Xtensa 窗口 ABI 穿异常帧的 gdb 回溯会失真）
printf 'targets esp32s3.cpu1\nhalt 20\nreg pc\nresume\nexit\n' | nc -w 6 localhost 4444   # 重复 N 次取直方图
xtensa-esp32s3-elf-addr2line -pfiaC -e build/xiaozhi.elf 0x...

# 3. panic 冻结：IDF 检测到调试器只打 3 行就 halt 等 gdb（串口没有完整 backtrace！）
#    此时用 gdb 拿 FreeRTOS 感知的全任务现场：
~/.espressif/tools/xtensa-esp-elf-gdb/17.1_20260402/xtensa-esp-elf-gdb/bin/xtensa-esp32s3-elf-gdb \
  -q build/xiaozhi.elf -ex "target remote localhost:3333" -ex "info threads" -ex "thread 3" -ex "bt 20" -ex "detach"
# gdb 未装时: cd ~/esp/esp-idf && python tools/idf_tools.py install xtensa-esp-elf-gdb
```

- **coredump 一键解析（已沉淀成脚本，别再手写解析器）**：

```bash
# 设备直读（⚠️ 会硬复位设备——gdb 活体取证全部做完之后再读！）
"$HOME/.espressif/python_env/idf5.5_py3.13_env/bin/python" \
  "$HOME/.agents/skills/metalio-e-ink4-flash/scripts/parse_coredump.py" --port /dev/cu.usbmodem11401 \
  --elf ~/Desktop/Metalio-E-INK4/build/xiaozhi.elf

# 已读出的分区文件
... parse_coredump.py /tmp/core.bin --elf .../build/xiaozhi.elf
```

  输出：panic 原因文本、崩溃固件指纹 vs 本地 build 对照、崩溃任务名（从 TCB 提取）、异常寄存器、全部任务 PC/A0/SP 符号化。
- coredump 分区在 `0xfe4000`（64 KB，`partitions/v1/16m.csv`）。关键布局（脚本头注释里有全表）：flash 前 24 字节是头（首 u32=总长），ELF 从 offset 24 开始；`NT_PRSTATUS` 的 gregs 在 desc 的 word18=pc / word82=a0 / word83=sp；`ESP_EXTRA_INFO` 的 `[0]`=崩溃任务 TCB（+52 处是任务名字符串）；`ESP_PANIC_DETAILS` 直接就是原因文本。
- **判读纪律**：① 先看指纹再下结论——coredump 可能是几个版本前的旧固件留的；② `Guru Meditation` 到达串口的只有 3 行（有调试器 attached 时），完整现场在 gdb/coredump 里；③ `ESP_ERR_NVS_KEY_TOO_LONG` 这类一行 ERROR 也可能就是根因（NVS 键 ≤15 字符）。
- **panic 处理器自身卡死时(无打印/无 coredump/USB 半死),异常帧仍在内存里**:gdb 连上后读 panic_handler 的静态数组 `p g_exc_frames`(每核一个帧指针),按 XT_STK 布局解码——`word[0]=exit, [1]=pc, [2]=ps, [3..18]=a0..a15, [20]=exccause, [21]=excvaddr`(mdw 26 字即够)。2026-10-09 实测从"冻在 panic_handler:136"的机器上解码出真凶 PC 在 EPD 桥 diff 函数。注意 g_exc_frames 可能两核都有值,只有真正 panic 的那个是异常帧(另一个可能是非异常栈内容,特征:pc 位置不是合法 flash 地址)。
- **USB-Serial-JTAG 串口丢日志的两个坑**:① 主机没监听时设备日志直接丢弃——开机前 0-10 秒日志永远缺失,新开的 cat 会以一条陈旧的 `I (115) Board: UUID` 开头(那是 JTAG FIFO 残留,不是本次时间戳!);要抓全 boot 必须先挂 cat 再用设备电源键重启(esptool 复位会和 cat 抢端口)。② cat 进程死亡/重启会造成 USB-CDC 阻塞,panic 打印可能永远出不来。
- **「点什么都没反应」≠ 死机——先查 I2C 总线**:待机 LP 轻睡眠可打断在途 I2C 事务 → 总线卡死,之后所有 I2C 设备(触摸/IO 扩展器按键/电量计/RTC)同时失联,但系统本体活着(系统监控日志照打、内核低负载)。特征串口线:`i2c.master: I2C transaction timeout detected` + `io_expander ... Read input reg failed` 每秒重复 + 电量计读回 0。恢复=重启;根因(轻睡眠×I2C 轮询竞态)待查——方向:进轻睡前静默 I2C 轮询源/给总线加恢复。2026-10-09 首次实锤,复发时用 OpenOCD 看哪个任务卡在 i2c 发送上。
- **验证"设备到底在跑哪个版本"三件套**（源码改了没烧/烧了没提交，经常分不清）：
  1. otadata `0xd000`：每条目 32 字节，首 u32=ota_seq，活动槽 = `(seq-1)%2`（ota_0/ota_1，分区表 `16m.csv`：ota_0=0x80000, ota_1=0x580000）；
  2. 读活动槽头部 0x200 字节，找 magic `0xABCD5432`，+16 version、+96 date、+80 time、+144 app_elf_sha256；
  3. sha 前 16 位对照 `build/xiaozhi.bin` 同位置的 sha；再对照 coredump 里的指纹（三个都一致才算"在跑我刚编的"）。
- **刷某个 PR 分支的构建 = 刷它的基线,不是刷 accumulated main**(实测两次:刷 PR10 丢了 PR9 对时;PR14 基线缺 id 字段)。规矩:烧 PR 分支构建前必须先 `git merge origin/main` 进该分支解决冲突后重编;烧完用三件套验证。
- gdb `info threads` 直接列 FreeRTOS 任务名（lvgl / main_event_loop / esp_timer / radio...），`thread N` + `bt` 是定位"哪个任务闯祸"的最快路径；`p obj` 能直接看到 `lv_label_set_text(obj=0x0)` 这种铁证。
- 判读纪律：coredump 可能是**旧固件**留的（对照构建指纹）；冻结 ≠ panic，两种形态要分开抓；"看起来像死机"的三种实体——LVGL 锁被死循环占死、flash 写饿死 lwIP、真 panic——对应三种完全不同的修法。

### 案例复盘：「点开页面无反应 + 内核1 100%」完整定位过程（2026-10-08 收音机页）

**价值**：整套流程没有改一行代码就找到了根因；遇到任何「无反应/假死/莫名重启」都按这个顺序走。

**第 0 步 · 分诊——「无反应」是三种完全不同的病，先看病是哪种：**

| 症状信号（都从串口日志读） | 诊断 | 抓法 |
|---|---|---|
| `系统监控` 每秒日志断流 / Guru Meditation / assert | panic 重启 | 读 coredump |
| 系统监控照打、touch down/up 照打、出现 `esp_lv_adapter_lock Failed`（每 30s） | 真冻结：LVGL worker 抱锁死循环 | 活体 attach + PC 采样 |
| CPU 低、WiFi 关联但 TCP 全断、MCP 无响应 | 网络栈饿死（TLS 内存） | 查内存水位 |

辅助判据：`内核1: 100%` = 忙循环（不是死锁——死锁会睡）；`@@@RAM 剩余` 平稳 = 不是分配泄漏；RAM 抽干 = 分配循环。本轮判决：系统监控/touch 照打 + LVGL 锁超时 + 内核1 100% → **LVGL worker 死循环占死锁**。

**第 1 步 · 串口时间线定边界**：找最后一条正常日志（`HomeScreen: launch 收音机`）→ 冻结始于页面导航；此后 `lcd_panel.ssd1677` EPD 刷新日志归零 → 渲染管线根本没走到 flush。**最后正常日志的位置 = 嫌疑代码的下界。**

**第 2 步 · 历史现场（coredump）要先验货**：flash 里的 coredump 对照构建指纹（`ESP_CORE_DUMP_INFO` 里的 sha 前缀 vs `build/xiaozhi.bin` 的 app_elf_sha256），旧固件的尸体对当前代码毫无意义。本轮第一次读到的就是两个版本之前的 `radio_boot` 现场，差点带偏。

**第 3 步 · 活体 attach（冻结现场是金矿，别手贱重启）**：
- `openocd -f board/esp32s3-builtin.cfg` → telnet 4444；
- 单次 `halt 20` + `reg pc` + `resume` 无害，可反复做；
- **严禁对冻结现场跑 esptool**——`--after hard_reset` 会把现场清掉（实际踩过：读完 coredump 把 panic 冻结现场洗没了）；
- panic 时 IDF 检测到调试器只打 3 行就 halt 等 gdb，**串口永远等不到完整 backtrace**，必须 gdb 上去看。
- ⚠️ **gdb 检查运行中的系统必须先整体 halt 双核**(`halt 30` 两个核都停):对运行中的系统做 `info threads`/`thread N`/`bt` 会反复 halt/resume,Xtensa 上有概率打坏任务上下文 → detach 后立刻 IllegalInstruction panic(2026-10-09 实测把活机搞崩)。安全顺序:telnet `halt` 双核 → gdb 只读不 resume → 完全离开时 `resume`。

**第 4 步 · gdb 任务级解剖**：`info threads` 直接列 FreeRTOS 任务名 → `thread 3`(lvgl) + `bt` 拿到嫌疑栈。**注意：单次 gdb 回溯会骗人**——cache error 是异步中断，epc 指向不相干的 idle hook；Xtensa 窗口 ABI 穿异常帧的 unwind 链失真。嫌疑栈当线索用，不当结论用。

**第 5 步 · 决定性手段——PC 采样直方图**（对「死循环假死」的通用杀手锏）：

```bash
# 循环 60 次：halt → 读 pc → resume，每次间隔 ~1s（或后台脚本批量做）
printf 'targets esp32s3.cpu1\nhalt 20\nreg pc\nresume\nexit\n' | nc -w 6 localhost 4444 > sample_N.txt
# 之后 addr2line 批量符号化，sort | uniq -c 出直方图
```

判读：40% 样本落在 `display_bridge_v9_prepare_mono` + `blend_to_i1` 的 `set_bit` → **渲染器在逐像素混合一个永远画不完的区域**——不是死锁（会睡）、不是递归（栈浅）、不是泄漏（RAM 平稳）。

**第 6 步 · 反读源码找「谁允许它永远画」**：`prepare_mono` 对 flush area 无任何边界钳制 → 坐标必须是垃圾才会卡 → 反推上游有控件给了垃圾几何。

**第 7 步 · 冻结中 gdb 直接看参数（一锤定音）**：`thread 3` + `bt` 输出 `lv_label_set_text (obj=0x0, text=0x3c2fafc7 "播放")` —— **obj 是 NULL**。铁证到手，不用再猜。

**第 8 步 · 回业务层找 NULL 从哪来**：`lv_obj_get_child(MakeButton(...), 0)` —— `MakeButton` 返回的就是 label，取它的 child=0 恒为 NULL；四个按钮标签全 NULL，批量刷新函数的守卫只查了第一个。修复：直接接返回值 + 守卫全量判空。

**验证闭环纪律**：修一层 → flash → 用户实测 → 必然暴露下一层（本轮：页面打开后暴露「无声」→ 顺 PA 链查到 `DeviceWantsPa` 只认语音状态机 → 把 `RadioPlayback` 接进 `want_pa`）。每一层都用同一套管线验证，直到用户确认。

**经验压缩**：
1. 「无反应」先分诊，别急着读代码；
2. 冻结现场别重启，OpenOCD attach 上去慢慢解剖；
3. 单点 gdb 回溯会骗人（Xtensa 异常帧 unwind 失真），**PC 采样直方图才是定位死循环的真手段**；
4. 采样命中的函数 + 「它为什么不结束」反读源码，通常一步到根因；
5. gdb 能打参数——`p obj` / bt 里的 `(obj=0x0)` 经常一锤定音；
6. 每个诡异现象都留证据链（串口日志、采样文件、coredump），回溯时全靠它们。

### ESP-IDF 编译/运行坑

- 设备端 `snprintf("%lld...")` 在本 IDF newlib 配置下不可靠，GLM 重置时间曾显示成 `old月old日`。时间戳一律先转 `time_t`，再用 `gmtime_r()`/`localtime_r()`，最后用 `%d` 打印。例如 UTC+8：

```cpp
time_t seconds = static_cast<time_t>(reset_ms / 1000 + 8 * 3600);
struct tm tm_value {};
gmtime_r(&seconds, &tm_value);
snprintf(buf, sizeof(buf), "%02d月%02d日 %02d时%02d分",
         tm_value.tm_mon + 1, tm_value.tm_mday,
         tm_value.tm_hour, tm_value.tm_min);
```

- ternary 里混用字符串字面量和 `std::string` 会编译失败；先构造 `const std::string text = cond ? std::string("x") : std::string("y");`，再传 `text.c_str()`。不要写 `lv_label_set_text(... ? std::string : "...").c_str()`，这既类型错又有悬空风险。
- 构建后 `sdkconfig` 可能被 IDF 重新写出无关 drift。提交前检查；非有意修改用 `git restore -- sdkconfig`。
- flash 抓串口时，日志里的 `load:0x...` 或 `Rebooting...` 可能是烧录/正常重启噪音；只有配合 `assert failed`、`Guru Meditation Error`、backtrace 才能确认真崩溃。
- 解析 crash backtrace：

```bash
export PATH="$HOME/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20260121/xtensa-esp-elf/bin:$PATH"
xtensa-esp32s3-elf-addr2line -pfiaC -e build/xiaozhi.elf \
  0x... 0x... 0x...
```

- **NVS 键名 ≤15 字符**（硬限制）：`output_volume_v2` 16 字符，读写一直 `ESP_ERR_NVS_KEY_TOO_LONG`，音量持久化静默失效数月。键名报错只在每次写入时打一行 ERROR，极易漏看——新建键先用 `strlen` 检查。
- **`-Werror=format-truncation` 是新代码最高频编译错**:含 `%d` 的日期/数字缓冲按 gcc 最坏情况(int=11 字符)给 40 字节;PR6/13/14/21 连续四轮同款错误,一次报错连环暴露同族多处,全改完再编。
- LVGL 页"点开无反应"排查顺序：① 串口有无 `launch xxx` 之后停滞；② 内核 1 是否 100%（LVGL worker 死循环）；③ OpenOCD 采样核 1 PC 直方图。不要先怀疑网络/音频。
- `esp_lvgl_adapter` 的 mono 桥（`display_bridge_v9_prepare_mono`）对 flush area **无边界钳制**，坐标异常就是分钟级像素循环；LVGL 页面把垃圾几何（NULL 控件、负尺寸）喂进失效区域是这个循环的唯一入口。

### PR 工作流

- feature 分支上 amend 后 push 被 non-fast-forward 拒绝时，先 `git fetch` 确认远端旧 tip 只包含同一批未合并工作，再用：

```bash
git push --force-with-lease origin <branch>
```

- 自己名下草稿分支可用 force-with-lease；不要对 main/upstream 分支强推。
- GitHub push 偶发 `remote: Internal Server Error` 时等几十秒或改 SSH/HTTPS 重试；不要盲目重做提交。
- **批量合并多个 PR 的管线**(2026-10-09 一晚 11 个 PR 的实测流程):逐个 `git merge origin/<branch>` 进本地 main → 解冲突(新 Tab/新 APP 类冲突=两边都留并重排枚举;kApps 每项补 id)→ 每步 build → 全部完成后 push main 一次 → GitHub 自动把各 PR 标记 MERGED → 烧一次固件。
- 合并后构建失败先查**残留冲突标记**:`grep -rn "^<<<<<<<\|^>>>>>>>" main/`——git 把 CMakeLists 的 keep-both hunks 按文本合并时会留下孤立的 `>>>>>>>` 行,构建报"Cannot find source file: .../>>>>>>>"。

## 已踩过的坑

- 直接 `idf.py build` 前缺少 `ESP_ROM_ELF_DIR` 会报 `OSError: ESP_ROM_ELF_DIR environment variable is not defined`。
- 资源构建需要 Python 包 `qoi-conv`。缺包时安装：

```bash
"$HOME/.espressif/python_env/idf5.5_py3.13_env/bin/python" -m pip install \
  -i https://pypi.tuna.tsinghua.edu.cn/simple qoi-conv numpy packaging
```

- `sdkconfig` 已针对 e-paper、octal PSRAM、16 MB Flash 和自定义分区调优；不要随手删除或修改。
- release/出厂整包应使用仓库根目录 `./merge_firmware.sh`；日常开发烧录用 `idf.py flash` 即可。
