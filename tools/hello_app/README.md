# hello_app —— 动态应用（.so）试点

墨水屏免刷机动态加载的最小可运行样例：应用为 SD 卡上的一个目录
（manifest.json + app.so + icon.png），点开才 dlopen，退出即 dlclose。

## 应用包格式

拷入 SD 卡 `metalio/e-ink/apps/<dir>/`（电脑上看到的路径，无 sdcard 前缀）：

```
metalio/e-ink/apps/hello/
├── manifest.json   {id, name, icon?, abi, version?}
├── app.so          代码
└── icon.png        可选，70x70 黑白 PNG；缺省用内置图标
```

- `id`：≤12 字符（NVS 开关键 en_<id>）；与内置应用同名时内置优先
- `abi`：整数闸门，固件升级后不匹配的包在扫描阶段点名拒绝（不执行代码）
- `icon`：目录内相对路径，经 LVGL POSIX FS（S: 盘符 → /sdcard）加载；
  禁止 `..`、绝对路径；FS+PNG 解码路线未经真机验证，失败降级内置图标
- 卸载 = 从卡上删目录；禁用 = 设置页应用开关（文件保留）

## 开发工作流

1. 写 `main/hello_main.c`（纯 C + LVGL；仅用固件符号表内的 API）：
   - `metalio_app_entry(metalio_app_desc_t*)`：填 id/name/icon/create，必须无副作用
   - create：`lv_obj_create(NULL)` 建屏；定时器在屏幕 `LV_EVENT_DELETE` 自行清理
2. 出 .so：`bash build_so.sh`（编译参数取自固件 build/compile_commands.json，ABI 一致）
3. 组包：app.so + manifest.json（abi 与固件 `METALIO_DYNAPP_ABI_VERSION` 一致）+ icon.png
4. 拷卡 → 重启 → 桌面出现 → 点开

新 .so 引用表外符号时，dlopen 在串口 `dlerror` 里点名缺失符号：
把 `main/services/dynamic_apps/dynamic_app_loader.cc` 的符号表补上重编固件。
