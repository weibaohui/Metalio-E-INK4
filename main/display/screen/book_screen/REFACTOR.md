# book_screen 结构拆分方案

目标：把 `book_screen.cc` 从巨石文件拆成可审、可并行改的模块，行为不变。

## 已完成

| 模块 | 文件 | 说明 |
|------|------|------|
| 共享状态 | `book_ui_state.h` / `.cc` | `BookUiState` + `State()`，供后续模块复用 |
| TTF 导入面板 | `book_ttf_panel.h` / `.cc` | SD `fonts_ttf` 扫描、列表/确认/进度/结果 UI、转换轮询 |
| 云书库 | `book_cloud_library.*` | 已有 |
| 封面加载 | `book_cover_loader.*` | 已有 |
| 字库管理 | `book_font_manager.*` | 已有（TTF UI 此前仍半留在主文件，现已拆出） |
| 阅读偏好 | `book_reader_prefs.*` | 已有 |

`book_screen.cc`：约 6954 → 6196 行（本轮 TTF 拆分后）。

## 建议顺序（未做）

1. **书架 UI** — `CreateBookshelfScreen` / `RenderBookshelfPage` / 多选；依赖封面队列，边界次清晰
2. **目录 UI** — `OpenReadToc` / `RenderTocList`；复用 `BookUiState`
3. **阅读页 UI** — 翻页手势、进度条、页图展示；与 session 解耦
4. **`book_session.cc` 按格式拆** — TXT / EPUB / 进度持久化分文件
5. （可选）设置卡「字体列表」与 TTF 面板对称再拆一层

## 纪律

- 行为不变；共享状态走 `BookUiState`，避免循环依赖
- 新 `.cc` 必须加入 `main/CMakeLists.txt` 的 `SOURCES`
- PSRAM 栈任务禁止直接 NVS/SPI flash；落盘走 `AsyncSettingsStore` / main Schedule
- 构建产生的无关 `sdkconfig` drift 勿提交

## 验证

每拆一块：`bash "$HOME/.agents/skills/metalio-e-ink4-flash/scripts/flash.sh" "$HOME/Desktop/Metalio-E-INK4" --build-only`
