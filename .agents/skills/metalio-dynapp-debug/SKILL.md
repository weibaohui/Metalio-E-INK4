---
name: metalio-dynapp-debug
description: Metalio E-Ink4 动态应用(.so)开发、SDK 构建、安装与远程真机调试方法论。用于提到动态应用/.so/SDK 构建、App load failed、应用装了不显示/显示了别人的界面/启动即退出、dlfcn 串台、符号缺失、-mlongcalls、e-ink 截屏验证、点击测试、刷帧滞后、UDP syslog 断流等场景;配套 metalio-e-ink4-flash skill(串口烧录与 OpenOCD 取证)。
---

# Metalio 动态应用(.so)开发与远程调试

针对 Metalio E-Ink4 的动态应用体系：第三方 .so 用 `-nostdlib` 编译，所有固件函数留空引用，
设备 dlopen 时按**符号名**在固件导出表(generated_symbol_table.inc)中查地址重定位。
SDK 唯一事实源在仓库 `sdk/` 目录；构建入口 `tools/package_app.sh`(委托 `sdk/package_app.sh`)。

## 一、构建期铁律(违反即崩溃/加载失败,全部实测)

1. **ABI 旗标必须与固件一致**:`-mlongcalls` 缺失 → 应用内短调用编码跳不到固件函数,
   首次调用即崩(设备重启回桌面,无任何报错)。同族还有 `-fno-builtin-memcpy/memset/bzero`
   `-mdisable-hardware-atomics`。已固化在 `sdk/build_app.sh`,勿删。
2. **.so 文件名必须唯一**:`lib/<应用id>.so`,禁止共用 `lib/app.so`。
   esp_dlfcn(`dlmod_insert`)按**基础文件名**去重缓存——重名时第二个应用的 dlopen
   直接返回第一个应用的缓存模块,表现为"启动 A 却显示 B 的界面"。
   `sdk/package_app.sh` 已自动按 manifest id 命名并改写 manifest 的 executable。
3. **libgcc 双精度/64 位全族必须导出**:第三方做任何浮点/64 位运算都需要
   `__muldf3/__divdf3/__adddf3/__eqdf2/__floatsidf...`(22 个,见加载器 libgcc 段)。
   缺一个 = 用到即静默失败(不崩,不显示)。
4. **fontpack 字体 API 可能返回 NULL**(字库按需载入,部分字号无字形时日志报
   "no glyphs for size=N")。`lv_obj_set_style_text_font(lbl, NULL)` 触发 LVGL
   断言崩机。所有 `fontpack_lv_font_get/ui()` 返回值必须判 NULL 再用——工程内
   全部应用都带此保护,新代码必须照做。
5. **lv_conf/字体配置以固件 sdkconfig.h 为准**:SDK 构建必须
   `-include sdk/include/lvgl/sdkconfig.h`(scripts/sync_sdk_lvgl.sh 物化)。

## 二、符号导出体系

- 导出表由 `scripts/gen_symbols.sh` **自动生成**(`generated_symbol_table.inc`),
  禁止手改。策略:ELF 中的 lv_*/dynapp_* + 手写基础库清单,再让**编译器当裁判**——
  用与加载器相同的编译参数编译探测文件取每个符号地址,"未声明"即剔除,循环到干净。
- 手工加符号的时代已结束;若确需新增服务,改 `dynamic_app_shims.h/.cc`(固件侧)
  后重跑脚本。libgcc 族在加载器文件头以 extern 声明手写(需 extern 才能取址)。
- `sdk/symbols.txt` = 第三方可见 API 清单(ABI 文档)。政策:签名只增不改,
  破坏性变更递增 `METALIO_DYNAPP_ABI_VERSION`。
- **每次固件构建后重跑脚本**;Release CI 会 diff 校验,过期即拒绝发布。

## 三、远程真机验证方法论(核心技能)

设备 REST(MCP):`http://<ip>:8080`,头 `Authorization: Bearer <PIN>`(见设备「设置 → MCP」)。
**设备 IP 随 DHCP 变化**,先探活 `GET /screen/screenshot` 再操作。

### 3.1 截屏不可作为状态判据(最重要的一条)

墨水屏刷帧在连续操作下合并延迟 10-20 秒,**截屏拍到的是旧帧**。
- 判定"功能是否生效"必须用**功能状态**:MCP 查询(radio.get_state、app.get_info、
  get_system_info)、应用自身可查状态(radio 播放态、沙箱文件、NVS)。
- 截屏只用于:布局/渲染审查;且应连续拍 2-3 帧(间隔 3s)对比是否仍在变化。
- 点击测试用 `tools/click.sh x y`(走真实触摸链路,含命中测试与震动反馈)。
  坐标从源码 flex 布局推算,注意**页面切换后坐标失效**。

### 3.2 启动是异步的

`POST /app/launch?id=X` 返回 "initiated" ≠ 已加载。加载失败静默回落桌面。
可靠判定:launch → 等 6-10s → 查该应用的功能状态;或 `self.app.get_info` 看注册态。

### 3.3 崩溃(重启)检测

ESP_LOG 行内含毫秒运行时长 `I (419310)`:启动应用前后各抓一条日志,
**毫秒计数归零 = 发生了崩溃重启**。UDP syslog(端口 5140)收包方式:

```bash
curl -s -X POST .../mcp -d '{"...":"self.syslog.configure","arguments":{"host":"<本机LAN IP>","port":5140}}'
curl -s ... -d '... "name":"self.syslog.enable","arguments":{"on":true}}'   # 参数名是 on 不是 enabled
python3 -c "import socket;sock=socket.socket(2,2);sock.setsockopt(1,2,1);sock.bind(('0.0.0.0',5140));..."
```

坑:syslog 推送在 OTA 后会断流(status 显示 running 但收不到包),
**重新 configure+enable 即恢复**;且恢复后只稳定很短时间,要在窗口内完成抓取。
本机口被残留监听占用时先 `lsof -nP -i :5140` 清理。注意 zsh 里 `path` 是绑定
PATH 的特殊变量,shell 变量命名避开它。

### 3.4 最小应用二分法(定位崩溃调用)

应用加载即崩且无日志时,用二分:`create` 只 `return lv_obj_create(NULL)` →
确认显示 → 每次只加一个 API 调用(status_bar → vk_attach → label → 字体...),
重新打包安装 → 哪一步加完就崩,崩溃点即该 API。实测曾用此法把"加载期崩溃"
收敛到具体调用,而非盲改。

## 四、故障速查表(症状 → 根因 → 处置)

| 症状 | 根因 | 处置 |
| :--- | :--- | :--- |
| 点图标/启动即设备重启 | .so 缺 `-mlongcalls` 等 ABI 旗标 | 检查 sdk/build_app.sh CFLAGS(已固化) |
| **dlopen 中途 LoadStoreError 崩机重启,日志停在 `ELF: elf->symtab[0]`** | **elf_loader TEXT size 4 字节取整吞掉紧贴 .text 的 .rodata 头 3 字节,其内指针被重定位进 I-window,固件数据读取即崩**(EXCVADDR = data_base + 字符串 VMA 可实锤;2026-10 扫描 13 个 app 有 11 个布局踩雷) | 已 vendor 补丁:`components/espressif__elf_loader`(PATCH_NOTES.md),TEXT size 不取整;**直接编辑 managed_components 会被重新拉取覆盖,必须 vendor** |
| **从 LVGL 任务(应用 create/按键/触摸回调)调 NVS/flash 操作,`assert esp_task_stack_is_sane_cache_disabled()` 崩机** | LVGL worker 栈在 PSRAM;flash 关 cache 窗口内不能跑 PSRAM 栈任务(读 NVS 也会触发) | shim 层用 `dynamic_app_shims.cc` 的 `RunOnInternalStack`(内部栈临时任务+同步等待);SD 卡 I/O 不需要 |
| **应用操作成功后随机 StoreProhibited/堆腐败崩机** | `(lv_event_cb_t)free` 当 LV_EVENT_DELETE 回调——LVGL 传给回调的是 lv_event_t 本体,free 它即堆腐败(backup_app 实测,7 处) | 写 `free_user_data_cb` trampoline:`free(lv_event_get_user_data(e))` |
| **切到无状态栏页面后任何触摸/状态刷新即 LoadProhibited** | `BindStatusWidgets` 的控件指针随旧屏销毁而悬空(backup 页无状态栏,实测触摸即崩) | 已修:`ScreenCreateStatusBar` 挂 LV_EVENT_DELETE,屏删时 `UnbindStatusWidgetsForScreen` 解绑(仅当仍绑该屏) |
| **异步渲染后界面空白/出现默认"Text"标签** | 在非 LVGL 任务里裸改控件:vk 派发在触摸喂入任务,`Application::Schedule` 落在主事件循环——两者都不持有 LVGL 适配器锁,与 lvgl worker 渲染竞态 | UI 变更一律走 `ScreenLvAsync(cb,ud)`(内部拿适配器锁 + lv_async,落在 lvgl worker);vk 处理器里只改纯数据(如页码 int) |
| **tools/list 一调就 abort 重启(如 AI 对话说话转文字后必崩)** | MCP 工具注册时整型 Property 的第三参是**默认值**不是描述,传入字符串污染 variant,序列化 `std::get<int>` 抛 bad_variant_access 未捕获 → terminate | 注册用 int 字面量;`Property::to_json` 已改 get_if 防御式(类型不符降级+告警点名);复现命令:`POST /mcp {"method":"tools/list"}` |
| **printf 输出 "0ld" 类乱码**(如运行时间显示 `0ld天 00ld:00ld`) | 固件 libc 是 **newlib NANO 格式**(`CONFIG_LIBC_NEWLIB_NANO_FORMAT=y`),不支持 long-long printf(%lld/%llu) | 用 32 位 `long/%ld`(秒级时间戳够用),或自行拼字符串;`sdk/build_app.sh` 构建时会扫描源码并警告 |
| 启动 A 显示 B 的界面 | dlfcn 按文件名缓存,重名 .so 串台 | manifest executable 用 `lib/<id>.so`(打包脚本自动) |
| App load failed 屏 | dlopen 失败:符号不在导出表 | 看 dlerror 点名;重跑 gen_symbols.sh;或改用 symbols.txt 内 API |
| 应用静默失败不显示 | 引用了未导出符号(如 __muldf3) | build_app.sh 符号校验警告会列出;缺的 libgcc 族在加载器补导出 |
| label 设置字体后崩 | fontpack 返回 NULL 未判 | 判 NULL 再 set_style(全工程约定) |
| OTA 传输中断/失败 | 设备 concurrent 负载(如 radio 拉流) | 先停 radio、/screen/home,再 OTA;失败后设备回滚启动需等 2-4 分钟 |
| 截屏与实际不符 | e-ink 刷帧合并延迟 | 以 MCP/功能状态为准;截屏仅看布局 |
| UDP syslog 收不到 | OTA 后断流 / 端口被残留监听占用 | 重新 configure+enable;lsof 清理 5140;窗口内完成抓取 |
| **/input/click 坐标无效(点了没反应)** | **注入帧 = 当前屏幕的 LVGL 逻辑坐标**。桌面逻辑空间=800x480 横屏,与截图像素 1:1;动态应用源码多为 480x800 竖屏逻辑(如 backup `lv_obj_set_size(scr,480,800)`),注入必须用**源码布局坐标**(backup"系统配置"按钮 ≈ (240,143))。click.sh 头注释"x 0-479 y 0-799"已过时,横屏下 x∈[0,800) y∈[0,480) | 从应用 `main/*.c` 的 lv_obj_set_pos/size/flex 推算逻辑坐标注入;盖板 vk 键仍走 `/input/key`,无坐标问题 |

### 4.2 截图方向与坐标换算(已程序化标定)

- `/screen/screenshot` 原始输出 **800x480 横版** BMP/PNG。桌面内容正向;
  **动态应用内容在截图里旋转了 90°——把截图顺时针转 90° 才是应用正向**
  (应用顶边贴在截图左边缘)。
- 所有动态应用逻辑空间统一 480x800 竖屏(`lv_obj_set_size(scr,480,800)`)。
  变换关系(逻辑→截图):`截图列 col = 逻辑 y`,`截图行 row = 479 - 逻辑 x`
  (479 = 应用逻辑宽 480 - 1,col/row 从 0 起算)。
  **逆变换(从截图像素还原动态应用点击坐标):`app_x = 479 - row, app_y = col`。**
  校准实例:backup 统计卡片逻辑 y=42 → 截图第 42 列 ✓;按钮逻辑 x [167,313] →
  截图行 [166,312] ✓(实测 [170,309],边框内缩)。
- 桌面页不需要换算:注入坐标 = 截图像素 1:1(800x480,x∈[0,800) y∈[0,480))。
- 捷径:动态应用优先按源码布局直接算逻辑坐标注入,截图只用来确认效果;
  从截图反推要过一次旋转,容易错。

### 4.1 崩溃类问题必须走 USB 串口(UDP syslog 看不到 panic)

UDP syslog 在 panic/重启瞬间静默断流,且设备重启后约 2 分钟才恢复推送——
**崩溃现场只存在于串口**。`/dev/cu.usbmodem*` 挂 python pyserial 后台落盘,
再复现一次,`Guru Meditation` + 寄存器现场 + Backtrace 一次抓全。
判读要点:`LoadStoreError` + EXCVADDR 落在 0x42xxxxxx(I-window)= 指针被
`elf_remap_text` 误映射;减去 0x6000000 再减 .so 段基址即 .so 内 VMA,
对照 readelf 各 section 定位是哪个对象。addr2line 用 `build/xiaozhi.elf`
解固件帧,`.so` 帧用 `xtensa-esp-elf-objdump -d` 按算出的基址反汇编。

## 五、破坏性操作红线(测试时)

- 电源管理三个按钮(息屏/重启/关机)**只看不点**;息屏后 WiFi 断,远程失联。
- 备份恢复的"恢复"、设置的开关类、日历的清除类,一律不点。
- 测试收音机播放前先把音量调低(`self.radio.set_volume level=5`),测完恢复。
- radio 拉流期间不要做 OTA(并发负载会导致传输中断→回滚启动 2-4 分钟)。
