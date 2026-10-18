# Metalio 动态应用 SDK —— ABI 变更记录

第三方 .so 应用只能调用 `symbols.txt` 中列出的符号。
ABI 稳定政策:**签名只增不改**;破坏性变更（删符号/改签名）递增 `METALIO_DYNAPP_ABI_VERSION`。

## v2.0.57 (2026-10-18) —— 首个自动化 SDK 发布

- 首次引入自动化符号导出:`scripts/gen_symbols.sh` 从固件 ELF 自动生成导出表
  (`generated_symbol_table.inc`)并经编译器取址验证,替代手工维护的白名单
- 导出符号共 849 个:LVGL 图形库 710 个(lv_* 前缀自动发现)、
  动态应用系统服务垫片 62 个(dynapp_*)、基础库 77 个(libc/cJSON/esp/radio/时间)
- 新增 `dynapp_image_decode_l8`(图片文件解码为 L8 灰度,壁纸缩略图/预览依赖)
- 新增 LVGL 图片组件导出:`lv_image_create`、`lv_image_set_src`
- SDK 目录确立为 ABI 唯一事实源:`sdk/include/metalio/abi.h`、`sdk/include/metalio/services.h`
  (原 `dynamic_app_abi.h`/`dynamic_app_shims.h` 保留为兼容转发头)
