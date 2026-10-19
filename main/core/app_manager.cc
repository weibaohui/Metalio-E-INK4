#include "app_manager.h"

#include "application.h"
#include "async_settings_store.h"
#include "assets/lang_config.h"
#include "screen_common.h"

#include "ai_voice_chat_screen/ai_voice_chat_screen.h"
#include "app_store_screen/app_store_screen.h"
#include "book_screen/book_screen.h"
#include "power_screen/power_screen.h"
#include "settings_screen/settings_screen.h"

#include "services/ai_chat/ai_chat_service.h"
#include "services/calendar/calendar_mcp.h"
#include "services/radio/radio_mcp.h"
#include "services/wifi_transfer/book_mcp.h"
#include "services/wifi_transfer/wifi_transfer_service.h"
#include "system_upgrade_screen/system_upgrade_screen.h"
#include "services/wifi_file/wifi_file_service.h"

#include <esp_log.h>
#include <cstring>

namespace {

constexpr const char *TAG = "AppManager";

const char *AppNameBook() { return Lang::Strings::HOME_APP_BOOK; }
const char *AppNameSettings() { return Lang::Strings::HOME_APP_SETTINGS; }
const char *AppNameAiChat() { return "AI 对话"; }
const char *AppNameUpgrade() { return "系统升级"; }
const char *AppNameStore() { return "应用商店"; }
const char *AppNamePower() { return "电源管理"; }

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
    // 保持后台 MCP 服务注册
    CalendarRegisterMcpTools();
    RadioRegisterMcpTools();
    BookRegisterMcpTools();

    // 注册内置核心系统应用 (只保留 set, power, store, upgrade, book, aichat)
    RegisterApp(std::make_shared<StandardAppModule>(
        "book", "A:ic_app_home_book.spng", AppNameBook, BookScreen::Create, nullptr, false,
        BookScreen::OnResumeFromStandby, BookScreen::OnEnterStandby, nullptr));
    RegisterApp(std::make_shared<StandardAppModule>(
        "aichat", "A:ic_app_home_buddy.spng", AppNameAiChat, AiVoiceChatScreen::Create, AiChat_BootRegister, false));
    RegisterApp(std::make_shared<StandardAppModule>(
        "store", "A:ic_app_home_cloud.spng", AppNameStore, AppStoreScreen::Create, nullptr, false));
    RegisterApp(std::make_shared<StandardAppModule>(
        "upgrade", "A:ic_app_home_setting.spng", AppNameUpgrade, SystemUpgradeScreen::Create, nullptr, false));
    RegisterApp(std::make_shared<StandardAppModule>(
        "power", "A:ic_app_home_power.spng", AppNamePower, PowerScreen::Create, nullptr, true));
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
        if (modules_[existing]->IsMandatory()) {
            ESP_LOGW(TAG, "cannot overwrite mandatory built-in app: %s", module->GetId());
            return;
        }
        modules_[existing] = module;
        ESP_LOGI(TAG, "updated app module: %s", module->GetId());
    } else {
        modules_.push_back(module);
        // 运行时注册的新应用（如 SD 推送）默认开启：否则 boot 后构建的
        // enabled_mask_ 没有它的位，GetEnabledApps 会把它过滤出桌面
        enabled_mask_ |= (1u << (modules_.size() - 1));
        ESP_LOGI(TAG, "registered app module: %s", module->GetId());
    }
}

bool AppManager::UnregisterApp(const char* id) {
    if (id == nullptr) {
        return false;
    }
    const int index = FindIndexById(id);
    if (index < 0) {
        return false;
    }
    if (active_app_ && std::strcmp(active_app_->GetId(), id) == 0) {
        active_app_.reset();
    }
    modules_.erase(modules_.begin() + index);
    ESP_LOGI(TAG, "unregistered app module: %s", id);
    return true;
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

void AppManager::SetAppEnabled(const char* id, bool enabled) {
    if (id == nullptr) return;
    int idx = FindIndexById(id);
    if (idx >= 0) {
        SetAppEnabled(idx, enabled);
    }
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

void AppManager::DispatchScreenLifecycle(const char* screen_id, bool loaded) {
    if (screen_id == nullptr) return;

    // 规范化 screen_id 到 app_id（处理子页面或历史别名）
    const char* app_id = screen_id;
    if (std::strcmp(screen_id, "bookshelf") == 0 ||
        std::strcmp(screen_id, "read") == 0 ||
        std::strcmp(screen_id, "detail") == 0) {
        app_id = "book";
    } else if (std::strcmp(screen_id, "wifi_transfer") == 0) {
        app_id = "transfer";
    }

    auto app = GetAppById(app_id);
    if (loaded) {
        if (active_app_ && active_app_ != app) {
            ESP_LOGI(TAG, "lifecycle: pause old app %s", active_app_->GetId());
            active_app_->OnPause();
        }
        active_app_ = app;
        if (active_app_) {
            ESP_LOGI(TAG, "lifecycle: resume app %s", active_app_->GetId());
            active_app_->OnResume();
        }
    } else {
        if (active_app_ && active_app_ == app) {
            ESP_LOGI(TAG, "lifecycle: pause & destroy app %s", active_app_->GetId());
            active_app_->OnPause();
            active_app_->OnDestroy();
            active_app_ = nullptr;
        }
    }
}
