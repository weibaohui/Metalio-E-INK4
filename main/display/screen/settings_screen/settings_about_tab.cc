#include "settings_about_tab.h"

#include "application.h"
#include "assets/lang_config.h"
#include "board.h"
#include "display.h"
#include "fontpack_lvgl.h"
#include "reader/file_size.h"
#include "settings_common.h"
#include "system_info.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include <esp_app_desc.h>
#include <esp_chip_info.h>
#include <esp_log.h>
#include <esp_lv_adapter.h>
#include <esp_psram.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

namespace {

constexpr const char* TAG = "SettingsAbout";
constexpr lv_coord_t kInfoRowH = 72;
constexpr lv_coord_t kInfoRowGap = 8;
constexpr lv_coord_t kInfoRowPadH = 12;
constexpr lv_coord_t kInfoRowPadV = 8;

std::atomic<bool> s_tab_alive{false};

struct InfoItem {
    const char* label;
    char value[96];
};

void CollectInfoItems(InfoItem* items, int* count) {
    const auto* app_desc = esp_app_get_description();
    esp_chip_info_t chip_info;
    esp_chip_info(&chip_info);

    int idx = 0;

    items[idx].label = Lang::Strings::SETTINGS_ABOUT_MODEL;
    std::snprintf(items[idx].value, sizeof(items[idx].value), "%s", BOARD_NAME);
    ++idx;

    items[idx].label = Lang::Strings::SETTINGS_ABOUT_CHIP;
    {
        const std::string chip = SystemInfo::GetChipModelName();
        std::snprintf(items[idx].value, sizeof(items[idx].value), "%s", chip.c_str());
    }
    ++idx;

    items[idx].label = Lang::Strings::SETTINGS_ABOUT_CORES;
    std::snprintf(items[idx].value, sizeof(items[idx].value), Lang::Strings::SETTINGS_ABOUT_CORES_FMT,
                  static_cast<unsigned>(chip_info.cores));
    ++idx;

    items[idx].label = Lang::Strings::SETTINGS_ABOUT_FW;
    std::snprintf(items[idx].value, sizeof(items[idx].value), "%s", app_desc->version);
    ++idx;

    items[idx].label = Lang::Strings::SETTINGS_ABOUT_BUILD;
    std::snprintf(items[idx].value, sizeof(items[idx].value), "%s %s", app_desc->date,
                  app_desc->time);
    ++idx;

    items[idx].label = Lang::Strings::SETTINGS_ABOUT_MAC;
    {
        const std::string mac = SystemInfo::GetMacAddress();
        std::snprintf(items[idx].value, sizeof(items[idx].value), "%s", mac.c_str());
    }
    ++idx;

    items[idx].label = Lang::Strings::SETTINGS_ABOUT_FLASH;
    reader::FormatFileSize(items[idx].value, sizeof(items[idx].value), SystemInfo::GetFlashSize());
    ++idx;

    items[idx].label = Lang::Strings::SETTINGS_ABOUT_PSRAM;
    {
        const size_t psram_total = esp_psram_get_size();
        if (psram_total == 0) {
            std::snprintf(items[idx].value, sizeof(items[idx].value), Lang::Strings::SETTINGS_ABOUT_NONE);
        } else {
            reader::FormatFileSize(items[idx].value, sizeof(items[idx].value), psram_total);
        }
    }
    ++idx;

    *count = idx;
}

lv_obj_t* CreateInfoRow(lv_obj_t* parent, const InfoItem& item, bool with_divider) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, kInfoRowH);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    if (with_divider) {
        lv_obj_set_style_border_side(row, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_border_color(row, lv_color_black(), 0);
    }
    lv_obj_set_style_radius(row, 0, 0);
    lv_obj_set_style_pad_hor(row, kInfoRowPadH, 0);
    lv_obj_set_style_pad_ver(row, kInfoRowPadV, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(row, 2, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* title = lv_label_create(row);
    lv_label_set_text(title, item.label);
    lv_obj_set_width(title, lv_pct(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(title, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* subtitle = lv_label_create(row);
    lv_label_set_text(subtitle, item.value);
    lv_obj_set_width(subtitle, lv_pct(100));
    lv_label_set_long_mode(subtitle, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(subtitle, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(subtitle, lv_color_black(), 0);
    lv_obj_set_style_text_align(subtitle, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_clear_flag(subtitle, LV_OBJ_FLAG_CLICKABLE);

    return row;
}

}  // namespace

void SettingsAboutTab_Reset() {
    s_tab_alive.store(false, std::memory_order_release);
}

void SettingsAboutTab_Build(lv_obj_t* page) {
    s_tab_alive.store(true, std::memory_order_release);

    InfoItem items[8];
    int count = 0;
    CollectInfoItems(items, &count);

    lv_obj_t* list = lv_obj_create(page);
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, lv_pct(100));
    lv_obj_set_height(list, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(list, kInfoRowGap, 0);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(list, LV_OBJ_FLAG_CLICKABLE);

    for (int i = 0; i < count; ++i) {
        CreateInfoRow(list, items[i], i + 1 < count);
    }
}
