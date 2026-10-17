# 动态应用（.so）：免刷机安装与加载

应用是 SD 卡上的一个目录（`manifest.json` + `app.so` + 可选 `icon.png`），经 WiFi 直传到设备即可出现在桌面，点开才加载（dlopen → PSRAM），退出即卸载（dlclose 释放代码段）。固件内置符号表供 .so 解析调用，应用与内置应用同路：桌面入口、设置页开关、生命周期一致。

与「系统升级」整包 OTA 无关；也无需拆机拷卡——常驻 MCP 服务自带文件直传端点。

---

## 1. 应用包格式

```
/sdcard/metalio/e-ink/apps/<dir>/
├── manifest.json   必需，元数据
├── app.so          必需，应用代码
└── icon.png        可选，70x70 黑白 PNG；缺省用内置图标
```

manifest.json 字段：

| 字段 | 必填 | 说明 |
|---|---|---|
| `id` | 是 | 应用短标识，1..12 字符（NVS 开关键 `en_<id>`）；与内置应用同名时内置优先 |
| `name` | 是 | 桌面显示名，UTF-8 中文可直接用 |
| `icon` | 否 | 应用目录内相对路径（如 `icon.png`）；禁止 `..`、绝对路径、反斜杠 |
| `abi` | 是 | 整数闸门，须等于固件 `METALIO_DYNAPP_ABI_VERSION`；不匹配的包在扫描阶段点名拒绝，不执行任何代码 |
| `version` | 否 | 应用版本号（预留，设置页显示用） |

生命周期：

- **开机/插卡扫描**：只读 manifest（cJSON 解析，零代码执行）→ 注册惰性入口
- **点开**：dlopen(app.so) → `metalio_app_entry(&desc)`（abi 二次校验）→ `desc.create()` 建屏
- **退出**：屏幕树删除完成后 dlclose → `esp_elf_deinit` 释放 PSRAM 代码段，真实卸载
- **删除**：删目录即卸载；经 `/fs/delete` 删除 `apps/<dir>` 会连桌面入口一起注销

## 2. 文件直传（AI 可操作，内容不经模型）

常驻 MCP 服务（`http://<设备IP>:8080`，开机即监听）提供两个端点，二进制直传，无 base64、无大模型中转：

```bash
# 上传（目录自动逐级创建；覆盖已有文件）
curl -X POST "http://<IP>:8080/fs/upload?path=<相对目录>" \
  -H "X-Filename: <文件名>" \
  [-H "Authorization: Bearer <PIN>"] \
  --data-binary @<本地文件>

# 删除（文件或目录递归；apps/<dir> 连桌面入口一起注销）
curl -X POST "http://<IP>:8080/fs/delete?path=<相对路径>" \
  [-H "Authorization: Bearer <PIN>"]
```

- `<相对路径>` 限 `/sdcard/metalio/e-ink/` 之下（字符白名单，拒绝 `..` / 绝对路径 / 反斜杠）
- 推送到 `apps/<dir>/` 的应用包即时重扫注册，**免重启上桌面**
- 认证与 `/mcp` 同一把 6 位数字 PIN（设备设置页查看；未设 PIN 则免认证）
- MCP 工具 `self.fs.save_file`（tools/list 可见）承载以上用法说明，AI 据此直接用 curl 推送

传输通道不限于 HTTP：USB 虚拟 U 盘模式、拔卡直拷同样可用（路径为卡根目录 `metalio/e-ink/apps/<dir>/`）。

## 3. 应用开发工作流

样例工程：`tools/hello_app/`（含 README 与可运行样例）。

1. 写应用（纯 C + LVGL，参考 `tools/hello_app/main/hello_main.c`）：
   - 导出 `void metalio_app_entry(metalio_app_desc_t* out)`，填 `abi_version/id/name/icon/create`
   - `entry` 必须无副作用（仅填 desc，不启动任务不定时器）
   - `create` 返回 `lv_obj_create(NULL)` 新建的屏幕；只调用固件符号表内的 API
   - 应用的定时器/任务必须在屏幕 `LV_EVENT_DELETE` 回调里自行清理——退出即卸载，跨屏幕的后台存活即悬空
2. 出 .so：`bash tools/hello_app/build_so.sh`——编译参数取自固件 `build/compile_commands.json`（头路径与宏定义和固件完全一致，保证 ABI 一致），链接参数与官方 elf_loader 的 project_so 相同（`-shared -fPIC -nostdlib`，符号运行期解析）
3. 组包：`manifest.json`（`abi` 填 `METALIO_DYNAPP_ABI_VERSION`）+ `app.so` + `icon.png`
4. 推送：上节 curl，或拷卡 / U 盘

## 4. 符号表（能力面）与扩展

.so 只能调用固件符号表导出的函数（`main/services/dynamic_apps/dynamic_app_loader.cc` 的 `g_metalio_dynapp_syms`）。表项地址由链接器在链接期解析（天生自洽），引用同时把被 `--gc-sections` 裁掉的函数钉在固件里。

- 当前导出：核心 `lv_*` 界面 API + `fontpack_lv_font_get`（大字体桥）
- libc（malloc/printf/字符串/pthread 等）与 lwip socket 由 elf_loader 组件内置符号表覆盖
- 扩展方式：`xtensa-esp32s3-elf-nm 新.so | grep ' U '`，把缺失符号加进表重编固件；表外符号在 dlopen 时由 dlerror 点名
- 固件 C++ 服务类无法直接导出（名字修饰），需要时在固件侧加 C ABI 垫片函数再导出

## 5. 边界

- **无后台常驻**：惰性模型退出即卸载，需要后台运行的应用（如后台播流）不适用
- **无 boot 期 MCP 注册**：动态应用开机不加载，`RegisterMcpTools` 通道不适用
- **固件升级 = abi 变化**：旧 .so 需重编；manifest 的 abi 字段让不匹配的包被点名拒绝而非崩溃
- **图标解码依赖 `esp_lv_decoder`**：已随显示初始化注册（`lv_adapter_display.cc`），S: 盘符（LVGL POSIX FS → /sdcard）+ PNG 路线；失败时图标空白，机制不受影响

## 6. 实现索引

| 组件 | 位置 |
|---|---|
| 加载器（扫描/注册/卸载钩子/符号表） | `main/services/dynamic_apps/dynamic_app_loader.cc` |
| 应用 ABI 契约 | `main/services/dynamic_apps/dynamic_app_abi.h` |
| elf_loader 组件（官方，v1.3.5） | `managed_components/espressif__elf_loader`（dlopen 模式 + S3 PSRAM 运行） |
| 常驻 HTTP 端点（upload/delete） | `main/services/local_mcp/local_mcp_server.cc` |
| 扫描时机（SD 挂载同步事件） | `main/core/boot_orchestrator.cc` + `main/boards/metalio-e-ink-4/metalio_e_ink_4_board.cc` |
| 样例应用 | `tools/hello_app/` |
