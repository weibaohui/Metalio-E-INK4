# 代码库彻底分析：可删 / 可优化 / 可框架化

> 分析基线：main@53d71a6（2026-10-09）+ 当日已合并的 PR。目标硬件：metalio-e-ink-4
> （ESP32-S3，800×480 墨水屏，8MB Octal PSRAM，ESP-IDF 5.5.4）。
> 方法：全仓 grep 可达性核对 + 逐文件读关键路径。本机无 ESP-IDF 工具链，
> 所有"删除"建议合并前都需 `idf.py build` + 真机回归。

---

## 一、可以删除的（死代码 / 零引用）

按收益从大到小：

### 1.1 唤醒词引擎（最大的一块，但要做减法而不是全删）
- 现状：esp-sr 2.2.0 依赖、`afe_wake_word.cc`、`custom_wake_word.cc`、`WakeWord` 成员
  （`audio_service.h:129`）全部在编；但业务路径里 `EnableWakeWordDetection(true)`
  只剩"唤醒触发后再开"和工厂测试页（`settings_test_audio.cc:211`）——开机明确不起
  唤醒词，实际入口是按键 PTT（`application.cc:1273 WakeWordInvoke` 发固定串）。
- 建议：删 `WakeWord` 体系与 wakenet 模型（`wakeword/srmodels.bin`、sdkconfig
  `CONFIG_USE_AFE_WAKE_WORD`/`CONFIG_SR_WN_*`），**保留** `AfeAudioProcessor`
  （AEC 回声消除，百问通话在用，`audio_service.cc:57`）。
- 收益：省模型分区/固件体积；AFE 唤醒词实例化本身占 RAM，删掉后内部 RAM 宽裕，
  直接缓解 HTTPS/TLS 连续块紧张（收音机/ICS 订阅都在吃这个）。
- 风险：工厂测试音频页引用了唤醒词开关，要一并改；合并后必须实测百问通话回声消除正常。

### 1.2 7 个零引用图标
`main/xingzhi-assets/`：`ic_app_home_{buddy,dictionary,phone,photo,recording,study,translate}.png`
——全仓无任何引用（kApps/kSlashApps 只用 10 个）。白占 SPIFFS 资源分区，直接删。

### 1.3 esp_debug_cloud（379 行）
`esp_debug_cloud.cc/.h`：`ESP_DEBUG_CLOUD_ENABLE` 默认 0，编译成空函数，
`main.cc:31` 留个空调用。功能已被 `syslog_sender`（vprintf 钩子+环形缓冲+UDP，
RFC3164）以平行实现取代。删文件 + 删 main.cc 调用。

### 1.4 esp32_camera（boards/common）
`esp32_camera.cc/.h`：全仓无实例化点（本板摄像头是 `UvcStillCamera`，
`metalio_e_ink_4_board.cc:1066`）。死代码，但因 `boards/common/*.cc` 是 GLOB
无条件全编（`CMakeLists.txt:151-160`）仍占编译产物。删。

### 1.5 MCP 里 #if 0 的拍照工具
`mcp_server.cc:100-123` `self.camera.take_photo` 整段 `#if 0`（本地拍照已走
百问 BOOT 双击）。删死分支，或恢复——二选一，别留着腐化。

### 1.6 bread 板的 Kconfig 壳
`Kconfig.projbuild:109-120` 的 `BOARD_TYPE_BREAD_COMPACT_WIFI` 选项没有对应
板目录，选了直接编译不过。删选项，或补上目录——当前纯陷阱。

### 1.7 boards/common 里的非本板外设
`knob` / `oled_display` / `ml307_board` / `nt26_board` / `uart_eth_modem` 等
跟着 GLOB 无条件编译。短期：CMakeLists 改成按 `CONFIG_BOARD_TYPE` 条件收编；
长期：用不到的删。注意 `usb_uvc_still.cc` 是活代码（UVC 相机依赖），别误删。

### 1.8 已删
设置→对话 Tab（PR #26 已删）。`SETTINGS_TAB_CONVERSATION` 文案留在
gen_lang.py 生成的 40 国语言表里，下次重生成语言表时顺带清。

---

## 二、可以优化升级的

### 2.1 TLS 预检阈值不一致（两处各写各的）
`calendar_ics.cc:43` 用 12KB，`radio_player.cc:447-454` 用 20KB/28KB 双阈值。
同一台机器同一个 TLS 栈，阈值却不同。抽成共享 helper
`NetTlsPreflightOk()`（放 api_http.h 或新 net_guard.h），阈值取实测值统一注释。

### 2.2 手写 HTTP 块散布 18 个文件
`CreateHttp + SetTimeout + Open + Read/ReadAll + Close` 模式重复 18 处
（ota/device_wifi_location/calendar_ics/glm_usage/book_progress_sync/cloud_screen×3…）。
抽 `api::HttpGetToString(url, cap, err)` / `api::HttpPostJson(url, body, err)`：
内置超时、1 跳重定向、体积上限、TLS 预检、鉴权头白名单（防 ICS 那类第三方 URL 泄 token）。

### 2.3 NVS 纪律靠每个文件自觉
"PSRAM 栈任务禁直接 NVS，须 Schedule 到 main" 这条纪律现在靠 30 个文件各自写注释
遵守。做 `SafeSettings`：读写前检测当前任务栈 caps，PSRAM 栈自动转发 main 任务，
从机制上杜绝 cache 断言崩机这一类 bug（已经咬过两次：收音机 NVS、GLM start-before-init）。

### 2.4 sys_mon 每秒打日志
`metalio_e_ink_4_board.cc:1031` 常驻 4KB 栈每秒打 CPU/RAM/电池。调试期有用，
发布版建议降频（60s）或收成 `ESP_LOGD`。

### 2.5 日期工具三处复制
`calendar_screen.cc`、`calendar_ics.cc`、（status bar/standby 里还有第三处 civil 时间换算）
各自写 `EpochToYmd/YmdToStr/WeekdayMonFirst`。抽 `services/date_utils.h` 纯函数。

### 2.6 语言表裁剪
40 国 locale 全量编进固件。产品实际只用中/英的话，gen_lang.py 加 locale 白名单，
省 flash/资源分区。

### 2.7 云端端点判空模式收敛
`cloudzao_endpoints.c` 全空串（开源版），靠 `api::HasHost()` 判空跳过——模式本身好，
但 18 处调用点各自判空。在 2.2 的 HTTP helper 里内置"无端点直接失败返回"。

### 2.8 application.cc 1597 行的 boot 流程
`Application::Start()` 一长串顺序调用+注释。拆成 `BootStep{ name, fn }` 表驱动，
每步打耗时日志——以后排查"开机慢/某步崩"直接看表。

---

## 三、可以统一成框架的

### 3.1 APP 注册表（最值得做）
现在新增一个 APP 要动 4 个地方：kApps/kSlashApps 表项、图标资源、应用开关
（NVS `apps/en_<id>`）、MCP 注册门控（application.cc 手写 if）。
收拢成一个注册表：

```c
struct AppDesc {
    const char* id;            // 稳定短 id(NVS 键、MCP 门控都用它)
    const char* icon;
    const char* (*name)();
    lv_obj_t* (*create)();
    void (*register_mcp)();    // 可为空
    bool mandatory;            // "设置"这种永驻
};
```

首页渲染、应用开关、MCP 门控全部遍历这张表。新 APP 一行注册，四件事自动齐。
配合已合并的 MCP 门控（PR #27）效果最佳。

### 3.2 一次性 PSRAM 工作线程助手
`xTaskCreatePinnedToCoreWithCaps(MALLOC_CAP_SPIRAM) + vTaskDeleteWithCaps(nullptr)`
模式已重复 15+ 处（radio/GLM/ICS/syslog/wallpaper/book×6…），每处都要记住：
PSRAM 栈、WithCaps 配对自删、失败落错误状态。封装
`SpawnPsrWorker(name, stack, prio, fn)`，创建失败自动回传错误，从根上消灭
"忘配对自删 / 失败没提示"两类 bug（GLM "尚未查询"卡死就是这类）。

### 3.3 屏幕骨架共享件
`MakeLabel/MakeBtn` 在 6 个 screen 文件各自 static 重定义；`lv_obj_remove_style_all`
散布 30 个文件。在 `screen_common` 里补：
`ScreenMakeLabel / ScreenMakeBtn / MakeSection / MakeScrollColumn / NewPlainObj()`。
新屏幕（电源/校时/系统信息都是这套）直接复用，样板代码减半。

### 3.4 服务生命周期三段式
Init（boot·main 任务·可读 NVS）/ Start（进屏·LVGL 任务·不碰 flash）/ Stop（离屏·释放）
已被 GLM、收音机、日历 ICS 各自手写三遍，每遍都踩过"NVS 写错任务栈"的坑。
统一 `AppService` 接口 + 状态位（inited/running/last_error），系统信息 APP
还能直接展示各服务状态。

### 3.5 设置 Tab 表驱动
每个 Tab 都是 Build/Reset/OnActivated/OnDeactivated 四件套 + settings_screen.cc
里三个 switch 各加一行。改成 `TabDesc{ id, name_fn, build, reset, on_act, on_deact }`
数组驱动，三个 switch 合成一个循环，新 Tab 一行注册。

---

## 四、建议执行顺序（风险从低到高）

| 序 | 项 | 风险 | 收益 |
|---|---|---|---|
| 1 | 删死图标(1.2)、esp_debug_cloud(1.3)、esp32_camera(1.4)、#if 0 拍照(1.5) | 零 | 体积+可读性 |
| 2 | 屏幕共享件(3.3)、SpawnPsrWorker(3.2)、HTTP helper(2.2) | 低（纯重构） | 以后写新页减半 |
| 3 | TLS 预检统一(2.1)、日期工具(2.5)、SafeSettings(2.3) | 低 | 防已踩过的坑复发 |
| 4 | APP 注册表(3.1)、设置 Tab 表驱动(3.5)、boot 表驱动(2.8) | 中（动框架） | 扩展性 |
| 5 | 唤醒词删除(1.1)、boards/common 条件编译(1.7) | 高（动音频/编译） | RAM+体积大头 |

每条仍按"一类需求一个 PR"拆开做。1/2/3 可以马上做；4 建议等现有 open PR
落地后做；5 需要先真机验证百问通话（AEC）再动手。

---

## 跟进(2026-10-10,PR #32 落地)

§3.2 / §2.5 / §2.1 三项已在 PR #32 完成(纯提取不改行为):

- **§3.2 PSRAM 工作线程** → `utils/psr_worker.h` 的 `SpawnPsrWorker`,任务函数
  return 即自动配对自删;已迁移 glm_usage / calendar_ics / syslog / time_svc 4 处。
- **§2.5 日期工具** → `services/date_utils.h`;calendar_screen / calendar_ics 已迁移。
- **§2.1 TLS 预检** → `api::TlsPreflightOk`(api_http.h);ICS 与收音机已收敛。
- 新增 `tools/calendar_grid_check`:用固件真实 date_utils.h 对拍 2000~2200 全 2412 个月,
  零偏差,日历格子算法从此有一键回归。

期间上游另行完成:PR #25(系统信息 APP)/#26(删对话 Tab)/#27(MCP 跟随开关)/
#24(日历选中态)/#31(圆点页码+斜切分页)均已合入,首页布局另有两处时钟遮挡修复。

剩余待办优先级不变:§3.3 屏幕骨架共享件(MakeLabel/MakeBtn 各屏重定义)、
§3.1 APP 注册表、§3.5 设置 Tab 表驱动 → 中风险框架项;§1.1 唤醒词删除 →
高风险,先真机验证百问通话 AEC。
