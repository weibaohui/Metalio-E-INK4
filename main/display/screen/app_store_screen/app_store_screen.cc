// main/display/screen/app_store_screen/app_store_screen.cc
// 应用商店：扫描 SD 卡 apps/ 下的应用包，支持分页浏览（上一页/下一页、手势与硬件触控键联动）、
// 一键安装（注册进桌面 + 置安装标志）与卸载（注销 + 清标志，文件保留可重装）。
#include "app_store_screen/app_store_screen.h"

#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

#include "core/haptic_feedback.h"
#include "display/screen/epd_widget_kit.h"
#include "display/screen/screen_common.h"
#include "display/screen/home_screen/home_screen.h"
#include "display/screen/vk_key_handler.h"
#include "fontpack_lvgl.h"
#include "services/dynamic_apps/dynamic_app_loader.h"

namespace {

constexpr const char* kScreenId = "app_store";
constexpr size_t kAppsPerPage = 5;

lv_obj_t* s_scr = nullptr;
lv_obj_t* s_body = nullptr;
lv_obj_t* s_header_row = nullptr;
lv_obj_t* s_count_lbl = nullptr;
lv_obj_t* s_list_box = nullptr;
lv_obj_t* s_nav_bar = nullptr;
lv_obj_t* s_prev_btn = nullptr;
lv_obj_t* s_next_btn = nullptr;
lv_obj_t* s_page_lbl = nullptr;

std::vector<dynamic_apps::DynPackage> s_pkgs;
int s_cur_page = 0;
bool s_alive = false;

void disable_scroll(lv_obj_t* obj) {
    if (obj != nullptr) lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

const lv_font_t* UiFont() {
    const lv_font_t* f = fontpack_lv_font_ui();
    if (f == nullptr) f = fontpack_lv_font_get(25, 2);
    return f;
}

lv_obj_t* MakeText(lv_obj_t* parent, const char* text, int font_size) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    const lv_font_t* f = fontpack_lv_font_get(font_size, 2);
    lv_obj_set_style_text_font(lbl, f != nullptr ? f : UiFont(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return lbl;
}

int TotalPages() {
    if (s_pkgs.empty()) return 1;
    return static_cast<int>((s_pkgs.size() + kAppsPerPage - 1) / kAppsPerPage);
}

void RenderCurrentPage();

void OnActionClicked(lv_event_t* e) {
    const intptr_t idx = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    if (!s_alive || idx < 0 || static_cast<size_t>(idx) >= s_pkgs.size()) {
        return;
    }
    const dynamic_apps::DynPackage& pkg = s_pkgs[static_cast<size_t>(idx)];
    if (pkg.installed) {
        dynamic_apps::UninstallPackage(pkg.id);
    } else {
        dynamic_apps::InstallPackage(pkg.id);
    }
    HomeScreenRequestAppsRefresh();
    s_pkgs = dynamic_apps::ListPackages();
    const int total = TotalPages();
    if (s_cur_page >= total) {
        s_cur_page = total > 0 ? total - 1 : 0;
    }
    RenderCurrentPage();
}

ScreenPaintCoalesce s_store_paint{};

void RequestRenderCurrentPage() {
    if (s_store_paint.paint == nullptr) {
        s_store_paint.paint = RenderCurrentPage;
    }
    ScreenPaintCoalesceRequest(&s_store_paint);
}

void OnNavPrevClicked(lv_event_t* /*e*/) {
    if (s_cur_page > 0) {
        --s_cur_page;
        RequestRenderCurrentPage();
    }
}

void OnNavNextClicked(lv_event_t* /*e*/) {
    if (s_cur_page + 1 < TotalPages()) {
        ++s_cur_page;
        RequestRenderCurrentPage();
    }
}

void OnScreenGesture(lv_event_t* /*e*/) {
    if (!s_alive) return;
    lv_indev_t* indev = lv_indev_active();
    if (indev == nullptr) return;
    const lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_LEFT) {
        // 向左滑：下一页
        if (s_cur_page + 1 < TotalPages()) {
            ++s_cur_page;
            RequestRenderCurrentPage();
            HapticPulseIfEnabled();
        }
    } else if (dir == LV_DIR_RIGHT) {
        // 向右滑：上一页
        if (s_cur_page > 0) {
            --s_cur_page;
            RequestRenderCurrentPage();
            HapticPulseIfEnabled();
        }
    }
}

bool OnStoreVkKey(const char* key) {
    if (key == nullptr || !s_alive) return false;
    if (std::strcmp(key, "vk_prev") == 0 || std::strcmp(key, "prev") == 0) {
        if (s_cur_page > 0) {
            --s_cur_page;
            RequestRenderCurrentPage();
            HapticPulseIfEnabled();
            return true;
        }
        return false;
    }
    if (std::strcmp(key, "vk_next") == 0 || std::strcmp(key, "next") == 0) {
        if (s_cur_page + 1 < TotalPages()) {
            ++s_cur_page;
            RequestRenderCurrentPage();
            HapticPulseIfEnabled();
        }
        return true;
    }
    // vk_home 返回 false，由系统默认返回栈切回桌面
    return false;
}

void RenderCurrentPage() {
    if (!s_alive || s_list_box == nullptr) return;

    // 1. 更新顶部计数
    if (s_count_lbl != nullptr) {
        char count_buf[64];
        std::snprintf(count_buf, sizeof(count_buf), "已识别 %u 个应用", static_cast<unsigned>(s_pkgs.size()));
        lv_label_set_text(s_count_lbl, count_buf);
    }

    // 2. 清空并重建列表卡片
    lv_obj_clean(s_list_box);

    const int total_pages = TotalPages();
    if (s_pkgs.empty()) {
        lv_obj_t* empty_box = lv_obj_create(s_list_box);
        lv_obj_remove_style_all(empty_box);
        lv_obj_set_size(empty_box, lv_pct(100), 260);
        lv_obj_set_style_border_color(empty_box, lv_palette_darken(LV_PALETTE_GREY, 1), 0);
        lv_obj_set_style_border_width(empty_box, 1, 0);
        lv_obj_set_style_radius(empty_box, 10, 0);
        lv_obj_set_style_pad_all(empty_box, 20, 0);
        disable_scroll(empty_box);

        lv_obj_t* t1 = MakeText(empty_box, "SD 卡暂无应用安装包", 25);
        lv_obj_align(t1, LV_ALIGN_CENTER, 0, -25);
        lv_obj_set_style_text_align(t1, LV_TEXT_ALIGN_CENTER, 0);

        lv_obj_t* t2 = MakeText(empty_box, "将包含 manifest.json 的 app 包\n推送到 /sdcard/metalio/e-ink/apps/ 目录\n即可在此一键安装或更新", 20);
        lv_obj_align(t2, LV_ALIGN_CENTER, 0, 30);
        lv_obj_set_style_text_align(t2, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(t2, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    } else {
        const size_t start_idx = static_cast<size_t>(s_cur_page * kAppsPerPage);
        const size_t end_idx = std::min(start_idx + kAppsPerPage, s_pkgs.size());

        for (size_t i = start_idx; i < end_idx; ++i) {
            const dynamic_apps::DynPackage& pkg = s_pkgs[i];

            lv_obj_t* row = lv_obj_create(s_list_box);
            lv_obj_remove_style_all(row);
            lv_obj_set_size(row, lv_pct(100), 108);
            lv_obj_set_style_border_color(row, lv_color_black(), 0);
            lv_obj_set_style_border_width(row, 2, 0);
            lv_obj_set_style_pad_all(row, 12, 0);
            lv_obj_set_style_radius(row, 8, 0);
            disable_scroll(row);
            lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

            // 左列上部：名称 + 版本号
            char title_str[128];
            const char* ver = pkg.version.empty() ? "1.0.0" : pkg.version.c_str();
            if (ver[0] == 'v' || ver[0] == 'V') {
                std::snprintf(title_str, sizeof(title_str), "%s  %s", pkg.name.c_str(), ver);
            } else {
                std::snprintf(title_str, sizeof(title_str), "%s  v%s", pkg.name.c_str(), ver);
            }
            lv_obj_t* name_lbl = MakeText(row, title_str, 25);
            lv_obj_align(name_lbl, LV_ALIGN_TOP_LEFT, 0, 4);

            // 左列下部：应用包 ID 与存储来源
            char meta[128];
            std::snprintf(meta, sizeof(meta), "ID: %s  ·  %s",
                          pkg.id.c_str(),
                          pkg.installed ? "已加入桌面" : "未安装");
            lv_obj_t* meta_lbl = MakeText(row, meta, 20);
            lv_obj_align(meta_lbl, LV_ALIGN_BOTTOM_LEFT, 0, -4);
            lv_obj_set_style_text_color(meta_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);

            // 右侧现代胶囊式操作按钮 (类 App Store 风格)
            lv_obj_t* btn = lv_obj_create(row);
            lv_obj_remove_style_all(btn);
            lv_obj_set_size(btn, 104, 48);
            lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
            lv_obj_set_style_radius(btn, 24, 0); // 胶囊圆角
            lv_obj_set_style_border_color(btn, lv_color_black(), 0);
            lv_obj_set_style_border_width(btn, 2, 0);
            // 未安装：黑底白字高对比；已安装：白底黑字线框
            lv_obj_set_style_bg_color(btn, pkg.installed ? lv_color_white() : lv_color_black(), 0);
            lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
            lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
            HapticAttachClick(btn);
            lv_obj_add_event_cb(btn, OnActionClicked, LV_EVENT_CLICKED,
                                reinterpret_cast<void*>(static_cast<intptr_t>(i)));

            lv_obj_t* btn_lbl = lv_label_create(btn);
            lv_label_set_text(btn_lbl, pkg.installed ? "卸载" : "安装");
            lv_obj_set_style_text_font(btn_lbl, UiFont(), 0);
            lv_obj_set_style_text_color(btn_lbl, pkg.installed ? lv_color_black() : lv_color_white(), 0);
            lv_obj_center(btn_lbl);
            lv_obj_clear_flag(btn_lbl, LV_OBJ_FLAG_CLICKABLE);
        }
    }

    // 3. 更新底部翻页栏
    if (s_page_lbl != nullptr) {
        char page_buf[64];
        std::snprintf(page_buf, sizeof(page_buf), "第 %d / %d 页", s_cur_page + 1, total_pages);
        lv_label_set_text(s_page_lbl, page_buf);
    }

    if (s_prev_btn != nullptr) {
        const bool prev_avail = s_cur_page > 0;
        if (prev_avail) {
            lv_obj_add_flag(s_prev_btn, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_border_color(s_prev_btn, lv_color_black(), 0);
            lv_obj_set_style_text_color(s_prev_btn, lv_color_black(), LV_PART_MAIN);
        } else {
            lv_obj_clear_flag(s_prev_btn, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_border_color(s_prev_btn, lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
            lv_obj_set_style_text_color(s_prev_btn, lv_palette_darken(LV_PALETTE_GREY, 1), LV_PART_MAIN);
        }
    }

    if (s_next_btn != nullptr) {
        const bool next_avail = s_cur_page + 1 < total_pages;
        if (next_avail) {
            lv_obj_add_flag(s_next_btn, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_border_color(s_next_btn, lv_color_black(), 0);
            lv_obj_set_style_text_color(s_next_btn, lv_color_black(), LV_PART_MAIN);
        } else {
            lv_obj_clear_flag(s_next_btn, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_border_color(s_next_btn, lv_palette_lighten(LV_PALETTE_GREY, 1), 0);
            lv_obj_set_style_text_color(s_next_btn, lv_palette_darken(LV_PALETTE_GREY, 1), LV_PART_MAIN);
        }
    }
}

void OnScreenUnloaded(lv_event_t* /*e*/) {
    s_alive = false;
    ScreenPaintCoalesceReset(&s_store_paint);
    s_scr = nullptr;
    s_body = nullptr;
    s_header_row = nullptr;
    s_count_lbl = nullptr;
    s_list_box = nullptr;
    s_nav_bar = nullptr;
    s_prev_btn = nullptr;
    s_next_btn = nullptr;
    s_page_lbl = nullptr;
    s_pkgs.clear();
    s_cur_page = 0;
}

}  // namespace

lv_obj_t* AppStoreScreen::Create() {
    // 页面脚手架：去除冗余空白返回顶栏 (show_header=false)，挂接统一状态栏，禁用连续滚动
    EpdWidgetKit::EpdPageScaffold::Config cfg;
    cfg.title = "应用商店";
    cfg.show_header = false;
    cfg.show_status_bar = true;
    cfg.show_back_btn = false;
    cfg.scrollable = false;
    cfg.pad_h = 16;
    cfg.pad_v = 8;
    cfg.flex_column = true;
    cfg.row_gap = 0;
    auto* scaffold = EpdWidgetKit::EpdPageScaffold::Create(cfg);
    s_scr = scaffold->GetScreen();
    s_body = scaffold->GetContentContainer();
    s_alive = true;
    s_cur_page = 0;

    lv_obj_add_event_cb(s_scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOAD_START, NULL);
    lv_obj_add_event_cb(s_scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, NULL);
    lv_obj_add_event_cb(s_scr, OnScreenGesture, LV_EVENT_GESTURE, NULL);

    // 1. 顶部紧凑标题与信息栏（高度约 36px，充分利用屏幕）
    s_header_row = lv_obj_create(s_body);
    lv_obj_remove_style_all(s_header_row);
    lv_obj_set_size(s_header_row, lv_pct(100), 38);
    lv_obj_set_flex_flow(s_header_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_header_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    disable_scroll(s_header_row);

    lv_obj_t* title_lbl = MakeText(s_header_row, "应用商店", 25);
    lv_obj_set_style_text_font(title_lbl, fontpack_lv_font_get(30, 2) ? fontpack_lv_font_get(30, 2) : UiFont(), 0);

    s_count_lbl = MakeText(s_header_row, "正在扫描...", 20);
    lv_obj_set_style_text_color(s_count_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);

    // 2. 居中应用列表卡片容器 (容纳 5 个应用行)
    s_list_box = lv_obj_create(s_body);
    lv_obj_remove_style_all(s_list_box);
    lv_obj_set_size(s_list_box, lv_pct(100), 600);
    lv_obj_set_flex_flow(s_list_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list_box, 10, 0);
    lv_obj_set_style_pad_top(s_list_box, 8, 0);
    lv_obj_set_style_pad_bottom(s_list_box, 4, 0);
    disable_scroll(s_list_box);

    // 3. 底部专属翻页导航栏 (上一页 / 第 X/Y 页 / 下一页)
    s_nav_bar = lv_obj_create(s_body);
    lv_obj_remove_style_all(s_nav_bar);
    lv_obj_set_size(s_nav_bar, lv_pct(100), 52);
    lv_obj_set_flex_flow(s_nav_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_nav_bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    disable_scroll(s_nav_bar);

    // [◀ 上一页]
    s_prev_btn = lv_obj_create(s_nav_bar);
    lv_obj_remove_style_all(s_prev_btn);
    lv_obj_set_size(s_prev_btn, 120, 44);
    lv_obj_set_style_radius(s_prev_btn, 6, 0);
    lv_obj_set_style_border_width(s_prev_btn, 2, 0);
    lv_obj_set_style_border_color(s_prev_btn, lv_color_black(), 0);
    lv_obj_set_style_bg_color(s_prev_btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_prev_btn, LV_OPA_COVER, 0);
    HapticAttachClick(s_prev_btn);
    lv_obj_add_event_cb(s_prev_btn, OnNavPrevClicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t* prev_lbl = lv_label_create(s_prev_btn);
    lv_label_set_text(prev_lbl, "◀ 上一页");
    lv_obj_set_style_text_font(prev_lbl, UiFont(), 0);
    lv_obj_center(prev_lbl);
    lv_obj_clear_flag(prev_lbl, LV_OBJ_FLAG_CLICKABLE);

    // 中间页码
    s_page_lbl = MakeText(s_nav_bar, "第 1 / 1 页", 22);

    // [下一页 ▶]
    s_next_btn = lv_obj_create(s_nav_bar);
    lv_obj_remove_style_all(s_next_btn);
    lv_obj_set_size(s_next_btn, 120, 44);
    lv_obj_set_style_radius(s_next_btn, 6, 0);
    lv_obj_set_style_border_width(s_next_btn, 2, 0);
    lv_obj_set_style_border_color(s_next_btn, lv_color_black(), 0);
    lv_obj_set_style_bg_color(s_next_btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_next_btn, LV_OPA_COVER, 0);
    HapticAttachClick(s_next_btn);
    lv_obj_add_event_cb(s_next_btn, OnNavNextClicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t* next_lbl = lv_label_create(s_next_btn);
    lv_label_set_text(next_lbl, "下一页 ▶");
    lv_obj_set_style_text_font(next_lbl, UiFont(), 0);
    lv_obj_center(next_lbl);
    lv_obj_clear_flag(next_lbl, LV_OBJ_FLAG_CLICKABLE);

    // 加载软件包列表并初次渲染
    s_pkgs = dynamic_apps::ListPackages();
    RenderCurrentPage();

    // 挂入虚拟键栈：支持 vk_prev / vk_next 硬件键翻页，vk_home 返回桌面
    VkKeyScreenDesc desc;
    desc.on_key = OnStoreVkKey;
    VkKey_AttachScreen(s_scr, kScreenId, desc);

    return s_scr;
}
