#include "app_manager.h"

#include "application.h"
#include "async_settings_store.h"
#include "assets/lang_config.h"
#include "screen_common.h"

#include "ai_voice_chat_screen/ai_voice_chat_screen.h"
#include "backup_screen/backup_screen.h"
#include "book_screen/book_screen.h"
#include "calendar_screen/calendar_screen.h"
#include "glm_usage_screen/glm_usage_screen.h"
#include "power_screen/power_screen.h"
#include "radio_screen/radio_screen.h"
#include "settings_screen/settings_screen.h"
#include "system_info_screen/system_info_screen.h"
#include "time_sync_screen/time_sync_screen.h"

#include "services/ai_chat/ai_chat_service.h"
#include "services/calendar/calendar_mcp.h"
#include "services/radio/radio_mcp.h"

#include <esp_log.h>
#include <cstring>

namespace {

constexpr const char *TAG = "AppManager";

const char *AppNameGlm() { return "GLM 用量"; }
const char *AppNameRadio() { return "收音机"; }
const char *AppNameBook() { return Lang::Strings::HOME_APP_BOOK; }
const char *AppNameSettings() { return Lang::Strings::HOME_APP_SETTINGS; }
const char *AppNameCalendar() { return "日历"; }
const char *AppNameTimeSync() { return "校时"; }
const char *AppNamePower() { return "电源"; }
const char *AppNameSysInfo() { return "系统信息"; }
const char *AppNameAiChat() { return "AI 对话"; }
const char *AppNameBackup() { return "备份恢复"; }

} // namespace

AppManager& AppManager::GetInstance() {
    static AppManager instance;
    return instance;
}

AppManager::AppManager() {
    RegisterDefaultApps();
}

void AppManager::RegisterDefaultApps() {
    modules_.clear();
    // 注册内置默认应用 (全部采用新架构 AppModule)
    RegisterApp(std::make_shared<StandardAppModule>(
        "glm", "A:ic_app_home_glm.spng", AppNameGlm, GlmUsageScreen::Create, nullptr, false));
    RegisterApp(std::make_shared<StandardAppModule>(
        "book", "A:ic_app_home_book.spng", AppNameBook, BookScreen::Create, nullptr, false));
    RegisterApp(std::make_shared<StandardAppModule>(
        "radio", "A:ic_app_home_music.spng", AppNameRadio, RadioScreen::Create, RadioRegisterMcpTools, false));
    RegisterApp(std::make_shared<StandardAppModule>(
        "time", "A:ic_app_home_clock.spng", AppNameTimeSync, TimeSyncScreen::Create, nullptr, false));
    RegisterApp(std::make_shared<StandardAppModule>(
        "cal", "A:ic_app_home_task.spng", AppNameCalendar, CalendarScreen::Create, CalendarRegisterMcpTools, false));
    RegisterApp(std::make_shared<StandardAppModule>(
        "power", "A:ic_app_home_power.spng", AppNamePower, PowerScreen::Create, nullptr, false));
    RegisterApp(std::make_shared<StandardAppModule>(
        "info", "A:ic_app_home_sysinfo.spng", AppNameSysInfo, SystemInfoScreen::Create, nullptr, false));
    RegisterApp(std::make_shared<StandardAppModule>(
        "aichat", "A:ic_app_home_buddy.spng", AppNameAiChat, AiVoiceChatScreen::Create, AiChat_BootRegister, false));
    RegisterApp(std::make_shared<StandardAppModule>(
        "backup", "A:ic_app_home_cloud.spng", AppNameBackup, BackupScreen::Create, nullptr, false));
    RegisterApp(std::make_shared<StandardAppModule>(
        "set", "A:ic_app_home_setting.spng", AppNameSettings, SettingsScreen::Create, nullptr, true));
}

void AppManager::RegisterApp(std::shared_ptr<AppModule> module) {
    if (!module || module->GetId() == nullptr) {
        ESP_LOGE(TAG, "invalid app module");
        return;
    }
    int existing = FindIndexById(module->GetId());
    if (existing >= 0) {
        modules_[existing] = module;
        ESP_LOGI(TAG, "updated app module: %s", module->GetId());
    } else {
        modules_.push_back(module);
        ESP_LOGI(TAG, "registered app module: %s", module->GetId());
    }
}

void AppManager::Init() {
    LoadToggles();
}

void AppManager::LoadToggles() {
    uint32_t mask = 0;
    auto& store = AsyncSettingsStore::GetInstance();
    for (size_t i = 0; i < modules_.size(); ++i) {
        if (modules_[i]->IsMandatory()) {
            mask |= (1u << i);
            continue;
        }
        char key[24];
        std::snprintf(key, sizeof(key), "en_%s", modules_[i]->GetId());
        if (store.GetBool("apps", key, true)) {
            mask |= (1u << i);
        }
    }
    enabled_mask_ = mask;
    toggles_loaded_ = true;
    ESP_LOGI(TAG, "app toggles loaded from store: mask=0x%02x, count=%u",
             static_cast<unsigned>(mask), static_cast<unsigned>(modules_.size()));
}

int AppManager::FindIndexById(const char* id) const {
    if (id == nullptr) return -1;
    for (size_t i = 0; i < modules_.size(); ++i) {
        if (std::strcmp(modules_[i]->GetId(), id) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::shared_ptr<AppModule> AppManager::GetAppById(const char* id) const {
    int idx = FindIndexById(id);
    if (idx >= 0) return modules_[idx];
    return nullptr;
}

std::shared_ptr<AppModule> AppManager::GetAppByIndex(int index) const {
    if (index >= 0 && index < static_cast<int>(modules_.size())) {
        return modules_[index];
    }
    return nullptr;
}

std::vector<std::shared_ptr<AppModule>> AppManager::GetEnabledApps() const {
    std::vector<std::shared_ptr<AppModule>> result;
    result.reserve(modules_.size());
    for (size_t i = 0; i < modules_.size(); ++i) {
        if (IsAppEnabledByIndex(static_cast<int>(i))) {
            result.push_back(modules_[i]);
        }
    }
    return result;
}

int AppManager::GetEnabledAppCount() const {
    int cnt = 0;
    for (size_t i = 0; i < modules_.size(); ++i) {
        if (IsAppEnabledByIndex(static_cast<int>(i))) {
            ++cnt;
        }
    }
    return cnt;
}

bool AppManager::IsAppEnabled(const char* id) const {
    int idx = FindIndexById(id);
    if (idx < 0) return true;
    return IsAppEnabledByIndex(idx);
}

bool AppManager::IsAppEnabledByIndex(int index) const {
    if (index < 0 || index >= static_cast<int>(modules_.size())) return false;
    if (modules_[index]->IsMandatory()) return true;
    if (!toggles_loaded_) return true;
    return (enabled_mask_ & (1u << index)) != 0;
}

bool AppManager::IsAppMandatoryByIndex(int index) const {
    if (index < 0 || index >= static_cast<int>(modules_.size())) return false;
    return modules_[index]->IsMandatory();
}

const char* AppManager::GetAppNameByIndex(int index) const {
    if (index < 0 || index >= static_cast<int>(modules_.size())) return "";
    return modules_[index]->GetName();
}

void AppManager::SetAppEnabled(int index, bool enabled) {
    if (index < 0 || index >= static_cast<int>(modules_.size())) return;
    if (modules_[index]->IsMandatory()) return; // 永驻应用不可关

    if (enabled) {
        enabled_mask_ |= (1u << index);
    } else {
        enabled_mask_ &= ~(1u << index);
    }

    char key[24];
    std::snprintf(key, sizeof(key), "en_%s", modules_[index]->GetId());
    AsyncSettingsStore::GetInstance().SetBool("apps", key, enabled);
    ESP_LOGI(TAG, "app %s -> %s", modules_[index]->GetId(), enabled ? "on" : "off");
}

void AppManager::RegisterEnabledMcpTools() {
    for (const auto& module : modules_) {
        if (!IsAppEnabled(module->GetId())) {
            ESP_LOGI(TAG, "app %s off: skip MCP tools", module->GetId());
            continue;
        }
        module->RegisterMcpTools();
        ESP_LOGI(TAG, "app mcp tools registered: %s", module->GetId());
    }
}

void AppManager::LaunchApp(std::shared_ptr<AppModule> app) {
    if (!app) return;
    ESP_LOGI(TAG, "launching app: %s (%s)", app->GetName(), app->GetId());
    auto factory = app->GetScreenFactory();
    if (factory) {
        ScreenNavigateTo(factory);
    } else {
        ScreenSetIsHome(false);
        ScreenLoadReplace(app->CreateUI(nullptr));
    }
}

void AppManager::LaunchApp(const char* id) {
    auto app = GetAppById(id);
    if (app) {
        LaunchApp(app);
    }
}
