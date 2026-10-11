#include "settings_debug_tab.h"

#include <cstdio>
#include <string>

#include "application.h"
#include "assets/lang_config.h"
#include "board_debug.h"
#include "fontpack_lvgl.h"
#include "mem_report/mem_report.h"
#include "settings.h"
#include "settings_common.h"
#include "syslog_sender.h"

namespace {

struct DebugUi {
    lv_obj_t* syslog_btns[2] = {};      // 开 / 关
    lv_obj_t* target_btns[2] = {};      // 单播 / 广播
    lv_obj_t* syslog_status = nullptr;  // 当前 host:port
    lv_obj_t* mon_btns[5] = {};         // 关 / 1s / 5s / 30s / 60s
    lv_obj_t* mem_btn = nullptr;
};

DebugUi s_ui;

constexpr int kMonChoices[] = {0, 1, 5, 30, 60};

bool SyslogIsBroadcast() {
    return Syslog_GetHost() == "broadcast";
}

void RefreshDebugTab();

// 刷新选中态与状态行(纯 LVGL 控件操作,无 NVS/网络调用)
void RefreshDebugTab() {
    const bool on = Syslog_IsEnabled();
    if (s_ui.syslog_btns[0] != nullptr) {
        SettingsStyleSelectable(s_ui.syslog_btns[0], lv_obj_get_child(s_ui.syslog_btns[0], 0), on);
        SettingsStyleSelectable(s_ui.syslog_btns[1], lv_obj_get_child(s_ui.syslog_btns[1], 0), !on);
    }
    const bool bcast = SyslogIsBroadcast();
    if (s_ui.target_btns[0] != nullptr) {
        SettingsStyleSelectable(s_ui.target_btns[0], lv_obj_get_child(s_ui.target_btns[0], 0),
                                !bcast);
        SettingsStyleSelectable(s_ui.target_btns[1], lv_obj_get_child(s_ui.target_btns[1], 0),
                                bcast);
    }
    if (s_ui.syslog_status != nullptr) {
        const std::string host = Syslog_GetHost();
        char line[80];
        snprintf(line, sizeof(line), "%s:%d", host.empty() ? "-" : host.c_str(),
                 Syslog_GetPort());
        lv_label_set_text(s_ui.syslog_status, line);
    }
    if (s_ui.mon_btns[0] != nullptr) {
        const int cur = BoardDebug_GetMonitorInterval();
        for (int i = 0; i < 5; ++i) {
            SettingsStyleSelectable(s_ui.mon_btns[i], lv_obj_get_child(s_ui.mon_btns[i], 0),
                                    kMonChoices[i] == cur);
        }
    }
}

// 切到广播前把当前单播 host 备份到 NVS,切回时恢复(设备无文本输入,host 编辑走 MCP)
void ApplySyslogBroadcast(bool broadcast) {
    const int port = Syslog_GetPort();
    if (broadcast) {
        if (!SyslogIsBroadcast()) {
            Settings backup("dbg", true);
            backup.SetString("sl_uc", Syslog_GetHost());
            Syslog_Configure("broadcast", port);
        }
    } else if (SyslogIsBroadcast()) {
        Settings backup("dbg", false);
        Syslog_Configure(backup.GetString("sl_uc", ""), port);
    }
}

void OnSyslogToggle(lv_event_t* e) {
    const bool enable = (reinterpret_cast<intptr_t>(lv_event_get_user_data(e)) != 0);
    // NVS 写/配置应用都在 main 任务(MCP 工具同款约束)
    Application::GetInstance().Schedule([enable]() { Syslog_SetEnabled(enable); });
    RefreshDebugTab();
}

void OnTargetToggle(lv_event_t* e) {
    const bool broadcast = (reinterpret_cast<intptr_t>(lv_event_get_user_data(e)) != 0);
    Application::GetInstance().Schedule([broadcast]() { ApplySyslogBroadcast(broadcast); });
    RefreshDebugTab();
}

void OnMonChoice(lv_event_t* e) {
    const intptr_t idx = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    const int sec = kMonChoices[idx];
    Application::GetInstance().Schedule([sec]() { BoardDebug_SetMonitorInterval(sec); });
    RefreshDebugTab();
}

void OnMemReport(lv_event_t*) {
    // 堆遍历/统计数组分配较重,投递 main 任务;结果走 ESP_LOG(串口/syslog)
    Application::GetInstance().Schedule([]() { MemReport_PerTask(true); });
}

}  // namespace

void SettingsDebugTab_Reset() {
    s_ui = {};
}

void SettingsDebugTab_Build(lv_obj_t* page) {
    s_ui = {};

    auto add_label = [&](const char* text) {
        lv_obj_t* lbl = lv_label_create(page);
        lv_label_set_text(lbl, text);
        lv_obj_set_style_text_font(lbl, fontpack_lv_font_ui(), 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
        return lbl;
    };

    add_label(Lang::Strings::SETTINGS_DBG_TITLE);

    // ---- 日志转发(syslog) ----
    add_label(Lang::Strings::SETTINGS_DBG_SYSLOG_TITLE);
    add_label(Lang::Strings::SETTINGS_DBG_SYSLOG_HINT);
    s_ui.syslog_btns[0] = SettingsCreateSelectableOption(
        page, Lang::Strings::COMMON_ON, OnSyslogToggle, 1);
    s_ui.syslog_btns[1] = SettingsCreateSelectableOption(
        page, Lang::Strings::COMMON_OFF, OnSyslogToggle, 0);
    s_ui.target_btns[0] = SettingsCreateSelectableOption(
        page, Lang::Strings::SETTINGS_DBG_SYSLOG_UCAST, OnTargetToggle, 0);
    s_ui.target_btns[1] = SettingsCreateSelectableOption(
        page, Lang::Strings::SETTINGS_DBG_SYSLOG_BCAST, OnTargetToggle, 1);
    s_ui.syslog_status = add_label("");

    // ---- 系统监控打印 ----
    add_label(Lang::Strings::SETTINGS_DBG_MON_TITLE);
    add_label(Lang::Strings::SETTINGS_DBG_MON_HINT);
    const char* mon_names[5] = {
        Lang::Strings::COMMON_OFF,
        Lang::Strings::SETTINGS_DBG_MON_1S,
        Lang::Strings::SETTINGS_DBG_MON_5S,
        Lang::Strings::SETTINGS_DBG_MON_30S,
        Lang::Strings::SETTINGS_DBG_MON_60S,
    };
    for (int i = 0; i < 5; ++i) {
        s_ui.mon_btns[i] = SettingsCreateSelectableOption(
            page, mon_names[i], OnMonChoice, static_cast<intptr_t>(i));
    }

    // ---- 内存报告 ----
    add_label(Lang::Strings::SETTINGS_DBG_MEM_TITLE);
    add_label(Lang::Strings::SETTINGS_DBG_MEM_HINT);
    s_ui.mem_btn = SettingsCreateSelectableOption(
        page, Lang::Strings::SETTINGS_DBG_MEM_BTN, OnMemReport, 0);

    RefreshDebugTab();
}
