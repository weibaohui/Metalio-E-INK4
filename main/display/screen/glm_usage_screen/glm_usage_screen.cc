#include "glm_usage_screen/glm_usage_screen.h"

#include <atomic>
#include <ctime>
#include <cstdio>
#include <cstring>
#include <vector>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "board.h"
#include "application.h"
#include "display.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "screen_common.h"
#include "services/glm_usage/glm_usage_service.h"
#include "vk_key_handler.h"

namespace {

constexpr const char* TAG = "GlmUsage";
constexpr const char* kScreenId = "glm_usage";

constexpr lv_coord_t kPad = 16;
constexpr lv_coord_t kRowGap = 12;
constexpr lv_coord_t kCardPad = 14;
constexpr lv_coord_t kCardRadius = 18;
constexpr lv_coord_t kButtonH = 46;
constexpr lv_coord_t kContextTabH = 52;
constexpr lv_coord_t kFooterH = 42;

struct GlmUi {
    lv_obj_t* screen = nullptr;
    lv_obj_t* context_row = nullptr;
    lv_obj_t* refresh_btn = nullptr;
    lv_obj_t* title_lbl = nullptr;
    lv_obj_t* plan_lbl = nullptr;
    lv_obj_t* five_pct_lbl = nullptr;
    lv_obj_t* five_bar = nullptr;
    lv_obj_t* five_detail_lbl = nullptr;
    lv_obj_t* week_pct_lbl = nullptr;
    lv_obj_t* week_bar = nullptr;
    lv_obj_t* week_detail_lbl = nullptr;
    lv_obj_t* five_reset_lbl = nullptr;
    lv_obj_t* week_reset_lbl = nullptr;
    lv_obj_t* footer_lbl = nullptr;
};

GlmUi s_ui;
bool s_screen_alive = false;
size_t s_rendered_context_count = 0;
lv_timer_t* s_status_timer = nullptr;
// 进/出屏代际:每次 Create/Unload 自增。离屏的 Stop 是异步 Schedule 到 main 的,
// 快速"退出→重进"时旧的 Stop 可能在新会话重建 worker 后才跑到 —— 若不加守卫,
// 它会把新会话的 worker 误杀(需求二"多试几次才有概率刷新"的另一根因)。
std::atomic<int> s_open_gen{0};

const lv_font_t* UiFont() {
    return fontpack_lv_font_ui();
}

void DisableScroll(lv_obj_t* obj) {
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
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

lv_obj_t* MakeButton(lv_obj_t* parent, const char* text, lv_coord_t width,
                     lv_event_cb_t callback, void* user_data = nullptr) {
    lv_obj_t* button = lv_obj_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_set_size(button, width, kButtonH);
    lv_obj_set_style_bg_color(button, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(button, lv_color_black(), 0);
    lv_obj_set_style_border_width(button, 2, 0);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(button, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(button);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, width - 20);
    lv_obj_set_style_text_font(label, UiFont(), 0);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_center(label);
    lv_obj_clear_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return button;
}

void StyleContextButton(lv_obj_t* button, bool selected) {
    if (button == nullptr) {
        return;
    }
    lv_obj_set_style_bg_color(button, selected ? lv_color_black() : lv_color_white(), 0);
    lv_obj_set_style_border_color(button, selected ? lv_color_black() : lv_color_black(), 0);
    lv_obj_t* label = lv_obj_get_child(button, 0);
    if (label != nullptr) {
        lv_obj_set_style_text_color(label, selected ? lv_color_white() : lv_color_black(), 0);
    }
}

void SetFooter(const std::string& text) {
    if (s_ui.footer_lbl != nullptr) {
        lv_label_set_text(s_ui.footer_lbl, text.c_str());
    }
}

std::string PercentText(int percent) {
    return percent >= 0 ? std::to_string(percent) + "%" : "--";
}

std::string UpdatedText(time_t updated_at) {
    if (updated_at <= 0) {
        return "尚未更新";
    }
    struct tm local {};
    localtime_r(&updated_at, &local);
    char text[16] = {};
    std::snprintf(text, sizeof(text), "%02d:%02d", local.tm_hour, local.tm_min);
    return "已更新 " + std::string(text);
}

void OnContextClicked(lv_event_t* event) {
    if (!s_screen_alive || event == nullptr) {
        return;
    }
    const uintptr_t index = reinterpret_cast<uintptr_t>(lv_event_get_user_data(event));
    GlmUsageService::GetInstance().SelectContext(static_cast<size_t>(index));
}

void RebuildContextTabs(const std::vector<GlmUsageContext>& contexts) {
    if (s_ui.context_row == nullptr) {
        return;
    }
    while (lv_obj_get_child_cnt(s_ui.context_row) > 0) {
        lv_obj_del(lv_obj_get_child(s_ui.context_row, 0));
    }

    for (size_t i = 0; i < contexts.size(); ++i) {
        void* user_data = reinterpret_cast<void*>(static_cast<uintptr_t>(i));
        lv_obj_t* button = MakeButton(s_ui.context_row, contexts[i].name.c_str(),
                                      210, OnContextClicked, user_data);
        StyleContextButton(button, false);
    }
    s_rendered_context_count = contexts.size();
}

void RefreshUsageUi() {
    if (!s_screen_alive || s_ui.title_lbl == nullptr) {
        return;
    }

    auto& service = GlmUsageService::GetInstance();
    const std::vector<GlmUsageContext> contexts = service.GetContexts();
    const GlmUsageStatus status = service.GetStatus();
    if (contexts.size() != s_rendered_context_count) {
        RebuildContextTabs(contexts);
    }
    for (size_t i = 0; i < s_rendered_context_count &&
         i < static_cast<size_t>(lv_obj_get_child_cnt(s_ui.context_row)); ++i) {
        StyleContextButton(lv_obj_get_child(s_ui.context_row, static_cast<int32_t>(i)),
                           i == status.active_context);
    }

    if (contexts.empty()) {
        lv_label_set_text(s_ui.title_lbl, "GLM 用量");
        lv_label_set_text(s_ui.plan_lbl, "等待 MCP 配置");
        lv_label_set_text(s_ui.five_pct_lbl, "5 小时窗口  --");
        lv_bar_set_value(s_ui.five_bar, 0, LV_ANIM_OFF);
        lv_label_set_text(s_ui.five_detail_lbl, "重置 --");
        lv_label_set_text(s_ui.week_pct_lbl, "本周额度  --");
        lv_bar_set_value(s_ui.week_bar, 0, LV_ANIM_OFF);
        lv_label_set_text(s_ui.week_detail_lbl, "重置 --");
        lv_label_set_text(s_ui.five_reset_lbl, "5小时重置次数 --");
        lv_label_set_text(s_ui.week_reset_lbl, "周重置次数 --");
        return;
    }

    const size_t selected = status.active_context < contexts.size()
        ? status.active_context : 0;
    const GlmUsageContext& context = contexts[selected];
    const GlmUsage& usage = context.usage;
    lv_label_set_text(s_ui.title_lbl, context.name.c_str());

    const std::string plan_state = context.last_ok
        ? UpdatedText(context.updated_at) : "尚未成功";
    const std::string level = usage.level.empty() ? std::string("套餐") : usage.level;
    lv_label_set_text(s_ui.plan_lbl, (level + " · " + plan_state).c_str());

    lv_label_set_text(s_ui.five_pct_lbl,
                      ("5 小时窗口  " + PercentText(usage.tokens_5h_used_pct)).c_str());
    lv_bar_set_value(s_ui.five_bar,
                     usage.tokens_5h_used_pct < 0 ? 0 : usage.tokens_5h_used_pct,
                     LV_ANIM_OFF);
    std::string five_detail = "重置 " + GlmFormatResetMs(usage.tokens_5h_reset_ms);
    if (usage.tokens_5h_remaining >= 0) {
        five_detail += " · 剩余 " + std::to_string(usage.tokens_5h_remaining);
    }
    lv_label_set_text(s_ui.five_detail_lbl, five_detail.c_str());

    lv_label_set_text(s_ui.week_pct_lbl,
                      ("本周额度  " + PercentText(usage.tokens_week_used_pct)).c_str());
    lv_bar_set_value(s_ui.week_bar,
                     usage.tokens_week_used_pct < 0 ? 0 : usage.tokens_week_used_pct,
                     LV_ANIM_OFF);
    std::string week_detail = "重置 " + GlmFormatResetMs(usage.tokens_week_reset_ms);
    if (usage.tokens_week_remaining >= 0) {
        week_detail += " · 剩余 " + std::to_string(usage.tokens_week_remaining);
    }
    lv_label_set_text(s_ui.week_detail_lbl, week_detail.c_str());

    const std::string five_reset_text =
        usage.five_hour_resets_left >= 0
            ? "5小时重置次数 " + std::to_string(usage.five_hour_resets_left)
            : "5小时重置次数 --";
    const std::string week_reset_text =
        usage.week_resets_left >= 0
            ? "周重置次数 " + std::to_string(usage.week_resets_left)
            : "周重置次数 --";
    lv_label_set_text(s_ui.five_reset_lbl, five_reset_text.c_str());
    lv_label_set_text(s_ui.week_reset_lbl, week_reset_text.c_str());
}

void SyncFooter() {
    auto& service = GlmUsageService::GetInstance();
    const GlmUsageStatus status = service.GetStatus();
    const std::vector<GlmUsageContext> contexts = service.GetContexts();
    if (!status.configured) {
        SetFooter("请通过 MCP 配置 API Key 和 JWT");
    } else if (status.discovering) {
        SetFooter("正在发现机构/项目...");
    } else if (status.busy) {
        SetFooter("正在查询...");
    } else if (status.active_context < contexts.size()) {
        const GlmUsageContext& context = contexts[status.active_context];
        if (context.last_ok) {
            SetFooter(context.name + " · " + UpdatedText(context.updated_at));
        } else {
            SetFooter("查询失败：" + context.error);
        }
    } else {
        SetFooter(status.error);
    }
}

void OnScreenUnloaded(lv_event_t* /*event*/) {
    s_screen_alive = false;
    if (s_status_timer != nullptr) {
        lv_timer_delete(s_status_timer);
        s_status_timer = nullptr;
    }
    s_ui = {};
    s_rendered_context_count = 0;
    // 离页停掉 16K 常驻 worker,释放任务栈。Stop() 轮询任务退出会阻塞(最多 3s,
    // 等 worker 收尾),不能在 LVGL worker 上跑,故 Schedule 到 main 任务。
    // 代际守卫:若 Schedule 还没跑就又重进(Create 已把 s_open_gen 自增),这次
    // 旧会话的 Stop 作废,worker 留给新会话复用 —— 不会误杀新 worker。
    const int gen = ++s_open_gen;
    Application::GetInstance().Schedule([gen]() {
        if (s_open_gen.load() == gen) {
            GlmUsageService::GetInstance().Stop();
        }
    });
}

// 进页自动连网（与百问同机制）：开机保网期错过会让 STA Pause 停扫，
// 这里在后台任务里切片 EnsureNetworkReady 拉起重连，成功后立即刷新一次。
// 离页即停，不占主循环。
void GlmNetworkWaitTask(void* /*arg*/) {
    auto& board = Board::GetInstance();
    constexpr int kSliceMs = 200;
    constexpr int kBudgetMs = 30000;
    for (int elapsed = 0; elapsed < kBudgetMs && s_screen_alive; elapsed += kSliceMs) {
        if (board.IsNetworkReady()) {
            if (auto* display = board.GetDisplay()) {
                display->UpdateStatusBar(true);
            }
            GlmUsageService::GetInstance().RequestRefresh();
            vTaskDelete(nullptr);
            return;
        }
        (void)board.EnsureNetworkReady(kSliceMs);
    }
    vTaskDelete(nullptr);
}

void StartNetworkWaitIfNeeded() {
    auto& board = Board::GetInstance();
    if (board.IsNetworkReady() || board.IsWifiConfigMode()) {
        return;
    }
    if (xTaskCreate(GlmNetworkWaitTask, "glm_netwait", 4096, nullptr, 4, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "xTaskCreate(glm_netwait) failed");
    }
}

void OnStatusTimer(lv_timer_t* /*timer*/) {
    if (!s_screen_alive) {
        return;
    }
    SyncFooter();
    RefreshUsageUi();
}

bool OnVkKey(const char* key_name) {
    if (key_name == nullptr || !s_screen_alive) {
        return false;
    }
    if (std::strcmp(key_name, "vk_prev") == 0 || std::strcmp(key_name, "vk_next") == 0) {
        auto& service = GlmUsageService::GetInstance();
        const size_t count = service.GetStatus().context_count;
        if (count == 0) {
            return true;
        }
        const size_t active = service.GetStatus().active_context;
        const bool next = std::strcmp(key_name, "vk_next") == 0;
        service.SelectContext(next ? (active + 1) % count
                                   : (active + count - 1) % count);
        return true;
    }
    return false;
}

lv_obj_t* CreateBody(lv_obj_t* parent) {
    lv_obj_t* page = lv_obj_create(parent);
    lv_obj_remove_style_all(page);
    lv_obj_set_size(page, lv_pct(100), lv_pct(100));
    lv_obj_set_flex_flow(page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(page, kRowGap, 0);
    DisableScroll(page);
    lv_obj_clear_flag(page, LV_OBJ_FLAG_CLICKABLE);

    s_ui.context_row = lv_obj_create(page);
    lv_obj_remove_style_all(s_ui.context_row);
    lv_obj_set_size(s_ui.context_row, lv_pct(100), kContextTabH);
    lv_obj_set_flex_flow(s_ui.context_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_ui.context_row, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_ui.context_row, 12, 0);
    lv_obj_set_scroll_dir(s_ui.context_row, LV_DIR_HOR);

    lv_obj_t* title_row = lv_obj_create(page);
    lv_obj_remove_style_all(title_row);
    lv_obj_set_width(title_row, lv_pct(100));
    lv_obj_set_height(title_row, kButtonH);
    lv_obj_set_flex_flow(title_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(title_row, 12, 0);
    DisableScroll(title_row);
    lv_obj_clear_flag(title_row, LV_OBJ_FLAG_CLICKABLE);

    s_ui.title_lbl = ScreenMakeLabel(title_row, "GLM 用量", 30, true);
    lv_obj_set_width(s_ui.title_lbl, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(s_ui.title_lbl, 1);
    s_ui.refresh_btn = MakeButton(title_row, "刷新", 110,
        [](lv_event_t* /*event*/) {
            GlmUsageService::GetInstance().RequestRefresh();
        });

    s_ui.plan_lbl = ScreenMakeLabel(page, "尚未更新", 25, true);

    lv_obj_t* five_card = MakeCard(page);
    s_ui.five_pct_lbl = ScreenMakeLabel(five_card, "5 小时窗口  --", 30, true);
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
    s_ui.five_detail_lbl = ScreenMakeLabel(five_card, "重置 --", 30, true);

    lv_obj_t* week_card = MakeCard(page);
    s_ui.week_pct_lbl = ScreenMakeLabel(week_card, "本周额度  --", 30, true);
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
    s_ui.week_detail_lbl = ScreenMakeLabel(week_card, "重置 --", 30, true);

    lv_obj_t* extra_card = MakeCard(page);
    s_ui.five_reset_lbl = ScreenMakeLabel(extra_card, "5小时重置次数 --", 30, true);
    s_ui.week_reset_lbl = ScreenMakeLabel(extra_card, "周重置次数 --", 30, true);
    return page;
}

}  // namespace

lv_obj_t* GlmUsageScreen::Create() {
    s_ui = {};
    s_screen_alive = true;
    s_rendered_context_count = 0;
    ScreenSetIsHome(false);

    // 进屏自增代际:使任何还在路上的"旧会话 Stop"(异步 Schedule 到 main)失效。
    ++s_open_gen;

    // 进页才启动 16K 常驻 worker(boot 期只 Init 注册工具+载入持久化)。
    // Start() 不碰 NVS(inited_ 已 true),可在 LVGL worker 上直调;且 Start 内部
    // 已配置就置刷新位 —— 进屏即自动查询(需求二)。
    GlmUsageService::GetInstance().Start();

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

    lv_obj_t* page = CreateBody(body);
    lv_obj_set_width(page, lv_pct(100));
    lv_obj_set_flex_grow(page, 1);

    s_ui.footer_lbl = lv_label_create(body);
    lv_label_set_text(s_ui.footer_lbl, "正在初始化");
    lv_obj_set_width(s_ui.footer_lbl, lv_pct(100));
    lv_obj_set_height(s_ui.footer_lbl, kFooterH);
    lv_label_set_long_mode(s_ui.footer_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_ui.footer_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_ui.footer_lbl, UiFont(), 0);
    lv_obj_clear_flag(s_ui.footer_lbl, LV_OBJ_FLAG_CLICKABLE);

    RefreshUsageUi();
    SyncFooter();
    StartNetworkWaitIfNeeded();
    s_status_timer = lv_timer_create(OnStatusTimer, 1000, nullptr);
    VkKey_AttachScreen(screen, kScreenId,
                       VkKeyScreenDesc{GlmUsageScreen::Create, OnVkKey});
    return screen;
}
