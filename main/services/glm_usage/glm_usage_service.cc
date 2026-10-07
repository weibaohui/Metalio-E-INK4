#include "services/glm_usage/glm_usage_service.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <memory>
#include <vector>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include "board.h"
#include "http.h"
#include "mcp_server.h"
#include "network_interface.h"
#include "power_policy.h"
#include "settings.h"

namespace {

constexpr const char* TAG = "GlmUsageService";
constexpr EventBits_t kStopBit = BIT0;
constexpr EventBits_t kRefreshBit = BIT1;
constexpr TickType_t kPollIntervalMs = 5 * 60 * 1000 / portTICK_PERIOD_MS;
constexpr int kHttpTimeoutMs = 15000;

std::string Trim(const std::string& value) {
    size_t begin = 0;
    size_t end = value.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(value[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1]))) --end;
    return value.substr(begin, end - begin);
}

std::string MaskApiKey(const std::string& key) {
    if (key.empty()) {
        return "";
    }
    if (key.size() <= 10) {
        return "***";
    }
    return key.substr(0, 5) + "..." + key.substr(key.size() - 4);
}

struct HttpResult {
    bool ok = false;
    int status = 0;
    std::string body;
    std::string error;
};

bool HttpRequest(const std::string& url,
                 const std::vector<std::pair<std::string, std::string>>& headers,
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
                       const GlmUsageConfig& config) {
    headers.emplace_back("Authorization", config.api_key);
    if (config.IsTeam()) {
        headers.emplace_back("bigmodel-organization", config.organization);
        headers.emplace_back("bigmodel-project", config.project);
    }
}

cJSON* CreateConfigJson(const GlmUsageConfig& config) {
    cJSON* json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "has_api_key", config.HasKey());
    cJSON_AddStringToObject(json, "api_key_masked", MaskApiKey(config.api_key).c_str());
    cJSON_AddBoolToObject(json, "team_mode", config.IsTeam());
    cJSON_AddStringToObject(json, "organization", config.organization.c_str());
    cJSON_AddStringToObject(json, "project", config.project.c_str());
    return json;
}

}  // namespace

GlmUsageService& GlmUsageService::GetInstance() {
    static GlmUsageService instance;
    return instance;
}

GlmUsageConfig GlmUsageService::GetConfig() {
    std::lock_guard<std::mutex> lock(mutex_);
    return config_;
}

void GlmUsageService::SaveConfig(GlmUsageConfig input) {
    input.api_key = Trim(input.api_key);
    input.organization = Trim(input.organization);
    input.project = Trim(input.project);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = input;
        status_.configured = config_.HasKey();
        status_.team = config_.IsTeam();
    }

    Settings settings("glm", true);
    settings.SetString("api_key", input.api_key);
    settings.SetString("org_id", input.organization);
    settings.SetString("project_id", input.project);
}

void GlmUsageService::RequestRefresh() {
    if (event_group_ != nullptr) {
        xEventGroupSetBits(event_group_, kRefreshBit);
    }
}

GlmUsage GlmUsageService::GetSnapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    return usage_;
}

GlmUsageStatus GlmUsageService::GetStatus() {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void GlmUsageService::StoreResult(bool ok, const GlmUsage& usage, std::string error) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ok) {
        usage_ = usage;
        status_.last_ok = true;
        status_.error.clear();
    } else {
        status_.last_ok = false;
        status_.error = std::move(error);
    }
    status_.busy = false;
    status_.updated_at = time(nullptr);
    ++status_.version;
}

void GlmUsageService::FetchOnce() {
    GlmUsageConfig config;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config = config_;
        status_.busy = true;
    }
    if (!config.HasKey()) {
        StoreResult(false, GlmUsage{}, "请先配置 API Key");
        return;
    }

    std::vector<std::pair<std::string, std::string>> headers;
    AppendAuthHeaders(headers, config);
    const std::string url = "https://open.bigmodel.cn/api/monitor/usage/quota/limit" +
                            std::string(config.IsTeam() ? "?type=2" : "");
    HttpResult http_result;
    HttpRequest(url, headers, http_result);

    GlmUsage usage;
    const bool recognized = GlmUsageParse(http_result.body, usage);
    bool ok = http_result.status == 200 && recognized && usage.ok;
    std::string error;
    if (!ok) {
        if (recognized && !usage.msg.empty()) {
            error = usage.msg;
        } else if (!http_result.error.empty()) {
            error = http_result.error;
        } else {
            error = "响应格式错误";
        }
    }

    if (ok && config.IsTeam()) {
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
            usage.five_hour_resets_left = five;
            usage.week_resets_left = week;
        } else {
            ESP_LOGW(TAG, "reset counts unavailable; keep previous values");
            usage.five_hour_resets_left = last_five;
            usage.week_resets_left = last_week;
        }
    }

    StoreResult(ok, usage, error);
    ESP_LOGI(TAG, "quota fetch ok=%d http=%d", ok, http_result.status);
}

void GlmUsageService::Worker() {
    for (;;) {
        EventBits_t bits = xEventGroupWaitBits(event_group_, kStopBit | kRefreshBit,
                                               pdTRUE, pdFALSE, kPollIntervalMs);
        if (bits & kStopBit) {
            break;
        }
        FetchOnce();
    }
    vTaskDelete(nullptr);
}

void GlmUsageService::RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool("self.glm_usage.configure",
        "Configure GLM Coding Plan credentials. api_key is required. For a personal plan leave organization and project empty; for a team plan both are required. Values are stored in NVS.",
        PropertyList({
            Property("api_key", kPropertyTypeString),
            Property("organization", kPropertyTypeString, std::string("")),
            Property("project", kPropertyTypeString, std::string("")),
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            GlmUsageConfig config;
            config.api_key = properties["api_key"].value<std::string>();
            config.organization = properties["organization"].value<std::string>();
            config.project = properties["project"].value<std::string>();
            SaveConfig(config);
            RequestRefresh();
            return "GLM credentials saved";
        });

    mcp.AddTool("self.glm_usage.set_api_key",
        "Set or replace only the GLM API key, preserving team context.",
        PropertyList({
            Property("api_key", kPropertyTypeString),
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            GlmUsageConfig config = GetConfig();
            config.api_key = properties["api_key"].value<std::string>();
            SaveConfig(config);
            RequestRefresh();
            return "GLM API key saved";
        });

    mcp.AddTool("self.glm_usage.set_team_context",
        "Set both GLM team context values. organization and project are required together.",
        PropertyList({
            Property("organization", kPropertyTypeString),
            Property("project", kPropertyTypeString),
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            GlmUsageConfig config = GetConfig();
            config.organization = properties["organization"].value<std::string>();
            config.project = properties["project"].value<std::string>();
            SaveConfig(config);
            RequestRefresh();
            return "GLM team context saved";
        });

    mcp.AddTool("self.glm_usage.clear_team_context",
        "Switch GLM back to personal-plan mode by clearing organization and project.",
        PropertyList(),
        [this](const PropertyList& /*properties*/) -> ReturnValue {
            GlmUsageConfig config = GetConfig();
            config.organization.clear();
            config.project.clear();
            SaveConfig(config);
            RequestRefresh();
            return "GLM team context cleared";
        });

    mcp.AddTool("self.glm_usage.get_config",
        "Get GLM configuration state. The API key is masked and never returned in full.",
        PropertyList(),
        [this](const PropertyList& /*properties*/) -> ReturnValue {
            return CreateConfigJson(GetConfig());
        });

    mcp.AddTool("self.glm_usage.get_usage",
        "Get the latest GLM Coding Plan quota snapshot.",
        PropertyList(),
        [this](const PropertyList& /*properties*/) -> ReturnValue {
            const GlmUsage usage = GetSnapshot();
            const GlmUsageStatus status = GetStatus();
            cJSON* json = cJSON_CreateObject();
            cJSON_AddBoolToObject(json, "ok", status.last_ok);
            cJSON_AddNumberToObject(json, "version", status.version);
            cJSON_AddNumberToObject(json, "updated_at", static_cast<double>(status.updated_at));
            cJSON_AddStringToObject(json, "error", status.error.c_str());
            cJSON_AddStringToObject(json, "level", usage.level.c_str());
            cJSON_AddNumberToObject(json, "tokens_5h_used_pct", usage.tokens_5h_used_pct);
            cJSON_AddNumberToObject(json, "tokens_5h_reset_ms", static_cast<double>(usage.tokens_5h_reset_ms));
            cJSON_AddNumberToObject(json, "tokens_5h_remaining", usage.tokens_5h_remaining);
            cJSON_AddNumberToObject(json, "tokens_week_used_pct", usage.tokens_week_used_pct);
            cJSON_AddNumberToObject(json, "tokens_week_reset_ms", static_cast<double>(usage.tokens_week_reset_ms));
            cJSON_AddNumberToObject(json, "tokens_week_remaining", usage.tokens_week_remaining);
            cJSON_AddNumberToObject(json, "mcp_used", usage.mcp_used);
            cJSON_AddNumberToObject(json, "mcp_total", usage.mcp_total);
            cJSON_AddNumberToObject(json, "mcp_remaining", usage.mcp_remaining);
            cJSON_AddNumberToObject(json, "five_hour_resets_left", usage.five_hour_resets_left);
            cJSON_AddNumberToObject(json, "week_resets_left", usage.week_resets_left);
            return json;
        });

    mcp.AddTool("self.glm_usage.refresh",
        "Request an immediate GLM quota refresh and return current service state.",
        PropertyList(),
        [this](const PropertyList& /*properties*/) -> ReturnValue {
            RequestRefresh();
            const GlmUsageStatus status = GetStatus();
            std::string text = status.busy ? "Refresh already running"
                                           : "Refresh requested; latest_ok=" +
                                                 std::string(status.last_ok ? "true" : "false");
            return text;
        });
}

void GlmUsageService::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (event_group_ == nullptr) {
        event_group_ = xEventGroupCreate();
    }
    if (event_group_ == nullptr) {
        ESP_LOGE(TAG, "event group creation failed");
        return;
    }

    if (worker_ == nullptr) {
        RegisterMcpTools();
        Settings settings("glm", false);
        config_.api_key = settings.GetString("api_key");
        config_.organization = settings.GetString("org_id");
        config_.project = settings.GetString("project_id");
        status_.configured = config_.HasKey();
        status_.team = config_.IsTeam();

        BaseType_t ok = xTaskCreatePinnedToCore(
            [](void* context) {
                static_cast<GlmUsageService*>(context)->Worker();
            },
            "glm_usage", 10240, this, tskIDLE_PRIORITY + 2, &worker_, 0);
        if (ok != pdPASS) {
            worker_ = nullptr;
            ESP_LOGE(TAG, "resident worker start failed");
            return;
        }
        ESP_LOGI(TAG, "resident service started, configured=%d team=%d",
                 status_.configured, status_.team);
        if (config_.HasKey()) {
            xEventGroupSetBits(event_group_, kRefreshBit);
        }
    }
}
