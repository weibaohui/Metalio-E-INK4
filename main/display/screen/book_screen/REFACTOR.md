# book_screen 结构拆分方案

目标：把 `book_screen.cc` 从巨石文件拆成可审、可并行改的模块，行为不变。

## 已完成

| 模块 | 文件 | 说明 |
|------|------|------|
| 共享状态 | `book_ui_state.h` / `.cc` | `BookUiState` + `State()`，供后续模块复用 |
| TTF 导入面板 | `book_ttf_panel.h` / `.cc` | SD `fonts_ttf` 扫描、列表/确认/进度/结果 UI、转换轮询 |
| 书架 UI | `book_bookshelf.h` / `.cc` | `CreateBookshelfScreen` / `RenderBookshelfPage` / 多选；封面格供首页复用；经 hooks 接封面队列与进屏收尾 |
| 目录 UI | `book_toc.h` / `.cc` | `OpenReadToc` / `RenderTocList` / 翻页 coalesce；经 hooks 接状态栏、封面队列与回正文 |
| 阅读页 UI | `book_reader.h` / `.cc` | 翻页手势 / coalesce / 底栏进度 / 页图展示与解码；`CreateReaderScreen` 与打开 worker 仍留主文件 |
| 云书库 | `book_cloud_library.*` | 已有 |
| 封面加载 | `book_cover_loader.*` | 已有 |
| 字库管理 | `book_font_manager.*` | 已有（TTF UI 此前仍半留在主文件，现已拆出） |
| 阅读偏好 | `book_reader_prefs.*` | 已有 |

`book_screen.cc`：约 6954 → 6196（TTF）→ 约 5566（书架 UI）→ 约 5310（目录 UI）→ 约 4755（阅读页 UI）行。

## 已完成（续）

| 模块 | 文件 | 说明 |
|------|------|------|
| Session 按格式 | `reader/book_session_*.cc` | 进度 / TXT / EPUB·EBOOK 分文件；共享 helpers 在 `book_session_internal.*`；核心 Open/Close/导航/块分页仍在 `book_session.cc` |
| 阅读设置卡片 | `book_settings_sheet.h` / `.cc` | `CreateSettingsSheet` / `ShowReadSettingsSheet` / 字体选择多选删除 / 间距 / 页进度 / 目录按钮；hooks 接排版生效与状态栏 |

`reader/book_session.cc`：约 3223 → 约 980（core）+ progress/txt/epub/internal。
`book_screen.cc`：约 6954 → ... → 约 4755（阅读页 UI）→ 约 3378（设置卡片）。

## 建议顺序（未做）

1. （可选）`CreateReaderScreen` / 打开 worker 从 `book_screen.cc` 外移到阅读编排模块
2. （可选）设置卡「字体列表」与 TTF 面板对称再拆一层

## 纪律

- 行为不变；共享状态走 `BookUiState`，避免循环依赖
- 新 `.cc` 必须加入 `main/CMakeLists.txt` 的 `SOURCES`
- PSRAM 栈任务禁止直接 NVS/SPI flash；落盘走 `AsyncSettingsStore` / main Schedule
- 构建产生的无关 `sdkconfig` drift 勿提交

## 验证

每拆一块：`bash "$HOME/.agents/skills/metalio-e-ink4-flash/scripts/flash.sh" "$HOME/Desktop/Metalio-E-INK4" --build-only`
