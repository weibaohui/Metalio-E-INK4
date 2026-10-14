#include "app_manager.h"

#include "application.h"
#include "assets/lang_config.h"
#include "settings.h"

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
    apps_.clear();
    // 注册内置默认应用（保持与既有首页显示顺序完全一致）
    RegisterApp({"glm", "A:ic_app_home_glm.spng", AppNameGlm, GlmUsageScreen::Create, nullptr, false});
    RegisterApp({"book", "A:ic_app_home_book.spng", AppNameBook, BookScreen::Create, nullptr, false});
    RegisterApp({"radio", "A:ic_app_home_music.spng", AppNameRadio, RadioScreen::Create, RadioRegisterMcpTools, false});
    RegisterApp({"time", "A:ic_app_home_clock.spng", AppNameTimeSync, TimeSyncScreen::Create, nullptr, false});
    RegisterApp({"cal", "A:ic_app_home_task.spng", AppNameCalendar, CalendarScreen::Create, CalendarRegisterMcpTools, false});
    RegisterApp({"power", "A:ic_app_home_power.spng", AppNamePower, PowerScreen::Create, nullptr, false});
    RegisterApp({"info", "A:ic_app_home_sysinfo.spng", AppNameSysInfo, SystemInfoScreen::Create, nullptr, false});
    RegisterApp({"aichat", "A:ic_app_home_buddy.spng", AppNameAiChat, AiVoiceChatScreen::Create, AiChat_BootRegister, false});
    RegisterApp({"backup", "A:ic_app_home_cloud.spng", AppNameBackup, BackupScreen::Create, nullptr, false});
    // “设置”为系统管理必备入口，永驻不可关闭
    RegisterApp({"set", "A:ic_app_home_setting.spng", AppNameSettings, SettingsScreen::Create, nullptr, true});
}

void AppManager::RegisterApp(const AppDesc& desc) {
    if (desc.id == nullptr || desc.create == nullptr) {
        ESP_LOGE(TAG, "invalid app descriptor");
        return;
    }
    int existing = FindIndexById(desc.id);
    if (existing >= 0) {
        apps_[existing] = desc;
        ESP_LOGI(TAG, "updated app: %s", desc.id);
    } else {
        apps_.push_back(desc);
        ESP_LOGI(TAG, "registered app: %s", desc.id);
    }
}

namespace {
std::vector<std::shared_ptr<AppModule>> s_registered_modules;
}

void AppManager::RegisterApp(std::shared_ptr<AppModule> module) {
    if (!module) return;
    s_registered_modules.push_back(module);
    AppModule* m = module.get();

    AppDesc desc;
    desc.id = m->GetId();
    desc.icon_path = m->GetIconPath();
    desc.is_mandatory = m->IsMandatory();
    // 由于 AppDesc 使用纯函数指针, 通过静态查找派发
    desc.name = []() -> const char* {
        if (!s_registered_modules.empty()) {
            return s_registered_modules.back()->GetName();
        }
        return "";
    };
    desc.create = []() -> lv_obj_t* {
        if (!s_registered_modules.empty()) {
            return s_registered_modules.back()->CreateUI(nullptr);
        }
        return nullptr;
    };
    desc.register_mcp = []() {
        if (!s_registered_modules.empty()) {
            s_registered_modules.back()->RegisterMcpTools();
        }
    };
    RegisterApp(desc);
}

void AppManager::Init() {
    LoadTogglesFromNvs();
}

void AppManager::LoadTogglesFromNvs() {
    uint32_t mask = 0;
    Settings settings("apps", false);
    for (size_t i = 0; i < apps_.size(); ++i) {
        if (apps_[i].is_mandatory) {
            mask |= (1u << i);
            continue;
        }
        char key[16];
        std::snprintf(key, sizeof(key), "en_%s", apps_[i].id);
        if (settings.GetBool(key, true)) {
            mask |= (1u << i);
        }
    }
    enabled_mask_ = mask;
    toggles_loaded_ = true;
    ESP_LOGI(TAG, "app toggles loaded from NVS: mask=0x%02x, count=%u",
             static_cast<unsigned>(mask), static_cast<unsigned>(apps_.size()));
}

int AppManager::FindIndexById(const char* id) const {
    if (id == nullptr) return -1;
    for (size_t i = 0; i < apps_.size(); ++i) {
        if (std::strcmp(apps_[i].id, id) == 0) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

std::vector<const AppDesc*> AppManager::GetEnabledApps() const {
    std::vector<const AppDesc*> result;
    result.reserve(apps_.size());
    for (size_t i = 0; i < apps_.size(); ++i) {
        if (IsAppEnabledByIndex(static_cast<int>(i))) {
            result.push_back(&apps_[i]);
        }
    }
    return result;
}

int AppManager::GetEnabledAppCount() const {
    int cnt = 0;
    for (size_t i = 0; i < apps_.size(); ++i) {
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
    if (index < 0 || index >= static_cast<int>(apps_.size())) return false;
    if (apps_[index].is_mandatory) return true;
    if (!toggles_loaded_) return true; // 未加载异常保护：默认允许
    return (enabled_mask_ & (1u << index)) != 0;
}

bool AppManager::IsAppMandatoryByIndex(int index) const {
    if (index < 0 || index >= static_cast<int>(apps_.size())) return false;
    return apps_[index].is_mandatory;
}

const char* AppManager::GetAppNameByIndex(int index) const {
    if (index < 0 || index >= static_cast<int>(apps_.size())) return "";
    return apps_[index].name != nullptr ? apps_[index].name() : apps_[index].id;
}

void AppManager::SetAppEnabled(int index, bool enabled) {
    if (index < 0 || index >= static_cast<int>(apps_.size())) return;
    if (apps_[index].is_mandatory) return; // 永驻应用不可关

    if (enabled) {
        enabled_mask_ |= (1u << index);
    } else {
        enabled_mask_ &= ~(1u << index);
    }

    // 调度至 main 任务落盘（因为调用方通常在 PSRAM 栈的 LVGL worker 中，禁直接写 flash）
    const std::string key = std::string("en_") + apps_[index].id;
    Application::GetInstance().Schedule([key, enabled]() {
        Settings settings("apps", true);
        settings.SetBool(key.c_str(), enabled);
    });
    ESP_LOGI(TAG, "app %s -> %s", apps_[index].id, enabled ? "on" : "off");
}

void AppManager::RegisterEnabledMcpTools() {
    for (const auto& app : apps_) {
        if (app.register_mcp == nullptr) continue;
        if (!IsAppEnabled(app.id)) {
            ESP_LOGI(TAG, "app %s off: skip MCP tools", app.id);
            continue;
        }
        app.register_mcp();
        ESP_LOGI(TAG, "app mcp tools registered: %s", app.id);
    }
}
