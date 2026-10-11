#include "services/glm_usage/glm_usage_service.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <memory>
#include <vector>

#include <esp_log.h>

#include "application.h"
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include "utils/psr_worker.h"
#include <mbedtls/base64.h>
#include <mbedtls/md.h>

#include <cJSON.h>

#include "api_http.h"
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
constexpr const char* kCustomerInfoUrl =
    "https://bigmodel.cn/api/biz/customer/getCustomerInfo";
constexpr const char* kQuotaUrl =
    "https://open.bigmodel.cn/api/monitor/usage/quota/limit?type=2";
constexpr const char* kResetUrl =
    "https://open.bigmodel.cn/api/biz/customer-package-reset/list?targetType=TEAM";

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

std::string Base64Url(const unsigned char* data, size_t size) {
    size_t encoded_size = ((size + 2) / 3) * 4 + 1;
    std::string encoded(encoded_size, '\0');
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(encoded.data()),
                              encoded.size(), &encoded_size, data, size) != 0) {
        ESP_LOGE(TAG, "base64 encode failed: size=%u buffer=%u",
                 static_cast<unsigned>(size), static_cast<unsigned>(encoded.size()));
        return {};
    }
    encoded.resize(encoded_size);
    std::replace(encoded.begin(), encoded.end(), '+', '-');
    std::replace(encoded.begin(), encoded.end(), '/', '_');
    while (!encoded.empty() && encoded.back() == '=') {
        encoded.pop_back();
    }
    return encoded;
}

std::string Base64UrlText(const std::string& text) {
    return Base64Url(reinterpret_cast<const unsigned char*>(text.data()), text.size());
}

// BigModel monitor APIs expect a short-lived HS256 JWT generated from the
// normal `<id>.<secret>` API key, not the raw credential.
std::string MakeApiAuthorization(const std::string& api_key) {
    const size_t dot = api_key.find('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 >= api_key.size()) {
        return api_key;
    }

    const std::string key_id = api_key.substr(0, dot);
    const std::string secret = api_key.substr(dot + 1);
    const time_t now = time(nullptr);
    cJSON* claims = cJSON_CreateObject();
    cJSON_AddStringToObject(claims, "api_key", key_id.c_str());
    cJSON_AddNumberToObject(claims, "exp", static_cast<double>(now + 3600));
    cJSON_AddNumberToObject(claims, "timestamp", static_cast<double>(now));
    char* claims_text = cJSON_PrintUnformatted(claims);
    cJSON_Delete(claims);
    if (claims_text == nullptr) {
        return {};
    }

    const std::string header = Base64UrlText(R"({"alg":"HS256","sign_type":"SIGN"})");
    const std::string payload = Base64UrlText(claims_text);
    cJSON_free(claims_text);
    if (header.empty() || payload.empty()) {
        return {};
    }
    const std::string signing_input = header + "." + payload;
    if (header.empty() || payload.empty()) {
        ESP_LOGE(TAG, "api jwt encode failed: header=%u payload=%u",
                 static_cast<unsigned>(header.size()),
                 static_cast<unsigned>(payload.size()));
        return {};
    }

    unsigned char mac[32] = {};
    const mbedtls_md_info_t* md_info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (md_info == nullptr ||
        mbedtls_md_hmac(md_info,
                        reinterpret_cast<const unsigned char*>(secret.data()), secret.size(),
                        reinterpret_cast<const unsigned char*>(signing_input.data()),
                        signing_input.size(), mac) != 0) {
        ESP_LOGE(TAG, "api jwt hmac failed");
        return {};
    }
    const std::string signature = Base64Url(mac, sizeof(mac));
    if (signature.empty()) {
        ESP_LOGE(TAG, "api jwt signature encode failed");
        return {};
    }
    return signing_input + "." + signature;
}

std::string BusinessErrorMessage(const std::string& body, const std::string& fallback) {
    cJSON* json = cJSON_Parse(body.c_str());
    if (json != nullptr) {
        cJSON* message = cJSON_GetObjectItem(json, "msg");
        if (cJSON_IsString(message) && message->valuestring != nullptr &&
            message->valuestring[0] != '\0') {
            std::string result = message->valuestring;
            cJSON_Delete(json);
            return result;
        }
        cJSON_Delete(json);
    }
    return fallback;
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
    // bigmodel 是第三方接口:不带 Device-Id 公共头,Authorization 走显式头。
    api::HttpFetchOptions opts;
    opts.timeout_ms = kHttpTimeoutMs;
    opts.send_common_headers = false;
    api::HeaderList all_headers;
    all_headers.emplace_back("Accept", "application/json");
    for (const auto& header : headers) {
        all_headers.push_back(header);
    }
    api::HttpReply rep = api::HttpFetch("GET", url, std::string(), opts, all_headers);
    result.status = rep.status;
    result.body = std::move(rep.body);
    result.ok = rep.ok;
    result.error = rep.ok ? std::string() : rep.error;
    return result.ok;
}

void AppendContextHeaders(std::vector<std::pair<std::string, std::string>>& headers,
                          const std::string& api_key, const GlmUsageContext& context) {
    const std::string authorization = MakeApiAuthorization(api_key);
    ESP_LOGI(TAG, "quota auth prepared: api_key_len=%u auth_len=%u org_len=%u project_len=%u",
             static_cast<unsigned>(api_key.size()),
             static_cast<unsigned>(authorization.size()),
             static_cast<unsigned>(context.organization.size()),
             static_cast<unsigned>(context.project.size()));
    headers.emplace_back("Authorization", authorization);
    headers.emplace_back("bigmodel-organization", context.organization);
    headers.emplace_back("bigmodel-project", context.project);
}

std::string GetText(const cJSON* object, const char* key, const std::string& fallback = {}) {
    if (!cJSON_IsObject(object)) {
        return fallback;
    }
    const cJSON* value = cJSON_GetObjectItem(object, key);
    return cJSON_IsString(value) && value->valuestring != nullptr
        ? std::string(value->valuestring) : fallback;
}

bool GetBool(const cJSON* object, const char* key) {
    if (!cJSON_IsObject(object)) {
        return false;
    }
    const cJSON* value = cJSON_GetObjectItem(object, key);
    return cJSON_IsBool(value) && cJSON_IsTrue(value);
}

int GetInt(const cJSON* object, const char* key, int fallback = 0) {
    if (!cJSON_IsObject(object)) {
        return fallback;
    }
    const cJSON* value = cJSON_GetObjectItem(object, key);
    return cJSON_IsNumber(value) ? value->valueint : fallback;
}

cJSON* ContextsToJson(const std::vector<GlmUsageContext>& contexts) {
    cJSON* root = cJSON_CreateArray();
    for (const auto& context : contexts) {
        cJSON* item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "organization", context.organization.c_str());
        cJSON_AddStringToObject(item, "project", context.project.c_str());
        cJSON_AddStringToObject(item, "name", context.name.c_str());
        cJSON_AddNumberToObject(item, "project_type", context.project_type);
        cJSON_AddBoolToObject(item, "organization_default", context.organization_default);
        cJSON_AddBoolToObject(item, "project_default", context.project_default);
        cJSON_AddItemToArray(root, item);
    }
    return root;
}

std::vector<GlmUsageContext> ContextsFromJson(const std::string& json) {
    std::vector<GlmUsageContext> contexts;
    cJSON* root = cJSON_Parse(json.c_str());
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return contexts;
    }

    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, root) {
        GlmUsageContext context;
        context.organization = GetText(item, "organization");
        context.project = GetText(item, "project");
        context.name = GetText(item, "name");
        context.project_type = GetInt(item, "project_type");
        context.organization_default = GetBool(item, "organization_default");
        context.project_default = GetBool(item, "project_default");
        if (!context.organization.empty() && !context.project.empty()) {
            if (context.name.empty()) {
                context.name = context.organization;
            }
            contexts.push_back(context);
        }
    }
    cJSON_Delete(root);
    return contexts;
}

}  // namespace

namespace {

// GLM worker 任务栈 16KB,被 IDF 静默放到 PSRAM(CONFIG_SPIRAM_ALLOW_STACK_
// EXTERNAL_MEMORY)。PSRAM 栈任务做 NVS 写 = flash 写关 cache = 断言崩机
// (实测:每次周期刷新随机崩,表现为"点任何东西都可能死机 20s")。
// 所有 NVS 写统一投递到 main 任务(12KB 内部 RAM 栈,语音页同款安全路径)。
void PersistOnMainTask(std::function<void()> write) {
    Application::GetInstance().Schedule([write]() { write(); });
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

std::vector<GlmUsageContext> GlmUsageService::GetContexts() {
    std::lock_guard<std::mutex> lock(mutex_);
    return contexts_;
}

GlmUsageStatus GlmUsageService::GetStatus() {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void GlmUsageService::Configure(const std::string& api_key, const std::string& jwt_token) {
    GlmUsageConfig config;
    config.api_key = Trim(api_key);
    const std::string token = Trim(jwt_token);
    if (!config.HasKey()) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.configured = config_.HasKey();
        status_.error = "API Key 不能为空";
        ++status_.version;
        return;
    }

    SaveApiKey(config.api_key);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config_ = config;
        pending_web_token_ = token;
        status_.configured = true;
        status_.busy = true;
        status_.discovering = !token.empty();
        status_.last_ok = false;
        status_.updated_at = time(nullptr);
        status_.error = token.empty() ? "使用已保存上下文刷新" : "正在发现机构/项目";
        ++status_.version;
    }
    SignalWorker();
}

void GlmUsageService::SelectContext(size_t index) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (index >= contexts_.size()) {
            return;
        }
        status_.active_context = index;
        status_.last_ok = contexts_[index].last_ok;
        status_.error = contexts_[index].error;
        status_.updated_at = contexts_[index].updated_at;
        ++status_.version;
    }
    RequestRefresh();
}

void GlmUsageService::RequestRefresh() {
    if (event_group_ != nullptr) {
        xEventGroupSetBits(event_group_, kRefreshBit);
    }
}

void GlmUsageService::StoreContextResult(size_t index, const GlmUsageContext& context) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (index >= contexts_.size()) {
        return;
    }
    contexts_[index] = context;
    if (index == status_.active_context) {
        status_.last_ok = context.last_ok;
        status_.error = context.error;
        status_.updated_at = context.updated_at;
    }
    ++status_.version;
}

void GlmUsageService::StoreGlobalResult() {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.busy = false;
    status_.discovering = false;
    status_.updated_at = time(nullptr);
    if (status_.active_context < contexts_.size()) {
        const GlmUsageContext& context = contexts_[status_.active_context];
        status_.last_ok = context.last_ok;
        status_.error = context.error;
    } else if (contexts_.empty()) {
        status_.last_ok = false;
        status_.error = "没有已发现的机构/项目";
    }
    ++status_.version;
}

void GlmUsageService::FetchOnce() {
    GlmUsageConfig config;
    std::string token;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        config = config_;
        token = pending_web_token_;
        status_.busy = true;
        status_.discovering = !token.empty();
        status_.error = token.empty() ? status_.error : "正在发现机构/项目";
        ++status_.version;
    }

    if (!config.HasKey()) {
        std::lock_guard<std::mutex> lock(mutex_);
        status_.busy = false;
        status_.discovering = false;
        status_.last_ok = false;
        status_.error = "请先通过 MCP 配置 API Key";
        ++status_.version;
        return;
    }

    if (!token.empty()) {
        std::string error;
        if (!DiscoverContexts(token, error)) {
            std::lock_guard<std::mutex> lock(mutex_);
            pending_web_token_.clear();
            status_.busy = false;
            status_.discovering = false;
            status_.last_ok = false;
            status_.error = error;
            ++status_.version;
            return;
        }
        std::lock_guard<std::mutex> lock(mutex_);
        pending_web_token_.clear();
        status_.discovering = false;
        ++status_.version;
    }

    std::vector<GlmUsageContext> contexts;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        contexts = contexts_;
    }
    if (contexts.empty()) {
        StoreGlobalResult();
        return;
    }

    for (size_t i = 0; i < contexts.size(); ++i) {
        GlmUsageContext context = contexts[i];
        std::string error;
        const bool ok = FetchContext(context, error);
        context.has_usage = true;
        context.last_ok = ok;
        context.error = ok ? "" : error;
        context.updated_at = time(nullptr);
        StoreContextResult(i, context);
        contexts[i] = context;
    }
    StoreGlobalResult();
    PersistContexts();
    ESP_LOGI(TAG, "quota refresh complete: contexts=%u", static_cast<unsigned>(contexts.size()));
}

bool GlmUsageService::DiscoverContexts(const std::string& jwt_token, std::string& error) {
    HttpResult result;
    HttpRequest(kCustomerInfoUrl, {{"Authorization", "Bearer " + jwt_token}}, result);
    cJSON* root = cJSON_Parse(result.body.c_str());
    if (root == nullptr) {
        error = result.error.empty() ? "发现接口响应格式错误" : result.error;
        return false;
    }

    const cJSON* code = cJSON_GetObjectItem(root, "code");
    const cJSON* data = cJSON_GetObjectItem(root, "data");
    const cJSON* organizations = cJSON_IsObject(data)
        ? cJSON_GetObjectItem(data, "organizations") : nullptr;
    if (!cJSON_IsNumber(code) || code->valueint != 200 || !cJSON_IsArray(organizations)) {
        error = BusinessErrorMessage(result.body, "Web Token 无效或无法读取机构");
        cJSON_Delete(root);
        return false;
    }

    std::vector<GlmUsageContext> contexts;
    size_t best_index = 0;
    int best_score = -1;
    const cJSON* org_item = nullptr;
    cJSON_ArrayForEach(org_item, organizations) {
        const std::string organization = GetText(org_item, "organizationId");
        if (organization.empty()) {
            continue;
        }
        std::string organization_name = GetText(org_item, "organizationName");
        if (organization_name.empty()) {
            organization_name = organization;
        }
        const bool org_default = GetBool(org_item, "isDefault");
        const std::string org_role = GetText(org_item, "role");

        const cJSON* projects = cJSON_GetObjectItem(org_item, "projects");
        if (!cJSON_IsArray(projects)) {
            continue;
        }
        const cJSON* project_item = nullptr;
        cJSON_ArrayForEach(project_item, projects) {
            GlmUsageContext context;
            context.organization = organization;
            context.project = GetText(project_item, "projectId");
            if (context.project.empty()) {
                continue;
            }
            context.name = GetText(project_item, "projectName");
            if (context.name.empty()) {
                context.name = organization_name;
            }
            context.project_type = GetInt(project_item, "projectType");
            context.organization_default = org_default;
            context.project_default = GetBool(project_item, "isDefault");

            int score = context.project_type == 2 ? 100 : 0;
            if (context.project_default) score += 8;
            if (context.organization_default) score += 4;
            if (org_role == "owner") score += 1;
            if (score > best_score) {
                best_score = score;
                best_index = contexts.size();
            }
            contexts.push_back(context);
        }
    }
    cJSON_Delete(root);

    if (contexts.empty()) {
        error = "未发现可用的机构/项目";
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        contexts_ = contexts;
        status_.active_context = best_index;
        status_.context_count = contexts.size();
        status_.configured = config_.HasKey();
        status_.last_ok = false;
        status_.updated_at = time(nullptr);
        status_.error = "发现完成";
        ++status_.version;
    }
    PersistContexts();

    ESP_LOGI(TAG, "discovered GLM contexts: count=%u active=%u",
             static_cast<unsigned>(contexts.size()), static_cast<unsigned>(best_index));
    return true;
}

bool GlmUsageService::FetchContext(GlmUsageContext& context, std::string& error) {
    std::vector<std::pair<std::string, std::string>> headers;
    std::string api_key;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        api_key = config_.api_key;
    }
    AppendContextHeaders(headers, api_key, context);

    HttpResult result;
    HttpRequest(kQuotaUrl, headers, result);
    const bool recognized = GlmUsageParse(result.body, context.usage);
    bool ok = result.status == 200 && recognized && context.usage.ok;
    if (!ok) {
        if (recognized && !context.usage.msg.empty()) {
            error = context.usage.msg;
        } else if (!result.error.empty()) {
            error = result.error;
        } else {
            error = "响应格式错误";
        }
        return false;
    }

    int five = -1;
    int week = -1;
    bool got = false;
    for (int attempt = 0; attempt < 4 && !got; ++attempt) {
        HttpResult reset_result;
        std::vector<std::pair<std::string, std::string>> reset_headers;
        AppendContextHeaders(reset_headers, api_key, context);
        HttpRequest(kResetUrl, reset_headers, reset_result);
        if (reset_result.ok && GlmResetsParse(reset_result.body, five, week) &&
            five >= 0 && week >= 0) {
            got = true;
        } else if (attempt < 3) {
            vTaskDelay(pdMS_TO_TICKS(400));
        }
    }
    if (got) {
        context.usage.five_hour_resets_left = five;
        context.usage.week_resets_left = week;
    } else {
        ESP_LOGW(TAG, "reset counts unavailable");
    }
    return true;
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
    // 退出前自清 worker_ 句柄;Stop 靠轮询 worker_==nullptr 判断已收尾 —— 不能对
    // 已自删任务调 eTaskGetState(其 TCB 可能已被 idle 任务回收,读悬空句柄即读
    // 已释放内存,会崩)。return 后由 SpawnPsrWorker 包装层 vTaskDeleteWithCaps 自删。
    {
        std::lock_guard<std::mutex> lock(mutex_);
        worker_ = nullptr;
    }
}

void GlmUsageService::RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool("self.glm_usage.configure",
        "Configure GLM with api_key and a one-shot web jwt_token. The resident worker discovers and stores every organization/project; the token is never persisted.",
        PropertyList({
            Property("api_key", kPropertyTypeString),
            Property("jwt_token", kPropertyTypeString),
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            Configure(properties["api_key"].value<std::string>(),
                      properties["jwt_token"].value<std::string>());
            return "GLM credentials saved; discovering all organization/project contexts";
        });

    mcp.AddTool("self.glm_usage.get_config",
        "Get masked GLM configuration and discovered organization/project contexts.",
        PropertyList(),
        [this](const PropertyList& /*properties*/) -> ReturnValue {
            const GlmUsageConfig config = GetConfig();
            const std::vector<GlmUsageContext> contexts = GetContexts();
            const GlmUsageStatus status = GetStatus();
            cJSON* json = cJSON_CreateObject();
            cJSON_AddBoolToObject(json, "has_api_key", config.HasKey());
            cJSON_AddStringToObject(json, "api_key_masked",
                                    MaskApiKey(config.api_key).c_str());
            cJSON_AddNumberToObject(json, "active_context",
                                    static_cast<double>(status.active_context));
            cJSON* context_json = cJSON_AddArrayToObject(json, "contexts");
            for (const auto& context : contexts) {
                cJSON* item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "name", context.name.c_str());
                cJSON_AddStringToObject(item, "organization", context.organization.c_str());
                cJSON_AddStringToObject(item, "project", context.project.c_str());
                cJSON_AddNumberToObject(item, "project_type",
                                        static_cast<double>(context.project_type));
                cJSON_AddBoolToObject(item, "last_ok", context.last_ok);
                cJSON_AddItemToArray(context_json, item);
            }
            return json;
        });

    mcp.AddTool("self.glm_usage.get_usage",
        "Get GLM usage snapshots for all discovered organization/project contexts.",
        PropertyList(),
        [this](const PropertyList& /*properties*/) -> ReturnValue {
            const std::vector<GlmUsageContext> contexts = GetContexts();
            const GlmUsageStatus status = GetStatus();
            cJSON* json = cJSON_CreateObject();
            cJSON_AddBoolToObject(json, "configured", status.configured);
            cJSON_AddBoolToObject(json, "busy", status.busy);
            cJSON_AddBoolToObject(json, "discovering", status.discovering);
            cJSON_AddNumberToObject(json, "active_context",
                                    static_cast<double>(status.active_context));
            cJSON_AddStringToObject(json, "error", status.error.c_str());
            cJSON* context_json = cJSON_AddArrayToObject(json, "contexts");
            for (const auto& context : contexts) {
                cJSON* item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "name", context.name.c_str());
                cJSON_AddBoolToObject(item, "ok", context.last_ok);
                cJSON_AddStringToObject(item, "error", context.error.c_str());
                cJSON_AddNumberToObject(item, "updated_at",
                                        static_cast<double>(context.updated_at));
                cJSON_AddNumberToObject(item, "tokens_5h_used_pct",
                                        context.usage.tokens_5h_used_pct);
                cJSON_AddNumberToObject(item, "tokens_week_used_pct",
                                        context.usage.tokens_week_used_pct);
                cJSON_AddNumberToObject(item, "mcp_used", context.usage.mcp_used);
                cJSON_AddNumberToObject(item, "mcp_total", context.usage.mcp_total);
                cJSON_AddNumberToObject(item, "mcp_remaining", context.usage.mcp_remaining);
                cJSON_AddNumberToObject(item, "five_hour_resets_left",
                                        context.usage.five_hour_resets_left);
                cJSON_AddNumberToObject(item, "week_resets_left",
                                        context.usage.week_resets_left);
                cJSON_AddItemToArray(context_json, item);
            }
            return json;
        });

    mcp.AddTool("self.glm_usage.refresh",
        "Request an immediate refresh of all discovered GLM contexts.",
        PropertyList(),
        [this](const PropertyList& /*properties*/) -> ReturnValue {
            RequestRefresh();
            return "Refresh requested";
        });
}

void GlmUsageService::PersistContexts() {
    std::vector<GlmUsageContext> contexts;
    size_t active = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        contexts = contexts_;
        active = status_.active_context;
    }

    cJSON* root = ContextsToJson(contexts);
    char* printed = root == nullptr ? nullptr : cJSON_PrintUnformatted(root);
    // NVS 写必须离开本任务(PSRAM 栈),见 PersistOnMainTask 注释。
    const std::string contexts_json = printed != nullptr ? printed : "";
    const int32_t active_saved = static_cast<int32_t>(active);
    PersistOnMainTask([contexts_json, active_saved]() {
        Settings settings("glm", true);
        if (!contexts_json.empty()) {
            settings.SetString("contexts_json", contexts_json);
        }
        settings.SetInt("active_context", active_saved);
        settings.EraseKey("org_id");
        settings.EraseKey("project_id");
    });
    if (printed != nullptr) {
        cJSON_free(printed);
    }
    cJSON_Delete(root);
}

void GlmUsageService::LoadPersisted() {
    Settings settings("glm", false);
    config_.api_key = settings.GetString("api_key");
    contexts_ = ContextsFromJson(settings.GetString("contexts_json"));
    status_.active_context = static_cast<size_t>(
        std::max<int32_t>(0, settings.GetInt("active_context", 0)));
    if (!contexts_.empty() && status_.active_context >= contexts_.size()) {
        status_.active_context = 0;
    }
    status_.context_count = contexts_.size();
    status_.configured = config_.HasKey();
    status_.error = contexts_.empty() ? "尚未通过 MCP 配置" : "尚未查询";
}

void GlmUsageService::SaveApiKey(const std::string& api_key) {
    PersistOnMainTask([api_key]() {
        Settings settings("glm", true);
        settings.SetString("api_key", api_key);
    });
}

void GlmUsageService::SignalWorker() {
    if (event_group_ != nullptr) {
        xEventGroupSetBits(event_group_, kRefreshBit);
    }
}

ServiceState GlmUsageService::GetState() const {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!inited_) {
        return ServiceState::Uninitialized;
    }
    if (worker_ != nullptr) {
        return ServiceState::Running;
    }
    return ServiceState::Stopped;
}

esp_err_t GlmUsageService::Init() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (inited_) {
        return ESP_OK;
    }
    if (event_group_ == nullptr) {
        event_group_ = xEventGroupCreate();
        if (event_group_ == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }
    LoadPersisted();
    RegisterMcpTools();
    inited_ = true;
    ESP_LOGI(TAG, "inited: configured=%d contexts=%u (worker 未启动,不占 16K)",
             status_.configured, static_cast<unsigned>(status_.context_count));
    return ESP_OK;
}

esp_err_t GlmUsageService::Start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (event_group_ == nullptr) {
        event_group_ = xEventGroupCreate();
    }
    if (event_group_ == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    // 工具注册与持久化载入在 boot 期 Init()(main 任务,内部栈)完成。此处
    // 绝不做 NVS:Start 由 GLM 用量屏在 LVGL worker(PSRAM 栈)上直调,flash
    // 读会关 cache 触发断言崩机。正常时序 Init 一定先于任何屏幕,走不到这里。
    if (!inited_) {
        ESP_LOGW(TAG, "start before init (unexpected order); skip NVS on LVGL stack");
        return ESP_ERR_INVALID_STATE;
    }

    // 进屏即查:无论本次是新建还是复用 worker,已配置都置刷新位。原来只在
    // "新建 worker 成功"后才置位,快速"退出→重进"复用旧 worker 时走了
    // worker_!=nullptr 的早退、不置位 → 不触发刷新 → 卡"尚未查询"(需求二根因)。
    if (config_.HasKey()) {
        xEventGroupSetBits(event_group_, kRefreshBit);
    }
    if (worker_ != nullptr) {
        return ESP_OK;   // 已在跑:复用(刷新位已置,worker 会拾取)
    }

    // 清残留 kStopBit:上次 Stop 若因 worker 卡在拉取而超时,stop 位可能没被
    // WaitBits 消费掉。不清掉会让新 worker 一启动 WaitBits 就读到 kStopBit 自杀。
    xEventGroupClearBits(event_group_, kStopBit);

    auto* service = this;
    // 栈放 PSRAM(统一入口 SpawnPsrWorker):16KB 连续内部 RAM 在 syslog 环(8KB)
    // +time_svc(4KB)加入后常态分配失败(实测 "resident worker start failed")。
    // 先例:radio 任务同为 PSRAM 栈且跑完整 https 流。Worker return 即自动自删。
    if (!SpawnPsrWorker("glm_usage", 16384, tskIDLE_PRIORITY + 2,
                        [](void* context) {
                            static_cast<GlmUsageService*>(context)->Worker();
                        },
                        service, &worker_, 0)) {
        // 创建失败(内部 RAM 不足/碎片化)要落进状态,否则 UI 只显示默认的
        // "尚未查询",用户看不出是任务没起来。已在 mutex_ 内,可直接写 status_。
        status_.last_ok = false;
        status_.error = "查询任务启动失败(内存不足),请重进或重启";
        ++status_.version;
        ESP_LOGE(TAG, "resident worker start failed (internal RAM low/fragmented)");
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "resident service started, configured=%d contexts=%u",
             status_.configured, static_cast<unsigned>(status_.context_count));
    return ESP_OK;
}

void GlmUsageService::Stop() {
    // 由屏幕 OnScreenUnloaded Schedule 到 main 任务(轮询会阻塞,不能跑在 LVGL worker)。
    // 置 kStopBit 让 Worker() 退出;Worker 退出前自清 worker_,故这里轮询 worker_
    // 成员判断收尾 —— 绝不 eTaskGetState 悬空句柄(自删任务的 TCB 可能已被 idle 回收)。
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (event_group_ == nullptr || worker_ == nullptr) {
            return;
        }
        xEventGroupSetBits(event_group_, kStopBit);
    }
    int waited = 0;
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (worker_ == nullptr) break;   // worker 已退出并自清句柄
        }
        if (waited >= 3000) {
            // 病态:worker 卡在一次慢拉取里。此处不置空 worker_ —— 留着它,Start 看到
            // worker_!=nullptr 便幂等返回,不会重复建任务;等旧 worker 的网络调用超时
            // 返回后,它会读 kStopBit 自行退出并自清 worker_。
            ESP_LOGW(TAG, "stop: worker 3s 未退出(可能在慢网络拉取中),待其自行收尾");
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
        waited += 50;
    }
    ESP_LOGI(TAG, "stopped: worker 已退出,16K 任务栈已释放");
}
