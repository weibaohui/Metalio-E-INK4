#include "settings_debug_tab.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <esp_log.h>

#include "application.h"
#include "assets/lang_config.h"
#include "board_debug.h"
#include "fontpack_lvgl.h"
#include "mem_report/mem_report.h"
#include "settings.h"
#include "settings_common.h"
#include "screen_common.h"
#include "syslog_sender.h"

namespace {

struct DebugUi {
    lv_obj_t* syslog_btns[2] = {};      // 开 / 关
    lv_obj_t* syslog_status = nullptr;  // 当前 host:port(页面开启即广播)
    lv_obj_t* mon_btns[5] = {};         // 关 / 1s / 5s / 30s / 60s
    lv_obj_t* mem_btn = nullptr;
};

DebugUi s_ui;
lv_obj_t* s_page = nullptr;  // 设置壳的内容容器(调试 Tab 激活期间有效)
int s_dbg_page = 0;
constexpr int kDbgPages = 3;

constexpr int kMonChoices[] = {0, 1, 5, 30, 60};

void RefreshDebugTab();

// 刷新选中态与状态行(纯 LVGL 控件操作,无 NVS/网络调用)
void RefreshDebugTab() {
    const bool on = Syslog_IsEnabled();
    if (s_ui.syslog_btns[0] != nullptr) {
        SettingsStyleSelectable(s_ui.syslog_btns[0], lv_obj_get_child(s_ui.syslog_btns[0], 0), on);
        SettingsStyleSelectable(s_ui.syslog_btns[1], lv_obj_get_child(s_ui.syslog_btns[1], 0), !on);
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

lv_obj_t* AddLabel(lv_obj_t* parent, const char* text);
void OnSyslogToggle(lv_event_t* e);
void OnMonChoice(lv_event_t* e);
void OnMemReport(lv_event_t*);

lv_obj_t* AddLabel(lv_obj_t* parent, const char* text) {
    lv_obj_t* lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return lbl;
}

// ---- 第 1 页:日志转发(syslog) ----
void BuildSyslogPage(lv_obj_t* page) {
    AddLabel(page, Lang::Strings::SETTINGS_DBG_TITLE);
    AddLabel(page, Lang::Strings::SETTINGS_DBG_SYSLOG_TITLE);
    AddLabel(page, Lang::Strings::SETTINGS_DBG_SYSLOG_HINT);
    s_ui.syslog_btns[0] = SettingsCreateSelectableOption(
        page, Lang::Strings::COMMON_ON, OnSyslogToggle, 1);
    s_ui.syslog_btns[1] = SettingsCreateSelectableOption(
        page, Lang::Strings::COMMON_OFF, OnSyslogToggle, 0);
    s_ui.syslog_status = AddLabel(page, "");
}

// ---- 第 2 页:系统监控打印 ----
void BuildMonPage(lv_obj_t* page) {
    AddLabel(page, Lang::Strings::SETTINGS_DBG_MON_TITLE);
    AddLabel(page, Lang::Strings::SETTINGS_DBG_MON_HINT);
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
}

// ---- 第 3 页:内存报告 ----
void BuildMemPage(lv_obj_t* page) {
    AddLabel(page, Lang::Strings::SETTINGS_DBG_MEM_TITLE);
    AddLabel(page, Lang::Strings::SETTINGS_DBG_MEM_HINT);
    s_ui.mem_btn = SettingsCreateSelectableOption(
        page, Lang::Strings::SETTINGS_DBG_MEM_BTN, OnMemReport, 0);
}

// 重建当前页(只清本 Tab 容器;每次重建后 s_ui.* 指向新控件)
void RenderDebugPage() {
    if (s_page == nullptr || !lv_obj_is_valid(s_page)) {
        return;
    }
    lv_obj_clean(s_page);
    s_ui = {};  // 其它页的控件指针随 clean 失效,统一清零防 RefreshDebugTab 踩悬空
    if (s_dbg_page == 0) {
        BuildSyslogPage(s_page);
    } else if (s_dbg_page == 1) {
        BuildMonPage(s_page);
    } else {
        BuildMemPage(s_page);
    }
    char ind[64];
    snprintf(ind, sizeof(ind), "第 %d / %d 页(侧键翻页)", s_dbg_page + 1, kDbgPages);
    lv_obj_t* ind_lbl = AddLabel(s_page, ind);
    lv_obj_set_style_pad_top(ind_lbl, 8, 0);
    lv_obj_set_style_text_color(ind_lbl, lv_color_black(), 0);
    // 上一页内容更长时容器留有滚动偏移,短页会整个停在视口外:归零
    lv_obj_scroll_to(s_page, 0, 0, LV_ANIM_OFF);
    ESP_LOGI("DbgTab", "render page=%d children=%u", s_dbg_page,
             (unsigned)lv_obj_get_child_count(s_page));
    RefreshDebugTab();
}

void OnSyslogToggle(lv_event_t* e) {
    const bool enable = (reinterpret_cast<intptr_t>(lv_event_get_user_data(e)) != 0);
    // NVS 写/配置应用都在 main 任务(MCP 工具同款约束);从本页开启即用广播
    Application::GetInstance().Schedule([enable]() {
        if (enable && Syslog_GetHost() != "broadcast") {
            Syslog_Configure("broadcast", Syslog_GetPort());
        }
        Syslog_SetEnabled(enable);
    });
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
    s_page = nullptr;
}

void SettingsDebugTab_Build(lv_obj_t* page) {
    s_ui = {};
    s_page = page;
    s_dbg_page = 0;
    RenderDebugPage();
}

bool SettingsDebugTab_OnVkKey(const char* key) {
    if (key == nullptr || s_page == nullptr) {
        return false;
    }
    int delta = 0;
    if (std::strcmp(key, "vk_next") == 0 || std::strcmp(key, "next") == 0) {
        delta = 1;
    } else if (std::strcmp(key, "vk_prev") == 0 || std::strcmp(key, "prev") == 0) {
        delta = -1;
    } else {
        return false;
    }
    const int next = s_dbg_page + delta;
    if (next < 0 || next >= kDbgPages) {
        return true;  // 边界:吞掉按键保持当前页,也不让壳做退出等兜底
    }
    s_dbg_page = next;
    // vk 派发在触摸喂入任务,禁止直接操作 LVGL;渲染必须落在 LVGL worker
    // 上下文(ScreenLvAsync 内部拿适配器锁走 lv_async)。走 Application::Schedule
    // 会在主事件循环裸改控件,与 worker 渲染竞态,实测翻页后内容区空白。
    ScreenLvAsync([](void*) { RenderDebugPage(); }, nullptr);
    return true;
}
