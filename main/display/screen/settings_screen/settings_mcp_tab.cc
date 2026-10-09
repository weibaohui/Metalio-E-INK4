#include "settings_mcp_tab.h"

#include <cstdio>
#include <string>

#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "services/local_mcp/local_mcp_server.h"
#include "settings_common.h"

namespace {

struct McpUi {
    lv_obj_t* btns[2] = {};       // 0=开启认证 1=关闭认证
    lv_obj_t* pin_lbl = nullptr;  // 当前 PIN 显示
    lv_obj_t* regen_btn = nullptr;
};

McpUi s_ui;

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

void Refresh() {
    auto& srv = LocalMcpServer::GetInstance();
    const bool enabled = srv.AuthEnabled();
    if (s_ui.btns[0] != nullptr) {
        SettingsStyleSelectable(s_ui.btns[0], lv_obj_get_child(s_ui.btns[0], 0), enabled);
    }
    if (s_ui.btns[1] != nullptr) {
        SettingsStyleSelectable(s_ui.btns[1], lv_obj_get_child(s_ui.btns[1], 0), !enabled);
    }
    if (s_ui.pin_lbl != nullptr) {
        if (enabled) {
            // PIN 大字号,方便隔着局域网在客户端上手抄填写
            char buf[40];
            snprintf(buf, sizeof(buf), "当前 PIN:%s", srv.GetPin().c_str());
            lv_label_set_text(s_ui.pin_lbl, buf);
        } else {
            lv_label_set_text(s_ui.pin_lbl, "未启用(局域网 MCP 不需要认证)");
        }
    }
    // 重新生成只在启用时有意义;未启用时灰掉
    if (s_ui.regen_btn != nullptr) {
        if (enabled) {
            lv_obj_clear_state(s_ui.regen_btn, LV_STATE_DISABLED);
        } else {
            lv_obj_add_state(s_ui.regen_btn, LV_STATE_DISABLED);
        }
    }
}

void OnToggleClicked(lv_event_t* e) {
    const bool enable = (reinterpret_cast<intptr_t>(lv_event_get_user_data(e)) != 0);
    auto& srv = LocalMcpServer::GetInstance();
    if (srv.AuthEnabled() == enable) {
        return;
    }
    HapticPulseIfEnabled();
    srv.SetAuthEnabled(enable);   // 内部:开=生成 PIN,关=清空;NVS 落盘调度到 main
    Refresh();
}

void OnRegenClicked(lv_event_t* /*e*/) {
    auto& srv = LocalMcpServer::GetInstance();
    if (!srv.AuthEnabled()) {
        return;
    }
    HapticPulseIfEnabled();
    srv.RegeneratePin();
    Refresh();
}

}  // namespace

void SettingsMcpTab_Reset() { s_ui = {}; }

void SettingsMcpTab_Build(lv_obj_t* page) {
    s_ui = {};

    MakeText(page, "MCP 认证(局域网)", 30);
    MakeText(page,
             "开启后,局域网内 MCP 客户端需在 HTTP 头携带:\n"
             "Authorization: Bearer <6位PIN>\n"
             "设备 MCP 地址:http://<本机IP>:8080/mcp");

    s_ui.btns[0] = SettingsCreateSelectableOption(page, "开启认证", OnToggleClicked, 1);
    s_ui.btns[1] = SettingsCreateSelectableOption(page, "关闭认证", OnToggleClicked, 0);

    s_ui.pin_lbl = MakeText(page, "", 30);

    s_ui.regen_btn = SettingsCreateSelectableOption(page, "重新生成 PIN", OnRegenClicked, 0);

    Refresh();
}
