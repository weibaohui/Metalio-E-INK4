// main/display/screen/app_store_screen/app_store_screen.cc
// 应用商店：列出 SD 卡 apps/ 下的应用包（manifest 有效者），按行提供
// 安装（注册进桌面 + 置安装标志）/ 卸载（注销 + 清标志，文件保留可重装）。
// 数据来自 dynamic_apps::ListPackages；动作走 InstallPackage/UninstallPackage
// （RAM/NVS-RAM 操作，可在 LVGL 线程直调），完成后刷新商店列表与桌面宫格。
#include "app_store_screen/app_store_screen.h"

#include <cstdio>
#include <cstring>
#include <vector>

#include "core/haptic_feedback.h"
#include "display/screen/epd_widget_kit.h"
#include "display/screen/screen_common.h"
#include "display/screen/home_screen/home_screen.h"
#include "display/screen/vk_key_handler.h"
#include "fontpack_lvgl.h"
#include "services/dynamic_apps/dynamic_app_loader.h"

namespace {

constexpr const char* kScreenId = "app_store";

lv_obj_t* s_scr = nullptr;
lv_obj_t* s_list_body = nullptr;
std::vector<dynamic_apps::DynPackage> s_pkgs;  // 行索引 → 包（回调经索引取 id）
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

// 操作后：刷新商店列表 + 桌面宫格
void RebuildList(lv_obj_t* body);
void RefreshAll() {
    HomeScreenRequestAppsRefresh();
    if (s_alive && s_list_body != nullptr) {
        lv_obj_clean(s_list_body);
        RebuildList(s_list_body);
    }
}

void OnActionClicked(lv_event_t* e) {
    const intptr_t index = (intptr_t)lv_event_get_user_data(e);
    if (!s_alive || index < 0 || static_cast<size_t>(index) >= s_pkgs.size()) {
        return;
    }
    const dynamic_apps::DynPackage& pkg = s_pkgs[static_cast<size_t>(index)];
    if (pkg.installed) {
        dynamic_apps::UninstallPackage(pkg.id);
    } else {
        dynamic_apps::InstallPackage(pkg.id);
    }
    RefreshAll();
}

// 重建列表行
void RebuildList(lv_obj_t* body) {
    // 清掉旧行（body 里除滚动视图自带结构外的行容器由我们全权管理：
    // scaffold 的 body 直接作为列表容器使用）
    s_pkgs = dynamic_apps::ListPackages();

    if (s_pkgs.empty()) {
        lv_obj_t* empty = MakeText(body, "SD 卡暂无应用包\n推送 app 包到 apps/ 目录后在此安装", 25);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_width(empty, lv_pct(100));
        return;
    }

    for (size_t i = 0; i < s_pkgs.size(); ++i) {
        const dynamic_apps::DynPackage& pkg = s_pkgs[i];

        lv_obj_t* row = lv_obj_create(body);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 96);
        lv_obj_set_style_border_color(row, lv_color_black(), 0);
        lv_obj_set_style_border_width(row, 1, 0);
        lv_obj_set_style_pad_all(row, 12, 0);
        lv_obj_set_style_radius(row, 8, 0);
        disable_scroll(row);
        lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);

        // 左列：名称 + 版本号
        char title_str[128];
        const char* ver = pkg.version.empty() ? "1.0.0" : pkg.version.c_str();
        if (ver[0] == 'v' || ver[0] == 'V') {
            snprintf(title_str, sizeof(title_str), "%s  %s", pkg.name.c_str(), ver);
        } else {
            snprintf(title_str, sizeof(title_str), "%s  v%s", pkg.name.c_str(), ver);
        }
        lv_obj_t* name_lbl = MakeText(row, title_str, 25);
        lv_obj_align(name_lbl, LV_ALIGN_TOP_LEFT, 0, 0);

        // 次行：ID 和安装状态
        char meta[96];
        snprintf(meta, sizeof(meta), "ID: %s  ·  %s",
                 pkg.id.c_str(),
                 pkg.installed ? "已安装" : "未安装");
        lv_obj_t* meta_lbl = MakeText(row, meta, 20);
        lv_obj_align(meta_lbl, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        lv_obj_set_style_text_color(meta_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);

        // 右侧按钮：已安装 → 卸载；未安装 → 安装
        lv_obj_t* btn = lv_obj_create(row);
        lv_obj_remove_style_all(btn);
        lv_obj_set_size(btn, 110, 56);
        lv_obj_align(btn, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_set_style_radius(btn, 6, 0);
        lv_obj_set_style_border_color(btn, lv_color_black(), 0);
        lv_obj_set_style_border_width(btn, 2, 0);
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

void OnScreenUnloaded(lv_event_t* e) {
    (void)e;
    s_alive = false;
    s_list_body = nullptr;
    s_scr = nullptr;
    s_pkgs.clear();
}

}  // namespace

lv_obj_t* AppStoreScreen::Create() {
    EpdWidgetKit::EpdPageScaffold::Config cfg;
    cfg.title = "应用商店";
    cfg.show_header = true;
    cfg.show_status_bar = true;
    cfg.show_back_btn = false;
    cfg.scrollable = true;
    cfg.pad_h = 16;
    cfg.pad_v = 10;
    cfg.flex_column = true;
    cfg.row_gap = 10;
    auto* scaffold = EpdWidgetKit::EpdPageScaffold::Create(cfg);
    s_scr = scaffold->GetScreen();
    s_list_body = scaffold->GetContentContainer();
    s_alive = true;

    lv_obj_add_event_cb(s_scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOAD_START, NULL);
    lv_obj_add_event_cb(s_scr, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, NULL);

    RebuildList(s_list_body);

    // 挂入虚拟键栈：否则按键分发器仍视活跃屏为首页，HOME 失效（同占位屏教训）
    VkKeyScreenDesc desc;  // 空描述：HOME/vk_prev 走默认返回桌面
    VkKey_AttachScreen(s_scr, kScreenId, desc);
    return s_scr;
}
