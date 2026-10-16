// main/core/boot_orchestrator.cc —— 系统开机引导编排器
#include "core/boot_orchestrator.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <functional>

#include "application.h"
#include "board.h"
#include "display.h"
#include "core/app_manager.h"
#include "core/settings.h"
#include "core/system_info.h"
#include "display/font/fontpack_lvgl.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "reader/book_home_snapshot.h"
#include "reader/book_library_warmup.h"
#include "services/calendar/calendar_ics.h"
#include "services/glm_usage/glm_usage_service.h"
#include "services/local_mcp/local_mcp_server.h"
#include "services/mem_report/mem_report.h"
#include "services/radio/radio_player.h"
#include "services/syslog_sender.h"
#include "services/time_service.h"

namespace {
constexpr const char* TAG = "BootOrchestrator";
}

void BootOrchestrator::Run(Application& app) {
    auto& board = Board::GetInstance();
    ESP_LOGI(TAG, "board initialization begin");
    board.Initialize();
    ESP_LOGI(TAG, "board initialization complete");
    auto display = board.GetDisplay();

    struct BootStep {
        const char* name;
        std::function<void()> fn;
    };

    const BootStep kSteps[] = {
        {"lang",
         [&] {
             // UI 语言：NVS ui/language，无则 LANG_DEFAULT_CODE
             Lang::InitFromNvs();
         }},
        {"device_starting",
         [&] {
             app.SetDeviceState(kDeviceStateStarting);
         }},
        {"fontpack",
         [&] {
             // fontpack mmap 须在内部 DRAM 栈任务上完成；开机预热 UI 25@2
             fontpack_lv_ensure_ready();
             (void)fontpack_lv_font_ui();
         }},
        {"chat_message",
         [&] {
             display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());
         }},
        {"main_loop",
         [&] {
             app.StartMainEventLoop();
         }},
        {"reader_warmup",
         [&] {
             reader::book_home_snapshot::HydrateFromNvs();
             reader::book_library_warmup::RequestBootWarmup();
         }},
        {"app_toggles",
         [&] {
             AppManager::GetInstance().Init();
             if (AppManager::GetInstance().IsAppEnabled("glm")) {
                 GlmUsageService::GetInstance().Init();
             }
             if (AppManager::GetInstance().IsAppEnabled("radio")) {
                 RadioPlayer::GetInstance().Init();
             }
         }},
        {"network",
         [&] {
             board.StartNetwork();
             display->UpdateStatusBar(true);
         }},
        {"ota_url",
         [&] {
             Settings settings("wifi", true);
             settings.EraseKey("ota_url");
             ESP_LOGI(TAG, "cleared stale ota_url");
         }},
        {"mcp_services",
         [&] {
             auto& mcp_server = McpServer::GetInstance();
             mcp_server.AddCommonTools();
             mcp_server.AddUserOnlyTools();
             AppManager::GetInstance().RegisterEnabledMcpTools();
             SyslogRegisterMcpTools();
             MemReport_RegisterMcpTools();
             LocalMcpServer::GetInstance().Start();
             Syslog_Init();
             TimeService_Init();
             CalendarIcs_Init();
         }},
        {"boot_ready",
         [&] {
             app.SetBootReady(true);
             app.SetDeviceState(kDeviceStateIdle);
             ESP_LOGI(TAG, "Boot complete");
         }},
    };

    for (const auto& step : kSteps) {
        const int64_t t0 = esp_timer_get_time();
        step.fn();
        const int64_t cost_ms = (esp_timer_get_time() - t0) / 1000;
        ESP_LOGI(TAG, "boot[%s] %lu ms", step.name, static_cast<unsigned long>(cost_ms));
        if (cost_ms >= 1000) {
            ESP_LOGW(TAG, "boot step [%s] took %lu ms (slow)", step.name, static_cast<unsigned long>(cost_ms));
        }
    }
}
