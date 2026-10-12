#include "settings_apps_tab.h"

#include "app_manager.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "settings_common.h"

namespace {

constexpr int kMaxApps = 16;
constexpr lv_coord_t kRowH = 56;
constexpr lv_coord_t kRowGap = 8;   // 行间距(与原内容页 kSettingsOptionGap 视觉一致)

struct AppsUi {
    lv_obj_t* rows[kMaxApps] = {};
    lv_obj_t* state_lbls[kMaxApps] = {};
    int count = 0;
};
AppsUi s_ui;

const lv_font_t* UiFont() { return fontpack_lv_font_ui(); }

lv_obj_t* MakeText(lv_obj_t* parent, const char* text, int font_size = 25) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text != nullptr ? text : "");
    const lv_font_t* font = font_size > 25 ? fontpack_lv_font_get(font_size, 2) : UiFont();
    lv_obj_set_style_text_font(lbl, font != nullptr ? font : UiFont(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, lv_pct(100));
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return lbl;
}

void RefreshRow(int index) {
    if (index < 0 || index >= s_ui.count || s_ui.state_lbls[index] == nullptr) return;
    if (AppManager::GetInstance().IsAppMandatoryByIndex(index)) {
        lv_label_set_text(s_ui.state_lbls[index], "常驻");
        return;
    }
    lv_label_set_text(s_ui.state_lbls[index],
                      AppManager::GetInstance().IsAppEnabledByIndex(index) ? "开" : "关");
}

void OnAppRowClicked(lv_event_t* e) {
    const int index = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (index < 0 || index >= s_ui.count) return;
    if (AppManager::GetInstance().IsAppMandatoryByIndex(index)) return;   // 设置永驻不可关
    HapticPulseIfEnabled();
    AppManager::GetInstance().SetAppEnabled(index, !AppManager::GetInstance().IsAppEnabledByIndex(index));
    RefreshRow(index);
}

lv_obj_t* MakeAppRow(lv_obj_t* parent, int index) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), kRowH);
    lv_obj_set_style_bg_color(row, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(row, lv_color_black(), 0);
    lv_obj_set_style_border_width(row, 2, 0);
    lv_obj_set_style_radius(row, 12, 0);
    lv_obj_set_style_pad_left(row, 14, 0);
    lv_obj_set_style_pad_right(row, 14, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(row);
    lv_obj_add_event_cb(row, OnAppRowClicked, LV_EVENT_CLICKED,
                        reinterpret_cast<void*>(static_cast<intptr_t>(index)));

    lv_obj_t* name = lv_label_create(row);
    lv_label_set_text(name, AppManager::GetInstance().GetAppNameByIndex(index));
    lv_obj_set_style_text_font(name, UiFont(), 0);
    lv_obj_set_style_text_color(name, lv_color_black(), 0);
    lv_obj_set_flex_grow(name, 1);
    lv_obj_clear_flag(name, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* state = lv_label_create(row);
    lv_obj_set_style_text_font(state, UiFont(), 0);
    lv_obj_set_style_text_color(state, lv_color_black(), 0);
    lv_obj_clear_flag(state, LV_OBJ_FLAG_CLICKABLE);
    s_ui.state_lbls[index] = state;
    return row;
}

}  // namespace

void SettingsAppsTab_Reset() { s_ui = {}; }

void SettingsAppsTab_Build(lv_obj_t* page) {
    s_ui = {};
    s_ui.count = AppManager::GetInstance().GetAppCount();
    if (s_ui.count > kMaxApps) s_ui.count = kMaxApps;

    MakeText(page, "应用显示", 30);
    MakeText(page, "关闭的 APP 不在首页显示,也不再注册 MCP 工具(重启后生效);\"设置\"常驻(关了就没法再进这里)。列表可上下滑动。");

    // 行列表独立可滚动容器:标题/说明固定,行数超屏时上下滑动。
    // 滑动 vs 点按由 LVGL 原生区分:拖动超 scroll limit 后给被按行发
    // LV_EVENT_PRESS_LOST,CLICKED 不触发,滑动不会误开关应用。
    lv_obj_t* list = lv_obj_create(page);
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, lv_pct(100));
    lv_obj_set_flex_grow(list, 1);   // 吃满页面剩余高度,超出的行滚动看
    lv_obj_set_style_pad_row(list, kRowGap, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < s_ui.count; ++i) {
        s_ui.rows[i] = MakeAppRow(list, i);
        RefreshRow(i);
    }
}
