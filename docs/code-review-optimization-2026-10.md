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

## 跟进 2(2026-10-10 第二批,PR #33~#36)

- **§1.2-1.6 零风险删除** → PR #33:7 个死图标、esp_debug_cloud(379 行)、
  esp32_camera、#if 0 拍照工具、bread 板 Kconfig 壳,净删 1546 行。
- **§3.3 屏幕骨架共享件** → PR #34:ScreenMakeLabel/ScreenMakeBtn 收拢
  calendar/power/time_sync/sysinfo/glm/radio 六屏,净删 56 行重复。
- **§3.5 设置 Tab 表驱动** → PR #35:TabEntry 带钩子,三个 switch + if 链收拢,
  净删 60 行;新增 Tab 一行注册。
- **§3.1 APP 注册表** → PR #36(最小风险版):AppEntry 加 register_mcp 钩子,
  MCP 门控遍历表执行;顺手修复斜切主题丢"系统信息"的合并事故。

剩余:§2.2 HTTP helper(18 处)、§2.3 SafeSettings、§2.4 sys_mon 降频、
§2.6 语言表裁剪、§2.8 boot 表驱动、§3.4 服务生命周期框架 → 低/中风险;
§1.1 唤醒词删除、§1.7 boards/common 条件编译 → 高风险,先真机验证。

## 跟进 3(2026-10-10,逐项对拍当前 main,修正过时项)

#32~#36 合入后(main@278d53d)逐项 grep 对拍,以下条目与正文有出入:

**已过时/已完成,从待办划掉:**
- §2.5 日期工具:✅ 彻底完成。"第三处 civil 换算(status bar/standby)"已不存在
  (sysinfo 里的 86400 是 uptime 折算天数,非日期换算);date_utils.h 消费方
  calendar_screen + calendar_ics。
- §2.6 语言表裁剪:✅ 前提过时。gen_lang.py 已是 `RUNTIME_LOCALES=("en-US","zh-CN")`
  运行时双语表,不存在"40 国全量编入"。只剩 §1.8 的 SETTINGS_TAB_CONVERSATION
  键清理(lang_tables.cc 5 处 + 2 个 language.json),下次重生成顺带。
- §2.7 端点判空:大半已收敛,HasHost() 仅剩 4 个文件;其余随 §2.2 helper 内置。

**数字修正(仍有效但规模变了):**
- §2.2 HTTP helper:手写块实测 **16 文件 24 处**(正文说 18;不含
  components/esp-ml307 的 4 处,vendored 库不动)。
- §3.2 PSRAM 工作线程:跟进 1 说"已迁 4 处",实际裸写仍 **10 文件 16 处**;
  但其中 **6 处不该迁**:audio_service 3 处是常驻服务任务(不适用 return-自删
  契约),power_policy_standby_lp / book_home_snapshot(NVS 持久化) /
  cloud_screen(下载) 共 3 处刻意 MALLOC_CAP_INTERNAL(低功耗期 PSRAM 不可用、
  NVS 纪律)。实际可迁候选 ≈12 处:book_screen×6(待逐个核对)、ttf_convert、
  assistant_boot_photo、standby_wallpaper、task_screen、wallpaper_screen×2。
- §3.3 屏幕共享件:六屏已收;剩 network_screen 的 MakeBtn(参数化高度)、
  MakeSection/MakeScrollColumn/NewPlainObj 未沉淀;lv_obj_remove_style_all
  散布 36 文件(正文说 30)。

**核实未变:**
- §2.3 SafeSettings:无任何机制,纪律仍靠 30 文件注释。
- §2.4 sys_mon:board.cc 仍 1000ms + ESP_LOGI。**建议改期到发布准备期再做**:
  当前真机调试期,@@@RAM/CPU 行是调试管线在用的信号,现在降频自断耳目。
- §2.8 boot 表驱动:application.cc 1598 行,Start() 约 425 行(1009-1434),无 BootStep。
- §3.1 APP 注册表:#36 只是 register_mcp 钩子最小版;kApps/kSlashApps 仍两张表,
  create/name/icon/toggle 未统一。
- §3.4 服务生命周期:无 AppService 接口,GLM 手写 Init/Start/Stop 依旧。
- §1.1 唤醒词:CONFIG_USE_AFE_WAKE_WORD=y 仍在编。§1.7:boards/common GLOB
  仍无条件。解锁前置不变:真机验证百问通话 AEC。

**修订后执行顺序:** ① §2.2 HTTP helper(24 处,安全+去重双收益) →
② §3.2 剩余 ~12 处一次性任务迁移(机械,逐个核对 book_screen) →
③ §2.3 SafeSettings → ④ §3.3 补全 + §2.8 + §3.1 完整版 + §3.4(中风险框架批) →
⑤ §1.1 + §1.7(高风险,先真机 AEC)。§2.4 挪到发布准备期。

## 跟进 4(2026-10-10 第三批落地,§3.2 全部 + §2.2 首批)

**§3.2 PSRAM 工作线程——收尾完成**(refactor/psr-worker-migration → main@5c9ef54):
- 迁移 11 处:book_screen×6、standby_wallpaper、task_screen BatchTask、
  assistant_boot_photo、wallpaper_screen×2。**其中 3 处是真 bug**:
  book_screen 6 处 + standby/task_batch 以裸 vTaskDelete(nullptr) 收尾,违反 IDF
  契约(WithCaps 创建必须 vTaskDeleteWithCaps 删),每次翻页/开书/批量操作泄漏
  一个 PSRAM 栈——此前收音机案之后又一个"配对"类事故源,已机制化消灭。
- 刻意不迁(终态):audio_service×4/afe×1(常驻任务)、standby_lp/cloud_download/
  book_home_snapshot(MALLOC_CAP_INTERNAL,内 RAM 栈是刻意的)、ttf_convert
  (配对正确,5 出口改写 churn>收益)、radio_player(常驻流任务,注释齐全)。

**§2.2 HTTP helper——首批落地**(feat/http-helper → main@cbe9d0e):
- `api::HttpFetch/HttpGetString/HttpPostJson`(api_http.h,header-only):
  超时/1 跳重定向(跨 host 剥鉴权头)/体积上限/TLS 预检(12KB)/空 body
  SetContent("") 特判/鉴权头永不自动携带,失败 W 级成功 D 级日志。
- 已迁 5 站:device_wifi_location、standby 天气、book_progress_sync、
  glm_usage(内部 helper,3 调用点)、calendar_ics(删手写重定向+预检)。
- **剩余迁移面**:push_resources×2(需取消钩子 DownloadGate)、ota×2(条件
  GET/POST + 身份头)、task_screen(DELETE/信封);另有字节缓冲变体待做——
  cloud_screen/book_cloud_library 的 DownloadUrlToBuffer 是明显复制粘贴,a2ui_image
  同构,需要 GetBytes(取消回调 + 调用方 buffer)再收。
- 明确不收(流式/multipart/刷写):http_download_file、ota Upgrade、mcp_server×2、
  assistant_boot_photo 上传、uvc Explain、assets Download。
- 真机基线(本批刷机后):空闲内 RAM 剩 42KB/历史最低 26KB——预检阈值 12KB
  与现状的余量很小,§1.1 删唤醒词(运行时堆分配)仍是内存大头。

顺带发现:cloud_screen.cc 的 ShowDownloadDialog/ShowActionDialog 已无调用
(编译警告既有,自开源初始提交就在),列入下轮死代码清理。

## 跟进 5(2026-10-10 第四批落地,§2.3 + §2.2 收官)

**§2.3 SafeSettings——落地**(feat/safe-settings → main@26da95b):
- 守卫内置 Settings 类本身(含 ctor 的 nvs_open/dtor 的 commit),30 个调用文件
  零改动自动受保护。内部栈(含 main)零开销直访;PSRAM 栈经 esp_ptr_external_ram
  探测后转发 main 任务同步执行,超时 3s 按默认值返回。
- 转发状态双方接力生命周期:超时方置 timed_out 脱手,迟到方收尾;正常路径双方
  fetch_add 最后一个清理——超时/迟到执行之间无 UAF。main 卡死 3s 的病态下整体
  泄漏换安全(注释写明)。
- 验证:构建 + 刷机启动干净;转发路径待首个 PSRAM worker 实际写设置时在
  D 级日志("PSRAM 栈 NVS ... 已转发")可见。

**§2.2 HTTP helper——收官**(main@fc5ed5b):
- ota CheckVersion/Activate 迁 helper(补上原缺失的超时;身份头改显式列表);
  task_screen HttpJson 迁 helper(GET×2/POST×2/DELETE×1 五调用点);
  删 SetupHttp 死代码。
- **手写 HTTP 终态清单(不再收拢)**:http_download_file/ota Upgrade(流式落盘/
  刷写)、mcp snapshot(multipart 上传)、mcp preview_image(二进制裸 buffer;
  其无上限 Content-Length 分配是独立风险,待单独处理)、assistant 上传/uvc
  Explain(multipart+chunked)、assets(flash 流式)、push_resources×2 与
  book_cloud_library(DownloadGate 硬关断语义)、a2ui(原始 SPIRAM buffer +
  区分错误码契约)。
- 底层事实确认:http_client.cc 只在 content 非空时发 Content-Length,
  空 SetContent("") 对 GET/DELETE 无副作用,对 POST/PUT 正是防挂死特判。

**累计净删:** ~460 行重复样板;§2.2/§2.3/§3.2 三项机制化完成后,
"忘配对自删/NVS 错任务栈/手写 HTTP 缺超时无上限"三类事故从机制上绝迹。

**剩余待办(更新):** §2.8 boot 表驱动、§3.1 APP 注册表完整版、§3.4 服务生命周期、
§3.3 补全(network_screen MakeBtn/MakeSection 等)、mcp preview_image 上限、
cloud_screen 两个无调用 dialog 函数清理、§1.8 对话 Tab 文案(随语言表重生成);
高风险 §1.1 唤醒词删除 + §1.7 boards/common 条件编译——前置百问通话 AEC 实测
(设备已刷最新固件,等用户按 PTT 实测);§2.4 sys_mon 降频(发布准备期)。

## 跟进 6(2026-10-10 第五批落地)

- **mcp preview_image 硬化**:Content-Length 上限 2MB + 零长拒绝 + 短读校验
  (原实现短读会把垃圾尾部当图像数据)+ 统一超时 15s。全仓最危险的
  "用户 URL 无上限分配"点已封。
- **cloud_screen 死代码**:ShowActionDialog/ShowDownloadDialog 删除(164 行,
  自开源初始提交就无调用)。
- **§2.8 boot 表驱动落地**:Start() 15 步 BootStep 表,每步 I 级耗时,≥1s 立
  W 级。**首刷即兑现价值**:开机总 10.9s,其中 network(StartNetwork 等网)
  8.2s、mcp_services 1.8s、boot_ready 0.9s——后续开机优化目标直接明确。
- 顺带根因一个格式化坑:本机 sdkconfig NEWLIB_NANO_FORMAT=y,%lld 打印损坏,
  ESP_LOG 一律用 %lu(PRId64 同理慎用)。

**剩余待办(再更新):** §3.1 APP 注册表完整版(kApps/kSlashApps 合一)、
§3.4 服务生命周期框架、§3.3 补全(network_screen MakeBtn/MakeSection)、
§1.8 对话 Tab 文案(随语言表重生成);开机优化候选:network 步 8.2s(等网
策略,动它属产品行为变更,单独立项);高风险 §1.1+§1.7 等百问通话 AEC 实测。

## 跟进 7(2026-10-10 第六批落地)

- **§3.1 APP 注册表完整版**:kSlashApps 不再手抄第二份,constexpr 引用主表
  条目重排(值拷贝,零运行时开销);新增 static_assert 校验斜切表覆盖主表全部
  id,"斜切丢系统信息"式合并事故从此编译期拦截。新 APP 注册=主表一行+索引一位。
- **§1.8 收尾**:en-US/zh-CN 删 SETTINGS_TAB_CONVERSATION 键,按 CMake 同参数
  重生成 lang_tables.cc(恰 5 行,lang_config.h 零变化)。
- **§3.3 关闭(终态)**:六屏已收;network_screen MakeBtn 刻意保留——圆角 6px
  (共享件是圆形)+Strip 多做 pad 归零,视觉语义不同;WiFi 设置屏是高频界面,
  eink 视觉回归无法自动化验证,20 行的收益不值得冒险。MakeSection 等投机性
  helper 不加(无第二个消费者前不加抽象)。

**剩余待办(终版):** §3.4 服务生命周期框架——建议缓:三个服务(GLM/收音机/ICS)
都在正常工作,而其回归验证需要真实设备交互(进收音机听流/进日历拉订阅/看 GLM
刷新),build+开机冒烟盖不住;等一个用户能配合点屏幕的会话再做。
开机优化候选:network 步 ~8s(等网策略,产品行为变更,单独立项)。
高风险 §1.1 唤醒词删除 + §1.7 boards/common 条件编译——**唯一硬前置**是
百问通话 AEC 实测,等用户按一次 PTT;这是把 42KB 空闲内 RAM 抬起来的大头。

## 跟进 8(2026-10-10 第七批:§1.7 + §1.1,含重大前提修正)

- **§1.7 落地**:boards/common 6 个死源(knob/axp2101/sy6970/adc_battery_monitor/
  press_to_talk_mcp_tool/afsk_demod)不进编译。ml307_board(dual_network_board 在用)/
  uart_eth_modem(nt26 依赖)/backlight(mcp_server 链接)/nt26_board(4G 路径)经查保留。
  收益在编译时间与可读性;链接器本就 GC 了死代码,flash 几乎不变。
- **§1.1 落地,但文档原前提不成立,特此修正**:调用图谱证实本 fork 的 wake_word_
  对象**从未被实例化**(SetModelsList 显式 reset:"本产品不建唤醒词对象"),
  "Hi 钛灵"伪造唤醒不可达,PTT 今天就经 StartListening 直连。故删除是零行为
  变化的死代码清除(净 -389 行+整删文件,flash -2.1KB),**并不释放内部 RAM**——
  正文"AFE 唤醒词实例化占 RAM,删掉后内部 RAM 宽裕"针对上游行为,本 fork 早已
  绕过。42KB 空闲内 RAM 的真实大头是 WiFi/TLS/LVGL/opus 等在用组件,继续压榨
  属产品级调优(缓存尺寸/碎片治理),不在死代码范畴。
- 唤醒词删除后的真机验收点:百问按住说话/松手停(BOOT 长按路径)、home/standby
  hold-through 进聆听、工厂自检录音。用户侧反馈:**百问功能一直未能正常使用,
  根因是开源版云端点为空串(cloudzao_host/ota_url 均 ""),无服务端可连**,非固件
  缺陷;启用需部署 xiaozhi 协议服务端或接入商业云后填 ota_url。
- **语音功能新方向**:feat/ai-voice-chat 分支(按住说话→MiniMax STT→OpenAI 兼容
  LLM→屏幕文字回复,自足不依赖自建服务)待正式审查后由用户决定合入;唤醒词
  删除与该路线完全兼容。

**§1.x 至此全部闭环。剩余:§3.4(缓,等可交互验证的会话)、开机 network 步
~8s(产品决策)、ai-voice-chat 审查合入(新)。
