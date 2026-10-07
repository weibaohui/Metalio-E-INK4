#include "glm_usage_screen/glm_usage_screen.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <memory>
#include <string>
#include <vector>

#include "http.h"
#include "network_interface.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "board.h"
#include "fontpack_lvgl.h"
#include "glm_usage_screen/glm_usage.h"
#include "haptic_feedback.h"
#include "power_policy.h"
#include "screen_common.h"
#include "settings.h"
#include "vk_key_handler.h"

namespace {

constexpr const char* TAG = "GlmUsage";
constexpr const char* kScreenId = "glm_usage";

constexpr lv_coord_t kPad = 16;
constexpr lv_coord_t kRowGap = 12;
constexpr lv_coord_t kCardPad = 14;
constexpr lv_coord_t kCardRadius = 18;
constexpr lv_coord_t kButtonH = 46;
constexpr lv_coord_t kFooterH = 42;
constexpr lv_coord_t kTabH = 48;
constexpr lv_coord_t kKeyboardH = 230;

constexpr size_t kApiKeyMax = 128;
constexpr size_t kOrgMax = 64;
constexpr size_t kProjectMax = 64;
constexpr int kHttpTimeoutMs = 15000;
constexpr int kRefreshIntervalMs = 5 * 60 * 1000;

struct GlmConfig {
    std::string api_key;
    std::string organization;
    std::string project;

    bool HasKey() const { return !api_key.empty(); }
    bool IsTeam() const { return !organization.empty() && !project.empty(); }
};

struct FetchResult {
    bool ok = false;
    GlmUsage usage;
    std::string error;
    time_t updated = 0;
};

void RefreshUsageUi();

struct GlmUi {
    lv_obj_t* screen = nullptr;
    lv_obj_t* usage_page = nullptr;
    lv_obj_t* config_page = nullptr;
    lv_obj_t* tab_usage = nullptr;
    lv_obj_t* tab_config = nullptr;
    lv_obj_t* refresh_btn = nullptr;
    lv_obj_t* plan_lbl = nullptr;
    lv_obj_t* five_pct_lbl = nullptr;
    lv_obj_t* five_bar = nullptr;
    lv_obj_t* five_detail_lbl = nullptr;
    lv_obj_t* week_pct_lbl = nullptr;
    lv_obj_t* week_bar = nullptr;
    lv_obj_t* week_detail_lbl = nullptr;
    lv_obj_t* mcp_lbl = nullptr;
    lv_obj_t* team_lbl = nullptr;
    lv_obj_t* footer_lbl = nullptr;

    lv_obj_t* key_ta = nullptr;
    lv_obj_t* org_ta = nullptr;
    lv_obj_t* project_ta = nullptr;
    lv_obj_t* keyboard = nullptr;
    lv_obj_t* save_btn = nullptr;
};

GlmUi s_ui;
bool s_screen_alive = false;
bool s_usage_tab = true;
std::atomic_bool s_fetch_busy{false};
lv_timer_t* s_refresh_timer = nullptr;

std::mutex s_state_mu;
GlmUsage s_usage;
std::string s_error = "尚未查询";
bool s_last_ok = false;
time_t s_updated_at = 0;

const lv_font_t* UiFont() {
    return fontpack_lv_font_ui();
}

void DisableScroll(lv_obj_t* obj) {
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

void HapticAttach(lv_obj_t* obj) {
    HapticAttachClick(obj);
}

lv_obj_t* MakeCard(lv_obj_t* parent) {
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, kCardRadius, 0);
    lv_obj_set_style_pad_all(card, kCardPad, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 8, 0);
    DisableScroll(card);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);
    return card;
}

lv_obj_t* MakeLabel(lv_obj_t* parent, const char* text, int font_size = 25) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text != nullptr ? text : "");
    const lv_font_t* font = font_size > 25 ? fontpack_lv_font_get(font_size, 2) : UiFont();
    lv_obj_set_style_text_font(label, font != nullptr ? font : UiFont(), 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return label;
}

lv_obj_t* MakeButton(lv_obj_t* parent, const char* text, lv_coord_t width,
                     lv_event_cb_t callback) {
    lv_obj_t* button = lv_obj_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, width, kButtonH);
    lv_obj_set_style_bg_color(button, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(button, lv_color_black(), 0);
    lv_obj_set_style_border_width(button, 2, 0);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    HapticAttach(button);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, UiFont(), 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_center(label);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return button;
}

void SetFooter(const char* text) {
    if (s_ui.footer_lbl != nullptr) {
        lv_label_set_text(s_ui.footer_lbl, text != nullptr ? text : "");
    }
}

void SetFooterFmt(const char* fmt, ...) {
    char text[160];
    va_list args;
    va_start(args, fmt);
    vsnprintf(text, sizeof(text), fmt, args);
    va_end(args);
    SetFooter(text);
}

GlmConfig LoadConfig() {
    Settings settings("glm", false);
    GlmConfig config;
    config.api_key = settings.GetString("api_key");
    config.organization = settings.GetString("org_id");
    config.project = settings.GetString("project_id");
    return config;
}

std::string Trim(const std::string& value) {
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(begin, end - begin);
}

bool SaveConfig(GlmConfig config) {
    Settings settings("glm", true);
    settings.SetString("api_key", config.api_key);
    settings.SetString("org_id", config.organization);
    settings.SetString("project_id", config.project);
    return true;
}

struct HttpResult {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string error;
};

bool HttpRequest(const std::string& url, const std::vector<std::pair<std::string, std::string>>& headers,
                 HttpResult& result) {
    result = {};
    PowerNeedHold hold_network(PowerNeed::OtaDownload);
    if (!Board::GetInstance().EnsureNetworkReady()) {
        result.error = "网络未就绪";
        return false;
    }
    NetworkInterface* network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        result.error = "网络不可用";
        return false;
    }
    std::unique_ptr<Http> http = network->CreateHttp(0);
    if (http == nullptr) {
        result.error = "创建请求失败";
        return false;
    }

    http->SetTimeout(kHttpTimeoutMs);
    http->SetHeader("Accept", "application/json");
    for (const auto& header : headers) {
        http->SetHeader(header.first, header.second);
    }
    ESP_LOGI(TAG, "quota request: %s", url.c_str());
    if (!http->Open("GET", url)) {
        result.error = "请求失败";
        return false;
    }
    result.status = http->GetStatusCode();
    result.body = http->ReadAll();
    http->Close();
    result.ok = result.status >= 200 && result.status < 300;
    if (!result.ok && result.error.empty()) {
        result.error = "HTTP " + std::to_string(result.status);
    }
    return result.ok;
}

void AppendAuthHeaders(std::vector<std::pair<std::string, std::string>>& headers,
                       const GlmConfig& config) {
    headers.emplace_back("Authorization", config.api_key);
    if (config.IsTeam()) {
        headers.emplace_back("bigmodel-organization", config.organization);
        headers.emplace_back("bigmodel-project", config.project);
    }
}

FetchResult FetchUsage() {
    FetchResult result;
    const GlmConfig config = LoadConfig();
    if (!config.HasKey()) {
        result.error = "请先配置 API Key";
        return result;
    }

    std::vector<std::pair<std::string, std::string>> headers;
    AppendAuthHeaders(headers, config);
    const std::string url = "https://open.bigmodel.cn/api/monitor/usage/quota/limit" +
                            std::string(config.IsTeam() ? "?type=2" : "");
    HttpResult http_result;
    HttpRequest(url, headers, http_result);

    bool recognized = GlmUsageParse(http_result.body, result.usage);
    if (http_result.status == 200 && recognized && result.usage.ok) {
        result.ok = true;
    } else if (recognized && !result.usage.msg.empty()) {
        result.error = result.usage.msg;
    } else if (!http_result.error.empty()) {
        result.error = http_result.error;
    } else {
        result.error = "响应格式错误";
    }

    if (result.ok && config.IsTeam()) {
        static int last_five = -1;
        static int last_week = -1;
        int five = -1;
        int week = -1;
        bool got = false;
        const std::string reset_url =
            "https://open.bigmodel.cn/api/biz/customer-package-reset/list?targetType=TEAM";
        for (int attempt = 0; attempt < 4 && !got; ++attempt) {
            HttpResult reset_result;
            std::vector<std::pair<std::string, std::string>> reset_headers;
            AppendAuthHeaders(reset_headers, config);
            HttpRequest(reset_url, reset_headers, reset_result);
            if (reset_result.ok && GlmResetsParse(reset_result.body, five, week) &&
                five >= 0 && week >= 0) {
                last_five = five;
                last_week = week;
                got = true;
            } else if (attempt < 3) {
                vTaskDelay(pdMS_TO_TICKS(400));
            }
        }
        if (got) {
            result.usage.five_hour_resets_left = five;
            result.usage.week_resets_left = week;
        } else {
            ESP_LOGW(TAG, "reset counts unavailable; keep previous values");
            result.usage.five_hour_resets_left = last_five;
            result.usage.week_resets_left = last_week;
        }
    }

    result.updated = time(nullptr);
    ESP_LOGI(TAG, "usage fetch ok=%d status=%d", result.ok, http_result.status);
    return result;
}

void ApplyResult(void* user_data) {
    FetchResult* result = static_cast<FetchResult*>(user_data);
    s_fetch_busy.store(false, std::memory_order_release);
    if (result == nullptr) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(s_state_mu);
        if (result->ok) {
            s_usage = result->usage;
            s_error.clear();
            s_last_ok = true;
        } else {
            s_error = result->error;
            s_last_ok = false;
        }
        s_updated_at = result->updated;
    }
    delete result;

    if (!s_screen_alive) {
        return;
    }
    RefreshUsageUi();
    if (s_last_ok) {
        SetFooter("已更新");
    } else {
        SetFooterFmt("查询失败：%s", s_error.c_str());
    }
}

void FetchTask(void* /*argument*/) {
    FetchResult* result = new FetchResult(FetchUsage());
    if (!ScreenLvAsyncUrgent(ApplyResult, result)) {
        delete result;
        s_fetch_busy.store(false, std::memory_order_release);
    }
    vTaskDelete(nullptr);
}

void StartFetch(bool force = true) {
    bool expected = false;
    if (!s_fetch_busy.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        SetFooter("正在查询...");
        return;
    }
    (void)force;
    SetFooter("正在查询...");
    TaskHandle_t handle = nullptr;
    if (xTaskCreatePinnedToCore(FetchTask, "glm_fetch", 8192, nullptr,
                                tskIDLE_PRIORITY + 2, &handle, 0) != pdPASS) {
        s_fetch_busy.store(false, std::memory_order_release);
        SetFooter("查询任务启动失败");
    }
}

void OnRefreshClicked(lv_event_t* /*event*/) {
    StartFetch();
}

std::string PercentText(int percent) {
    return percent >= 0 ? std::to_string(percent) + "%" : "--";
}

std::string UpdatedText() {
    time_t updated = 0;
    {
        std::lock_guard<std::mutex> lock(s_state_mu);
        updated = s_updated_at;
    }
    if (updated <= 0) {
        return "尚未更新";
    }
    struct tm local{};
    localtime_r(&updated, &local);
    char text[24];
    std::snprintf(text, sizeof(text), "%02d:%02d", local.tm_hour, local.tm_min);
    return "已更新 " + std::string(text);
}

void RefreshUsageUi() {
    if (!s_screen_alive || s_ui.plan_lbl == nullptr) {
        return;
    }
    GlmUsage usage;
    {
        std::lock_guard<std::mutex> lock(s_state_mu);
        usage = s_usage;
    }

    const std::string level = usage.level.empty() ? std::string("套餐") : usage.level;
    std::string plan = level + " · " + UpdatedText();
    lv_label_set_text(s_ui.plan_lbl, plan.c_str());

    lv_label_set_text(s_ui.five_pct_lbl,
                      ("5 小时窗口  " + PercentText(usage.tokens_5h_used_pct)).c_str());
    lv_bar_set_value(s_ui.five_bar, usage.tokens_5h_used_pct < 0 ? 0 : usage.tokens_5h_used_pct,
                     LV_ANIM_OFF);
    std::string five_detail = "重置 " + GlmFormatResetMs(usage.tokens_5h_reset_ms);
    if (usage.tokens_5h_remaining >= 0) {
        five_detail += " · 剩余 " + std::to_string(usage.tokens_5h_remaining);
    }
    lv_label_set_text(s_ui.five_detail_lbl, five_detail.c_str());

    lv_label_set_text(s_ui.week_pct_lbl,
                      ("本周额度  " + PercentText(usage.tokens_week_used_pct)).c_str());
    lv_bar_set_value(s_ui.week_bar, usage.tokens_week_used_pct < 0 ? 0 : usage.tokens_week_used_pct,
                     LV_ANIM_OFF);
    std::string week_detail = "重置 " + GlmFormatResetMs(usage.tokens_week_reset_ms);
    if (usage.tokens_week_remaining >= 0) {
        week_detail += " · 剩余 " + std::to_string(usage.tokens_week_remaining);
    }
    lv_label_set_text(s_ui.week_detail_lbl, week_detail.c_str());

    std::string mcp = "MCP 调用 ";
    if (usage.mcp_remaining >= 0) {
        mcp += std::to_string(usage.mcp_remaining);
        if (usage.mcp_total >= 0) {
            mcp += " / " + std::to_string(usage.mcp_total);
        }
    } else if (usage.mcp_used >= 0) {
        mcp += std::to_string(usage.mcp_used);
    } else {
        mcp += "--";
    }
    lv_label_set_text(s_ui.mcp_lbl, mcp.c_str());

    std::string team = "团队重置次数 ";
    if (usage.five_hour_resets_left >= 0 || usage.week_resets_left >= 0) {
        team += "5h " + std::to_string(usage.five_hour_resets_left) + " · 周 " +
                std::to_string(usage.week_resets_left);
    } else {
        team += "不适用";
    }
    lv_label_set_text(s_ui.team_lbl, team.c_str());
}

void StyleTabButton(lv_obj_t* button, bool selected) {
    lv_obj_t* label = lv_obj_get_child(button, 0);
    lv_obj_set_style_bg_color(button, selected ? lv_color_black() : lv_color_white(), 0);
    if (label != nullptr) {
        lv_obj_set_style_text_color(label, selected ? lv_color_white() : lv_color_black(), 0);
    }
}

void SwitchTab(bool usage) {
    s_usage_tab = usage;
    if (s_ui.usage_page != nullptr) {
        usage ? lv_obj_clear_flag(s_ui.usage_page, LV_OBJ_FLAG_HIDDEN)
              : lv_obj_add_flag(s_ui.usage_page, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_ui.config_page != nullptr) {
        usage ? lv_obj_add_flag(s_ui.config_page, LV_OBJ_FLAG_HIDDEN)
              : lv_obj_clear_flag(s_ui.config_page, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_ui.keyboard != nullptr) {
        lv_obj_add_flag(s_ui.keyboard, LV_OBJ_FLAG_HIDDEN);
    }
    StyleTabButton(s_ui.tab_usage, usage);
    StyleTabButton(s_ui.tab_config, !usage);
}

void OnUsageTabClicked(lv_event_t* /*event*/) {
    SwitchTab(true);
}

void OnConfigTabClicked(lv_event_t* /*event*/) {
    SwitchTab(false);
}

lv_obj_t* MakeTabButton(lv_obj_t* parent, const char* text, bool usage) {
    lv_obj_t* button = MakeButton(parent, text, 140, usage ? OnUsageTabClicked : OnConfigTabClicked);
    return button;
}

void LoadConfigFields() {
    const GlmConfig config = LoadConfig();
    if (s_ui.key_ta != nullptr) {
        lv_textarea_set_text(s_ui.key_ta, config.api_key.c_str());
    }
    if (s_ui.org_ta != nullptr) {
        lv_textarea_set_text(s_ui.org_ta, config.organization.c_str());
    }
    if (s_ui.project_ta != nullptr) {
        lv_textarea_set_text(s_ui.project_ta, config.project.c_str());
    }
}

void HideKeyboard() {
    if (s_ui.keyboard != nullptr) {
        lv_obj_add_flag(s_ui.keyboard, LV_OBJ_FLAG_HIDDEN);
    }
}

void OnSaveClicked(lv_event_t* /*event*/) {
    GlmConfig config;
    config.api_key = Trim(s_ui.key_ta != nullptr ? lv_textarea_get_text(s_ui.key_ta) : "");
    config.organization = Trim(s_ui.org_ta != nullptr ? lv_textarea_get_text(s_ui.org_ta) : "");
    config.project = Trim(s_ui.project_ta != nullptr ? lv_textarea_get_text(s_ui.project_ta) : "");
    HideKeyboard();
    SaveConfig(config);
    SetFooter("已保存，开始查询");
    StartFetch();
}

void OnTextareaFocused(lv_event_t* event) {
    lv_obj_t* textarea = static_cast<lv_obj_t*>(lv_event_get_target(event));
    if (s_ui.keyboard == nullptr) {
        return;
    }
    lv_keyboard_set_textarea(s_ui.keyboard, textarea);
    lv_obj_clear_flag(s_ui.keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_scroll_to_view(textarea, LV_ANIM_OFF);
}

void OnKeyboardReady(lv_event_t* /*event*/) {
    OnSaveClicked(nullptr);
}

bool OnVkKey(const char* key_name) {
    if (key_name == nullptr || !s_screen_alive) {
        return false;
    }
    const bool keyboard_visible =
        s_ui.keyboard != nullptr && !lv_obj_has_flag(s_ui.keyboard, LV_OBJ_FLAG_HIDDEN);
    if (keyboard_visible) {
        if (std::strcmp(key_name, "vk_home") == 0) {
            HideKeyboard();
        }
        return true;
    }
    if (!s_usage_tab && std::strcmp(key_name, "vk_home") == 0) {
        SwitchTab(true);
        return true;
    }
    return false;
}

void OnScreenUnloaded(lv_event_t* /*event*/) {
    s_screen_alive = false;
    if (s_refresh_timer != nullptr) {
        lv_timer_delete(s_refresh_timer);
        s_refresh_timer = nullptr;
    }
    s_ui = {};
}

void OnRefreshTimer(lv_timer_t* /*timer*/) {
    if (s_screen_alive && s_usage_tab) {
        StartFetch();
    }
}

lv_obj_t* CreateUsagePage(lv_obj_t* parent) {
    lv_obj_t* page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page, kRowGap, 0);
    DisableScroll(page);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* title_row = lv_obj_create(page);
    lv_obj_remove_style_all(title_row);
    lv_obj_set_width(title_row, lv_pct(100));
    lv_obj_set_height(title_row, kButtonH);
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(title_row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(title_row, 12, 0);
    DisableScroll(title_row);
    lv_obj_clear_flag(title_row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* title = MakeLabel(title_row, "GLM 用量", 30);
    lv_obj_set_width(title, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(title, 1);
    s_ui.refresh_btn = MakeButton(title_row, "刷新", 110, OnRefreshClicked);

    s_ui.plan_lbl = MakeLabel(page, "尚未更新");

    lv_obj_t* five_card = MakeCard(page);
    s_ui.five_pct_lbl = MakeLabel(five_card, "5 小时窗口  --", 30);
    s_ui.five_bar = lv_bar_create(five_card);
    lv_obj_set_size(s_ui.five_bar, lv_pct(100), 18);
    lv_obj_set_style_bg_color(s_ui.five_bar, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.five_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_ui.five_bar, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.five_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ui.five_bar, 9, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.five_bar, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_ui.five_bar, 9, LV_PART_INDICATOR);
    lv_bar_set_range(s_ui.five_bar, 0, 100);
    s_ui.five_detail_lbl = MakeLabel(five_card, "重置 --");

    lv_obj_t* week_card = MakeCard(page);
    s_ui.week_pct_lbl = MakeLabel(week_card, "本周额度  --", 30);
    s_ui.week_bar = lv_bar_create(week_card);
    lv_obj_set_size(s_ui.week_bar, lv_pct(100), 18);
    lv_obj_set_style_bg_color(s_ui.week_bar, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_ui.week_bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_ui.week_bar, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_border_width(s_ui.week_bar, 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s_ui.week_bar, 9, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_ui.week_bar, lv_color_black(), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_ui.week_bar, 9, LV_PART_INDICATOR);
    lv_bar_set_range(s_ui.week_bar, 0, 100);
    s_ui.week_detail_lbl = MakeLabel(week_card, "重置 --");

    lv_obj_t* extra_card = MakeCard(page);
    s_ui.mcp_lbl = MakeLabel(extra_card, "MCP 调用 --");
    s_ui.team_lbl = MakeLabel(extra_card, "团队重置次数 不适用");

    lv_obj_t* hint = MakeLabel(page, "触摸“刷新”立即更新，应用停留时每 5 分钟自动刷新。");
    lv_obj_set_style_text_opa(hint, LV_OPA_70, 0);
    return page;
}

lv_obj_t* CreateConfigField(lv_obj_t* parent, const char* title, const char* placeholder,
                            bool password) {
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_width(box, lv_pct(100));
    lv_obj_set_height(box, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(box, 6, 0);
    DisableScroll(box);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);

    MakeLabel(box, title);
    lv_obj_t* textarea = lv_textarea_create(box);
    lv_textarea_set_one_line(textarea, true);
    lv_textarea_set_password_mode(textarea, password);
    lv_textarea_set_placeholder_text(textarea, placeholder);
    lv_obj_set_width(textarea, lv_pct(100));
    lv_obj_set_height(textarea, 48);
    lv_obj_set_style_bg_color(textarea, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(textarea, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(textarea, lv_color_black(), 0);
    lv_obj_set_style_border_width(textarea, 2, 0);
    lv_obj_set_style_radius(textarea, 12, 0);
    lv_obj_set_style_pad_all(textarea, 10, 0);
    lv_obj_set_style_text_font(textarea, UiFont(), 0);
    lv_obj_set_style_text_color(textarea, lv_color_black(), 0);
    lv_obj_add_event_cb(textarea, OnTextareaFocused, LV_EVENT_FOCUSED, nullptr);
    return textarea;
}

lv_obj_t* CreateConfigPage(lv_obj_t* parent) {
    lv_obj_t* page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page, kRowGap, 0);
    lv_obj_set_scroll_dir(page, LV_DIR_VER);
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* hint = MakeLabel(page,
        "个人套餐只填 API Key。团队套餐必须同时填组织和项目 ID，否则接口会返回无效套餐。");
    lv_obj_set_style_text_opa(hint, LV_OPA_80, 0);

    s_ui.key_ta = CreateConfigField(page, "API Key", "粘贴智谱 API Key", true);
    lv_textarea_set_max_length(s_ui.key_ta, static_cast<uint32_t>(kApiKeyMax));
    s_ui.org_ta = CreateConfigField(page, "组织 ID", "org-...", false);
    lv_textarea_set_max_length(s_ui.org_ta, static_cast<uint32_t>(kOrgMax));
    s_ui.project_ta = CreateConfigField(page, "项目 ID", "proj-...", false);
    lv_textarea_set_max_length(s_ui.project_ta, static_cast<uint32_t>(kProjectMax));

    lv_obj_t* actions = lv_obj_create(page);
    lv_obj_remove_style_all(actions);
    lv_obj_set_width(actions, lv_pct(100));
    lv_obj_set_height(actions, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(actions, 12, 0);
    DisableScroll(actions);
    lv_obj_clear_flag(actions, LV_OBJ_FLAG_CLICKABLE);
    s_ui.save_btn = MakeButton(actions, "保存并刷新", 190, OnSaveClicked);

    return page;
}

}  // namespace

lv_obj_t* GlmUsageScreen::Create() {
    s_ui = {};
    s_screen_alive = true;
    s_usage_tab = true;
    ScreenSetIsHome(false);

    lv_obj_t* screen = lv_obj_create(nullptr);
    s_ui.screen = screen;
    lv_obj_remove_style_all(screen);
    lv_obj_set_size(screen, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);
    DisableScroll(screen);
    lv_obj_add_event_cb(screen, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);

    const EpdStatusBar status = ScreenCreateStatusBar(screen);
    lv_obj_t* body = lv_obj_create(screen);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, LV_VER_RES - status.height);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, status.height);
    lv_obj_set_style_pad_all(body, kPad, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, kRowGap, 0);
    DisableScroll(body);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* tab_row = lv_obj_create(body);
    lv_obj_remove_style_all(tab_row);
    lv_obj_set_width(tab_row, lv_pct(100));
    lv_obj_set_height(tab_row, kTabH);
    lv_obj_set_flex_flow(tab_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(tab_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(tab_row, 12, 0);
    DisableScroll(tab_row);
    lv_obj_clear_flag(tab_row, LV_OBJ_FLAG_CLICKABLE);
    s_ui.tab_usage = MakeTabButton(tab_row, "用量", true);
    s_ui.tab_config = MakeTabButton(tab_row, "配置", false);

    lv_obj_t* content = lv_obj_create(body);
    lv_obj_remove_style_all(content);
    lv_obj_set_width(content, lv_pct(100));
    lv_obj_set_flex_grow(content, 1);
    DisableScroll(content);
    lv_obj_clear_flag(content, LV_OBJ_FLAG_CLICKABLE);
    s_ui.usage_page = CreateUsagePage(content);
    s_ui.config_page = CreateConfigPage(content);

    s_ui.footer_lbl = lv_label_create(body);
    lv_label_set_text(s_ui.footer_lbl, "正在初始化");
    lv_obj_set_width(s_ui.footer_lbl, lv_pct(100));
    lv_obj_set_height(s_ui.footer_lbl, kFooterH);
    lv_label_set_long_mode(s_ui.footer_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_ui.footer_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_ui.footer_lbl, UiFont(), 0);
    lv_obj_clear_flag(s_ui.footer_lbl, LV_OBJ_FLAG_CLICKABLE);

    s_ui.keyboard = lv_keyboard_create(body);
    lv_obj_set_size(s_ui.keyboard, lv_pct(100), kKeyboardH);
    lv_obj_add_flag(s_ui.keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(s_ui.keyboard, OnKeyboardReady, LV_EVENT_READY, nullptr);

    LoadConfigFields();
    SwitchTab(true);
    RefreshUsageUi();
    {
        std::lock_guard<std::mutex> lock(s_state_mu);
        SetFooter(s_last_ok ? "已更新" : ("查询失败：" + s_error).c_str());
    }
    StartFetch();

    s_refresh_timer = lv_timer_create(OnRefreshTimer, kRefreshIntervalMs, nullptr);
    VkKey_AttachScreen(screen, kScreenId,
                       VkKeyScreenDesc{GlmUsageScreen::Create, OnVkKey});
    return screen;
}
