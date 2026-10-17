#include "book_reader.h"

#include "book_font_manager.h"
#include "book_reader_prefs.h"
#include "book_toc.h"
#include "book_ui_state.h"

#include "assets/lang_config.h"
#include "haptic_feedback.h"
#include "lv_adapter_display.h"
#include "reader/reader.h"
#include "reader/reader_page_render.h"
#include "screen_common.h"
#include "utils/psr_worker.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace book_screen_detail {

namespace {

constexpr const char* TAG = "BookReader";
constexpr uint32_t kPageImageWorkerStack = 16 * 1024;

ReaderHideChromeFn s_hide_chrome = nullptr;
ReaderShowSettingsSheetFn s_show_settings = nullptr;
ReaderSetStatusVisibleFn s_set_status_visible = nullptr;
ReaderSetSettingsSheetVisibleFn s_set_settings_visible = nullptr;
ReaderApplyImmersiveGeometryFn s_apply_immersive_geom = nullptr;
ReaderApplyLayoutLiveFn s_apply_layout_live = nullptr;
ReaderIsLayoutHintBusyFn s_is_layout_hint_busy = nullptr;
ReaderOnPageImageIdleFn s_on_page_image_idle = nullptr;
ReaderTruncateFn s_truncate = nullptr;

ScreenPaintCoalesce s_reader_paint{};
std::atomic<int> s_reader_page_delta{0};

struct PageImageDoneMsg {
    uint32_t token = 0;
    lv_obj_t* slot = nullptr;
    reader::RasterImage* image = nullptr;
    std::string href;
    bool ok = false;
    bool page_images = false;
};

struct PageImageWorkItem {
    std::string href;
    int max_w = 0;
    int max_h = 0;
    lv_obj_t* slot = nullptr;
};

struct PageImageWorkerArg {
    uint32_t token = 0;
    std::string book_path;
    reader::BookFormat format = reader::BookFormat::kTxt;
    bool page_images = false;
    std::vector<PageImageWorkItem> items;
};

void CallHideChrome() {
    if (s_hide_chrome != nullptr) {
        s_hide_chrome();
    }
}

void CallShowSettings() {
    if (s_show_settings != nullptr) {
        s_show_settings();
    }
}

void CallSetStatusVisible(bool visible) {
    if (s_set_status_visible != nullptr) {
        s_set_status_visible(visible);
    }
}

void CallSetSettingsVisible(bool visible) {
    if (s_set_settings_visible != nullptr) {
        s_set_settings_visible(visible);
    }
}

void CallApplyImmersiveGeometry() {
    if (s_apply_immersive_geom != nullptr) {
        s_apply_immersive_geom();
    }
}

void CallApplyLayoutLive(bool reload_font) {
    if (s_apply_layout_live != nullptr) {
        s_apply_layout_live(reload_font);
    }
}

bool CallIsLayoutHintBusy() {
    return s_is_layout_hint_busy != nullptr && s_is_layout_hint_busy();
}

void CallOnPageImageIdle() {
    if (s_on_page_image_idle != nullptr) {
        s_on_page_image_idle();
    }
}

std::string CallTruncate(const std::string& text, const lv_font_t* font, lv_coord_t max_w) {
    if (s_truncate != nullptr) {
        return s_truncate(text, font, max_w);
    }
    return text;
}

using book_font::BookFont;
using book_font::ListFont;
using book_font::PrewarmPageFont;

void DisableScroll(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(obj, LV_DIR_NONE);
}

void ShowMessage(lv_obj_t* parent, const char* msg, const lv_font_t* font = nullptr) {
    if (parent == nullptr) {
        return;
    }
    auto& st = State();
    if (parent == st.content) {
        st.open_bar = nullptr;
        st.open_pct_lbl = nullptr;
    }
    lv_obj_clean(parent);
    lv_obj_set_style_layout(parent, LV_LAYOUT_NONE, 0);
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, msg != nullptr ? msg : "");
    lv_obj_set_style_text_font(label, font != nullptr ? font : ListFont(), 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_width(label, LV_HOR_RES - 32);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_center(label);
    DisableScroll(label);
}

lv_coord_t MeasureTextWidth(const lv_font_t* font, const char* text) {
    if (font == nullptr || text == nullptr || text[0] == '\0') {
        return 0;
    }
    lv_point_t sz = {};
    lv_text_get_size(&sz, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return static_cast<lv_coord_t>(sz.x);
}

lv_coord_t ReadContentWidthLocal() {
    BookReaderPrefsEnsureLoaded();
    const int ml = BookReaderPrefsMarginLeft();
    const int mr = BookReaderPrefsMarginRight();
    lv_coord_t w = LV_HOR_RES - static_cast<lv_coord_t>(ml + mr);
    return w > 40 ? w : 40;
}

void OnReadContentClicked(lv_event_t* e);
void OnReadContentLongPressed(lv_event_t* e);
void AttachReaderUnderline(lv_obj_t* label, int mode);
void OnReaderUnderlineDraw(lv_event_t* e);
void StartPageImageWorker();
void AttachPageImageToSlot(lv_obj_t* slot, reader::RasterImage* heap_img, bool page_images);
void PaintReaderPage();
void EnsureReaderPaintFn();

bool ReadEventPoint(lv_event_t* e, lv_point_t* out) {
    if (out == nullptr) {
        return false;
    }
    lv_indev_t* indev = e != nullptr ? lv_event_get_indev(e) : nullptr;
    if (indev == nullptr) {
        indev = lv_indev_active();
    }
    if (indev == nullptr || lv_indev_get_type(indev) != LV_INDEV_TYPE_POINTER) {
        return false;
    }
    lv_indev_get_point(indev, out);
    return true;
}
void PaintReaderPage() {
    ESP_LOGI(TAG, "async RenderReaderPage begin");
    RenderReaderPage();
    ESP_LOGI(TAG, "async RenderReaderPage done");
}
void EnsureReaderPaintFn() {
    if (s_reader_paint.paint == nullptr) {
        s_reader_paint.paint = PaintReaderPage;
    }
}

void AttachPageImageToSlot(lv_obj_t* slot, reader::RasterImage* heap_img, bool page_images) {
    if (slot == nullptr || heap_img == nullptr || !lv_obj_is_valid(slot)) {
        delete heap_img;
        return;
    }
    lv_obj_clean(slot);
    heap_img->BindDsc();
    lv_obj_t* image = lv_image_create(slot);
    lv_image_set_src(image, &heap_img->dsc);
    lv_obj_set_user_data(image, heap_img);
    lv_obj_add_event_cb(
        image,
        [](lv_event_t* ev) {
            auto* p = static_cast<reader::RasterImage*>(
                lv_obj_get_user_data(static_cast<lv_obj_t*>(lv_event_get_target(ev))));
            delete p;
        },
        LV_EVENT_DELETE, nullptr);
    lv_obj_clear_flag(image, LV_OBJ_FLAG_CLICKABLE);
    if (page_images) {
        lv_obj_align(image, LV_ALIGN_TOP_LEFT, 0, 0);
    }
}

void AsyncPageImageDone(void* user_data) {
    auto* msg = static_cast<PageImageDoneMsg*>(user_data);
    auto& st = State();
    const bool mine = (msg->token == st.page_image_token.load());
    if (!mine || msg->slot == nullptr || !lv_obj_is_valid(msg->slot) || !msg->ok ||
        msg->image == nullptr || msg->image->empty()) {
        delete msg->image;
        delete msg;
        return;
    }
    if (st.session && !msg->href.empty()) {
        reader::RasterImage cache_copy;
        cache_copy.pixels = msg->image->pixels;
        cache_copy.width = msg->image->width;
        cache_copy.height = msg->image->height;
        cache_copy.BindDsc();
        st.session->SetCachedPageImage(msg->href, std::move(cache_copy));
    }
    AttachPageImageToSlot(msg->slot, msg->image, msg->page_images);
    msg->image = nullptr;
    delete msg;
}

void PageImageWorker(void* arg) {
    auto* ctx = static_cast<PageImageWorkerArg*>(arg);
    auto& st = State();

    // EPUB：独立打开 ZIP，避免长时间占用 session 的 ZipReader 锁堵翻页
    std::unique_ptr<reader::EpubDocument> epub_doc;
    if (ctx->format == reader::BookFormat::kEpub && !ctx->book_path.empty()) {
        epub_doc = std::make_unique<reader::EpubDocument>();
        if (!epub_doc->Open(ctx->book_path.c_str())) {
            ESP_LOGW(TAG, "book_pimg epub open fail");
            epub_doc.reset();
        }
    }

    for (const auto& item : ctx->items) {
        if (ctx->token != st.page_image_token.load()) {
            break;
        }
        auto* img = new reader::RasterImage();
        bool ok = false;
        if (epub_doc) {
            ok = epub_doc->DecodeItemImageToL8(item.href.c_str(), item.max_w, item.max_h, *img,
                                               &st.page_image_abort) &&
                 !img->empty();
        } else if (st.session) {
            ok = st.session->LoadPageImage(item.href, item.max_w, item.max_h, *img) && !img->empty();
        }
        if (st.page_image_abort.load(std::memory_order_relaxed) ||
            ctx->token != st.page_image_token.load()) {
            delete img;
            break;
        }
        auto* done = new PageImageDoneMsg{};
        done->token = ctx->token;
        done->slot = item.slot;
        done->href = item.href;
        done->ok = ok;
        done->page_images = ctx->page_images;
        if (ok) {
            done->image = img;
        } else {
            delete img;
            done->image = nullptr;
        }
        if (!ScreenLvAsync(AsyncPageImageDone, done)) {
            delete done->image;
            delete done;
        }
    }
    if (epub_doc) {
        epub_doc->Close();
        epub_doc.reset();
    }

    auto* finish = new uint32_t(ctx->token);
    if (!ScreenLvAsync(
            [](void* p) {
                auto* tok = static_cast<uint32_t*>(p);
                auto& st = State();
                st.page_image_busy.store(false);
                st.page_image_again = false;
                (void)tok;
                CallOnPageImageIdle();
                if (!st.page_image_pending.empty()) {
                    StartPageImageWorker();
                }
                delete tok;
            },
            finish)) {
        st.page_image_busy.store(false);
        delete finish;
    }
    delete ctx;
}

void StartPageImageWorker() {
    auto& st = State();
    if (st.page_image_pending.empty() || !st.session) {
        return;
    }
    if (st.page_image_busy.load()) {
        st.page_image_again = true;
        return;
    }
    const uint32_t token = st.page_image_token.load();
    auto* ctx = new PageImageWorkerArg{};
    ctx->token = token;
    ctx->book_path = st.session->Info().path;
    ctx->format = st.session->Info().format;
    ctx->page_images = st.session->IsPageImagesMode();
    ctx->items.reserve(st.page_image_pending.size());
    for (auto& p : st.page_image_pending) {
        PageImageWorkItem item;
        item.href = std::move(p.href);
        item.max_w = p.max_w;
        item.max_h = p.max_h;
        item.slot = p.slot;
        ctx->items.push_back(std::move(item));
    }
    st.page_image_pending.clear();
    st.page_image_abort.store(false, std::memory_order_relaxed);
    st.page_image_busy.store(true);
    st.page_image_again = false;
    if (!SpawnPsrWorker("book_pimg", kPageImageWorkerStack, tskIDLE_PRIORITY + 1, PageImageWorker,
                        ctx, nullptr, 0)) {
        ESP_LOGW(TAG, "book_pimg task failed");
        st.page_image_busy.store(false);
        delete ctx;
    }
}
void OnReadContentLongPressed(lv_event_t* e) {
    auto& st = State();
    if (st.content == nullptr || lv_event_get_target(e) != st.content) {
        return;
    }
    if (st.opening.load() || !st.session || !st.session->IsOpen()) {
        return;
    }
    if (st.read_chrome != BookUiState::ReadChrome::kReading) {
        return;
    }
    // 重绘堵住 LVGL 时 pr_timestamp 过旧，松手后仍可能冒出 LONG_PRESSED
    if (!TouchUiFingerIsDown()) {
        return;
    }
    st.read_touch.suppress_click = true;
    // 按下已由 HapticAttachClick 早震，长按不再二次震
    CallShowSettings();
}
void OnReadContentClicked(lv_event_t* e) {
    auto& st = State();
    if (st.content == nullptr || lv_event_get_target(e) != st.content) {
        return;
    }
    if (st.read_touch.suppress_click) {
        st.read_touch.suppress_click = false;
        return;
    }
    if (st.opening.load() || !st.session || !st.session->IsOpen()) {
        return;
    }
    if (st.read_chrome == BookUiState::ReadChrome::kSettings) {
        // 设置卡片打开时点正文只收起，不翻页
        CallHideChrome();
        return;
    }
    if (st.read_chrome == BookUiState::ReadChrome::kToc) {
        // 目录整页：页内触摸不退出、不翻页；仅盖板 vk 返回
        return;
    }
    // 左 1/3 上一页，右 2/3 下一页；震动由 HapticAttachClick 早震
    lv_point_t pt = {};
    const int dir = (ReadEventPoint(e, &pt) && pt.x < LV_HOR_RES / 3) ? -1 : 1;
    QueueReaderPageTurn(dir);
}
void OnReaderUnderlineDraw(lv_event_t* e) {
    lv_obj_t* label = static_cast<lv_obj_t*>(lv_event_get_target(e));
    if (label == nullptr) {
        return;
    }
    const int mode = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(label)));
    if (mode != kBookReaderUnderlineSolid && mode != kBookReaderUnderlineDashed) {
        return;
    }
    lv_layer_t* layer = lv_event_get_layer(e);
    const lv_font_t* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
    if (layer == nullptr || font == nullptr || font->line_height <= 0) {
        return;
    }

    lv_area_t coords;
    lv_obj_get_content_coords(label, &coords);
    const lv_coord_t line_h = font->line_height;
    // 相对 LVGL 默认下划线再下移 4px
    const lv_coord_t y_in_line =
        static_cast<lv_coord_t>(line_h - font->base_line - font->underline_position + 4);

    lv_draw_line_dsc_t dsc;
    lv_draw_line_dsc_init(&dsc);
    dsc.color = lv_color_black();
    dsc.width = font->underline_thickness > 0 ? font->underline_thickness : 1;
    dsc.opa = LV_OPA_COVER;
    if (mode == kBookReaderUnderlineDashed) {
        dsc.dash_width = 6;
        dsc.dash_gap = 4;
    }

    const char* txt = lv_label_get_text(label);
    lv_point_t sz = {};
    if (txt != nullptr && txt[0] != '\0') {
        lv_text_get_size(&sz, txt, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    }

    const lv_coord_t content_h = lv_area_get_height(&coords);
    const int lines = content_h > 0 ? static_cast<int>((content_h + line_h - 1) / line_h) : 1;
    for (int i = 0; i < lines; ++i) {
        const lv_coord_t y = coords.y1 + i * line_h + y_in_line;
        dsc.p1.x = coords.x1;
        dsc.p1.y = y;
        lv_coord_t x2 = coords.x2;
        if (lines == 1 && sz.x > 0) {
            x2 = coords.x1 + sz.x - 1;
            if (x2 > coords.x2) {
                x2 = coords.x2;
            }
        }
        if (x2 < dsc.p1.x) {
            continue;
        }
        dsc.p2.x = x2;
        dsc.p2.y = y;
        lv_draw_line(layer, &dsc);
    }
}
void AttachReaderUnderline(lv_obj_t* label, int mode) {
    if (label == nullptr || mode == kBookReaderUnderlineOff) {
        return;
    }
    lv_obj_set_user_data(label, reinterpret_cast<void*>(static_cast<intptr_t>(mode)));
    lv_obj_add_event_cb(label, OnReaderUnderlineDraw, LV_EVENT_DRAW_MAIN_END, nullptr);
}
}  // namespace

void ResetReadTouch() {
    State().read_touch.suppress_click = false;
}

void DetachReadBodyInput(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_remove_event_cb(obj, OnReadContentClicked);
    lv_obj_remove_event_cb(obj, OnReadContentLongPressed);
    // 正文整页热区：进目录/设置后须摘掉，否则空白处仍早震
    HapticDetachClick(obj);
}

void AttachReadBodyInput(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    DetachReadBodyInput(obj);
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    // 页内热区：touch_feed 按下早震；业务回调里勿再 Pulse
    HapticAttachClick(obj);
    lv_obj_add_event_cb(obj, OnReadContentClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(obj, OnReadContentLongPressed, LV_EVENT_LONG_PRESSED, nullptr);
}

void ApplyReaderPageDelta() {
    auto& st = State();
    if (!st.session || !st.session->IsOpen()) {
        s_reader_page_delta.store(0, std::memory_order_release);
        return;
    }
    // 一次吃光积压：长按按档累加，每帧画到「当前指到的页」（墨水慢则中间页跳过）
    int d = s_reader_page_delta.exchange(0, std::memory_order_acq_rel);
    while (d > 0) {
        if (!st.session->NextPage()) {
            break;
        }
        --d;
    }
    while (d < 0) {
        if (!st.session->PrevPage()) {
            break;
        }
        ++d;
    }
}

/** 盖板键/长按定时器：只累加 delta，真正翻页在 RenderReaderPage（LVGL） */
bool QueueReaderPageTurn(int dir) {
    auto& st = State();
    if (dir == 0 || !st.session || !st.session->IsOpen()) {
        return false;
    }
    const int pending = s_reader_page_delta.load(std::memory_order_acquire);
    if (dir > 0) {
        if (pending <= 0 && !st.session->HasNextPage()) {
            return false;
        }
    } else if (pending >= 0 && !st.session->HasPrevPage()) {
        return false;
    }
    s_reader_page_delta.fetch_add(dir, std::memory_order_acq_rel);
    // 上屏经 ScreenPaintCoalesce：有 pending 则等交完，连点跳到最终页再画
    RequestRenderReaderPage();
    return true;
}

void RequestRenderReaderPage() {
    EnsureReaderPaintFn();
    ScreenPaintCoalesceRequest(&s_reader_paint);
}

/** 作废正文插图后台解码；翻页 clean 前调用。 */
void CancelPageImageLoad() {
    auto& st = State();
    st.page_image_abort.store(true, std::memory_order_relaxed);
    st.page_image_token.fetch_add(1);
    st.page_image_pending.clear();
    st.page_image_again = false;
}

void ResetPointerLongPress() {
    for (lv_indev_t* indev = lv_indev_get_next(nullptr); indev != nullptr;
         indev = lv_indev_get_next(indev)) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_POINTER) {
            lv_indev_reset_long_press(indev);
        }
    }
}

void UpdateReadFooter() {
    auto& st = State();
    if (st.page_label == nullptr) {
        return;
    }
    char buf[112];
    const lv_font_t* footer_font = ListFont();
    const lv_coord_t footer_w = LV_HOR_RES - 16;
    if (!st.session || !st.session->IsOpen()) {
        return;
    }
    const int pct_x10 = st.session->ReadingProgressX10();
    if (st.read_chrome == BookUiState::ReadChrome::kToc) {
        const int n = st.session->TocCount();
        const int pages = std::max(1, (n + st.toc_page_size - 1) / st.toc_page_size);
        std::snprintf(buf, sizeof(buf), Lang::Strings::BOOK_TOC_PAGE_FMT, st.toc_list_page + 1, pages);
    } else {
        // 百分比右侧：全书当前页/总页；旁附编排中 / 完成
        const int cur = st.session->BookCurrentPage() + 1;
        const int total = std::max(1, st.session->BookPageCount());
        char hint[16] = "";
        const bool busy = CallIsLayoutHintBusy();
        const bool show_done = !busy && st.layout_hint_done_until_us > 0 &&
                               esp_timer_get_time() < st.layout_hint_done_until_us;
        if (busy) {
            std::snprintf(hint, sizeof(hint), " %s", Lang::Strings::BOOK_LAYOUT_BUSY);
        } else if (show_done) {
            std::snprintf(hint, sizeof(hint), " %s", Lang::Strings::BOOK_LAYOUT_DONE);
        }
        char suffix[64];
        std::snprintf(suffix, sizeof(suffix), " · %d.%d%%  %d/%d%s", pct_x10 / 10, pct_x10 % 10, cur,
                      total, hint);
        const lv_coord_t suffix_w = MeasureTextWidth(footer_font, suffix);
        const lv_coord_t title_budget = std::max<lv_coord_t>(0, footer_w - suffix_w);

        std::string title = st.session->CurrentTocTitle();
        if (title.empty()) {
            title = Lang::Strings::BOOK_BODY;
        }
        title = CallTruncate(title, footer_font, title_budget);
        std::snprintf(buf, sizeof(buf), "%s%s", title.c_str(), suffix);
    }
    lv_label_set_text(st.page_label, buf);
}

void RenderReaderPage() {
    ApplyReaderPageDelta();
    auto& st = State();
    if (st.content == nullptr || !st.session) {
        return;
    }
    st.open_bar = nullptr;
    st.open_pct_lbl = nullptr;
    if (st.read_chrome == BookUiState::ReadChrome::kToc) {
        RenderTocList();
        return;
    }
    // 设置卡片盖在正文上：重绘正文时保持卡片；其它情况回到沉浸
    const bool settings_overlay = (st.read_chrome == BookUiState::ReadChrome::kSettings);
    if (!settings_overlay) {
        st.read_chrome = BookUiState::ReadChrome::kReading;
        CallSetStatusVisible(false);
        CallSetSettingsVisible(false);
    }
    CallApplyImmersiveGeometry();
    // 目录等会把 content 设成 flex；正文用绝对坐标居中，必须关掉 layout
    lv_obj_set_style_layout(st.content, LV_LAYOUT_NONE, 0);

    const bool page_images = st.session->IsPageImagesMode();
    // 分页视口与当前几何不一致：重排（宽或高任一过期都会让末行挤进底栏 / 行宽偏短）
    if (!page_images) {
        const lv_coord_t vw = st.session->ViewportWidth();
        const lv_coord_t vh = st.session->ViewportHeight();
        if (st.viewport_w > 40 && st.viewport_h > 40 && vw > 40 && vh > 40 &&
            (vw != st.viewport_w || vh != st.viewport_h) && !st.opening.load() &&
            !st.layout_busy.load() && !st.session->LiveRelayoutPending()) {
            ESP_LOGW(TAG, "viewport mismatch session=%dx%d ui=%dx%d → relayout",
                     static_cast<int>(vw), static_cast<int>(vh),
                     static_cast<int>(st.viewport_w), static_cast<int>(st.viewport_h));
            CallApplyLayoutLive(false);
            return;
        }
    }

    CancelPageImageLoad();
    lv_obj_clean(st.content);
    DisableScroll(st.content);
    ResetReadTouch();
    AttachReadBodyInput(st.content);

    const reader::Page* page = st.session->CurrentPageData();
    if (page == nullptr) {
        ShowMessage(st.content, Lang::Strings::BOOK_PAGE_LOAD_FAILED);
        if (settings_overlay) {
            CallSetStatusVisible(true);
            CallSetSettingsVisible(true);
        }
        return;
    }

    PrewarmPageFont(page);

    const lv_font_t* book_font = BookFont();
    const lv_coord_t max_w = page_images ? LV_HOR_RES : ReadContentWidthLocal();

    // 按本页实际行宽收栏：行未撑满视口时居中整栏，避免满宽 label 左齐造成右空
    lv_coord_t block_w = 0;
    if (!page_images && book_font != nullptr) {
        for (const auto& item : page->items) {
            if (item.kind != reader::ContentKind::kText || item.text.empty()) {
                continue;
            }
            lv_point_t sz = {};
            lv_text_get_size(&sz, item.text.c_str(), book_font, 0, 0, LV_COORD_MAX,
                             LV_TEXT_FLAG_NONE);
            lv_coord_t w = static_cast<lv_coord_t>(sz.x);
            if (item.para_indent) {
                w = static_cast<lv_coord_t>(w + reader::ParaIndentPadPx(book_font));
            }
            if (w > block_w) {
                block_w = w;
            }
        }
    }
    lv_coord_t col_w = max_w;
    if (!page_images && block_w > 40 && block_w < max_w) {
        col_w = block_w;
    }
    const lv_coord_t col_x =
        page_images ? 0 : static_cast<lv_coord_t>((LV_HOR_RES - col_w) / 2);

    const lv_coord_t content_h = lv_obj_get_height(st.content);
    const lv_coord_t line_gap = st.session->LineGap();
    const lv_coord_t para_gap = st.session->ParaGap();
    lv_obj_t* col = lv_obj_create(st.content);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, col_w, page_images ? content_h : LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(col, 0, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(col, page_images ? 0 : line_gap, 0);
    lv_obj_set_pos(col, col_x, 0);
    lv_obj_clear_flag(col, LV_OBJ_FLAG_CLICKABLE);
    DisableScroll(col);

    for (const auto& item : page->items) {
        if (item.kind == reader::ContentKind::kText) {
            if (item.text.empty()) {
                continue;
            }
            lv_obj_t* label =
                reader::CreatePageTextLabel(col, item, col_w, book_font, line_gap, para_gap);
            AttachReaderUnderline(label, BookReaderPrefsUnderlineMode());
        } else {
            const lv_coord_t max_h =
                page_images ? content_h : (content_h * 3 / 4);
            const int img_h = max_h > 40 ? max_h : 200;
            lv_obj_t* slot = lv_obj_create(col);
            lv_obj_remove_style_all(slot);
            lv_obj_set_width(slot, col_w);
            lv_obj_set_height(slot, page_images ? content_h : LV_SIZE_CONTENT);
            lv_obj_clear_flag(slot, LV_OBJ_FLAG_CLICKABLE);

            reader::RasterImage cached;
            if (st.session->TryCopyCachedPageImage(item.image_href, cached) && !cached.empty()) {
                auto* heap_img = new reader::RasterImage(std::move(cached));
                AttachPageImageToSlot(slot, heap_img, page_images);
            } else {
                lv_obj_t* label = lv_label_create(slot);
                lv_label_set_text(label, Lang::Strings::BOOK_IMAGE_PLACEHOLDER);
                lv_obj_set_style_text_font(label, book_font, 0);
                lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
                BookUiState::PageImagePending pending;
                pending.href = item.image_href;
                pending.max_w = col_w;
                pending.max_h = img_h;
                pending.slot = slot;
                st.page_image_pending.push_back(std::move(pending));
            }
        }
    }

    StartPageImageWorker();

    UpdateReadFooter();
    // 重绘阻塞期间 tick 仍走：复位长按计时，避免恢复后一次判定就 LONG_PRESSED
    ResetPointerLongPress();
    if (settings_overlay) {
        CallSetStatusVisible(true);
        CallSetSettingsVisible(true);
    }
}

void ResetReaderPaint() {
    ScreenPaintCoalesceReset(&s_reader_paint);
}

void ClearReaderPageDelta() {
    s_reader_page_delta.store(0, std::memory_order_release);
}

bool HasPendingReaderPageDelta() {
    return s_reader_page_delta.load(std::memory_order_acquire) != 0;
}

void SetReaderHideChromeHandler(ReaderHideChromeFn fn) { s_hide_chrome = fn; }
void SetReaderShowSettingsSheetHandler(ReaderShowSettingsSheetFn fn) { s_show_settings = fn; }
void SetReaderSetStatusVisibleHandler(ReaderSetStatusVisibleFn fn) { s_set_status_visible = fn; }
void SetReaderSetSettingsSheetVisibleHandler(ReaderSetSettingsSheetVisibleFn fn) {
    s_set_settings_visible = fn;
}
void SetReaderApplyImmersiveGeometryHandler(ReaderApplyImmersiveGeometryFn fn) {
    s_apply_immersive_geom = fn;
}
void SetReaderApplyLayoutLiveHandler(ReaderApplyLayoutLiveFn fn) { s_apply_layout_live = fn; }
void SetReaderIsLayoutHintBusyHandler(ReaderIsLayoutHintBusyFn fn) { s_is_layout_hint_busy = fn; }
void SetReaderOnPageImageIdleHandler(ReaderOnPageImageIdleFn fn) { s_on_page_image_idle = fn; }
void SetReaderTruncateHandler(ReaderTruncateFn fn) { s_truncate = fn; }

}  // namespace book_screen_detail
