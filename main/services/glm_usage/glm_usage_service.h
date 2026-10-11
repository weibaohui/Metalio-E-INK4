#pragma once

#include <cstdint>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include "services/glm_usage/glm_usage.h"
#include "services/app_service.h"

// A discovered organization/project pair. Usage snapshots are stored per pair,
// so the dashboard can switch between contexts without refetching.
struct GlmUsageContext {
    std::string organization;
    std::string project;
    std::string name;

    int project_type = 0;
    bool organization_default = false;
    bool project_default = false;

    bool has_usage = false;
    bool last_ok = false;
    time_t updated_at = 0;
    std::string error = "尚未查询";
    GlmUsage usage;
};

// Only api_key is persisted. The Web JWT is one-shot and lives only long
// enough for the resident worker to discover all available contexts.
struct GlmUsageConfig {
    std::string api_key;

    bool HasKey() const { return !api_key.empty(); }
};

struct GlmUsageStatus {
    uint32_t version = 0;
    bool configured = false;
    bool busy = false;
    bool discovering = false;
    bool last_ok = false;
    time_t updated_at = 0;
    size_t active_context = 0;
    size_t context_count = 0;
    std::string error = "尚未查询";
};

// Resident GLM quota service and MCP tool provider. The LVGL app is only an
// observer and context switcher.
class GlmUsageService : public AppService {
public:
    static GlmUsageService& GetInstance();

    const char* GetName() const override { return "GlmUsageService"; }
    ServiceState GetState() const override;

    // boot 期一次性:创建 event group、载入持久化、注册 MCP 工具。不跑 worker,
    // 故不占 16K 任务内存;MCP self.glm_usage.* 在屏幕未开时仍可返回缓存快照。
    esp_err_t Init() override;
    // 启动 16K 常驻 worker(进入 GLM 用量屏时调);Stop 配对释放(离屏时调)。
    esp_err_t Start() override;
    void Stop() override;

    GlmUsageConfig GetConfig();
    std::vector<GlmUsageContext> GetContexts();
    GlmUsageStatus GetStatus();

    // MCP-facing one-step setup: save the API key and use the Web JWT once to
    // discover every organization/project offered by the account.
    void Configure(const std::string& api_key, const std::string& jwt_token);
    void SelectContext(size_t index);
    void RequestRefresh();

private:
    GlmUsageService() = default;
    ~GlmUsageService() = default;
    GlmUsageService(const GlmUsageService&) = delete;
    GlmUsageService& operator=(const GlmUsageService&) = delete;

    void RegisterMcpTools();
    void Worker();
    void FetchOnce();

    bool DiscoverContexts(const std::string& jwt_token, std::string& error);
    bool FetchContext(GlmUsageContext& context, std::string& error);
    void StoreContextResult(size_t index, const GlmUsageContext& context);
    void StoreGlobalResult();
    void PersistContexts();
    void LoadPersisted();
    void SaveApiKey(const std::string& api_key);
    void SignalWorker();

    mutable std::mutex mutex_;
    GlmUsageConfig config_;
    std::vector<GlmUsageContext> contexts_;
    GlmUsageStatus status_;
    std::string pending_web_token_;

    EventGroupHandle_t event_group_ = nullptr;
    TaskHandle_t worker_ = nullptr;
    bool inited_ = false;   // Init() 已执行(注册工具+载入持久化),幂等
};
