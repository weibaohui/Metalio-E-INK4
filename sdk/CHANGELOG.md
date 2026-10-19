# Metalio 动态应用 SDK —— ABI 变更记录

第三方 .so 应用只能调用 `symbols.txt` 中列出的符号。
ABI 稳定政策:**签名只增不改**;破坏性变更（删符号/改签名）递增 `METALIO_DYNAPP_ABI_VERSION`。

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
