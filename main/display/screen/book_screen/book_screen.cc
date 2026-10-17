#include "book_screen.h"

// xtensa-g++：本文件过大时 -O2 可能 ICE（cfgcleanup try_forward_edges）
#pragma GCC optimize("O1")

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <string>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "SdCardManager.hpp"
#include "assets/lang_config.h"
#include "book_screen/book_cover_loader.h"
#include "book_screen/book_font_manager.h"
#include "book_screen/book_ttf_panel.h"
#include "book_screen/book_bookshelf.h"
#include "book_screen/book_toc.h"
#include "book_screen/book_reader.h"
#include "book_screen/book_settings_sheet.h"
#include "book_screen/book_detail_screen.h"
#include "book_screen/book_ui_state.h"
#include "book_screen/book_reader_prefs.h"
#include "reader/book_cover_sidecar.h"
#include "reader/book_home_snapshot.h"
#include "reader/book_progress_cache.h"
#include "reader/book_stats_service.h"
#include "display/font/epdfont.h"
#include "display/status_bar_manager.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "lv_adapter_display.h"
#include "reader/reader.h"
#include "reader/reader_page_render.h"
#include "reader_core/text_encoding.h"
#include "screen_common.h"
#include "sd_paths.h"
#include "utils/psr_worker.h"
#include "vk_key_handler.h"
#include "vk_page_repeat.h"


namespace {

constexpr const char* TAG = "BookScreen";
constexpr const char* kScreenLibrary = "book";         // 阅读首页
constexpr const char* kScreenDetail = "book_detail";
constexpr const char* kScreenRead = "book_read";
constexpr lv_coord_t kListPad = 16;   // 首页/书架外边距
constexpr lv_coord_t kFooterH = 36;
// 分页高度相对正文区再收一点：末行字形/下划线(+4)可能略超 line_height，避免钻进底栏
constexpr lv_coord_t kReadViewportBottomSlack = 8;
constexpr lv_coord_t kShelfUnderlineH = 1;
constexpr lv_coord_t kShelfUnderlinePadHor = 5;
constexpr lv_coord_t kShelfMultiDotSize = 6;
constexpr lv_coord_t kShelfCheckSize = 24;     // 封面角勾选
constexpr int64_t kFontSuppressRowClickUs = 1200000;  // 设置卡刷行更慢，松手假点击窗更宽
constexpr lv_coord_t kHomeShelfBtnH = 76;  // 「我的书架」按钮
constexpr lv_coord_t kHomeShelfBtnRadius = 8; // 方框略圆，勿全胶囊
constexpr lv_coord_t kHomeCardRadius = 14;
constexpr lv_coord_t kHomeGap = 12;
constexpr lv_coord_t kHomeSectionGap = 12; // 继续阅读 ↔ 最近阅读
constexpr lv_coord_t kHomeShelfTopGap = 22; // 封面 ↔ 我的书架
constexpr lv_coord_t kHomeBottomPad = 8;   // 书架距屏底再上抬
constexpr lv_coord_t kHomeStatsPad = 8;    // 统计区上下内边距
constexpr lv_coord_t kHomeStatsGap = 8;    // 统计行距
constexpr lv_coord_t kRecentCoverH = 290;  // 最近阅读封面高度上限（比例适中，勿吃满余高）
constexpr lv_coord_t kRecentTitleGap = 14; // 最近阅读封面与书名间距
constexpr lv_coord_t kCoverFrameBorder = 1; // 封面外框；图须缩进，避免 L8 盖住描边
constexpr lv_coord_t kCoverSpineInset = 6;  // 书脊竖线相对左边框内缩
constexpr lv_coord_t kTocRowGap = 6;    // 字号列表行距（目录 UI 已拆到 book_toc）
constexpr lv_coord_t kTocLineThin = 1;     // 章间/字体行分隔线
constexpr lv_coord_t kTocLineCur = 3;      // 当前章/选中字体上下分隔线
constexpr lv_coord_t kRowBorderW = 2;
constexpr lv_coord_t kRowRadius = 8;
constexpr lv_coord_t kReadPadTop = kListPad;  // 与左右边距一致
constexpr lv_coord_t kSheetSidePad = 12;      // 悬浮设置卡片左右边距
constexpr lv_coord_t kSheetInnerPad = 12;
constexpr lv_coord_t kSheetGap = 10;
constexpr lv_coord_t kSheetIconBtn = 36;
constexpr lv_coord_t kSheetRadius = 16;
constexpr lv_coord_t kSheetBorderW = 2;       // 卡片描边（加粗）
constexpr int kFontListPageSize = 5;         // 设置卡字体列表每页固定行数
constexpr lv_coord_t kFontListRowH = 44;     // 与目录行高一致，列表总高固定
constexpr uint32_t kLayoutDebounceMs = 300;   // 抬起且停顿后再排版（定时器内仍会等手指）
constexpr uint32_t kLayoutHintTickMs = 500;   // 「完成」提示到期检查
constexpr int64_t kLayoutHintDoneUs = 1500 * 1000;  // 「完成」显示时长
constexpr int kBuiltinReadingSizePx = 25; // 无 SD .ef 时正文回退 fontpack
constexpr lv_coord_t kDetailCoverMaxW = 152;
constexpr lv_coord_t kDetailCoverMaxH = 214;
constexpr lv_coord_t kDetailCtaBottom = 24; // 继续阅读距屏幕底（+8）
constexpr lv_coord_t kDetailProgCardH = 88; // 进度卡固定高（标题与百分比同行）
constexpr lv_coord_t kDetailTitleLines = 1; // 书名单行省略；其余控件整体上移一行
constexpr lv_coord_t kDetailSectionGap = 8; // 详情纵距收紧
constexpr lv_coord_t kOpenBarW = 320;
constexpr lv_coord_t kOpenBarH = 28;
constexpr uint32_t kOpenWorkerStack = 16 * 1024;
constexpr uint32_t kDetailCoverWorkerStack = 16 * 1024;
constexpr uint32_t kListCoverWorkerStack = 16 * 1024;
// 阅读时长周期落盘：30–60s 中取 45s，掉电最多丢约半分钟
constexpr uint32_t kReadTimeCheckpointMs = 45000;

using book_ui::CoverFrameInner;
using book_ui::StyleBookCoverFrame;
using book_ui::DrawBookCoverBorder;

using book_font::ReadFontEntry;
using book_font::ParseEfSizePx;
using book_font::ReadFontEntryLess;
using book_font::DeleteReadFontFile;
using book_font::ListFont;
using book_font::ItemFont;
using book_font::BookFont;
using book_font::EnsureBookFont;
using book_font::ReleaseBookFont;
using book_font::PrewarmPageFont;

using book_screen_detail::BookUiState;
using book_screen_detail::State;
using book_screen_detail::OnFontImportClick;
using book_screen_detail::StopTtfPollTimer;
using book_screen_detail::TtfEnsurePanel;
using book_screen_detail::ClearTtfWidgetRefs;
using book_screen_detail::kTtfConvertSizeDefault;
using book_screen_detail::SetTtfFontsChangedHandler;
using book_screen_detail::SetTtfEfTemplateHandler;
using book_screen_detail::kScreenBookshelf;
using book_screen_detail::CreateBookshelfScreen;
using book_screen_detail::RenderBookshelfPage;
using book_screen_detail::RequestRenderBookshelfPage;
using book_screen_detail::RequestShelfCheckMarksPaint;
using book_screen_detail::ExitShelfMultiMode;
using book_screen_detail::ListPageCount;
using book_screen_detail::ClampListPage;
using book_screen_detail::CreateBookCoverCell;
using book_screen_detail::SetBookshelfPrepareEnterHandler;
using book_screen_detail::SetBookshelfCancelListCoverHandler;
using book_screen_detail::SetBookshelfScheduleListCoverHandler;
using book_screen_detail::SetBookshelfEnqueueListCoverHandler;
using book_screen_detail::SetBookshelfCreateCoverSlotHandler;
using book_screen_detail::SetBookshelfOpenDetailHandler;
using book_screen_detail::SetBookshelfTruncateHandler;
using book_screen_detail::SetBookshelfAttachVkHandler;
using book_screen_detail::OpenReadToc;
using book_screen_detail::RenderTocList;
using book_screen_detail::RequestRenderTocList;
using book_screen_detail::ResetTocPaint;
using book_screen_detail::SetTocFlushLayoutHandler;
using book_screen_detail::SetTocPrepareEnterHandler;
using book_screen_detail::SetTocCancelListCoverHandler;
using book_screen_detail::SetTocScheduleListCoverHandler;
using book_screen_detail::SetTocEnqueueListCoverHandler;
using book_screen_detail::SetTocSetStatusVisibleHandler;
using book_screen_detail::SetTocSetStatusTitleHandler;
using book_screen_detail::SetTocApplyOverlayPadsHandler;
using book_screen_detail::SetTocDetachBodyInputHandler;
using book_screen_detail::SetTocTruncateHandler;
using book_screen_detail::SetTocUpdateFooterHandler;
using book_screen_detail::SetTocResetLongPressHandler;
using book_screen_detail::SetTocRenderReaderPageHandler;
using book_screen_detail::SetReaderHideChromeHandler;
using book_screen_detail::SetReaderShowSettingsSheetHandler;
using book_screen_detail::SetReaderSetStatusVisibleHandler;
using book_screen_detail::SetReaderSetSettingsSheetVisibleHandler;
using book_screen_detail::SetReaderApplyImmersiveGeometryHandler;
using book_screen_detail::SetReaderApplyLayoutLiveHandler;
using book_screen_detail::SetReaderIsLayoutHintBusyHandler;
using book_screen_detail::SetReaderOnPageImageIdleHandler;
using book_screen_detail::SetReaderTruncateHandler;
using book_screen_detail::ResetReadTouch;
using book_screen_detail::DetachReadBodyInput;
using book_screen_detail::AttachReadBodyInput;
using book_screen_detail::QueueReaderPageTurn;
using book_screen_detail::ApplyReaderPageDelta;
using book_screen_detail::RequestRenderReaderPage;
using book_screen_detail::RenderReaderPage;
using book_screen_detail::UpdateReadFooter;
using book_screen_detail::ResetPointerLongPress;
using book_screen_detail::CancelPageImageLoad;
using book_screen_detail::ResetReaderPaint;
using book_screen_detail::ClearReaderPageDelta;
using book_screen_detail::HasPendingReaderPageDelta;
using book_screen_detail::CreateSettingsSheet;
using book_screen_detail::ShowReadSettingsSheet;
using book_screen_detail::SetReadSettingsSheetVisible;
using book_screen_detail::CloseReadSettingsSheet;
using book_screen_detail::RebuildSettingsSheet;
using book_screen_detail::RefreshSettingsSheet;
using book_screen_detail::RefreshSettingsSheetAdj;
using book_screen_detail::RefreshSettingsSheetFontList;
using book_screen_detail::ClearSettingsSheetWidgetRefs;
using book_screen_detail::SettingsSheetWidgetsReady;
using book_screen_detail::EnsureSettingsSheetAfterLayout;
using book_screen_detail::RequestSettingsSheetPaint;
using book_screen_detail::ResetSettingsSheetPaint;
using book_screen_detail::ScanReadFonts;
using book_screen_detail::ExitFontMultiMode;
using book_screen_detail::CurrentFontIndex;
using book_screen_detail::FontListPageCount;
using book_screen_detail::ClampFontListPage;
using book_screen_detail::EnsureFontListPageShowsSelection;
using book_screen_detail::FormatFontListName;
using book_screen_detail::SetSheetBtnEnabled;
using book_screen_detail::UpdateSheetBoundTip;
using book_screen_detail::SetSettingsScheduleLayoutApplyHandler;
using book_screen_detail::SetSettingsApplyReaderLayoutLiveHandler;
using book_screen_detail::SetSettingsFlushLayoutDebounceHandler;
using book_screen_detail::SetSettingsRenderReaderPageHandler;
using book_screen_detail::SetSettingsSetStatusVisibleHandler;
using book_screen_detail::SetSettingsBindSessionLayoutPrefsHandler;
using book_screen_detail::CreateDetailScreen;
using book_screen_detail::ResumeDetailScreen;
using book_screen_detail::CancelDetailCoverLoad;
using book_screen_detail::ClearDetailCoverUiPtrs;
using book_screen_detail::OpenDetailAsync;
using book_screen_detail::BackToDetailAsync;
using book_screen_detail::SetDetailStartReadHandler;
using book_screen_detail::SetDetailCreateHomeScrHandler;
using book_screen_detail::SetDetailAttachVkHandler;
using book_screen_detail::SetDetailReleaseSessionHandler;
using book_screen_detail::IsCoverEmbedMiss;
using book_screen_detail::RememberCoverEmbedMiss;
using book_screen_detail::PostCoverEmbedMiss;
// 盖板键/长按定时器只累加；真正 Next/Prev 在 LVGL 绘制里做，避免跨章重建 pages_ 时 UAF

struct OpenProgressMsg {
    uint32_t token = 0;
    int percent = 0;
};

struct OpenDoneMsg {
    uint32_t token = 0;
    bool ok = false;
};

struct OpenWorkerArg {
    uint32_t token = 0;
    reader::BookInfo info;  // 拷贝，避免 Create 重建书库时 vector 失效
};

struct ListCoverDoneMsg {
    uint32_t token = 0;
    lv_obj_t* host = nullptr;
    reader::RasterImage* slot = nullptr;
    reader::RasterImage* cover = nullptr;
    std::string path;
    bool cover_ok = false;
};

struct ListCoverFillWork {
    uint32_t token = 0;
    std::vector<BookUiState::ListCoverPending> items;
};

bool IsReadOverlayChrome(BookUiState::ReadChrome c) {
    using C = BookUiState::ReadChrome;
    return c == C::kToc || c == C::kSettings;
}

void BindSessionLayoutPrefs(reader::BookSession& session) {
    BookReaderPrefsEnsureLoaded();
    session.SetFont(BookFont());
    session.SetGaps(static_cast<lv_coord_t>(BookReaderPrefsLineGap()),
                    static_cast<lv_coord_t>(BookReaderPrefsParaGap()));
}

/**
 * 正文内容区宽度（阅读页边距）；与沉浸态居中栏宽一致。
 * 书库/详情继续用 ContentWidth()（kListPad），互不影响。
 */
lv_coord_t ReadContentWidth() {
    BookReaderPrefsEnsureLoaded();
    const int ml = BookReaderPrefsMarginLeft();
    const int mr = BookReaderPrefsMarginRight();
    lv_coord_t w = LV_HOR_RES - static_cast<lv_coord_t>(ml + mr);
    return w > 40 ? w : 40;
}

/** 目录/设置等列表：固定用书库边距，避免阅读页边距档位挤乱菜单行宽 */
void ApplyOverlayListContentPads(lv_coord_t top_pad) {
    auto& st = State();
    if (st.content == nullptr) {
        return;
    }
    lv_obj_set_style_pad_top(st.content, top_pad, 0);
    lv_obj_set_style_pad_bottom(st.content, 0, 0);
    lv_obj_set_style_pad_left(st.content, kListPad, 0);
    lv_obj_set_style_pad_right(st.content, kListPad, 0);
}

/**
 * 阅读区几何模式（与 read_chrome 解耦，避免「浮层态算出的 viewport 写进 session」）：
 * - kImmersive：正文分页/渲染唯一真源；hide_prog 时不占底栏高度
 * - kOverlayList：目录/设置等临时 UI，始终留底栏文案区；不得用其结果 SetViewport
 */
enum class ReaderGeomMode : uint8_t { kImmersive = 0, kOverlayList };

/** 是否整页图模式（未 Open 时用 Peek，便于建屏设 viewport） */
bool ReaderPageImagesHint() {
    auto& st = State();
    if (st.session && st.session->IsOpen()) {
        return st.session->IsPageImagesMode();
    }
    if (st.selected < 0 || st.selected >= static_cast<int>(st.books.size())) {
        return false;
    }
    const reader::BookInfo& info = st.books[static_cast<size_t>(st.selected)];
    return info.format == reader::BookFormat::kEbook &&
           reader::EbookDocument::PeekPageImages(info.path.c_str());
}

/**
 * @brief 按偏好刷新正文 body 尺寸/边距/底栏（须在 LVGL 任务）
 * @param mode 仅 kImmersive 更新 st.viewport_*（供 SetViewport/重开）；浮层保留上次沉浸值
 */
void ApplyReaderPageGeometry(ReaderGeomMode mode) {
    auto& st = State();
    if (st.read_scr == nullptr || st.content == nullptr || !lv_obj_is_valid(st.content)) {
        return;
    }
    BookReaderPrefsEnsureLoaded();
    const bool show_footer =
        (mode == ReaderGeomMode::kOverlayList) || (BookReaderPrefsHideProgress() == 0);
    const lv_coord_t footer_h = show_footer ? kFooterH : 0;
    const lv_coord_t body_h = LV_VER_RES - footer_h;

    if (st.page_label != nullptr && lv_obj_is_valid(st.page_label)) {
        if (show_footer) {
            lv_obj_clear_flag(st.page_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(st.page_label, LV_OBJ_FLAG_HIDDEN);
        }
    }

    lv_obj_set_size(st.content, LV_HOR_RES, body_h);
    lv_obj_align(st.content, LV_ALIGN_TOP_MID, 0, 0);
    // 子控件画出 body 时勿盖住底栏
    lv_obj_remove_flag(st.content, LV_OBJ_FLAG_OVERFLOW_VISIBLE);

    // 浮层列表：只调 body/底栏；pad 由 ApplyOverlayListContentPads 设置，且禁止改 viewport_*
    if (mode == ReaderGeomMode::kOverlayList) {
        return;
    }

    const bool page_images = ReaderPageImagesHint();
    const int mt = BookReaderPrefsMarginTop();
    const int mb = BookReaderPrefsMarginBottom();
    if (page_images) {
        lv_obj_set_style_pad_top(st.content, 0, 0);
        lv_obj_set_style_pad_bottom(st.content, 0, 0);
        lv_obj_set_style_pad_left(st.content, 0, 0);
        lv_obj_set_style_pad_right(st.content, 0, 0);
        st.viewport_w = LV_HOR_RES;
        st.viewport_h = body_h;
    } else {
        // 左右 pad=0：水平居中由 RenderReaderPage 显式算 x，避免 align/flex 失效时贴左
        lv_obj_set_style_pad_top(st.content, static_cast<lv_coord_t>(mt), 0);
        lv_obj_set_style_pad_bottom(st.content, static_cast<lv_coord_t>(mb), 0);
        lv_obj_set_style_pad_left(st.content, 0, 0);
        lv_obj_set_style_pad_right(st.content, 0, 0);
        st.viewport_w = ReadContentWidth();
        lv_coord_t vh =
            body_h - static_cast<lv_coord_t>(mt + mb) - kReadViewportBottomSlack;
        st.viewport_h = vh > 40 ? vh : 40;
    }
}

void StopReadTimeCheckpointTimer() {
    auto& st = State();
    if (st.read_time_ckpt_timer == nullptr) {
        return;
    }
    lv_timer_del(st.read_time_ckpt_timer);
    st.read_time_ckpt_timer = nullptr;
}

void OnReadTimeCheckpoint(lv_timer_t* /*t*/) {
    auto& st = State();
    // 打开中 / session 已关：跳过；不删 timer（AsyncOpenDone 会重建）
    if (st.opening.load() || !st.session || !st.session->IsOpen()) {
        return;
    }
    const bool ok = st.session->SaveProgress();
    const uint32_t sec = st.session->ReadingSeconds();
    const uint32_t daily = st.session->ReadingDailySeconds();
    const uint32_t min = sec / 60u;
    const uint32_t rem = sec % 60u;
    ESP_LOGI(TAG, "read-time checkpoint (%us): %s total=%u sec (%u min %u sec) daily=%u sec title=%s",
             static_cast<unsigned>(kReadTimeCheckpointMs / 1000u), ok ? "saved" : "FAIL",
             static_cast<unsigned>(sec), static_cast<unsigned>(min), static_cast<unsigned>(rem),
             static_cast<unsigned>(daily), st.session->Title().c_str());
}

void StartReadTimeCheckpointTimer() {
    auto& st = State();
    StopReadTimeCheckpointTimer();
    st.read_time_ckpt_timer =
        lv_timer_create(OnReadTimeCheckpoint, kReadTimeCheckpointMs, nullptr);
    if (st.read_time_ckpt_timer == nullptr) {
        ESP_LOGW(TAG, "read time checkpoint timer create failed");
    }
}

/** 正文可读后开始累计；须在 LVGL 任务调用 */
void BeginReadingTimeTracking() {
    auto& st = State();
    if (!st.session || !st.session->IsOpen()) {
        return;
    }
    const uint32_t sec = st.session->ReadingSeconds();
    const uint32_t daily = st.session->ReadingDailySeconds();
    const uint32_t min = sec / 60u;
    const uint32_t rem = sec % 60u;
    ESP_LOGI(TAG, "read-time open: total=%u sec (%u min %u sec) daily=%u sec title=%s path=%s",
             static_cast<unsigned>(sec), static_cast<unsigned>(min), static_cast<unsigned>(rem),
             static_cast<unsigned>(daily), st.session->Title().c_str(),
             st.session->Info().path.c_str());
    st.session->StartReadingClock();
    StartReadTimeCheckpointTimer();
}

/**
 * 离开正文前停 checkpoint + 停墙钟（秒数进内存）。
 * session.reset()/Close 会再 SaveProgress 落盘；本函数不写盘，避免双写路径分叉。
 */
void EndReadingTimeTracking() {
    StopReadTimeCheckpointTimer();
    auto& st = State();
    st.reading_paused_by_standby = false;
    if (st.session && st.session->IsOpen()) {
        st.session->StopReadingClock();
    }
}

/** 进待机：停累计；退出待机由 ResumeReadingTimeAfterStandby 恢复 */
void PauseReadingTimeForStandby() {
    auto& st = State();
    if (!st.session || !st.session->IsReadingClockActive()) {
        return;
    }
    EndReadingTimeTracking();
    st.reading_paused_by_standby = true;
    ESP_LOGI(TAG, "read-time paused for standby");
}

void ResumeReadingTimeAfterStandby() {
    auto& st = State();
    if (!st.reading_paused_by_standby) {
        return;
    }
    st.reading_paused_by_standby = false;
    BeginReadingTimeTracking();
    ESP_LOGI(TAG, "read-time resumed after standby");
}

using reader::HomeStats;
using reader::BookStatsService;

lv_coord_t ContentWidth() {
    return LV_HOR_RES - kListPad * 2;
}

lv_coord_t MeasureTextWidth(const lv_font_t* font, const char* text) {
    if (font == nullptr || text == nullptr || text[0] == '\0') {
        return 0;
    }
    lv_point_t sz = {};
    lv_text_get_size(&sz, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return static_cast<lv_coord_t>(sz.x);
}

/** 单行截断：过长则末尾加「…」，按像素宽度预算。 */
std::string TruncateTextToWidth(const std::string& text, const lv_font_t* font, lv_coord_t max_w) {
    if (font == nullptr || max_w <= 0 || MeasureTextWidth(font, text.c_str()) <= max_w) {
        return text;
    }
    constexpr const char* kEllipsis = "…";
    const lv_coord_t ellipsis_w = MeasureTextWidth(font, kEllipsis);
    const lv_coord_t budget = max_w - ellipsis_w;
    if (budget <= 0) {
        return kEllipsis;
    }
    std::string out;
    out.reserve(text.size() + 3);
    const uint8_t* d = reinterpret_cast<const uint8_t*>(text.data());
    size_t i = 0;
    lv_coord_t used = 0;
    while (i < text.size()) {
        uint32_t cp = 0;
        const size_t n = reader::Utf8Next(d + i, text.size() - i, &cp);
        if (n == 0) {
            break;
        }
        lv_coord_t gw = static_cast<lv_coord_t>(lv_font_get_glyph_width(font, cp, 0));
        if (gw <= 0) {
            gw = 1;
        }
        if (used + gw > budget) {
            break;
        }
        out.append(text, i, n);
        used += gw;
        i += n;
    }
    out.append(kEllipsis);
    return out;
}

void DisableScroll(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(obj, LV_DIR_NONE);
}

void ShowMessage(lv_obj_t* parent, const char* msg, const lv_font_t* font = nullptr) {
    auto& st = State();
    if (parent == st.content) {
        st.open_bar = nullptr;
        st.open_pct_lbl = nullptr;
    }
    lv_obj_clean(parent);
    // 提示页需屏幕居中；关掉 flex，避免 START 把文案顶到上方
    lv_obj_set_style_layout(parent, LV_LAYOUT_NONE, 0);
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, msg != nullptr ? msg : "");
    lv_obj_set_style_text_font(label, font != nullptr ? font : ListFont(), 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, ContentWidth());
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_center(label);
    DisableScroll(label);
}

int EnsureBookInLibrary(const reader::BookInfo& info) {
    auto& st = State();
    for (int i = 0; i < static_cast<int>(st.books.size()); ++i) {
        if (st.books[static_cast<size_t>(i)].path == info.path) {
            st.books[static_cast<size_t>(i)] = info;
            // 更新后按最新优先挪到队首
            if (i > 0) {
                reader::BookInfo moved = std::move(st.books[static_cast<size_t>(i)]);
                st.books.erase(st.books.begin() + i);
                st.books.insert(st.books.begin(), std::move(moved));
            }
            // 与磁盘列表对齐缓存，避免进首页又复用开机旧快照
            reader::ReplaceBookLibraryCache(st.books);
            return 0;
        }
    }
    // 新书插入队首（与书库按最新 mtime 排序一致）
    st.books.insert(st.books.begin(), info);
    reader::ReplaceBookLibraryCache(st.books);
    return 0;
}

lv_obj_t* CreateReaderScreen(const reader::BookInfo& info);
lv_obj_t* ResumeReaderScreen();   // 返回栈：关 session 后再开，依 .pos 续读
void ReleaseReaderSessionForLeave();
void RenderReadingHome();
void ShowOpenProgress(int percent);
void StartOpenWorker(int book_index);
void CancelListCoverFill();
void EnqueueListCoverFill(const reader::BookInfo& info, int max_w, int max_h, lv_obj_t* host,
                          reader::RasterImage* slot);
void ScheduleListCoverFill();
void FinishDeferredCleanup();
void HideReadChrome();
void ReopenReaderAfterPrefsChange();
void ApplyReaderLayoutLive(bool reload_font);
void ScheduleLayoutApply(bool reload_font);
void FlushLayoutDebounce();
void CancelLayoutDebounce();
void StartLayoutWorker();
void StartChapterPageWorker();
void BindSessionLayoutPrefs(reader::BookSession& session);
void ApplyReaderPageGeometry(ReaderGeomMode mode);
bool IsReadOverlayChrome(BookUiState::ReadChrome c);
lv_coord_t ReadContentWidth();
void EnsureLayoutHintTimer();
void StopLayoutHintTimer();
void MarkLayoutHintBusy();
void MarkLayoutHintDone();
bool IsLayoutHintBusy();
bool BookLvAsync(void (*cb)(void*), void* user_data = nullptr);

// 书库/详情/正文共用：仅 BOOT 长按进百问；盖板 prev/next 长按连翻（vk_home 长按不进百问）
VkKeyScreenDesc BookAiLongPressDesc(ScreenFactory factory) {
    return VkKeyScreenDesc{factory, BookScreen::OnVkKey, nullptr, BookScreen::OnBootLongPress,
                           nullptr, nullptr, BookScreen::OnVkKeyLongPress,
                           BookScreen::OnVkKeyPressUp};
}





void GoHomeFromBookAsync(void* /*user_data*/) {
    // 勿在此 Reset detail_cover：旧详情屏可能仍引用 dsc，等 DELETE 回调再清
    CancelDetailCoverLoad();
    ReleaseReaderSessionForLeave();
    ScreenRequestBack();
}

void RequestBackHome() {
    BookLvAsync(GoHomeFromBookAsync);
}

void FinishDeferredCleanup() {
    auto& st = State();
    if (!st.deferred_cleanup) {
        return;
    }
    // worker 仍可能握着 session/font_ 计宽：busy 未清不得释字体
    if (st.opening.load() || st.layout_busy.load() || st.chapter_pages_busy.load() ||
        st.page_image_busy.load()) {
        return;
    }
    st.deferred_cleanup = false;
    EndReadingTimeTracking();
    st.session.reset();
    ReleaseBookFont();
}

bool ReaderWorkersBusy() {
    auto& st = State();
    return st.opening.load() || st.layout_busy.load() || st.chapter_pages_busy.load() ||
           st.page_image_busy.load();
}

// 离开正文（进百问等）：落盘 .pos、释放 session/字库；打开中则延后清。
void ReleaseReaderSessionForLeave() {
    auto& st = State();
    st.open_token.fetch_add(1);
    st.read_scr = nullptr;
    st.content = nullptr;
    st.page_label = nullptr;
    st.title_label = nullptr;
    st.open_bar = nullptr;
    st.open_pct_lbl = nullptr;
    st.status_bar = nullptr;
    st.status_overlay = nullptr;
    st.status_h = 0;
    st.settings_sheet = nullptr;
    ClearSettingsSheetWidgetRefs();
    StopTtfPollTimer();
    st.ttf_mode = BookUiState::TtfMode::kNone;
    st.ttf_pick.clear();
    st.ttf_page = 0;
    st.ttf_size_px = kTtfConvertSizeDefault;
    st.viewport_w = 0;
    st.viewport_h = 0;
    st.font_entries.clear();
    st.font_list_page = 0;
    st.font_multi = false;
    st.font_selected.clear();
    st.font_suppress_click_until_us = 0;
    st.font_suppress_click_idx = -1;
    st.font_suppress_next_click = false;
    st.read_chrome = BookUiState::ReadChrome::kReading;
    st.settings_resume_after_open = false;
    st.toc_list_page = 0;
    const bool layout_running = st.layout_busy.load();
    const bool chapter_pages_running = st.chapter_pages_busy.load();
    const bool page_image_running = st.page_image_busy.load();
    st.layout_token.fetch_add(1);
    st.layout_again = false;
    st.layout_reload_font = false;
    st.layout_anchor = {};
    st.chapter_pages_token.fetch_add(1);
    st.chapter_pages_again = false;
    CancelPageImageLoad();
    if (st.session) {
        st.session->InvalidateChapterPageTable();
        st.session->AbortTxtPaginate();
    }
    CancelLayoutDebounce();
    StopLayoutHintTimer();
    st.layout_hint_done_until_us = 0;
    ResetReaderPaint();
    ResetTocPaint();
    ResetSettingsSheetPaint();
    ClearReaderPageDelta();
    ResetReadTouch();
    if (st.opening.load() || layout_running || chapter_pages_running || page_image_running) {
        // 勿把 *_busy 置 false，否则其它路径会误 ReleaseBookFont 与计页抢字体
        StopReadTimeCheckpointTimer();
        st.deferred_cleanup = true;
    } else {
        EndReadingTimeTracking();
        st.session.reset();
        ReleaseBookFont();
    }
}

lv_obj_t* ResumeReaderScreen() {
    auto& st = State();
    // 解析尚未结束 / 后台计页未结束：退回书库，避免与 worker 抢 session/font
    if (ReaderWorkersBusy()) {
        return BookScreen::Create();
    }
    if (st.deferred_cleanup) {
        FinishDeferredCleanup();
    } else if (st.session) {
        EndReadingTimeTracking();
        st.session.reset();
        ReleaseBookFont();
    }
    if (st.selected < 0 || st.selected >= static_cast<int>(st.books.size())) {
        return BookScreen::Create();
    }
    return CreateReaderScreen(st.books[static_cast<size_t>(st.selected)]);
}

/** 盖板键在 touch_feed：须 ScreenLvAsync，禁止同步 lv_obj_clean/create（否则 tlsf 双释放）。 */
bool BookLvAsync(void (*cb)(void*), void* user_data) {
    const int64_t t0 = esp_timer_get_time();
    const bool ok = ScreenLvAsync(cb, user_data);
    ESP_LOGI(TAG, "BookLvAsync queue %s in %d us", ok ? "ok" : "fail",
             static_cast<int>(esp_timer_get_time() - t0));
    return ok;
}


void AsyncHideReadChrome(void* /*user_data*/) {
    ESP_LOGI(TAG, "async HideReadChrome begin");
    HideReadChrome();
    ESP_LOGI(TAG, "async HideReadChrome done");
}

/** 作废列表/目录封面补全；须在清槽/离页前调用。 */
void CancelListCoverFill() {
    auto& st = State();
    st.list_cover_token.fetch_add(1);
    st.list_cover_pending.clear();
}

void EnqueueListCoverFill(const reader::BookInfo& info, int max_w, int max_h, lv_obj_t* host,
                          reader::RasterImage* slot) {
    if (host == nullptr || slot == nullptr || max_w <= 0 || max_h <= 0 || info.path.empty()) {
        return;
    }
    if (info.format != reader::BookFormat::kEpub && info.format != reader::BookFormat::kEbook) {
        return;
    }
    if (IsCoverEmbedMiss(info.path)) {
        return;
    }
    auto& st = State();
    BookUiState::ListCoverPending p;
    p.info = info;
    p.max_w = max_w;
    p.max_h = max_h;
    p.host = host;
    p.slot = slot;
    st.list_cover_pending.push_back(std::move(p));
}

void ApplyListCoverImage(lv_obj_t* host, reader::RasterImage& cover) {
    if (host == nullptr || !lv_obj_is_valid(host) || cover.empty()) {
        return;
    }
    cover.BindDsc();
    lv_obj_clean(host);
    lv_obj_t* img = lv_image_create(host);
    lv_image_set_src(img, &cover.dsc);
    lv_obj_center(img);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
}

void AsyncListCoverDone(void* user_data) {
    auto* msg = static_cast<ListCoverDoneMsg*>(user_data);
    auto& st = State();
    if (!msg->cover_ok) {
        RememberCoverEmbedMiss(msg->path);
    }
    if (msg->token != st.list_cover_token.load() || msg->cover == nullptr) {
        delete msg->cover;
        delete msg;
        return;
    }
    if (msg->host == nullptr || !lv_obj_is_valid(msg->host) || msg->slot == nullptr) {
        delete msg->cover;
        delete msg;
        return;
    }
    if (msg->cover_ok && !msg->cover->empty()) {
        *msg->slot = std::move(*msg->cover);
        ApplyListCoverImage(msg->host, *msg->slot);
    }
    delete msg->cover;
    delete msg;
}

void ListCoverFillWorker(void* arg) {
    auto* work = static_cast<ListCoverFillWork*>(arg);
    auto& st = State();
    for (size_t i = 0; i < work->items.size(); ++i) {
        if (work->token != st.list_cover_token.load()) {
            break;
        }
        const BookUiState::ListCoverPending& item = work->items[i];
        auto* cover = new reader::RasterImage();
        reader::BookInfo info = item.info;
        const bool ok = book_ui::LoadBookDetailCover(info, item.max_w, item.max_h, *cover) &&
                        !cover->empty();
        auto* done = new ListCoverDoneMsg{};
        done->token = work->token;
        done->host = item.host;
        done->slot = item.slot;
        done->path = item.info.path;
        done->cover_ok = ok;
        if (ok) {
            done->cover = cover;
        } else {
            delete cover;
            done->cover = nullptr;
        }
        if (!ScreenLvAsync(AsyncListCoverDone, done)) {
            if (!ok) {
                PostCoverEmbedMiss(item.info.path);
            }
            delete done->cover;
            delete done;
        }
        // 串行抽封面，让出 CPU，避免列表滑动/点击发闷
        vTaskDelay(pdMS_TO_TICKS(ok ? 15 : 5));
    }
    delete work;
}

void ScheduleListCoverFill() {
    auto& st = State();
    if (st.list_cover_pending.empty()) {
        return;
    }
    ESP_LOGI(TAG, "list_cover fill n=%u", static_cast<unsigned>(st.list_cover_pending.size()));
    auto* work = new ListCoverFillWork{};
    work->token = st.list_cover_token.load();
    work->items = std::move(st.list_cover_pending);
    st.list_cover_pending.clear();
    if (!SpawnPsrWorker("list_cover", kListCoverWorkerStack, tskIDLE_PRIORITY + 1,
                        ListCoverFillWorker, work, nullptr, 0)) {
        ESP_LOGW(TAG, "list_cover task failed; keep placeholders");
        delete work;
    }
}





void ShowOpenProgress(int percent) {
    auto& st = State();
    if (st.content == nullptr) {
        return;
    }
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }

    const bool need_build =
        st.open_bar == nullptr || lv_obj_get_parent(st.open_bar) != st.content ||
        st.open_pct_lbl == nullptr || lv_obj_get_parent(st.open_pct_lbl) != st.content;

    if (need_build) {
        st.open_bar = nullptr;
        st.open_pct_lbl = nullptr;
        lv_obj_clean(st.content);
        // 打开进度 UI 用书库边距；避免阅读页边距档位导致 ContentWidth 溢出
        ApplyOverlayListContentPads(0);
        lv_obj_set_style_layout(st.content, LV_LAYOUT_NONE, 0);
        DisableScroll(st.content);

        lv_obj_t* panel = lv_obj_create(st.content);
        lv_obj_remove_style_all(panel);
        lv_obj_set_size(panel, ContentWidth(), LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(panel, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(panel, 18, 0);
        lv_obj_clear_flag(panel, LV_OBJ_FLAG_CLICKABLE);
        DisableScroll(panel);
        lv_obj_center(panel);

        lv_obj_t* title = lv_label_create(panel);
        lv_label_set_text(title, Lang::Strings::BOOK_OPENING);
        lv_obj_set_style_text_font(title, ListFont(), 0);
        lv_obj_set_style_text_color(title, lv_color_black(), 0);
        lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

        // 轨道：白底黑框；指示器：实心黑块（墨水屏清晰）
        lv_coord_t bar_w = kOpenBarW;
        if (bar_w > ContentWidth() - 8) {
            bar_w = ContentWidth() - 8;
        }
        lv_obj_t* bar = lv_bar_create(panel);
        lv_obj_set_size(bar, bar_w, kOpenBarH);
        lv_bar_set_range(bar, 0, 100);
        lv_bar_set_mode(bar, LV_BAR_MODE_NORMAL);
        lv_obj_set_style_radius(bar, 6, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(bar, 2, LV_PART_MAIN);
        lv_obj_set_style_border_color(bar, lv_color_black(), LV_PART_MAIN);
        lv_obj_set_style_pad_all(bar, 3, LV_PART_MAIN);
        lv_obj_set_style_radius(bar, 3, LV_PART_INDICATOR);
        lv_obj_set_style_bg_color(bar, lv_color_black(), LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        st.open_bar = bar;

        lv_obj_t* pct = lv_label_create(panel);
        lv_obj_set_style_text_font(pct, ListFont(), 0);
        lv_obj_set_style_text_color(pct, lv_color_black(), 0);
        lv_obj_set_style_text_align(pct, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_clear_flag(pct, LV_OBJ_FLAG_CLICKABLE);
        st.open_pct_lbl = pct;
    }

    lv_bar_set_value(st.open_bar, percent, LV_ANIM_OFF);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d%%", percent);
    lv_label_set_text(st.open_pct_lbl, buf);
}

void AsyncOpenProgress(void* user_data) {
    auto* msg = static_cast<OpenProgressMsg*>(user_data);
    auto& st = State();
    if (msg->token == st.open_token.load() && st.content != nullptr && st.opening.load()) {
        ShowOpenProgress(msg->percent);
    }
    delete msg;
}

void AsyncOpenDone(void* user_data) {
    auto* msg = static_cast<OpenDoneMsg*>(user_data);
    auto& st = State();
    st.opening.store(false);

    if (msg->token != st.open_token.load() || st.deferred_cleanup || st.content == nullptr) {
        FinishDeferredCleanup();
        delete msg;
        return;
    }

    if (!msg->ok || !st.session || !st.session->IsOpen()) {
        EndReadingTimeTracking();
        ReleaseBookFont();
        if (st.opening_format == reader::BookFormat::kTxt) {
            ShowMessage(st.content, Lang::Strings::BOOK_OPEN_FAILED_TXT);
        } else if (st.opening_format == reader::BookFormat::kEbook) {
            ShowMessage(st.content, Lang::Strings::BOOK_OPEN_FAILED_CHAPTER);
        } else {
            ShowMessage(st.content, Lang::Strings::BOOK_OPEN_FAILED_EPUB);
        }
        delete msg;
        return;
    }

    // 文档元数据回写书库，下次详情秒开可见 OPF 书名/作者
    if (st.selected >= 0 && st.selected < static_cast<int>(st.books.size()) &&
        st.books[static_cast<size_t>(st.selected)].path == st.session->Info().path) {
        auto& bi = st.books[static_cast<size_t>(st.selected)];
        if (!st.session->Title().empty()) {
            bi.title = st.session->Title();
        }
        if (!st.session->Author().empty()) {
            bi.author = st.session->Author();
        }
    }

    if (st.settings_resume_after_open) {
        st.settings_resume_after_open = false;
        st.read_chrome = BookUiState::ReadChrome::kReading;
        RenderReaderPage();
        ShowReadSettingsSheet();
        BeginReadingTimeTracking();
        if (st.session->NeedsChapterPageRebuild()) {
            StartChapterPageWorker();
        }
        delete msg;
        return;
    }

    RenderReaderPage();
    BeginReadingTimeTracking();
    if (st.session->NeedsChapterPageRebuild()) {
        StartChapterPageWorker();
    }
    delete msg;
}

void OnOpenProgress(int percent, void* user) {
    auto* msg = new OpenProgressMsg{};
    msg->token = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(user));
    msg->percent = percent;
    // OpenBookWorker 非 LVGL：须 ScreenLvAsync，裸 lv_async_call 会搞坏链表 → Cache/MMU
    if (!ScreenLvAsync(AsyncOpenProgress, msg)) {
        delete msg;
    }
}

void OpenBookWorker(void* arg) {
    auto* ctx = static_cast<OpenWorkerArg*>(arg);
    auto& st = State();
    bool ok = false;

    const uint32_t token_now = st.open_token.load();
    ESP_LOGI(TAG, "book_open worker start token=%u cur=%u fmt=%d int=%u spiram=%u path=%s",
             static_cast<unsigned>(ctx->token), static_cast<unsigned>(token_now),
             static_cast<int>(ctx->info.format),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             ctx->info.path.c_str());

    if (st.session == nullptr) {
        ESP_LOGE(TAG, "book_open skip: session null");
    } else if (ctx->token != token_now) {
        ESP_LOGW(TAG, "book_open skip: token stale %u!=%u", static_cast<unsigned>(ctx->token),
                 static_cast<unsigned>(token_now));
    } else {
        st.session->SetOpenProgress(OnOpenProgress,
                                    reinterpret_cast<void*>(static_cast<uintptr_t>(ctx->token)));
        ok = st.session->Open(ctx->info);
        st.session->SetOpenProgress(nullptr, nullptr);
        // 打开路径统一补旁路封面，供书架/详情/目录取用（失败不阻断阅读）
        if (ok && ctx->token == st.open_token.load() &&
            (ctx->info.format == reader::BookFormat::kEbook ||
             ctx->info.format == reader::BookFormat::kEpub)) {
            char sidecar[192];
            if (reader::BookCoverSidecarPath(ctx->info.path.c_str(), sidecar, sizeof(sidecar)) &&
                access(sidecar, R_OK) != 0) {
                reader::SaveBookCoverSidecar(ctx->info.path.c_str());
            }
        }
    }

    ESP_LOGI(TAG, "book_open worker done ok=%d int=%u spiram=%u", ok ? 1 : 0,
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

    auto* done = new OpenDoneMsg{};
    done->token = ctx->token;
    done->ok = ok;
    if (!ScreenLvAsync(AsyncOpenDone, done)) {
        delete done;
        st.opening.store(false);
        // 禁止在 worker 里直接 reset session；标记延后清理
        st.deferred_cleanup = true;
        ESP_LOGW(TAG, "book_open done: ScreenLvAsync fail, deferred_cleanup");
    }
    delete ctx;
}

void StartOpenWorker(int book_index) {
    auto& st = State();
    if (st.opening.load()) {
        ESP_LOGW(TAG, "StartOpenWorker ignored: already opening");
        return;
    }
    if (book_index < 0 || book_index >= static_cast<int>(st.books.size()) || !st.session) {
        ESP_LOGE(TAG, "StartOpenWorker bad state idx=%d books=%u session=%d", book_index,
                 static_cast<unsigned>(st.books.size()), st.session ? 1 : 0);
        ShowMessage(st.content, Lang::Strings::BOOK_OPEN_FAILED);
        return;
    }

    const uint32_t token = st.open_token.fetch_add(1) + 1;
    st.opening.store(true);
    st.deferred_cleanup = false;
    st.opening_format = st.books[static_cast<size_t>(book_index)].format;
    ShowOpenProgress(0);

    auto* ctx = new OpenWorkerArg{};
    ctx->token = token;
    ctx->info = st.books[static_cast<size_t>(book_index)];
    ESP_LOGI(TAG, "StartOpenWorker token=%u fmt=%d int=%u spiram=%u path=%s",
             static_cast<unsigned>(token), static_cast<int>(ctx->info.format),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
             ctx->info.path.c_str());
    if (!SpawnPsrWorker("book_open", kOpenWorkerStack, tskIDLE_PRIORITY + 2, OpenBookWorker, ctx,
                        nullptr, 0)) {
        const size_t free_int = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        const size_t free_spiram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
        ESP_LOGE(TAG, "book_open task failed: int free=%u spiram=%u stack=%u",
                 static_cast<unsigned>(free_int), static_cast<unsigned>(free_spiram),
                 static_cast<unsigned>(kOpenWorkerStack));
        delete ctx;
        st.opening.store(false);
        ShowMessage(st.content, Lang::Strings::BOOK_OPEN_FAILED_TASK);
    }
}

void StartReadAsync(void* user_data) {
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(user_data));
    auto& st = State();
    if (index < 0 || index >= static_cast<int>(st.books.size())) {
        return;
    }
    // 编排/打开 worker 仍握旧 session：勿 CreateReaderScreen 拆掉
    if (ReaderWorkersBusy()) {
        ESP_LOGW(TAG, "start read ignored: worker busy");
        return;
    }
    st.selected = index;
    CancelDetailCoverLoad();
    // 勿在此 Reset detail_cover：旧详情屏异步删除前仍引用封面缓冲
    ScreenLoadReplace(CreateReaderScreen(st.books[static_cast<size_t>(index)]));
}

void BackToLibraryAsync(void* /*user_data*/) {
    // 详情 vk_home / vk_prev → 书架或阅读首页。
    CancelDetailCoverLoad();
    // Create / CreateBookshelf 统一收尾正文 session / SD 字库（含 opening 中途离开的 deferred）。
    // detail_scr / cover 由详情屏 LV_EVENT_DELETE 清理，勿在此 Reset。
    if (State().back_root == BookUiState::NavRoot::kShelf) {
        ScreenLoadReplace(CreateBookshelfScreen());
    } else {
        ScreenLoadReplace(BookScreen::Create());
    }
}



void OpenBookshelfAsync(void* /*user_data*/) {
    State().list_page = 0;
    ScreenLoadReplace(CreateBookshelfScreen());
}

void BackToReadingHomeAsync(void* /*user_data*/) {
    ScreenLoadReplace(BookScreen::Create());
}

void OnContinueReadingClicked(lv_event_t* e) {
    auto& st = State();
    if (ReaderWorkersBusy()) {
        return;
    }
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (index < 0 || index >= static_cast<int>(st.books.size())) {
        return;
    }
    st.back_root = BookUiState::NavRoot::kHome;
    st.selected = index;
    lv_async_call(StartReadAsync, reinterpret_cast<void*>(static_cast<intptr_t>(index)));
}

void OnOpenShelfClicked(lv_event_t* /*e*/) {
    auto& st = State();
    if (st.opening.load()) {
        return;
    }
    lv_async_call(OpenBookshelfAsync, nullptr);
}



void SetReadStatusVisible(bool visible) {
    auto& st = State();
    if (st.status_bar != nullptr && lv_obj_is_valid(st.status_bar)) {
        if (visible) {
            // 盖在正文上时需白底，否则字迹透出
            lv_obj_set_style_bg_color(st.status_bar, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(st.status_bar, LV_OPA_COVER, 0);
            lv_obj_clear_flag(st.status_bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(st.status_bar);
        } else {
            lv_obj_add_flag(st.status_bar, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_opa(st.status_bar, LV_OPA_TRANSP, 0);
        }
    }
    if (st.status_overlay != nullptr && lv_obj_is_valid(st.status_overlay)) {
        // overlay 叠在图标行上，保持透明以免盖住电量/网络
        if (visible) {
            lv_obj_set_style_bg_opa(st.status_overlay, LV_OPA_TRANSP, 0);
            lv_obj_clear_flag(st.status_overlay, LV_OBJ_FLAG_HIDDEN);
            lv_obj_move_foreground(st.status_overlay);
        } else {
            lv_obj_add_flag(st.status_overlay, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/** @brief 状态栏中央（时钟位）：有文案则显示，nullptr/空则清除业务持续状态 */
void SetReadStatusCenterTitle(const char* title) {
    if (title != nullptr && title[0] != '\0') {
        StatusBarManager::GetInstance().SetOngoing("book", title);
    } else {
        StatusBarManager::GetInstance().ClearOngoing("book");
    }
}

void HideReadChrome() {
    auto& st = State();
    CloseReadSettingsSheet();
    SetReadStatusCenterTitle(nullptr);
    SetReadStatusVisible(false);
    const bool was_overlay = IsReadOverlayChrome(st.read_chrome);
    const bool was_settings = (st.read_chrome == BookUiState::ReadChrome::kSettings);
    st.settings_resume_after_open = false;
    st.read_chrome = BookUiState::ReadChrome::kReading;
    // 设置卡片盖在正文上：关掉即可，不必重绘；目录整页需恢复正文
    if (was_overlay && !was_settings && st.session && st.session->IsOpen() &&
        st.content != nullptr) {
        CancelListCoverFill();  // 目录封面槽将随 content 重建销毁
        ApplyReaderPageGeometry(ReaderGeomMode::kImmersive);
        RenderReaderPage();
    } else if (was_settings) {
        // 关卡：落盘未完成的 ± 排版；StartChapterPageWorker 自检 Needs（卡上曾推迟）
        FlushLayoutDebounce();
        ApplyReaderPageGeometry(ReaderGeomMode::kImmersive);
        StartChapterPageWorker();
    }
}


void ApplyReaderLayoutLive(bool reload_font) {
    auto& st = State();
    if (st.opening.load() || !st.session || !st.session->IsOpen()) {
        return;
    }
    if (st.viewport_w <= 0 || st.viewport_h <= 0) {
        return;
    }
    const bool layout_running = st.layout_busy.load();
    // 换字库与 worker 抢 font_：只记 again；边距/行距仍即时刷预览
    if (layout_running && reload_font) {
        st.layout_again = true;
        st.layout_reload_font = true;
        return;
    }
    if (layout_running) {
        st.layout_again = true;
    }
    // 章节页表后台正用 font_ 计宽：禁止 ReleaseBookFont，否则 InstrFetchProhibited
    if (reload_font && st.chapter_pages_busy.load()) {
        st.layout_again = true;
        st.layout_reload_font = true;
        st.chapter_pages_again = true;
        st.session->InvalidateChapterPageTable();
        return;
    }

    ClearReaderPageDelta();
    if (reload_font) {
        ReleaseBookFont();
        EnsureBookFont();
    }
    BindSessionLayoutPrefs(*st.session);
    ApplyReaderPageGeometry(ReaderGeomMode::kImmersive);
    st.session->SetViewport(st.viewport_w, st.viewport_h);

    reader::BookSession::LayoutAnchor anchor = st.session->CaptureLayoutAnchor();
    if (!st.session->BeginLiveRelayout(&anchor)) {
        ESP_LOGW(TAG, "live relayout failed, fallback reopen");
        st.settings_resume_after_open = (st.read_chrome == BookUiState::ReadChrome::kSettings);
        ReopenReaderAfterPrefsChange();
        return;
    }

    const bool keep_sheet = (st.read_chrome == BookUiState::ReadChrome::kSettings);
    RenderReaderPage();
    if (keep_sheet) {
        EnsureSettingsSheetAfterLayout();
    } else {
        UpdateReadFooter();
    }

    if (st.session->NeedsTxtIndexRebuild() && st.session->LiveRelayoutPending()) {
        st.layout_anchor = anchor;
        if (layout_running) {
            // worker 结束后用最新锚点/参数再全量扫
            st.layout_again = true;
        } else {
            st.layout_again = false;
            st.layout_reload_font = false;
            StartLayoutWorker();
        }
    } else if (st.session->NeedsChapterPageRebuild()) {
        StartChapterPageWorker();
    }
}

void CancelLayoutDebounce() {
    auto& st = State();
    if (st.layout_debounce_timer != nullptr) {
        lv_timer_del(st.layout_debounce_timer);
        st.layout_debounce_timer = nullptr;
    }
    st.layout_debounce_reload_font = false;
}

bool IsLayoutHintBusy() {
    auto& st = State();
    if (st.chapter_pages_busy.load() || st.layout_busy.load()) {
        return true;
    }
    return st.session && st.session->IsOpen() && st.session->LiveRelayoutPending();
}

void StopLayoutHintTimer() {
    auto& st = State();
    if (st.layout_hint_timer != nullptr) {
        lv_timer_del(st.layout_hint_timer);
        st.layout_hint_timer = nullptr;
    }
}

void OnLayoutHintTick(lv_timer_t* /*t*/) {
    auto& st = State();
    if (st.page_label == nullptr || !lv_obj_is_valid(st.page_label)) {
        StopLayoutHintTimer();
        return;
    }
    if (IsLayoutHintBusy()) {
        UpdateReadFooter();
        return;
    }
    if (esp_timer_get_time() < st.layout_hint_done_until_us) {
        UpdateReadFooter();
        return;
    }
    st.layout_hint_done_until_us = 0;
    StopLayoutHintTimer();
    UpdateReadFooter();
}

void EnsureLayoutHintTimer() {
    auto& st = State();
    if (st.layout_hint_timer != nullptr) {
        return;
    }
    st.layout_hint_timer = lv_timer_create(OnLayoutHintTick, kLayoutHintTickMs, nullptr);
}

void MarkLayoutHintBusy() {
    auto& st = State();
    st.layout_hint_done_until_us = 0;
    StopLayoutHintTimer();
    UpdateReadFooter();
}

void MarkLayoutHintDone() {
    auto& st = State();
    st.layout_hint_done_until_us = esp_timer_get_time() + kLayoutHintDoneUs;
    EnsureLayoutHintTimer();
    UpdateReadFooter();
}

void OnLayoutDebounceTimer(lv_timer_t* t) {
    auto& st = State();
    if (st.layout_debounce_timer == t) {
        st.layout_debounce_timer = nullptr;
    }
    // 与 ScreenPaintCoalesce 同形：手指未抬 / 边沿未交完则再等一拍，避免连点中途开排版
    if (TouchUiFingerIsDown() || TouchUiHasPendingEdges()) {
        st.layout_debounce_timer =
            lv_timer_create(OnLayoutDebounceTimer, kLayoutDebounceMs, nullptr);
        if (st.layout_debounce_timer != nullptr) {
            lv_timer_set_repeat_count(st.layout_debounce_timer, 1);
        }
        return;
    }
    const bool reload = st.layout_debounce_reload_font;
    st.layout_debounce_reload_font = false;
    ApplyReaderLayoutLive(reload);
}

void ScheduleLayoutApply(bool reload_font) {
    auto& st = State();
    if (st.opening.load() || !st.session || !st.session->IsOpen()) {
        return;
    }
    // prefs/卡文案已由调用方写好；抬起且停顿后再排版（合并为最后一档）
    st.layout_debounce_reload_font = st.layout_debounce_reload_font || reload_font;
    if (st.layout_debounce_timer != nullptr) {
        lv_timer_reset(st.layout_debounce_timer);
        return;
    }
    st.layout_debounce_timer = lv_timer_create(OnLayoutDebounceTimer, kLayoutDebounceMs, nullptr);
    if (st.layout_debounce_timer != nullptr) {
        lv_timer_set_repeat_count(st.layout_debounce_timer, 1);
    }
}

void FlushLayoutDebounce() {
    auto& st = State();
    if (st.layout_debounce_timer == nullptr) {
        return;
    }
    const bool reload = st.layout_debounce_reload_font;
    CancelLayoutDebounce();
    ApplyReaderLayoutLive(reload);
}

struct LayoutDoneMsg {
    uint32_t token = 0;
    bool ok = false;
};

struct LayoutWorkerArg {
    uint32_t token = 0;
    reader::BookSession::LayoutAnchor anchor{};
};

void AsyncLayoutDone(void* user_data) {
    auto* msg = static_cast<LayoutDoneMsg*>(user_data);
    auto& st = State();
    const uint32_t token = msg->token;
    const bool ok = msg->ok;
    delete msg;

    const bool mine = (token == st.layout_token.load());
    // 无论 token 是否作废，本 worker 已结束，必须清 busy（离开时不再提前清）
    st.layout_busy.store(false);
    // 离开正文时 layout worker 仍在跑：结束后再关 session
    if (st.deferred_cleanup && !st.opening.load() && !st.layout_busy.load() &&
        !st.chapter_pages_busy.load() && !st.page_image_busy.load()) {
        FinishDeferredCleanup();
        return;
    }
    if (!mine || st.content == nullptr || !st.session) {
        return;
    }
    if (!ok) {
        ESP_LOGW(TAG, "layout worker failed");
        if (st.session &&
            (st.session->LiveRelayoutPending() || st.session->NeedsTxtIndexRebuild())) {
            st.session->ClearLivePreview();
            st.settings_resume_after_open = (st.read_chrome == BookUiState::ReadChrome::kSettings);
            ReopenReaderAfterPrefsChange();
        } else if (st.session) {
            UpdateReadFooter();
        }
        return;
    }

    const bool keep_sheet = (st.read_chrome == BookUiState::ReadChrome::kSettings);
    RenderReaderPage();
    if (keep_sheet) {
        EnsureSettingsSheetAfterLayout();
    } else {
        UpdateReadFooter();
    }

    if (st.layout_again) {
        const bool reload = st.layout_reload_font;
        st.layout_again = false;
        st.layout_reload_font = false;
        ApplyReaderLayoutLive(reload);
        return;
    }
    if (!IsLayoutHintBusy()) {
        MarkLayoutHintDone();
    }
}

void LayoutWorker(void* arg) {
    auto* ctx = static_cast<LayoutWorkerArg*>(arg);
    auto& st = State();
    bool ok = false;
    if (st.session && ctx->token == st.layout_token.load()) {
        ok = st.session->FinishTxtRelayout(ctx->anchor);
    }
    auto* done = new LayoutDoneMsg{};
    done->token = ctx->token;
    done->ok = ok;
    if (!ScreenLvAsync(AsyncLayoutDone, done)) {
        delete done;
        st.layout_busy.store(false);
    }
    delete ctx;
}

struct ChapterPagesDoneMsg {
    uint32_t token = 0;
    bool ok = false;
};

void AsyncChapterPagesDone(void* user_data) {
    auto* msg = static_cast<ChapterPagesDoneMsg*>(user_data);
    auto& st = State();
    const uint32_t token = msg->token;
    delete msg;

    const bool mine = (token == st.chapter_pages_token.load());
    st.chapter_pages_busy.store(false);

    if (st.deferred_cleanup && !st.opening.load() && !st.layout_busy.load() &&
        !st.chapter_pages_busy.load() && !st.page_image_busy.load()) {
        FinishDeferredCleanup();
        return;
    }
    if (!mine || st.content == nullptr || !st.session) {
        return;
    }
    if (st.read_chrome == BookUiState::ReadChrome::kSettings) {
        EnsureSettingsSheetAfterLayout();
    } else {
        UpdateReadFooter();
    }
    // 换字号曾因页表 worker 占用 font 而 defer：worker 结束后再落地
    if (st.layout_again) {
        const bool reload = st.layout_reload_font;
        st.layout_again = false;
        st.layout_reload_font = false;
        st.chapter_pages_again = false;
        ApplyReaderLayoutLive(reload);
        return;
    }
    if (st.chapter_pages_again || st.session->NeedsChapterPageRebuild()) {
        st.chapter_pages_again = false;
        StartChapterPageWorker(); // 设置卡内会直接 return，关卡后再建
        return;
    }
    MarkLayoutHintDone();
}

void ChapterPageWorker(void* arg) {
    auto* ctx = static_cast<uint32_t*>(arg);
    const uint32_t token = *ctx;
    delete ctx;
    auto& st = State();
    bool ok = false;
    if (st.session && token == st.chapter_pages_token.load()) {
        ok = st.session->FinishChapterPageTable();
    }
    auto* done = new ChapterPagesDoneMsg{};
    done->token = token;
    done->ok = ok;
    if (!ScreenLvAsync(AsyncChapterPagesDone, done)) {
        delete done;
        st.chapter_pages_busy.store(false);
    }
}

void StartChapterPageWorker() {
    auto& st = State();
    if (!st.session || !st.session->NeedsChapterPageRebuild()) {
        return;
    }
    // 设置浮层只预览当前章；Needs 仍真，关卡 HideReadChrome 再调本函数
    if (st.read_chrome == BookUiState::ReadChrome::kSettings) {
        return;
    }
    if (st.chapter_pages_busy.load()) {
        st.chapter_pages_again = true;
        return;
    }
    const uint32_t token = st.chapter_pages_token.fetch_add(1) + 1;
    st.chapter_pages_busy.store(true);
    st.chapter_pages_again = false;
    MarkLayoutHintBusy();
    auto* ctx = new uint32_t(token);
    if (!SpawnPsrWorker("book_cpages", kOpenWorkerStack, tskIDLE_PRIORITY + 1, ChapterPageWorker, ctx,
                        nullptr, 0)) {
        ESP_LOGE(TAG, "book_cpages task failed");
        delete ctx;
        st.chapter_pages_busy.store(false);
        MarkLayoutHintDone();
    }
}

void StartLayoutWorker() {
    auto& st = State();
    if (st.layout_busy.load() || !st.session) {
        return;
    }
    const uint32_t token = st.layout_token.fetch_add(1) + 1;
    st.layout_busy.store(true);
    MarkLayoutHintBusy();
    auto* ctx = new LayoutWorkerArg{};
    ctx->token = token;
    ctx->anchor = st.layout_anchor;
    if (!SpawnPsrWorker("book_layout", kOpenWorkerStack, tskIDLE_PRIORITY + 1, LayoutWorker, ctx,
                        nullptr, 0)) {
        ESP_LOGE(TAG, "book_layout task failed");
        delete ctx;
        st.layout_busy.store(false);
        st.session->ClearLivePreview();
        st.settings_resume_after_open = (st.read_chrome == BookUiState::ReadChrome::kSettings);
        ReopenReaderAfterPrefsChange();
    }
}

void ReopenReaderAfterPrefsChange() {
    auto& st = State();
    if (st.opening.load()) {
        return;
    }
    if (st.selected < 0 || st.selected >= static_cast<int>(st.books.size())) {
        return;
    }
    st.layout_token.fetch_add(1);
    st.layout_busy.store(false);
    st.layout_again = false;
    st.layout_reload_font = false;
    st.read_chrome = BookUiState::ReadChrome::kReading;
    SetReadStatusVisible(false);
    SetReadSettingsSheetVisible(false);
    ApplyReaderPageGeometry(ReaderGeomMode::kImmersive);
    if (st.viewport_w <= 0 || st.viewport_h <= 0) {
        ESP_LOGE(TAG, "reopen prefs: invalid viewport %d×%d", static_cast<int>(st.viewport_w),
                 static_cast<int>(st.viewport_h));
        st.settings_resume_after_open = false;
        return;
    }
    const lv_coord_t vw = st.viewport_w;
    const lv_coord_t vh = st.viewport_h;

    ClearReaderPageDelta();
    EndReadingTimeTracking();
    st.session.reset();
    ReleaseBookFont();
    EnsureBookFont();
    st.session = std::make_unique<reader::BookSession>();
    BindSessionLayoutPrefs(*st.session);
    st.session->SetViewport(vw, vh);
    StartOpenWorker(st.selected);
}

struct SettingsSheetHooksInit {
    SettingsSheetHooksInit() {
        SetSettingsScheduleLayoutApplyHandler(&ScheduleLayoutApply);
        SetSettingsApplyReaderLayoutLiveHandler(&ApplyReaderLayoutLive);
        SetSettingsFlushLayoutDebounceHandler(&FlushLayoutDebounce);
        SetSettingsRenderReaderPageHandler(&RenderReaderPage);
        SetSettingsSetStatusVisibleHandler(&SetReadStatusVisible);
        SetSettingsBindSessionLayoutPrefsHandler([](reader::BookSession& s) { BindSessionLayoutPrefs(s); });
    }
};
static SettingsSheetHooksInit s_settings_sheet_hooks_init;

struct DetailScreenHooksInit {
    DetailScreenHooksInit() {
        SetDetailStartReadHandler([](int book_index) {
            if (ReaderWorkersBusy()) {
                return;
            }
            lv_async_call(StartReadAsync, reinterpret_cast<void*>(static_cast<intptr_t>(book_index)));
        });
        SetDetailCreateHomeScrHandler(&BookScreen::Create);
        SetDetailAttachVkHandler([](lv_obj_t* scr) {
            VkKey_AttachScreen(scr, kScreenDetail, BookAiLongPressDesc(ResumeDetailScreen));
        });
        SetDetailReleaseSessionHandler(&ReleaseReaderSessionForLeave);
    }
};
static DetailScreenHooksInit s_detail_screen_hooks_init;









lv_obj_t* CreateReaderScreen(const reader::BookInfo& info) {
    auto& st = State();
    // 硬护栏：layout/open worker 仍用旧 session 时禁止 make_unique 拆掉（UAF）
    if (ReaderWorkersBusy()) {
        ESP_LOGW(TAG, "CreateReaderScreen blocked: worker busy");
        if (st.selected >= 0 && st.selected < static_cast<int>(st.books.size())) {
            return CreateDetailScreen(st.selected);
        }
        return BookScreen::Create();
    }
    if (st.deferred_cleanup) {
        FinishDeferredCleanup();
    }
    // 仅打开正文时加载 SD 字体；界面控件用固件字
    EnsureBookFont();
    // 预扫 SD 字库列表，长按设置卡时不必再等「加载中」
    ScanReadFonts();
    EnsureFontListPageShowsSelection();
    // 换新 session 前停旧 timer/墙钟，避免回调 UAF 或漏写本段秒数
    EndReadingTimeTracking();
    st.session = std::make_unique<reader::BookSession>();
    BindSessionLayoutPrefs(*st.session);
    st.opening_format = info.format;

    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, ListFont(), 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    DisableScroll(scr);

    EpdStatusBar status = ScreenCreateStatusBar(scr);
    st.status_bar = status.bar;
    st.status_overlay = status.overlay;
    st.status_h = status.height;
    lv_obj_add_event_cb(scr, [](lv_event_t*) {
        StatusBarManager::GetInstance().ClearOngoing("book");
    }, LV_EVENT_DELETE, nullptr);
    // 沉浸阅读：默认藏顶栏，正文占满状态栏区域
    SetReadStatusVisible(false);

    st.title_label = nullptr;

    lv_obj_t* body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, LV_VER_RES - kFooterH);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    DisableScroll(body);
    ResetReadTouch();
    AttachReadBodyInput(body);
    st.content = body;
    st.read_chrome = BookUiState::ReadChrome::kReading;
    st.toc_list_page = 0;

    // 顶部悬浮设置卡片（默认隐藏；页内长按唤出，盖在正文上）
    st.settings_sheet = CreateSettingsSheet(scr);

    lv_obj_t* footer = lv_label_create(scr);
    lv_obj_set_width(footer, LV_HOR_RES - 16);
    lv_obj_set_style_text_align(footer, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(footer, ListFont(), 0);
    lv_obj_set_style_text_color(footer, lv_color_black(), 0);
    lv_label_set_long_mode(footer, LV_LABEL_LONG_CLIP);
    lv_label_set_text(footer, "");
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, 0);
    DisableScroll(footer);
    st.page_label = footer;

    // 先展示加载页，再后台解析；避免点「开始阅读」后长时间无响应
    ShowOpenProgress(0);
    st.read_scr = scr;
    ApplyReaderPageGeometry(ReaderGeomMode::kImmersive);
    st.session->SetViewport(st.viewport_w, st.viewport_h);
    ScreenSetIsHome(false);
    // factory=ResumeReader：进百问压栈后返回依 .pos 续读；短按 HOME 回详情
    VkKey_AttachScreen(scr, kScreenRead, BookAiLongPressDesc(ResumeReaderScreen));
    StartOpenWorker(st.selected);
    return scr;
}




/** 封面槽：同步旁路 .a2i1 或格式占位；无旁路时由调用方排队后台抽内嵌 */
lv_obj_t* CreateCoverSlot(lv_obj_t* parent, const reader::BookInfo& info, lv_coord_t w, lv_coord_t h,
                          reader::RasterImage* cover_out, bool* need_embed_fill) {
    if (need_embed_fill != nullptr) {
        *need_embed_fill = false;
    }
    lv_obj_t* host = lv_obj_create(parent);
    lv_obj_remove_style_all(host);
    lv_obj_set_size(host, w, h);
    lv_obj_set_style_pad_all(host, kCoverFrameBorder, 0);
    lv_obj_set_style_bg_color(host, lv_color_hex(0xF0F0F0), 0);
    lv_obj_set_style_bg_opa(host, LV_OPA_COVER, 0);
    StyleBookCoverFrame(host);
    lv_obj_clear_flag(host, LV_OBJ_FLAG_CLICKABLE);
    DisableScroll(host);

    const int inner_w = CoverFrameInner(w);
    const int inner_h = CoverFrameInner(h);
    if (cover_out != nullptr &&
        book_ui::TryLoadBookDetailSidecar(info, inner_w, inner_h, *cover_out) &&
        !cover_out->empty()) {
        cover_out->BindDsc();
        lv_obj_t* img = lv_image_create(host);
        lv_image_set_src(img, &cover_out->dsc);
        lv_obj_center(img);
        lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
    } else {
        if (cover_out != nullptr) {
            cover_out->Reset();
        }
        lv_obj_t* ph_lbl = lv_label_create(host);
        lv_label_set_text(ph_lbl, reader::FormatLabel(info.format));
        lv_obj_set_style_text_font(ph_lbl, ListFont(), 0);
        lv_obj_set_style_text_color(ph_lbl, lv_color_black(), 0);
        lv_obj_center(ph_lbl);
        lv_obj_clear_flag(ph_lbl, LV_OBJ_FLAG_CLICKABLE);
        if (need_embed_fill != nullptr &&
            (info.format == reader::BookFormat::kEpub ||
             info.format == reader::BookFormat::kEbook)) {
            *need_embed_fill = true;
        }
    }
    return host;
}


void BookshelfPrepareEnterFromHost() {
    auto& st = State();
    const bool workers_busy = st.opening.load() || st.layout_busy.load() ||
                              st.chapter_pages_busy.load() || st.page_image_busy.load();
    if (workers_busy) {
        st.open_token.fetch_add(1);
        st.layout_token.fetch_add(1);
        st.chapter_pages_token.fetch_add(1);
        CancelPageImageLoad();
        if (st.session) {
            st.session->InvalidateChapterPageTable();
            st.session->AbortTxtPaginate();
        }
        StopReadTimeCheckpointTimer();
        st.deferred_cleanup = true;
    } else if (st.deferred_cleanup) {
        FinishDeferredCleanup();
    } else {
        EndReadingTimeTracking();
        st.session.reset();
        ReleaseBookFont();
    }

    st.read_scr = nullptr;
    st.content = nullptr;
    st.page_label = nullptr;
    st.title_label = nullptr;
    st.status_bar = nullptr;
    st.status_overlay = nullptr;
    st.status_h = 0;
    st.settings_sheet = nullptr;
    ClearSettingsSheetWidgetRefs();
    StopTtfPollTimer();
    ResetReadTouch();
    CancelLayoutDebounce();
    StopLayoutHintTimer();
}

void BookshelfOpenDetailFromHost(int index) {
    lv_async_call(OpenDetailAsync, reinterpret_cast<void*>(static_cast<intptr_t>(index)));
}

std::string BookshelfTruncateFromHost(const std::string& text, const lv_font_t* font,
                                      lv_coord_t max_w) {
    return TruncateTextToWidth(text, font, max_w);
}

void BookshelfAttachVkFromHost(lv_obj_t* scr) {
    VkKey_AttachScreen(scr, kScreenBookshelf, BookAiLongPressDesc(CreateBookshelfScreen));
}

struct BookshelfHooksInit {
    BookshelfHooksInit() {
        SetBookshelfPrepareEnterHandler(&BookshelfPrepareEnterFromHost);
        SetBookshelfCancelListCoverHandler(&CancelListCoverFill);
        SetBookshelfScheduleListCoverHandler(&ScheduleListCoverFill);
        SetBookshelfEnqueueListCoverHandler(&EnqueueListCoverFill);
        SetBookshelfCreateCoverSlotHandler(&CreateCoverSlot);
        SetBookshelfOpenDetailHandler(&BookshelfOpenDetailFromHost);
        SetBookshelfTruncateHandler(&BookshelfTruncateFromHost);
        SetBookshelfAttachVkHandler(&BookshelfAttachVkFromHost);
    }
};
static BookshelfHooksInit s_bookshelf_hooks_init;

void TocFlushLayoutFromHost() {
    FlushLayoutDebounce();
}

void TocPrepareEnterFromHost() {
    ClearReaderPageDelta();
    SetReadSettingsSheetVisible(false);
    ApplyReaderPageGeometry(ReaderGeomMode::kOverlayList);
}

struct TocHooksInit {
    TocHooksInit() {
        SetTocFlushLayoutHandler(&TocFlushLayoutFromHost);
        SetTocPrepareEnterHandler(&TocPrepareEnterFromHost);
        SetTocCancelListCoverHandler(&CancelListCoverFill);
        SetTocScheduleListCoverHandler(&ScheduleListCoverFill);
        SetTocEnqueueListCoverHandler(&EnqueueListCoverFill);
        SetTocSetStatusVisibleHandler(&SetReadStatusVisible);
        SetTocSetStatusTitleHandler(&SetReadStatusCenterTitle);
        SetTocApplyOverlayPadsHandler(&ApplyOverlayListContentPads);
        SetTocDetachBodyInputHandler(&DetachReadBodyInput);
        SetTocTruncateHandler(&BookshelfTruncateFromHost);
        SetTocUpdateFooterHandler(&UpdateReadFooter);
        SetTocResetLongPressHandler(&ResetPointerLongPress);
        SetTocRenderReaderPageHandler(&RenderReaderPage);
    }
};
static TocHooksInit s_toc_hooks_init;


void ReaderApplyImmersiveGeometryFromHost() {
    ApplyReaderPageGeometry(ReaderGeomMode::kImmersive);
}

void ReaderOnPageImageIdleFromHost() {
    auto& st = State();
    if (st.deferred_cleanup && !ReaderWorkersBusy()) {
        FinishDeferredCleanup();
    }
}

struct ReaderHooksInit {
    ReaderHooksInit() {
        SetReaderHideChromeHandler(&HideReadChrome);
        SetReaderShowSettingsSheetHandler(&ShowReadSettingsSheet);
        SetReaderSetStatusVisibleHandler(&SetReadStatusVisible);
        SetReaderSetSettingsSheetVisibleHandler(&SetReadSettingsSheetVisible);
        SetReaderApplyImmersiveGeometryHandler(&ReaderApplyImmersiveGeometryFromHost);
        SetReaderApplyLayoutLiveHandler(&ApplyReaderLayoutLive);
        SetReaderIsLayoutHintBusyHandler(&IsLayoutHintBusy);
        SetReaderOnPageImageIdleHandler(&ReaderOnPageImageIdleFromHost);
        SetReaderTruncateHandler(&BookshelfTruncateFromHost);
    }
};
static ReaderHooksInit s_reader_hooks_init;



void DrawHomeBarChart(lv_obj_t* parent, lv_coord_t area_w, lv_coord_t area_h, int finished,
                      int book_count) {
    lv_obj_t* chart = lv_obj_create(parent);
    lv_obj_remove_style_all(chart);
    lv_obj_set_size(chart, area_w, area_h);
    lv_obj_set_style_bg_opa(chart, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(chart, LV_OBJ_FLAG_CLICKABLE);
    DisableScroll(chart);

    constexpr int kBars = 6;
    constexpr lv_coord_t kBarW = 12; // 竖条加细
    const lv_coord_t bar_w = kBarW;
    const int seed = std::max(1, book_count) + finished * 3;
    const lv_coord_t heights[kBars] = {
        static_cast<lv_coord_t>(area_h * (35 + (seed * 11) % 45) / 100),
        static_cast<lv_coord_t>(area_h * (50 + (seed * 7) % 40) / 100),
        static_cast<lv_coord_t>(area_h * (70 + (seed * 3) % 25) / 100),
        static_cast<lv_coord_t>(area_h * (45 + (seed * 13) % 40) / 100),
        static_cast<lv_coord_t>(area_h * (55 + (seed * 5) % 35) / 100),
        static_cast<lv_coord_t>(area_h * (40 + (seed * 17) % 40) / 100),
    };
    const lv_coord_t stride = (area_w - bar_w * kBars) / (kBars - 1);
    for (int i = 0; i < kBars; ++i) {
        lv_obj_t* bar = lv_obj_create(chart);
        lv_obj_remove_style_all(bar);
        const lv_coord_t bh = std::max<lv_coord_t>(bar_w + 4, heights[i]);
        lv_obj_set_size(bar, bar_w, bh);
        lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar, bar_w / 2, 0); // 顶底圆弧
        lv_obj_set_pos(bar, i * (bar_w + stride), area_h - bh);
        lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        DisableScroll(bar);
    }
}

void RenderReadingHome() {
    auto& st = State();
    if (st.list_body == nullptr) {
        return;
    }
    CancelListCoverFill();
    lv_obj_set_style_layout(st.list_body, LV_LAYOUT_FLEX, 0);
    lv_obj_set_flex_flow(st.list_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(st.list_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(st.list_body, 0, 0);

    // 先拆控件再释放封面，避免 lv_image 引用失效
    lv_obj_clean(st.list_body);
    st.recent_covers.clear();
    DisableScroll(st.list_body);

    if (st.list_footer != nullptr) {
        lv_label_set_text(st.list_footer, "");
        lv_obj_add_flag(st.list_footer, LV_OBJ_FLAG_HIDDEN);
    }

    if (st.books.empty()) {
        char tip[160];
        std::snprintf(tip, sizeof(tip), Lang::Strings::BOOK_EMPTY_HOME_FMT,
                      SdUserPath(reader::kDefaultBooksDir));
        ShowMessage(st.list_body, tip, ListFont());
        return;
    }

    const HomeStats stats = BookStatsService::CollectHomeStats(st.books);
    const lv_coord_t content_w = ContentWidth();

    const lv_font_t* item_f = ItemFont();
    const lv_font_t* list_f = ListFont();
    const lv_font_t* num_font = fontpack_lv_font_get(30, 4);
    if (num_font == nullptr) {
        num_font = list_f;
    }
    const lv_coord_t lh =
        (list_f != nullptr && list_f->line_height > 0) ? list_f->line_height : 30;
    const lv_coord_t ih =
        (item_f != nullptr && item_f->line_height > 0) ? item_f->line_height : 25;
    const lv_coord_t nh =
        (num_font != nullptr && num_font->line_height > 0) ? num_font->line_height : lh;

    // 继续阅读黑卡略抬；余量留给最近阅读封面下单行书名
    constexpr lv_coord_t kContPad = 20;
    constexpr lv_coord_t kContBtnH = 52;
    constexpr lv_coord_t kBarH = 16;
    constexpr lv_coord_t kThumb = 22;
    const lv_coord_t dash_h =
        kContPad * 2 + lh + 16 + std::max(kBarH, kThumb) + 16 + kContBtnH + 36;

    lv_obj_t* dash = lv_obj_create(st.list_body);
    lv_obj_remove_style_all(dash);
    lv_obj_set_size(dash, content_w, dash_h);
    lv_obj_set_style_bg_opa(dash, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(dash, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(dash, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(dash, kHomeGap, 0);
    lv_obj_clear_flag(dash, LV_OBJ_FLAG_CLICKABLE);
    DisableScroll(dash);

    // 继续阅读 / 阅读数据 对半
    const lv_coord_t cont_w = (content_w - kHomeGap) / 2;
    const lv_coord_t stats_w = content_w - kHomeGap - cont_w;

    lv_obj_t* cont = lv_obj_create(dash);
    lv_obj_remove_style_all(cont);
    lv_obj_set_size(cont, cont_w, dash_h);
    lv_obj_set_style_bg_color(cont, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(cont, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cont, kHomeCardRadius, 0);
    lv_obj_set_style_pad_all(cont, kContPad, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(cont, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(cont, LV_OBJ_FLAG_CLICKABLE);
    DisableScroll(cont);

    char prog_txt[32];
    int bar_pct = 0;
    if (stats.continue_progress_x10 >= 0) {
        std::snprintf(prog_txt, sizeof(prog_txt), Lang::Strings::BOOK_READ_PCT_FMT,
                      stats.continue_progress_x10 / 10, stats.continue_progress_x10 % 10);
        bar_pct = stats.continue_progress_x10 / 10;
    } else if (stats.continue_index >= 0) {
        std::snprintf(prog_txt, sizeof(prog_txt), "%s", Lang::Strings::BOOK_START);
    } else {
        std::snprintf(prog_txt, sizeof(prog_txt), "%s", Lang::Strings::BOOK_NO_CONTINUE);
    }

    // 百分比 + 进度条成组贴顶，避免 SPACE_BETWEEN 把条甩到中间
    lv_obj_t* top_block = lv_obj_create(cont);
    lv_obj_remove_style_all(top_block);
    lv_obj_set_width(top_block, cont_w - kContPad * 2);
    lv_obj_set_height(top_block, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top_block, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(top_block, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(top_block, 8, 0);
    lv_obj_clear_flag(top_block, LV_OBJ_FLAG_CLICKABLE);
    DisableScroll(top_block);

    lv_obj_t* cont_sub = lv_label_create(top_block);
    lv_label_set_text(cont_sub, prog_txt);
    lv_obj_set_style_text_font(cont_sub, list_f, 0);
    lv_obj_set_style_text_color(cont_sub, lv_color_white(), 0);
    lv_obj_set_width(cont_sub, cont_w - kContPad * 2);
    lv_obj_clear_flag(cont_sub, LV_OBJ_FLAG_CLICKABLE);

    const lv_coord_t bar_w = cont_w - kContPad * 2;
    lv_obj_t* bar_wrap = lv_obj_create(top_block);
    lv_obj_remove_style_all(bar_wrap);
    lv_obj_set_size(bar_wrap, bar_w, std::max(kBarH, kThumb));
    lv_obj_set_style_bg_opa(bar_wrap, LV_OPA_TRANSP, 0);
    lv_obj_set_style_layout(bar_wrap, LV_LAYOUT_NONE, 0);
    lv_obj_clear_flag(bar_wrap, LV_OBJ_FLAG_CLICKABLE);
    DisableScroll(bar_wrap);

    lv_obj_t* bar_track = lv_obj_create(bar_wrap);
    lv_obj_remove_style_all(bar_track);
    lv_obj_set_size(bar_track, bar_w, kBarH);
    lv_obj_align(bar_track, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(bar_track, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(bar_track, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(bar_track, kBarH / 2, 0);
    lv_obj_set_style_clip_corner(bar_track, true, 0);
    lv_obj_clear_flag(bar_track, LV_OBJ_FLAG_CLICKABLE);
    DisableScroll(bar_track);

    // 白底轨道 + 黑色已读；内缩留白缝，避免黑填与卡面黑底糊边
    constexpr lv_coord_t kBarInset = 2;
    if (bar_pct > 0) {
        const lv_coord_t inner_h = kBarH - 2 * kBarInset;
        lv_coord_t fg_w =
            static_cast<lv_coord_t>((bar_w - 2 * kBarInset) * bar_pct / 100);
        if (fg_w < inner_h) {
            fg_w = inner_h;
        }
        if (fg_w > bar_w - 2 * kBarInset) {
            fg_w = bar_w - 2 * kBarInset;
        }
        lv_obj_t* bar_fg = lv_obj_create(bar_track);
        lv_obj_remove_style_all(bar_fg);
        lv_obj_set_size(bar_fg, fg_w, inner_h);
        lv_obj_set_pos(bar_fg, kBarInset, kBarInset);
        lv_obj_set_style_bg_color(bar_fg, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(bar_fg, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar_fg, inner_h / 2, 0);
        lv_obj_clear_flag(bar_fg, LV_OBJ_FLAG_CLICKABLE);
        DisableScroll(bar_fg);
    }

    lv_obj_t* thumb = lv_obj_create(bar_wrap);
    lv_obj_remove_style_all(thumb);
    lv_obj_set_size(thumb, kThumb, kThumb);
    lv_obj_set_style_bg_color(thumb, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(thumb, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(thumb, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_border_width(thumb, 2, 0);
    lv_obj_set_style_border_color(thumb, lv_color_black(), 0);
    lv_coord_t thumb_x = 0;
    if (bar_pct > 0) {
        thumb_x = static_cast<lv_coord_t>((bar_w - kThumb) * bar_pct / 100);
    }
    lv_obj_set_pos(thumb, thumb_x, (std::max(kBarH, kThumb) - kThumb) / 2);
    lv_obj_clear_flag(thumb, LV_OBJ_FLAG_CLICKABLE);
    DisableScroll(thumb);

    lv_obj_t* cont_btn = lv_obj_create(cont);
    lv_obj_remove_style_all(cont_btn);
    lv_obj_set_size(cont_btn, bar_w, kContBtnH);
    lv_obj_set_style_bg_color(cont_btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(cont_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(cont_btn, 8, 0);
    DisableScroll(cont_btn);
    if (stats.continue_index >= 0) {
        lv_obj_add_flag(cont_btn, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(cont_btn);
        lv_obj_add_event_cb(cont_btn, OnContinueReadingClicked, LV_EVENT_CLICKED,
                            reinterpret_cast<void*>(static_cast<intptr_t>(stats.continue_index)));
    } else {
        lv_obj_clear_flag(cont_btn, LV_OBJ_FLAG_CLICKABLE);
    }
    lv_obj_t* cont_btn_lbl = lv_label_create(cont_btn);
    lv_label_set_text(cont_btn_lbl, Lang::Strings::BOOK_CONTINUE);
    lv_obj_set_style_text_font(cont_btn_lbl, item_f, 0);
    lv_obj_set_style_text_color(cont_btn_lbl, lv_color_black(), 0);
    lv_obj_center(cont_btn_lbl);
    lv_obj_clear_flag(cont_btn_lbl, LV_OBJ_FLAG_CLICKABLE);

    // 右侧统计：柱图高度封顶，下方数字区固定，避免重叠
    lv_obj_t* stats_box = lv_obj_create(dash);
    lv_obj_remove_style_all(stats_box);
    lv_obj_set_size(stats_box, stats_w, dash_h);
    lv_obj_set_style_bg_opa(stats_box, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(stats_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(stats_box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(stats_box, 6, 0);
    lv_obj_clear_flag(stats_box, LV_OBJ_FLAG_CLICKABLE);
    DisableScroll(stats_box);

    // 数字+下划线+文案（含 pad_row×2），柱图高度据此封顶，避免叠到竖条上
    const lv_coord_t stats_block_h = nh + 4 + 2 + 4 + ih;
    const lv_coord_t chart_h =
        std::max<lv_coord_t>(36, dash_h - stats_block_h - 6);
    DrawHomeBarChart(stats_box, stats_w, chart_h, stats.finished_count,
                     static_cast<int>(st.books.size()));

    lv_obj_t* stats_row = lv_obj_create(stats_box);
    lv_obj_remove_style_all(stats_row);
    lv_obj_set_size(stats_row, stats_w, stats_block_h);
    lv_obj_set_flex_flow(stats_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(stats_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_set_style_clip_corner(stats_row, true, 0);
    lv_obj_clear_flag(stats_row, LV_OBJ_FLAG_CLICKABLE);
    DisableScroll(stats_row);

    auto make_stat_col = [&](const char* value, const char* label) {
        const lv_coord_t col_w = (stats_w - 8) / 2;
        lv_obj_t* col = lv_obj_create(stats_row);
        lv_obj_remove_style_all(col);
        lv_obj_set_size(col, col_w, stats_block_h);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(col, 4, 0);
        lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);
        DisableScroll(col);

        lv_obj_t* num = lv_label_create(col);
        lv_label_set_text(num, value);
        lv_obj_set_style_text_font(num, num_font, 0);
        lv_obj_set_width(num, col_w);
        lv_label_set_long_mode(num, LV_LABEL_LONG_CLIP);
        lv_obj_clear_flag(num, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t* line = lv_obj_create(col);
        lv_obj_remove_style_all(line);
        lv_obj_set_size(line, col_w, 2);
        lv_obj_set_style_bg_color(line, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_CLICKABLE);
        DisableScroll(line);

        lv_obj_t* lab = lv_label_create(col);
        lv_label_set_text(lab, label);
        lv_obj_set_style_text_font(lab, item_f, 0);
        lv_obj_set_width(lab, col_w);
        lv_label_set_long_mode(lab, LV_LABEL_LONG_CLIP);
        lv_obj_clear_flag(lab, LV_OBJ_FLAG_CLICKABLE);
    };

    char dur_val[24];
    BookStatsService::FormatDurationShort(dur_val, sizeof(dur_val), stats.total_seconds);
    char fin_val[24];
    std::snprintf(fin_val, sizeof(fin_val), Lang::Strings::BOOK_COUNT_FMT, stats.finished_count);
    make_stat_col(dur_val, Lang::Strings::BOOK_READ_DURATION);
    make_stat_col(fin_val, Lang::Strings::BOOK_FINISHED);

    // 封面恢复比例高度；余白落在继续阅读与最近阅读之间
    auto make_vgap = [&](lv_coord_t h) {
        lv_obj_t* g = lv_obj_create(st.list_body);
        lv_obj_remove_style_all(g);
        lv_obj_set_size(g, content_w, h);
        lv_obj_set_style_bg_opa(g, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(g, LV_OBJ_FLAG_CLICKABLE);
    };

    lv_obj_t* mid_spacer = lv_obj_create(st.list_body);
    lv_obj_remove_style_all(mid_spacer);
    lv_obj_set_width(mid_spacer, content_w);
    lv_obj_set_flex_grow(mid_spacer, 1);
    lv_obj_set_style_bg_opa(mid_spacer, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(mid_spacer, LV_OBJ_FLAG_CLICKABLE);

    // 最近阅读：始终保留标题 + 两个封面边框；空框内各自居中「暂无最近阅读」
    {
        lv_obj_t* recent_lbl = lv_label_create(st.list_body);
        lv_label_set_text(recent_lbl, Lang::Strings::BOOK_RECENT);
        lv_obj_set_style_text_font(recent_lbl, list_f, 0);
        lv_obj_set_width(recent_lbl, content_w);
        lv_obj_set_style_text_align(recent_lbl, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_clear_flag(recent_lbl, LV_OBJ_FLAG_CLICKABLE);

        make_vgap(8);

        const lv_coord_t recent_cell_w = (content_w - kHomeGap) / 2;
        lv_coord_t recent_cover_h =
            std::min(kRecentCoverH, static_cast<lv_coord_t>(recent_cell_w * 4 / 3));
        if (recent_cover_h < 140) {
            recent_cover_h = 140;
        }

        lv_obj_t* recent_row = lv_obj_create(st.list_body);
        lv_obj_remove_style_all(recent_row);
        lv_obj_set_width(recent_row, content_w);
        lv_obj_set_height(recent_row, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(recent_row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(recent_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                              LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_column(recent_row, kHomeGap, 0);
        lv_obj_clear_flag(recent_row, LV_OBJ_FLAG_CLICKABLE);
        DisableScroll(recent_row);

        auto make_empty_recent_slot = [&](lv_obj_t* parent) {
            lv_obj_t* host = lv_obj_create(parent);
            lv_obj_remove_style_all(host);
            lv_obj_set_size(host, recent_cell_w, recent_cover_h);
            lv_obj_set_style_pad_all(host, kCoverFrameBorder, 0);
            lv_obj_set_style_bg_color(host, lv_color_hex(0xF0F0F0), 0);
            lv_obj_set_style_bg_opa(host, LV_OPA_COVER, 0);
            StyleBookCoverFrame(host);
            lv_obj_clear_flag(host, LV_OBJ_FLAG_CLICKABLE);
            DisableScroll(host);

            lv_obj_t* empty_lbl = lv_label_create(host);
            lv_label_set_text(empty_lbl, Lang::Strings::BOOK_NO_RECENT);
            lv_obj_set_style_text_font(empty_lbl, list_f, 0);
            lv_obj_set_style_text_color(empty_lbl, lv_color_black(), 0);
            lv_obj_set_style_text_align(empty_lbl, LV_TEXT_ALIGN_CENTER, 0);
            lv_label_set_long_mode(empty_lbl, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(empty_lbl, CoverFrameInner(recent_cell_w) - 8);
            lv_obj_clear_flag(empty_lbl, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_align(empty_lbl, LV_ALIGN_CENTER, 0, 0);
            return host;
        };

        for (int k = 0; k < 2; ++k) {
            const int idx = stats.has_recent ? stats.recent_indices[k] : -1;
            if (idx >= 0 && idx < static_cast<int>(st.books.size())) {
                CreateBookCoverCell(recent_row, st.books[static_cast<size_t>(idx)], idx,
                                    recent_cell_w, recent_cover_h, st.recent_covers, false,
                                    kRecentTitleGap, true);
            } else {
                make_empty_recent_slot(recent_row);
            }
        }

        make_vgap(kHomeShelfTopGap);
    }

    // —— 底部「我的书架 >」——
    lv_obj_t* shelf_btn = lv_obj_create(st.list_body);
    lv_obj_remove_style_all(shelf_btn);
    lv_obj_set_size(shelf_btn, content_w, kHomeShelfBtnH);
    lv_obj_set_style_bg_color(shelf_btn, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(shelf_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(shelf_btn, kHomeShelfBtnRadius, 0);
    lv_obj_add_flag(shelf_btn, LV_OBJ_FLAG_CLICKABLE);
    DisableScroll(shelf_btn);
    HapticAttachClick(shelf_btn);
    lv_obj_add_event_cb(shelf_btn, OnOpenShelfClicked, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* shelf_lbl = lv_label_create(shelf_btn);
    char shelf_txt[64];
    std::snprintf(shelf_txt, sizeof(shelf_txt), "%s  >", Lang::Strings::BOOK_SHELF_TITLE);
    lv_label_set_text(shelf_lbl, shelf_txt);
    lv_obj_set_style_text_font(shelf_lbl, list_f, 0);
    lv_obj_set_style_text_color(shelf_lbl, lv_color_white(), 0);
    lv_obj_center(shelf_lbl);
    lv_obj_clear_flag(shelf_lbl, LV_OBJ_FLAG_CLICKABLE);

    ScheduleListCoverFill();
}

void RequestOpenAssistantFromBook() {
    const char* screen = VkKey_ActiveScreen();
    CancelDetailCoverLoad();
    CancelListCoverFill();
    ESP_LOGI(TAG, "AI long (%s) -> ignored (assistant removed)", screen != nullptr ? screen : "?");
}

}  // namespace

lv_obj_t* BookScreen::Create() {
    auto& st = State();
    const bool workers_busy = st.opening.load() || st.layout_busy.load() ||
                              st.chapter_pages_busy.load() || st.page_image_busy.load();
    if (workers_busy) {
        st.open_token.fetch_add(1);
        st.layout_token.fetch_add(1);
        st.chapter_pages_token.fetch_add(1);
        CancelPageImageLoad();
        if (st.session) {
            st.session->InvalidateChapterPageTable();
            st.session->AbortTxtPaginate();
        }
        StopReadTimeCheckpointTimer();
        st.deferred_cleanup = true;
    } else {
        // 收尾：含 worker 投递失败后留下的 deferred_cleanup
        if (st.deferred_cleanup) {
            FinishDeferredCleanup();
        } else {
            EndReadingTimeTracking();
            st.session.reset();
            ReleaseBookFont();
        }
    }
    // 封面由详情屏 DELETE 释放；此处不 Reset，避免旧屏仍引用时 UAF
    st.read_scr = nullptr;
    st.content = nullptr;
    st.page_label = nullptr;
    st.title_label = nullptr;
    st.status_bar = nullptr;
    st.status_overlay = nullptr;
    st.status_h = 0;
    st.settings_sheet = nullptr;
    ClearSettingsSheetWidgetRefs();
    StopTtfPollTimer();
    st.ttf_mode = BookUiState::TtfMode::kNone;
    st.ttf_pick.clear();
    st.ttf_page = 0;
    st.ttf_size_px = kTtfConvertSizeDefault;
    st.viewport_w = 0;
    st.viewport_h = 0;
    st.font_entries.clear();
    st.font_list_page = 0;
    st.font_multi = false;
    st.font_selected.clear();
    st.font_suppress_click_until_us = 0;
    st.font_suppress_click_idx = -1;
    st.font_suppress_next_click = false;
    st.read_chrome = BookUiState::ReadChrome::kReading;
    st.settings_resume_after_open = false;
    st.layout_again = false;
    st.layout_reload_font = false;
    st.layout_anchor = {};
    st.chapter_pages_again = false;
    CancelLayoutDebounce();
    StopLayoutHintTimer();
    st.layout_hint_done_until_us = 0;
    st.open_bar = nullptr;
    st.open_pct_lbl = nullptr;
    st.shelf_multi = false;
    st.shelf_selected.clear();
    st.shelf_suppress_click_until_us = 0;
    st.shelf_suppress_click_idx = -1;
    st.multi_bar = nullptr;
    st.back_root = BookUiState::NavRoot::kHome;
    CancelListCoverFill();
    st.recent_covers.clear();
    st.shelf_covers.clear();
    ResetReadTouch();

    ScreenSetIsHome(false);

    lv_obj_t* scr = lv_obj_create(nullptr);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, ListFont(), 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    DisableScroll(scr);
    st.list_scr = scr;
    lv_obj_add_event_cb(
        scr,
        [](lv_event_t* e) {
            auto& s = State();
            const auto* tgt = static_cast<lv_obj_t*>(lv_event_get_target(e));
            if (s.list_scr == tgt) {
                CancelListCoverFill();
                s.list_scr = nullptr;
                s.list_body = nullptr;
                s.list_footer = nullptr;
                s.multi_bar = nullptr;
                s.shelf_multi = false;
                s.shelf_selected.clear();
                s.recent_covers.clear();
                s.shelf_covers.clear();
            }
        },
        LV_EVENT_DELETE, nullptr);

    EpdStatusBar status = ScreenCreateStatusBar(scr);
    StatusBarManager::GetInstance().SetOngoing("book", Lang::Strings::BOOK_SHELF_TITLE);
    lv_obj_add_event_cb(scr, [](lv_event_t*) {
        StatusBarManager::GetInstance().ClearOngoing("book");
    }, LV_EVENT_DELETE, nullptr);

    const lv_coord_t body_h = LV_VER_RES - status.height;

    lv_obj_t* body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, body_h);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, status.height);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_left(body, kListPad, 0);
    lv_obj_set_style_pad_right(body, kListPad, 0);
    lv_obj_set_style_pad_top(body, kListPad, 0);
    lv_obj_set_style_pad_bottom(body, kListPad + kHomeBottomPad, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(body, kHomeGap, 0);
    DisableScroll(body);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* list_body = lv_obj_create(body);
    lv_obj_remove_style_all(list_body);
    lv_obj_set_width(list_body, ContentWidth());
    lv_obj_set_flex_grow(list_body, 1);
    lv_obj_set_flex_flow(list_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list_body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(list_body, 0, 0);
    DisableScroll(list_body);
    st.list_body = list_body;
    st.list_footer = nullptr;
    st.multi_bar = nullptr;

    auto& sd = SdCardManager::GetInstance();
    if (!sd.IsMounted() && !sd.Mount()) {
        ShowMessage(list_body, Lang::Strings::BOOK_NO_SD, ListFont());
        VkKey_AttachScreen(scr, kScreenLibrary, BookAiLongPressDesc(BookScreen::Create));
        return scr;
    }

    st.books.clear();
    // 优先复用开机/上次扫库缓存，未命中再扫盘
    (void)reader::LoadBookLibrary(reader::kDefaultBooksDir, st.books);
    // 仅丢掉已不在书库的 miss；保留仍在库中的，避免回首页再进又重复 Open 抽封面
    if (!st.cover_embed_miss.empty()) {
        st.cover_embed_miss.erase(
            std::remove_if(st.cover_embed_miss.begin(), st.cover_embed_miss.end(),
                           [&](const std::string& path) {
                               for (const auto& b : st.books) {
                                   if (b.path == path) {
                                       return false;
                                   }
                               }
                               return true;
                           }),
            st.cover_embed_miss.end());
    }
    RenderReadingHome();

    // factory=Create：进百问后返回阅读首页
    VkKey_AttachScreen(scr, kScreenLibrary, BookAiLongPressDesc(BookScreen::Create));
    return scr;
}


bool BookScreen::IsReadingActive() {
    // 仅正文沉浸翻页；目录/设置浮层仍走音量。编排中仍算阅读中，以便音量键走翻页提示。
    if (std::strcmp(VkKey_ActiveScreen(), kScreenRead) != 0) {
        return false;
    }
    auto& st = State();
    return !IsReadOverlayChrome(st.read_chrome);
}

bool BookPageRepeatStep(int page_delta) {
    const char* screen = VkKey_ActiveScreen();
    auto& st = State();
    if (page_delta == 0) {
        return false;
    }

    if (std::strcmp(screen, kScreenBookshelf) == 0) {
        const int last = std::max(0, ListPageCount() - 1);
        int next = st.list_page + page_delta;
        if (next < 0) {
            next = 0;
        } else if (next > last) {
            next = last;
        }
        if (next == st.list_page) {
            return false;
        }
        st.list_page = next;
        RequestRenderBookshelfPage();
        return page_delta < 0 ? st.list_page > 0 : st.list_page < last;
    }

    if (std::strcmp(screen, kScreenLibrary) == 0) {
        return false;
    }

    if (std::strcmp(screen, kScreenRead) != 0 || st.opening.load() || !st.session ||
        !st.session->IsOpen()) {
        return false;
    }

    if (st.read_chrome == BookUiState::ReadChrome::kToc) {
        const int pages =
            std::max(1, (st.session->TocCount() + st.toc_page_size - 1) /
                            std::max(1, st.toc_page_size));
        int next = st.toc_list_page + page_delta;
        const int last = std::max(0, pages - 1);
        if (next < 0) {
            next = 0;
        } else if (next > last) {
            next = last;
        }
        if (next == st.toc_list_page) {
            return false;
        }
        st.toc_list_page = next;
        RequestRenderTocList();
        return page_delta < 0 ? st.toc_list_page > 0 : st.toc_list_page < last;
    }
    if (st.read_chrome == BookUiState::ReadChrome::kSettings) {
        // 悬浮设置卡片：长按连翻不翻页
        return false;
    }

    // 正文：±10 写入积压，Apply 一次吃到目标页（或边界）
    if (!QueueReaderPageTurn(page_delta)) {
        return false;
    }
    return page_delta < 0 ? st.session->HasPrevPage() : st.session->HasNextPage();
}

bool BookScreen::OnBootLongPress() {
    RequestOpenAssistantFromBook();
    return true;
}

bool BookScreen::OnVkKeyLongPress(const char* key_name) {
    if (key_name == nullptr) {
        return false;
    }
    // 盖板 vk_home 长按不进百问（仅 BOOT）；交默认策略一键回系统首页
    if (std::strcmp(key_name, "vk_home") == 0) {
        return false;
    }
    return VkPageRepeatTryStart(key_name, BookPageRepeatStep);
}

bool BookScreen::OnVkKeyPressUp(const char* key_name) {
    if (!VkPageRepeatOnPressUp(key_name)) {
        return false;
    }
    // 松手时若还有未画的 delta，补一帧（通常定时器已停、积压很小）
    if (HasPendingReaderPageDelta()) {
        RequestRenderReaderPage();
    }
    return true;
}

bool BookScreen::OnVkKey(const char* key_name) {
    if (key_name == nullptr) {
        return true;
    }
    const char* screen = VkKey_ActiveScreen();
    auto& st = State();

    if (std::strcmp(screen, kScreenRead) == 0) {
        // 正文 vk_home 短按：设置/目录浮层→收起；长按由 VkKey 默认回系统首页
        if (std::strcmp(key_name, "vk_home") == 0) {
            if (st.read_chrome != BookUiState::ReadChrome::kReading) {
                BookLvAsync(AsyncHideReadChrome);
                return true;
            }
            BookLvAsync(BackToDetailAsync);
            return true;
        }
        if (st.opening.load()) {
            return true;  // 解析中：翻页键无效；HOME 已在上方处理
        }
        if (!st.session || !st.session->IsOpen()) {
            // 打开失败页：上一页回详情
            if (std::strcmp(key_name, "vk_prev") == 0) {
                BookLvAsync(BackToDetailAsync);
            }
            return true;
        }
        if (st.read_chrome == BookUiState::ReadChrome::kSettings) {
            // 悬浮设置：多选中 prev 取消批量；否则收起；next 忽略
            if (std::strcmp(key_name, "vk_prev") == 0) {
                if (st.font_multi) {
                    ExitFontMultiMode(true);
                } else {
                    BookLvAsync(AsyncHideReadChrome);
                }
            }
            return true;
        }
        // 目录列表：vk 翻列表页
        if (st.read_chrome == BookUiState::ReadChrome::kToc) {
            const int pages =
                std::max(1, (st.session->TocCount() + st.toc_page_size - 1) /
                                std::max(1, st.toc_page_size));
            if (std::strcmp(key_name, "vk_prev") == 0) {
                if (st.toc_list_page > 0) {
                    --st.toc_list_page;
                    RequestRenderTocList();
                } else {
                    BookLvAsync(AsyncHideReadChrome);
                }
                return true;
            }
            if (std::strcmp(key_name, "vk_next") == 0) {
                if (st.toc_list_page + 1 < pages) {
                    ++st.toc_list_page;
                    RequestRenderTocList();
                }
                return true;
            }
            return true;
        }
        if (std::strcmp(key_name, "vk_prev") == 0) {
            if (!QueueReaderPageTurn(-1)) {
                BookLvAsync(BackToDetailAsync);
            }
            return true;
        }
        if (std::strcmp(key_name, "vk_next") == 0) {
            QueueReaderPageTurn(1);
            return true;
        }
        return true;
    }

    if (std::strcmp(screen, kScreenDetail) == 0) {
        if (std::strcmp(key_name, "vk_home") == 0) {
            BookLvAsync(BackToLibraryAsync);
            return true;
        }
        if (std::strcmp(key_name, "vk_prev") == 0) {
            BookLvAsync(BackToLibraryAsync);
            return true;
        }
        if (std::strcmp(key_name, "vk_next") == 0) {
            if (!st.opening.load()) {
                BookLvAsync(StartReadAsync,
                            reinterpret_cast<void*>(static_cast<intptr_t>(st.selected)));
            }
            return true;
        }
        return true;
    }

    if (std::strcmp(screen, kScreenLibrary) == 0) {
        if (std::strcmp(key_name, "vk_home") == 0) {
            RequestBackHome();
            return true;
        }
        if (std::strcmp(key_name, "vk_prev") == 0) {
            return false; // 阅读首页：出栈回系统首页
        }
        if (std::strcmp(key_name, "vk_next") == 0) {
            BookLvAsync(OpenBookshelfAsync);
            return true;
        }
        return true;
    }

    if (std::strcmp(screen, kScreenBookshelf) == 0) {
        if (std::strcmp(key_name, "vk_home") == 0) {
            BookLvAsync(BackToReadingHomeAsync);
            return true;
        }
        if (std::strcmp(key_name, "vk_prev") == 0) {
            if (st.list_page > 0) {
                --st.list_page;
                RequestRenderBookshelfPage();
                return true;
            }
            // 第一页：多选中则取消批量，否则回阅读首页
            if (st.shelf_multi) {
                ExitShelfMultiMode(true);
                return true;
            }
            BookLvAsync(BackToReadingHomeAsync);
            return true;
        }
        if (std::strcmp(key_name, "vk_next") == 0) {
            if (st.list_page + 1 < ListPageCount()) {
                ++st.list_page;
                RequestRenderBookshelfPage();
            }
            return true;
        }
        return true;
    }
    return true;
}

void BookScreen::OnReadingFontPrefChanged() {
    if (!State().opening.load()) {
        ReleaseBookFont();
    }
}

void BookScreen::OnEnterStandby() {
    PauseReadingTimeForStandby();
}

void BookScreen::OnResumeFromStandby() {
    ResumeReadingTimeAfterStandby();
}

void BookScreen::OpenDownloadedBook(const reader::BookInfo& info) {
    if (info.path.empty()) {
        return;
    }
    auto& st = State();
    if (ReaderWorkersBusy()) {
        ESP_LOGW(TAG, "OpenDownloadedBook ignored: worker busy");
        return;
    }
    CancelDetailCoverLoad();
    CancelListCoverFill();
    const int index = EnsureBookInLibrary(info);
    st.selected = index;
    ScreenLoadReplace(CreateReaderScreen(info));
}
