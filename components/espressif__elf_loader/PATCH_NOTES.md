# Vendored 补丁说明（Metalio）

基线：espressif/elf_loader ==1.3.5。本目录为带补丁的 vendored 副本，
`main/idf_component.yml` 已不再声明该依赖，改用本目录构建。

## 补丁 1：TEXT section 尺寸不再 4 字节向上取整

位置：`src/esp_elf.c` → `esp_elf_load_section()`（`CONFIG_ELF_LOADER_BUS_ADDRESS_MIRROR` 路径）。

- 上游：`elf->sec[ELF_SEC_TEXT].size = ELF_ALIGN(shdr[i].size, 4);`
- 补丁：`elf->sec[ELF_SEC_TEXT].size = shdr[i].size;`

### 根因

Xtensa PIC 的 `.rodata`（`.rodata.str1.1`，1 字节对齐）会被链接器紧贴 `.text`
末尾放置（无填充）。上游把 TEXT 尺寸向上取整到 4 字节后，`map_sym()` /
`elf_remap_text()` 的 `[addr, addr+size)` 判定区间覆盖了 `.rodata` 头部最多
3 字节。落在这 3 字节内的字符串/对象指针在 `R_XTENSA_RELATIVE` 重定位时被
误判为代码，`elf_remap_text` 将其加 `OFFSET_TEXT_VALUE`（0x6000000）映射进
I-window（0x42xxxxxx）；固件随后对该指针做数据读取（如 `strlen`/`fopen(name)`）
即触发 LoadStoreError 崩机重启。

- 实测：game2048.so `dynapp_fopen_data("hiscore.txt")`，
  EXCVADDR=0x424e1d57 = data_window_base(0x3c4df33c) + 0x2a1b（字符串 VMA）。
- 2026-10-19 扫描：13 个在用 app 中 11 个存在此布局重叠（HAZARD），是否崩溃
  仅取决于是否有被引用指针恰落在头 3 字节——纯布局运气。

### 为何修加载器而非改 .so 侧对齐

加载器修复对已发布、在用、第三方的全部 .so 生效；改 SDK 构建旗标只能保护
未来构建，且需要自定义链接脚本才能强制 `.rodata` 对齐。
