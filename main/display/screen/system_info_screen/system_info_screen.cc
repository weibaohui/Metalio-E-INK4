// main/display/screen/system_info_screen/system_info_screen.cc —— 见 .h。
#include "system_info_screen/system_info_screen.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_timer.h>
#include <ff.h>   // f_getfree(SD 卡容量,与设置→存储 Tab 同法)
#include <nvs.h>

#include "SdCardManager.hpp"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "screen_common.h"
#include "system_info.h"
#include "vk_key_handler.h"

namespace {

constexpr const char* TAG = "SysInfoScreen";
constexpr const char* kScreenId = "sysinfo";
constexpr int kMaxNvsLines = 128;   // NVS 键名展示上限(防极端情况刷屏过长)

struct SysInfoUi {
    lv_obj_t* screen = nullptr;
    lv_obj_t* body = nullptr;       // 可滚动内容区;刷新时整体重建(按钮在头部,不在其内)
};
SysInfoUi s_ui;
bool s_alive = false;

const lv_font_t* UiFont() { return fontpack_lv_font_ui(); }

// 分节标题:黑底白字横条,墨水屏上分节最醒目
void MakeSection(lv_obj_t* parent, const char* title) {
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(bar, 6, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* lbl = lv_label_create(bar);
    lv_label_set_text(lbl, title);
    lv_obj_set_style_text_font(lbl, UiFont(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
}

void FmtKb(size_t bytes, char* out, size_t out_len) {
    if (bytes >= 1024 * 1024) {
        snprintf(out, out_len, "%.1f MB", bytes / 1048576.0);
    } else {
        snprintf(out, out_len, "%zu KB", bytes / 1024);
    }
}

// 一组堆的 空闲/总量/最大连续块/历史最低
void AddHeapLines(lv_obj_t* parent, const char* name, uint32_t caps) {
    const size_t free_b = heap_caps_get_free_size(caps);
    const size_t total_b = heap_caps_get_total_size(caps);
    const size_t largest = heap_caps_get_largest_free_block(caps);
    const size_t min_free = heap_caps_get_minimum_free_size(caps);
    char a[24], b[24], c[24], d[24];
    FmtKb(free_b, a, sizeof(a));
    FmtKb(total_b, b, sizeof(b));
    FmtKb(largest, c, sizeof(c));
    FmtKb(min_free, d, sizeof(d));
    char line[160];
    const int used_pct = total_b > 0 ? (int)((total_b - free_b) * 100 / total_b) : 0;
    snprintf(line, sizeof(line), "%s:空闲 %s / 共 %s(已用 %d%%)", name, a, b, used_pct);
    ScreenMakeLabel(parent, line, 25, true);
    snprintf(line, sizeof(line), "  最大连续块 %s;历史最低空闲 %s", c, d);
    ScreenMakeLabel(parent, line, 25, true);
}

void FillMemory(lv_obj_t* parent) {
    MakeSection(parent, "内存 (RAM / PSRAM)");
    AddHeapLines(parent, "内部 RAM", MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    AddHeapLines(parent, "PSRAM   ", MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void FillStorage(lv_obj_t* parent) {
    MakeSection(parent, "存储 (ROM / SD)");
    char line[160];

    char flash_a[24];
    FmtKb(SystemInfo::GetFlashSize(), flash_a, sizeof(flash_a));
    const esp_partition_t* running = esp_ota_get_running_partition();
    if (running != nullptr) {
        char part_a[24];
        FmtKb(running->size, part_a, sizeof(part_a));
        snprintf(line, sizeof(line), "Flash:%s;当前应用分区:%s(%s)", flash_a, part_a, running->label);
    } else {
        snprintf(line, sizeof(line), "Flash:%s", flash_a);
    }
    ScreenMakeLabel(parent, line, 25, true);

    auto& sd = SdCardManager::GetInstance();
    if (sd.IsMounted()) {
        FATFS* fs = nullptr;
        DWORD free_clusters = 0;
        if (f_getfree("0:", &free_clusters, &fs) == FR_OK && fs != nullptr) {
            constexpr DWORD kSectorSize = 512;
            const uint64_t total = (uint64_t)(fs->n_fatent - 2) * fs->csize * kSectorSize;
            const uint64_t freeb = (uint64_t)free_clusters * fs->csize * kSectorSize;
            char ta[24], fa[24];
            FmtKb((size_t)total, ta, sizeof(ta));
            FmtKb((size_t)freeb, fa, sizeof(fa));
            snprintf(line, sizeof(line), "SD 卡:已挂载,共 %s,可用 %s", ta, fa);
        } else {
            snprintf(line, sizeof(line), "SD 卡:已挂载(容量查询失败)");
        }
    } else {
        snprintf(line, sizeof(line), "SD 卡:未挂载/未插入");
    }
    ScreenMakeLabel(parent, line, 25, true);
}

const char* NvsTypeName(nvs_type_t t) {
    switch (t) {
        case NVS_TYPE_U8: return "u8";
        case NVS_TYPE_I8: return "i8";
        case NVS_TYPE_U16: return "u16";
        case NVS_TYPE_I16: return "i16";
        case NVS_TYPE_U32: return "u32";
        case NVS_TYPE_I32: return "i32";
        case NVS_TYPE_U64: return "u64";
        case NVS_TYPE_I64: return "i64";
        case NVS_TYPE_STR: return "str";
        case NVS_TYPE_BLOB: return "blob";
        default: return "any";
    }
}

void FillNvs(lv_obj_t* parent) {
    MakeSection(parent, "NVS (键值存储)");
    char line[160];

    nvs_stats_t stats = {};
    if (nvs_get_stats(nullptr, &stats) == ESP_OK) {
        snprintf(line, sizeof(line), "条目:已用 %u / 共 %u(命名空间 %u 个)",
                 (unsigned)stats.used_entries, (unsigned)stats.total_entries,
                 (unsigned)stats.namespace_count);
        ScreenMakeLabel(parent, line, 25, true);
    }

    ScreenMakeLabel(parent, "键名清单(仅键名,不含值):", 25, true);
    nvs_iterator_t it = nullptr;
    esp_err_t res = nvs_entry_find(nullptr, nullptr, NVS_TYPE_ANY, &it);
    int shown = 0;
    while (res == ESP_OK && it != nullptr && shown < kMaxNvsLines) {
        nvs_entry_info_t info = {};
        if (nvs_entry_info(it, &info) == ESP_OK) {
            snprintf(line, sizeof(line), "  %s / %s (%s)",
                     info.namespace_name, info.key, NvsTypeName(info.type));
            ScreenMakeLabel(parent, line, 25, true);
            ++shown;
        }
        res = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    if (shown == 0) {
        ScreenMakeLabel(parent, "  (空)", 25, true);
    } else if (shown >= kMaxNvsLines) {
        ScreenMakeLabel(parent, "  …(过多,截断)", 25, true);
    }
}

void FillSystem(lv_obj_t* parent) {
    MakeSection(parent, "系统");
    char line[160];
    snprintf(line, sizeof(line), "芯片:%s  IDF:v%s",
             SystemInfo::GetChipModelName().c_str(), esp_get_idf_version());
    ScreenMakeLabel(parent, line, 25, true);
    snprintf(line, sizeof(line), "MAC:%s", SystemInfo::GetMacAddress().c_str());
    ScreenMakeLabel(parent, line, 25, true);
    const int64_t up_s = esp_timer_get_time() / 1000000;
    snprintf(line, sizeof(line), "本次开机运行:%" PRId64 " 天 %02" PRId64 ":%02" PRId64 ":%02" PRId64,
             up_s / 86400, (up_s / 3600) % 24, (up_s / 60) % 60, up_s % 60);
    ScreenMakeLabel(parent, line, 25, true);
}

lv_timer_t* s_refresh_timer = nullptr;

void FillBody() {
    if (s_ui.body == nullptr) return;
    // 记住滚动位置,自动刷新不把用户拉回顶部
    const lv_coord_t scroll_y = lv_obj_get_scroll_y(s_ui.body);
    lv_obj_clean(s_ui.body);
    lv_obj_scroll_to_y(s_ui.body, scroll_y, LV_ANIM_OFF);
    FillMemory(s_ui.body);
    FillStorage(s_ui.body);
    FillNvs(s_ui.body);
    FillSystem(s_ui.body);
}

void OnRefreshClicked(lv_event_t*) {
    HapticPulseIfEnabled();
    // 刷新按钮在头部行,不在 body 内 → 就地 clean 重建安全
    FillBody();
}

void OnRefreshTimer(lv_timer_t*) {
    // 只在屏活着时刷新;5s 与状态栏局部刷新同管线,eink 撑得住
    if (s_alive) {
        FillBody();
    }
}

void OnScreenUnloaded(lv_event_t*) {
    s_alive = false;
    if (s_refresh_timer != nullptr) {
        lv_timer_del(s_refresh_timer);
        s_refresh_timer = nullptr;
    }
    s_ui = {};
}

}  // namespace

lv_obj_t* SystemInfoScreen::Create() {
    s_ui = {};
    s_alive = true;
    ScreenSetIsHome(false);

    lv_obj_t* screen = lv_obj_create(nullptr);
    s_ui.screen = screen;
    lv_obj_remove_style_all(screen);
    lv_obj_set_size(screen, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);
    lv_obj_set_style_text_font(screen, UiFont(), 0);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(screen, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);

    const EpdStatusBar status = ScreenCreateStatusBar(screen);

    lv_obj_t* body = lv_obj_create(screen);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, LV_VER_RES - status.height);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, status.height);
    lv_obj_set_style_pad_all(body, 12, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 6, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);   // 外层不动;只滚内容容器
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);

    // 头部行:标题 + 刷新按钮(固定在顶部,不随内容滚动)
    lv_obj_t* head = lv_obj_create(body);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, lv_pct(100), 52);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* title = lv_label_create(head);
    lv_label_set_text(title, "系统信息");
    {
        const lv_font_t* f = fontpack_lv_font_get(30, 2);
        lv_obj_set_style_text_font(title, f != nullptr ? f : UiFont(), 0);
    }
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* btn = lv_obj_create(head);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, 110, 44);
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, OnRefreshClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* bl = lv_label_create(btn);
    lv_label_set_text(bl, "刷新");
    lv_obj_set_style_text_font(bl, UiFont(), 0);
    lv_obj_set_style_text_color(bl, lv_color_black(), 0);
    lv_obj_center(bl);
    lv_obj_clear_flag(bl, LV_OBJ_FLAG_CLICKABLE);

    // 可滚动内容容器(与设置页滑动同款;头部行在其外,固定)
    lv_obj_t* scroll = lv_obj_create(body);
    lv_obj_remove_style_all(scroll);
    lv_obj_set_width(scroll, lv_pct(100));
    lv_obj_set_flex_grow(scroll, 1);
    lv_obj_set_style_pad_row(scroll, 6, 0);
    lv_obj_set_flex_flow(scroll, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scroll, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_flag(scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(scroll, LV_DIR_VER);
    lv_obj_clear_flag(scroll, LV_OBJ_FLAG_CLICKABLE);
    s_ui.body = scroll;

    FillBody();
    if (s_refresh_timer != nullptr) {
        lv_timer_del(s_refresh_timer);
    }
    s_refresh_timer = lv_timer_create(OnRefreshTimer, 5000, nullptr);
    VkKey_AttachScreen(screen, kScreenId, VkKeyScreenDesc{SystemInfoScreen::Create, nullptr});
    return screen;
}
