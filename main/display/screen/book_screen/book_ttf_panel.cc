#include "book_ttf_panel.h"

#include "book_font_manager.h"
#include "book_ui_state.h"
#include "display/font/ttf_convert.h"
#include "haptic_feedback.h"
#include "sd_paths.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <strings.h>

namespace book_screen_detail {
namespace {

constexpr lv_coord_t kShelfUnderlineH = 1;
constexpr lv_coord_t kShelfUnderlinePadHor = 5;
constexpr lv_coord_t kTocLineThin = 1;
constexpr lv_coord_t kSheetIconBtn = 36;
constexpr lv_coord_t kSheetBorderW = 2;
constexpr lv_coord_t kFontListRowH = 44;

TtfFontsChangedFn s_fonts_changed = nullptr;
TtfEfTemplateFn s_ef_template = nullptr;

void DisableScroll(lv_obj_t* obj) {
    if (obj == nullptr) {
        return;
    }
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(obj, LV_DIR_NONE);
}

void SetSheetBtnEnabled(lv_obj_t* btn, bool enabled) {
    if (btn == nullptr || !lv_obj_is_valid(btn)) {
        return;
    }
    if (enabled) {
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(btn, LV_OPA_COVER, 0);
    } else {
        lv_obj_clear_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_opa(btn, LV_OPA_40, 0);
    }
}

lv_obj_t* MakeSheetIconBtn(lv_obj_t* parent, const char* txt, lv_event_cb_t cb, bool enabled) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, kSheetIconBtn, kSheetIconBtn);
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, kSheetBorderW, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_radius(btn, kSheetIconBtn / 2, 0);
    DisableScroll(btn);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(
        btn, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED, nullptr);
    SetSheetBtnEnabled(btn, enabled);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, txt);
    lv_obj_set_style_text_font(lbl, book_font::ListFont(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return btn;
}

void NotifyFontsChanged() {
    if (s_fonts_changed != nullptr) {
        s_fonts_changed();
    }
}

void FillEfTemplatePath(char* out, size_t out_len) {
    if (out == nullptr || out_len == 0) {
        return;
    }
    out[0] = '\0';
    if (s_ef_template != nullptr) {
        s_ef_template(out, out_len);
    }
    if (out[0] == '\0') {
        std::snprintf(out, out_len, "%s", SD_PATH_BOOK_FONT);
    }
}

}  // namespace

static bool IsTtfFileName(const char* name);
static int TtfPageCount();
static void TtfScanFiles();
void TtfEnsurePanel();
static void TtfRebuildContent();
static void TtfStartConvert();
static void OnTtfRowClick(lv_event_t* e);
static void OnTtfPagePrev(lv_event_t* e);
static void OnTtfPageNext(lv_event_t* e);
static void OnTtfConfirmYes(lv_event_t* e);
static void OnTtfConfirmNo(lv_event_t* e);
static void OnTtfClose(lv_event_t* e);
static void OnTtfResultOk(lv_event_t* e);
static void OnTtfSizeDec(lv_event_t* e);
static void OnTtfSizeInc(lv_event_t* e);
static void ClampTtfSizePx();
static void RefreshTtfConfirmTexts();
static void TtfPollTimerCb(lv_timer_t* t);


// ---------- TTF 导入转换面板（覆盖设置卡；转换 worker 独立，UI 只轮询） ----------

static bool IsTtfFileName(const char* name) {
    const size_t n = std::strlen(name);
    if (n < 5 || n > 63) {
        return false;
    }
    return strcasecmp(name + n - 4, ".ttf") == 0 || strcasecmp(name + n - 4, ".otf") == 0;
}

static int TtfPageCount() {
    const int n = static_cast<int>(State().ttf_files.size());
    if (n <= 0) {
        return 1;
    }
    return (n + kFontListPageSize - 1) / kFontListPageSize;
}

static void TtfScanFiles() {
    auto& st = State();
    st.ttf_files.clear();
    DIR* dir = opendir(SD_PATH_FONT_SRC);
    if (dir == nullptr) {
        return;
    }
    while (dirent* ent = readdir(dir)) {
        if (ent->d_name[0] == '.' || !IsTtfFileName(ent->d_name)) {
            continue;
        }
        st.ttf_files.push_back(ent->d_name);
    }
    closedir(dir);
    std::sort(st.ttf_files.begin(), st.ttf_files.end());
}

void TtfPanelClose() {
    auto& st = State();
    if (st.ttf_sheet != nullptr && lv_obj_is_valid(st.ttf_sheet)) {
        lv_obj_del(st.ttf_sheet);
    }
    st.ttf_sheet = nullptr;
    st.ttf_bar = nullptr;
    st.ttf_pct_lbl = nullptr;
    st.ttf_msg_lbl = nullptr;
    st.ttf_size_value = nullptr;
    st.ttf_size_dec = nullptr;
    st.ttf_size_inc = nullptr;
    for (int i = 0; i < kFontListPageSize; ++i) {
        st.ttf_rows[i] = {};
    }
    st.ttf_mode = BookUiState::TtfMode::kNone;
}

void StopTtfPollTimer() {
    auto& st = State();
    if (st.ttf_poll_timer != nullptr) {
        lv_timer_del(st.ttf_poll_timer);
        st.ttf_poll_timer = nullptr;
    }
}

void TtfEnsurePanel() {
    auto& st = State();
    if (st.settings_sheet == nullptr || !lv_obj_is_valid(st.settings_sheet)) {
        return;
    }
    // 设置卡是 COLUMN + SIZE_CONTENT：overlay 必须 FLOATING，否则会被 flex 排到卡底，
    // 且相对 content 父级的 pct(100) 高度常为 0 → 点「导入 TTF」像没反应。
    lv_obj_update_layout(st.settings_sheet);
    if (st.ttf_sheet != nullptr && lv_obj_is_valid(st.ttf_sheet)) {
        lv_obj_set_size(st.ttf_sheet, lv_pct(100), lv_pct(100));
        lv_obj_align(st.ttf_sheet, LV_ALIGN_TOP_LEFT, 0, 0);
        lv_obj_move_foreground(st.ttf_sheet);
        TtfRebuildContent();
        return;
    }
    st.ttf_sheet = lv_obj_create(st.settings_sheet);
    lv_obj_remove_style_all(st.ttf_sheet);
    lv_obj_add_flag(st.ttf_sheet, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(st.ttf_sheet, lv_pct(100), lv_pct(100));
    lv_obj_align(st.ttf_sheet, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(st.ttf_sheet, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(st.ttf_sheet, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(st.ttf_sheet, 12, 0);
    lv_obj_set_style_pad_all(st.ttf_sheet, 16, 0);
    lv_obj_set_style_pad_row(st.ttf_sheet, 10, 0);
    lv_obj_set_flex_flow(st.ttf_sheet, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(st.ttf_sheet, LV_OBJ_FLAG_CLICKABLE);
    DisableScroll(st.ttf_sheet);
    lv_obj_move_foreground(st.ttf_sheet);
    TtfRebuildContent();
}

static void TtfRebuildContent() {
    auto& st = State();
    if (st.ttf_sheet == nullptr || !lv_obj_is_valid(st.ttf_sheet)) {
        return;
    }
    lv_obj_clean(st.ttf_sheet);
    for (int i = 0; i < kFontListPageSize; ++i) {
        st.ttf_rows[i] = {};
    }
    st.ttf_bar = nullptr;
    st.ttf_pct_lbl = nullptr;
    st.ttf_msg_lbl = nullptr;
    st.ttf_size_value = nullptr;
    st.ttf_size_dec = nullptr;
    st.ttf_size_inc = nullptr;

    const bool is_progress = (st.ttf_mode == BookUiState::TtfMode::kProgress);
    auto make_action = [](lv_obj_t* parent, const char* text, lv_event_cb_t cb) -> lv_obj_t* {
        lv_obj_t* btn = lv_obj_create(parent);
        lv_obj_remove_style_all(btn);
        lv_obj_set_height(btn, 36);
        lv_obj_set_width(btn, LV_SIZE_CONTENT);
        lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
        lv_obj_set_style_pad_hor(btn, 4, 0);
        lv_obj_set_flex_flow(btn, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(btn, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        DisableScroll(btn);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(btn);
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, nullptr);
        lv_obj_add_event_cb(
            btn, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED, nullptr);
        lv_obj_t* wrap = lv_obj_create(btn);
        lv_obj_remove_style_all(wrap);
        lv_obj_set_size(wrap, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_style_border_side(wrap, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(wrap, kShelfUnderlineH, 0);
        lv_obj_set_style_border_color(wrap, lv_color_black(), 0);
        lv_obj_set_style_pad_bottom(wrap, 2, 0);
        lv_obj_set_style_pad_hor(wrap, kShelfUnderlinePadHor, 0);
        lv_obj_set_style_bg_opa(wrap, LV_OPA_TRANSP, 0);
        DisableScroll(wrap);
        lv_obj_clear_flag(wrap, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_t* lbl = lv_label_create(wrap);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_font(lbl, book_font::ItemFont(), 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        return btn;
    };

    // 头部：标题 + 关闭（进度模式不可关）
    lv_obj_t* head = lv_obj_create(st.ttf_sheet);
    lv_obj_remove_style_all(head);
    lv_obj_set_width(head, lv_pct(100));
    lv_obj_set_height(head, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_CLICKABLE);
    DisableScroll(head);
    const char* title = "";
    switch (st.ttf_mode) {
        case BookUiState::TtfMode::kList: title = "选择 TTF 字体"; break;
        case BookUiState::TtfMode::kConfirm: title = "导入 TTF"; break;
        case BookUiState::TtfMode::kProgress: title = "转换中"; break;
        case BookUiState::TtfMode::kResult: title = st.ttf_result_ok ? "转换完成" : "转换失败"; break;
        default: break;
    }
    lv_obj_t* title_lbl = lv_label_create(head);
    lv_label_set_text(title_lbl, title);
    lv_obj_set_style_text_font(title_lbl, book_font::ListFont(), 0);
    lv_obj_clear_flag(title_lbl, LV_OBJ_FLAG_CLICKABLE);
    if (!is_progress) {
        make_action(head, "关闭", OnTtfClose);
    }

    switch (st.ttf_mode) {
        case BookUiState::TtfMode::kList: {
            if (st.ttf_files.empty()) {
                lv_obj_t* empty = lv_label_create(st.ttf_sheet);
                lv_label_set_text(
                    empty, "未找到 TTF/OTF 字体\n请将字体文件放入 SD 卡 metalio/e-ink/fonts_ttf 目录");
                lv_label_set_long_mode(empty, LV_LABEL_LONG_WRAP);
                lv_obj_set_width(empty, lv_pct(100));
                lv_obj_set_style_text_font(empty, book_font::ItemFont(), 0);
                lv_obj_set_style_text_opa(empty, LV_OPA_70, 0);
                lv_obj_clear_flag(empty, LV_OBJ_FLAG_CLICKABLE);
                break;
            }
            lv_obj_t* list = lv_obj_create(st.ttf_sheet);
            lv_obj_remove_style_all(list);
            lv_obj_set_width(list, lv_pct(100));
            lv_obj_set_height(list, kFontListRowH * kFontListPageSize);
            lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
            lv_obj_clear_flag(list, LV_OBJ_FLAG_CLICKABLE);
            DisableScroll(list);
            const int start = st.ttf_page * kFontListPageSize;
            for (int i = 0; i < kFontListPageSize; ++i) {
                const int idx = start + i;
                lv_obj_t* row = lv_obj_create(list);
                lv_obj_remove_style_all(row);
                lv_obj_set_width(row, lv_pct(100));
                lv_obj_set_height(row, kFontListRowH);
                lv_obj_set_style_pad_hor(row, 12, 0);
                lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
                lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                                      LV_FLEX_ALIGN_CENTER);
                if (i + 1 < kFontListPageSize) {
                    lv_obj_set_style_border_width(row, kTocLineThin, 0);
                    lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
                    lv_obj_set_style_border_color(row, lv_color_black(), 0);
                }
                DisableScroll(row);
                lv_obj_t* name = lv_label_create(row);
                lv_obj_set_flex_grow(name, 1);
                lv_label_set_long_mode(name, LV_LABEL_LONG_CLIP);
                lv_obj_set_style_text_font(name, book_font::ItemFont(), 0);
                lv_obj_clear_flag(name, LV_OBJ_FLAG_CLICKABLE);
                st.ttf_rows[i].row = row;
                st.ttf_rows[i].name = name;
                if (idx >= static_cast<int>(st.ttf_files.size())) {
                    lv_label_set_text(name, "");
                    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
                    lv_obj_set_user_data(row, reinterpret_cast<void*>(static_cast<intptr_t>(-1)));
                    continue;
                }
                HapticAttachClick(row);
                lv_obj_add_event_cb(row, OnTtfRowClick, LV_EVENT_CLICKED, nullptr);
                lv_obj_add_event_cb(
                    row, [](lv_event_t* ev) { lv_event_stop_bubbling(ev); }, LV_EVENT_CLICKED,
                    nullptr);
                lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_set_user_data(row, reinterpret_cast<void*>(static_cast<intptr_t>(idx)));
                lv_label_set_text(name, st.ttf_files[static_cast<size_t>(idx)].c_str());
            }
            const int pages = TtfPageCount();
            if (pages > 1) {
                lv_obj_t* pager = lv_obj_create(st.ttf_sheet);
                lv_obj_remove_style_all(pager);
                lv_obj_set_width(pager, lv_pct(100));
                lv_obj_set_height(pager, 36);
                lv_obj_set_flex_flow(pager, LV_FLEX_FLOW_ROW);
                lv_obj_set_flex_align(pager, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                                      LV_FLEX_ALIGN_CENTER);
                lv_obj_clear_flag(pager, LV_OBJ_FLAG_CLICKABLE);
                DisableScroll(pager);
                make_action(pager, "▲", OnTtfPagePrev);
                char meta[32];
                std::snprintf(meta, sizeof(meta), "%d / %d", st.ttf_page + 1, pages);
                lv_obj_t* lab = lv_label_create(pager);
                lv_label_set_text(lab, meta);
                lv_obj_set_style_text_font(lab, book_font::ItemFont(), 0);
                lv_obj_set_style_text_opa(lab, LV_OPA_70, 0);
                lv_obj_clear_flag(lab, LV_OBJ_FLAG_CLICKABLE);
                make_action(pager, "▼", OnTtfPageNext);
            }
            break;
        }
        case BookUiState::TtfMode::kConfirm: {
            ClampTtfSizePx();
            lv_obj_t* msg = lv_label_create(st.ttf_sheet);
            lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(msg, lv_pct(100));
            lv_obj_set_style_text_font(msg, book_font::ItemFont(), 0);
            lv_obj_clear_flag(msg, LV_OBJ_FLAG_CLICKABLE);
            st.ttf_msg_lbl = msg;

            // 字号步进器：20–40，步进 1，默认 25；只生成一档
            lv_obj_t* size_row = lv_obj_create(st.ttf_sheet);
            lv_obj_remove_style_all(size_row);
            lv_obj_set_width(size_row, lv_pct(100));
            lv_obj_set_height(size_row, kSheetIconBtn);
            lv_obj_set_flex_flow(size_row, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(size_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_clear_flag(size_row, LV_OBJ_FLAG_CLICKABLE);
            DisableScroll(size_row);
            lv_obj_t* size_lab = lv_label_create(size_row);
            lv_label_set_text(size_lab, "字号");
            lv_obj_set_style_text_font(size_lab, book_font::ItemFont(), 0);
            lv_obj_clear_flag(size_lab, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_t* stepper = lv_obj_create(size_row);
            lv_obj_remove_style_all(stepper);
            lv_obj_set_height(stepper, kSheetIconBtn);
            lv_obj_set_width(stepper, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(stepper, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(stepper, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_column(stepper, 10, 0);
            lv_obj_clear_flag(stepper, LV_OBJ_FLAG_CLICKABLE);
            DisableScroll(stepper);
            st.ttf_size_dec =
                MakeSheetIconBtn(stepper, "-", OnTtfSizeDec, st.ttf_size_px > kTtfConvertSizeMin);
            lv_obj_t* size_val = lv_label_create(stepper);
            char size_txt[16];
            std::snprintf(size_txt, sizeof(size_txt), "%dpx", st.ttf_size_px);
            lv_label_set_text(size_val, size_txt);
            lv_obj_set_style_text_font(size_val, book_font::ListFont(), 0);
            lv_obj_clear_flag(size_val, LV_OBJ_FLAG_CLICKABLE);
            st.ttf_size_value = size_val;
            st.ttf_size_inc =
                MakeSheetIconBtn(stepper, "+", OnTtfSizeInc, st.ttf_size_px < kTtfConvertSizeMax);

            RefreshTtfConfirmTexts();

            lv_obj_t* btns = lv_obj_create(st.ttf_sheet);
            lv_obj_remove_style_all(btns);
            lv_obj_set_width(btns, lv_pct(100));
            lv_obj_set_height(btns, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_set_style_pad_hor(btns, 24, 0);
            lv_obj_clear_flag(btns, LV_OBJ_FLAG_CLICKABLE);
            DisableScroll(btns);
            make_action(btns, "开始转换", OnTtfConfirmYes);
            make_action(btns, "取消", OnTtfConfirmNo);
            break;
        }
        case BookUiState::TtfMode::kProgress: {
            ClampTtfSizePx();
            const uint16_t sz = static_cast<uint16_t>(st.ttf_size_px);
            char out_name[80];
            ttf_convert::FormatOutputFileList(st.ttf_pick.c_str(), &sz, 1, out_name,
                                              sizeof(out_name));
            lv_obj_t* msg = lv_label_create(st.ttf_sheet);
            char mbuf[200];
            std::snprintf(mbuf, sizeof(mbuf),
                          "正在转换：\n%.40s\n→ %s\n请勿拔出 SD 卡…", st.ttf_pick.c_str(),
                          out_name);
            lv_label_set_text(msg, mbuf);
            lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(msg, lv_pct(100));
            lv_obj_set_style_text_font(msg, book_font::ItemFont(), 0);
            lv_obj_clear_flag(msg, LV_OBJ_FLAG_CLICKABLE);
            st.ttf_msg_lbl = msg;

            lv_obj_t* bar = lv_bar_create(st.ttf_sheet);
            lv_obj_set_size(bar, lv_pct(100), 16);
            lv_bar_set_range(bar, 0, 100);
            lv_bar_set_value(bar, ttf_convert::Percent(), LV_ANIM_OFF);
            lv_obj_set_style_bg_color(bar, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(bar, lv_color_black(), 0);
            lv_obj_set_style_border_width(bar, 2, 0);
            lv_obj_set_style_radius(bar, 4, 0);
            lv_obj_set_style_bg_color(bar, lv_color_black(), LV_PART_INDICATOR);
            lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
            st.ttf_bar = bar;

            lv_obj_t* pct = lv_label_create(st.ttf_sheet);
            char pbuf[16];
            std::snprintf(pbuf, sizeof(pbuf), "%d%%", ttf_convert::Percent());
            lv_label_set_text(pct, pbuf);
            lv_obj_set_style_text_font(pct, book_font::ListFont(), 0);
            lv_obj_clear_flag(pct, LV_OBJ_FLAG_CLICKABLE);
            st.ttf_pct_lbl = pct;

            lv_obj_t* tip = lv_label_create(st.ttf_sheet);
            lv_label_set_text(tip, "转换完成前，请勿退出页面");
            lv_label_set_long_mode(tip, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(tip, lv_pct(100));
            lv_obj_set_style_text_font(tip, book_font::ItemFont(), 0);
            lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_clear_flag(tip, LV_OBJ_FLAG_CLICKABLE);
            break;
        }
        case BookUiState::TtfMode::kResult: {
            lv_obj_t* msg = lv_label_create(st.ttf_sheet);
            if (st.ttf_result_ok) {
                const uint16_t sz = static_cast<uint16_t>(st.ttf_size_px);
                char out_name[80];
                ttf_convert::FormatOutputFileList(st.ttf_pick.c_str(), &sz, 1, out_name,
                                                  sizeof(out_name));
                char buf[160];
                std::snprintf(buf, sizeof(buf), "转换完成：\n%s\n已加入字库列表。", out_name);
                lv_label_set_text(msg, buf);
            } else {
                char buf[96];
                std::snprintf(buf, sizeof(buf), "转换失败：%.60s", ttf_convert::Error());
                lv_label_set_text(msg, buf);
            }
            lv_label_set_long_mode(msg, LV_LABEL_LONG_WRAP);
            lv_obj_set_width(msg, lv_pct(100));
            lv_obj_set_style_text_font(msg, book_font::ItemFont(), 0);
            lv_obj_clear_flag(msg, LV_OBJ_FLAG_CLICKABLE);
            st.ttf_msg_lbl = msg;
            lv_obj_t* btns = lv_obj_create(st.ttf_sheet);
            lv_obj_remove_style_all(btns);
            lv_obj_set_width(btns, lv_pct(100));
            lv_obj_set_height(btns, LV_SIZE_CONTENT);
            lv_obj_set_flex_flow(btns, LV_FLEX_FLOW_ROW);
            lv_obj_set_flex_align(btns, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                                  LV_FLEX_ALIGN_CENTER);
            lv_obj_clear_flag(btns, LV_OBJ_FLAG_CLICKABLE);
            DisableScroll(btns);
            make_action(btns, "确定", OnTtfResultOk);
            break;
        }
        default:
            break;
    }
}

void OnFontImportClick(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = State();
    if (st.opening.load() || ttf_convert::Busy()) {
        return;
    }
    if (st.font_multi) {
        return;
    }
    TtfScanFiles();
    st.ttf_page = 0;
    st.ttf_mode = BookUiState::TtfMode::kList;
    TtfEnsurePanel();
}

static void OnTtfRowClick(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = State();
    if (st.opening.load()) {
        return;
    }
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    const int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_obj_get_user_data(target)));
    if (idx < 0 || idx >= static_cast<int>(st.ttf_files.size())) {
        return;
    }
    st.ttf_pick = st.ttf_files[static_cast<size_t>(idx)];
    st.ttf_size_px = kTtfConvertSizeDefault;
    st.ttf_mode = BookUiState::TtfMode::kConfirm;
    TtfRebuildContent();
}

static void ClampTtfSizePx() {
    auto& st = State();
    if (st.ttf_size_px < kTtfConvertSizeMin) {
        st.ttf_size_px = kTtfConvertSizeMin;
    } else if (st.ttf_size_px > kTtfConvertSizeMax) {
        st.ttf_size_px = kTtfConvertSizeMax;
    }
}

static void RefreshTtfConfirmTexts() {
    auto& st = State();
    ClampTtfSizePx();
    if (st.ttf_size_value != nullptr && lv_obj_is_valid(st.ttf_size_value)) {
        char size_txt[16];
        std::snprintf(size_txt, sizeof(size_txt), "%dpx", st.ttf_size_px);
        lv_label_set_text(st.ttf_size_value, size_txt);
    }
    SetSheetBtnEnabled(st.ttf_size_dec, st.ttf_size_px > kTtfConvertSizeMin);
    SetSheetBtnEnabled(st.ttf_size_inc, st.ttf_size_px < kTtfConvertSizeMax);
    if (st.ttf_msg_lbl != nullptr && lv_obj_is_valid(st.ttf_msg_lbl)) {
        const uint16_t sz = static_cast<uint16_t>(st.ttf_size_px);
        char out_name[80];
        ttf_convert::FormatOutputFileList(st.ttf_pick.c_str(), &sz, 1, out_name, sizeof(out_name));
        char buf[200];
        std::snprintf(buf, sizeof(buf),
                      "将 %.32s 转为：\n%s\n约需 1-2 分钟，同名 .ef 将被覆盖。",
                      st.ttf_pick.c_str(), out_name);
        lv_label_set_text(st.ttf_msg_lbl, buf);
    }
}

static void OnTtfSizeDec(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = State();
    if (st.ttf_mode != BookUiState::TtfMode::kConfirm || st.opening.load() ||
        ttf_convert::Busy()) {
        return;
    }
    if (st.ttf_size_px > kTtfConvertSizeMin) {
        --st.ttf_size_px;
        RefreshTtfConfirmTexts();
    }
}

static void OnTtfSizeInc(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = State();
    if (st.ttf_mode != BookUiState::TtfMode::kConfirm || st.opening.load() ||
        ttf_convert::Busy()) {
        return;
    }
    if (st.ttf_size_px < kTtfConvertSizeMax) {
        ++st.ttf_size_px;
        RefreshTtfConfirmTexts();
    }
}

static void OnTtfPagePrev(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = State();
    if (st.ttf_page > 0) {
        --st.ttf_page;
        TtfRebuildContent();
    }
}

static void OnTtfPageNext(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = State();
    if (st.ttf_page + 1 < TtfPageCount()) {
        ++st.ttf_page;
        TtfRebuildContent();
    }
}

static void TtfStartConvert() {
    auto& st = State();
    ClampTtfSizePx();
    // 字集模板：当前选中 .ef；无则回退默认 misans（路径由 book_screen 钩子提供）
    char tmpl[192];
    FillEfTemplatePath(tmpl, sizeof(tmpl));
    const std::string ttf_path = std::string(SD_PATH_FONT_SRC) + "/" + st.ttf_pick;
    const uint16_t sizes[1] = {static_cast<uint16_t>(st.ttf_size_px)};
    if (!ttf_convert::Start(ttf_path.c_str(), tmpl, SD_PATH_FONTS, sizes, 1)) {
        st.ttf_result_ok = false;
        st.ttf_mode = BookUiState::TtfMode::kResult;
        TtfRebuildContent();
        return;
    }
    st.ttf_mode = BookUiState::TtfMode::kProgress;
    TtfRebuildContent();
    if (st.ttf_poll_timer == nullptr) {
        st.ttf_poll_timer = lv_timer_create(TtfPollTimerCb, 500, nullptr);
    }
}

static void OnTtfConfirmYes(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = State();
    if (st.opening.load() || ttf_convert::Busy()) {
        return;
    }
    TtfStartConvert();
}

static void OnTtfConfirmNo(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    auto& st = State();
    st.ttf_mode = BookUiState::TtfMode::kList;
    TtfRebuildContent();
}

static void OnTtfClose(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    TtfPanelClose();
}

static void OnTtfResultOk(lv_event_t* e) {
    lv_event_stop_bubbling(e);
    TtfPanelClose();
}

static void TtfPollTimerCb(lv_timer_t* t) {
    auto& st = State();
    if (ttf_convert::Busy()) {
        if (st.ttf_bar != nullptr && lv_obj_is_valid(st.ttf_bar)) {
            lv_bar_set_value(st.ttf_bar, ttf_convert::Percent(), LV_ANIM_OFF);
        }
        if (st.ttf_pct_lbl != nullptr && lv_obj_is_valid(st.ttf_pct_lbl)) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), "%d%%", ttf_convert::Percent());
            lv_label_set_text(st.ttf_pct_lbl, buf);
        }
        return;
    }
    const int s = ttf_convert::State();
    lv_timer_del(t);
    st.ttf_poll_timer = nullptr;
    // 阅读屏已拆掉：只收尾定时器，勿碰已销毁控件
    if (st.read_scr == nullptr || !lv_obj_is_valid(st.read_scr)) {
        st.ttf_mode = BookUiState::TtfMode::kNone;
        return;
    }
    st.ttf_result_ok = (s == 2);
    if (st.ttf_mode == BookUiState::TtfMode::kProgress && st.settings_sheet != nullptr &&
        lv_obj_is_valid(st.settings_sheet) && st.ttf_sheet != nullptr &&
        lv_obj_is_valid(st.ttf_sheet)) {
        st.ttf_mode = BookUiState::TtfMode::kResult;
        TtfRebuildContent();
        NotifyFontsChanged();
    } else {
        // 面板已关：静默收尾；字库仍刷新，下次打开设置可见
        st.ttf_mode = BookUiState::TtfMode::kNone;
        NotifyFontsChanged();
    }
}


void SetTtfFontsChangedHandler(TtfFontsChangedFn fn) {
    s_fonts_changed = fn;
}

void SetTtfEfTemplateHandler(TtfEfTemplateFn fn) {
    s_ef_template = fn;
}

void ClearTtfWidgetRefs() {
    auto& st = State();
    st.ttf_sheet = nullptr;
    for (int i = 0; i < kFontListPageSize; ++i) {
        st.ttf_rows[i] = {};
    }
    st.ttf_bar = nullptr;
    st.ttf_pct_lbl = nullptr;
    st.ttf_msg_lbl = nullptr;
    st.ttf_size_value = nullptr;
    st.ttf_size_dec = nullptr;
    st.ttf_size_inc = nullptr;
}

}  // namespace book_screen_detail
