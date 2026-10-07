#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <mutex>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include "services/glm_usage/glm_usage.h"

// Configuration persisted in the `glm` NVS namespace.
struct GlmUsageConfig {
    std::string api_key;
    std::string organization;
    std::string project;

    bool HasKey() const { return !api_key.empty(); }
    bool IsTeam() const { return !organization.empty() && !project.empty(); }
};

// Observable client state. version increases after every worker fetch attempt,
// so lightweight UI polling can distinguish new results from unchanged data.
struct GlmUsageStatus {
    uint32_t version = 0;
    bool configured = false;
    bool team = false;
    bool busy = false;
    bool last_ok = false;
    time_t updated_at = 0;
    std::string error = "尚未查询";
};

// Resident GLM quota client. It owns credentials, polling, HTTP, parsing, and
// MCP registration. The LVGL app is only an observer/configurator.
class GlmUsageService {
public:
    static GlmUsageService& GetInstance();

    // Idempotent. Starts the resident worker and registers MCP tools.
    void Start();

    GlmUsageConfig GetConfig();
    void SaveConfig(GlmUsageConfig config);
    void RequestRefresh();

    GlmUsage GetSnapshot();
    GlmUsageStatus GetStatus();

private:
    GlmUsageService() = default;
    ~GlmUsageService() = default;
    GlmUsageService(const GlmUsageService&) = delete;
    GlmUsageService& operator=(const GlmUsageService&) = delete;

    void RegisterMcpTools();
    void Worker();
    void FetchOnce();
    void StoreResult(bool ok, const GlmUsage& usage, std::string error);

    std::mutex mutex_;
    GlmUsageConfig config_;
    GlmUsage usage_;
    GlmUsageStatus status_;

    EventGroupHandle_t event_group_ = nullptr;
    TaskHandle_t worker_ = nullptr;
};
