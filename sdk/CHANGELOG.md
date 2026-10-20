# Metalio 动态应用 SDK —— ABI 变更记录

第三方 .so 应用只能调用 `symbols.txt` 中列出的符号。
ABI 稳定政策:**签名只增不改**;破坏性变更（删符号/改签名）递增 `METALIO_DYNAPP_ABI_VERSION`。

## v2.0.58 (2026-10-20)

- **WiFi 传书 / WiFi 文件管理合并为一个「WiFi 传输」应用**(固件内置服务
  `wifi_transfer` 已下线,能力并入 `wifi_file`):
  - Web 端改为「传书 / 文件管理」双页签:传书页签直传设备书库
    (`/sdcard/metalio/e-ink/books`),传完立即失效书库缓存,阅读器马上能看到新书;
    文件管理页签保持全盘浏览/上传/下载/删除/新建目录
  - 新增 HTTP 接口 `GET /api/fs/roots` → `{"books":"/metalio/e-ink/books"}`，
    供前端取图书库权威路径(页面不再硬编码产品目录)
  - 固件侧新增 `dynapp_wifi_file_set_callback`(见下),活动事件含
    「传书/上传/删除/新建目录」+ 路径
- **新增符号**(纯增量,不破坏 ABI):
  - `dynapp_wifi_file_set_callback(dynapp_fs_activity_cb_t, void*)`
  - `dynapp_fs_activity_cb_t` = `void (*)(const char* action, const char* path, void* user_data)`
  - `fontpack_lv_font_math(size)` —— 取数学/符号字体(fontpack 内 18/28/36@2),
    专供公式渲染,不做汉字检查
- **字体接口行为修正**(影响所有第三方应用的取字行为):
  `fontpack_lv_font_get(size, bpp)` 现在**只返回含汉字的字体**。此前它的冒烟检查是
  「中 **和** A 都没找到才返回 null」,而 fontpack 里的 18@2 / 28@2 / 36@2 是
  **纯数学符号包**(各 333 字、零汉字),只满足「有 ASCII」,于是被误判为可用 ——
  凡是用这些字号渲染的中文全部变成空白、只剩 ASCII 露出来(典型症状:地址未启动时
  显示成 `()`,因为「服务未运行 (请点击下方启动)」里只有括号是 ASCII)。
  现在这类规格一律回退到 UI 字体 25@2。公式渲染改用 `fontpack_lv_font_math()`;
  第三方应用若用 `fontpack_lv_font_get(18|28|36, 2)` 取数学字体,需一并改用新接口。
- **deprecated**(符号保留为空实现,不删符号 → 不递增 ABI 版本):
  `dynapp_wifi_transfer_start/stop/is_running/get_port/get_url/set_callback`
  旧「WiFi 传书」应用的功能已并入 `wififile`,固件装载器会把残留的
  `transfer` 应用包判为退役,不再出现在桌面与商店。
- 端口回退: `wifi_file` 与原 `wifi_transfer` 行为对齐,80 端口被占用时回退 8088

## v2.0.57-r2 (2026-10-19)

- **重要**: .so 命名规范变更为 `lib/<应用id>.so`(打包脚本自动处理)。原因:
  esp_dlfcn 按基础文件名缓存已加载模块,所有应用共用 `lib/app.so` 时,第二个
  应用的 dlopen 会命中第一个应用的缓存模块(表现为"启动 A 却显示 B")
- libgcc 双精度浮点全族导出(22 个:__adddf3/__muldf3/__divdf3/__eqdf2 等),
  第三方应用做任何浮点运算不再撞"符号缺失"
- SDK 新增头文件:cJSON.h(电台 JSON 解析)、services/radio/(播放器+目录 API)、
  esp_random.h(硬件随机数)、lvgl 全量头 + 固件 sdkconfig.h
- 全部 13 个官方应用已迁移至 SDK 构建,静态符号回归全部通过

## v2.0.57 (2026-10-18) —— 首个自动化 SDK 发布

- 首次引入自动化符号导出:`scripts/gen_symbols.sh` 从固件 ELF 自动生成导出表
  (`generated_symbol_table.inc`)并经编译器取址验证,替代手工维护的白名单
- 导出符号共 849 个:LVGL 图形库 710 个(lv_* 前缀自动发现)、
  动态应用系统服务垫片 62 个(dynapp_*)、基础库 77 个(libc/cJSON/esp/radio/时间)
- 新增 `dynapp_image_decode_l8`(图片文件解码为 L8 灰度,壁纸缩略图/预览依赖)
- 新增 LVGL 图片组件导出:`lv_image_create`、`lv_image_set_src`
- SDK 目录确立为 ABI 唯一事实源:`sdk/include/metalio/abi.h`、`sdk/include/metalio/services.h`
  (原 `dynamic_app_abi.h`/`dynamic_app_shims.h` 保留为兼容转发头)
