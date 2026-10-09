#include "local_mcp_server.h"

#include <condition_variable>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <cstring>

#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_random.h>

#include <cJSON.h>

#include "application.h"
#include "mcp_server.h"
#include "settings.h"

#define TAG "LocalMCP"

namespace {

constexpr const char* kMcpProtocolVersion = "2024-11-05";
constexpr size_t kMaxRequestBody = 16 * 1024;
constexpr TickType_t kToolCallTimeoutMs = 10000;

struct PendingResponse {
    std::mutex mutex;
    std::condition_variable condition;
    bool done = false;
    std::string payload;
};

using PendingResponsePtr = std::shared_ptr<PendingResponse>;

std::string ReadBody(httpd_req_t* req) {
    const size_t content_length = req->content_len;
    if (content_length == 0 || content_length > kMaxRequestBody) {
        return {};
    }

    std::string body(content_length, '\0');
    size_t received = 0;
    while (received < content_length) {
        const int result = httpd_req_recv(req, body.data() + received,
                                          content_length - received);
        if (result <= 0) {
            if (result == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            return {};
        }
        received += static_cast<size_t>(result);
    }
    return body;
}

void SetCommonHeaders(httpd_req_t* req) {
    static const char* kAllowOrigin = "*";
    static const char* kAllowHeaders =
        "Content-Type, Authorization, MCP-Protocol-Version, Mcp-Session-Id";
    static const char* kProtocol = kMcpProtocolVersion;
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", kAllowOrigin);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", kAllowHeaders);
    httpd_resp_set_hdr(req, "MCP-Protocol-Version", kProtocol);
}

void SendJsonRpc(httpd_req_t* req, const std::string& payload) {
    SetCommonHeaders(req);
    httpd_resp_set_type(req, "application/json");
    const esp_err_t result = httpd_resp_send(
        req, payload.data(), static_cast<ssize_t>(payload.size()));
    if (result != ESP_OK) {
        ESP_LOGW(TAG, "response send failed: %s", esp_err_to_name(result));
    }
}

std::string ErrorPayload(const char* code_json_literal, const char* message) {
    std::string payload =
        "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":";
    payload += code_json_literal;
    payload += ",\"message\":\"";
    payload += message;
    payload += "\"}}";
    return payload;
}

bool CheckAuthorization(httpd_req_t* req, const std::string& token) {
    if (token.empty()) {
        return true;
    }

    const size_t header_length = httpd_req_get_hdr_value_len(req, "Authorization");
    if (header_length == 0) {
        return false;
    }
    std::string value(header_length + 1, '\0');
    if (httpd_req_get_hdr_value_str(req, "Authorization", value.data(),
                                    value.size()) != ESP_OK) {
        return false;
    }
    value.resize(header_length);
    return value == "Bearer " + token;
}

esp_err_t PostHandler(httpd_req_t* req) {
    // PIN 即 bearer token(6 位数字),从 RAM 缓存读(避免每次请求都开 NVS)。
    const std::string token = LocalMcpServer::GetInstance().GetPin();
    if (!CheckAuthorization(req, token)) {
        SetCommonHeaders(req);
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_type(req, "text/plain");
        httpd_resp_sendstr(req, "Authorization bearer token required");
        return ESP_OK;
    }

    if (req->content_len == 0) {
        SendJsonRpc(req, ErrorPayload("-32700", "Empty request body"));
        return ESP_OK;
    }
    if (req->content_len > kMaxRequestBody) {
        SendJsonRpc(req, ErrorPayload("-32600", "Request body too large"));
        return ESP_OK;
    }

    const std::string body = ReadBody(req);
    if (body.empty()) {
        SendJsonRpc(req, ErrorPayload("-32700", "Failed to read request body"));
        return ESP_OK;
    }

    cJSON* request = cJSON_ParseWithLength(body.c_str(), body.size());
    if (request == nullptr) {
        SendJsonRpc(req, ErrorPayload("-32700", "Invalid JSON"));
        return ESP_OK;
    }

    // If there is no response after a notification, Streamable HTTP requires
    // HTTP 202 with no body. Requests with an invalid shape fall back to an
    // internal JSON-RPC error so clients never hang.
    const cJSON* id = cJSON_GetObjectItem(request, "id");
    const bool expects_response = id != nullptr && !cJSON_IsNull(id);
    const cJSON* method = cJSON_GetObjectItem(request, "method");
    const bool is_notification = !expects_response && cJSON_IsString(method) &&
        strncmp(method->valuestring, "notifications", sizeof("notifications") - 1) == 0;
    auto pending = std::make_shared<PendingResponse>();
    McpServer::ResponseSink sink = [pending](const std::string& payload) {
        {
            std::lock_guard<std::mutex> lock(pending->mutex);
            pending->payload = payload;
            pending->done = true;
        }
        pending->condition.notify_all();
    };

    McpServer::GetInstance().ParseMessage(request, sink);
    cJSON_Delete(request);

    if (!pending->done && is_notification) {
        httpd_resp_set_status(req, "202 Accepted");
        SetCommonHeaders(req);
        httpd_resp_send(req, nullptr, 0);
        return ESP_OK;
    }

    {
        std::unique_lock<std::mutex> lock(pending->mutex);
        const bool finished = pending->condition.wait_for(
            lock, std::chrono::milliseconds(kToolCallTimeoutMs),
            [&pending] { return pending->done; });
        if (finished) {
            SendJsonRpc(req, pending->payload);
            return ESP_OK;
        }
    }

    if (expects_response) {
        SendJsonRpc(req, ErrorPayload("-32603", "MCP tool call timed out"));
    } else {
        httpd_resp_set_status(req, "202 Accepted");
        SetCommonHeaders(req);
        httpd_resp_send(req, nullptr, 0);
    }
    return ESP_OK;
}

esp_err_t OptionsHandler(httpd_req_t* req) {
    static const char* kAllowMethods = "GET, POST, DELETE, OPTIONS";
    SetCommonHeaders(req);
    httpd_resp_set_hdr(req, "Allow", kAllowMethods);
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

esp_err_t GetHandler(httpd_req_t* req) {
    (void)req;
    SetCommonHeaders(req);
    httpd_resp_set_status(req, "405 Method Not Allowed");
    httpd_resp_set_type(req, "application/json");
    static const char* response =
        "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32000,"
        "\"message\":\"GET SSE is not supported; use POST /mcp\"}}";
    httpd_resp_sendstr(req, response);
    return ESP_OK;
}

esp_err_t DeleteHandler(httpd_req_t* req) {
    (void)req;
    SetCommonHeaders(req);
    httpd_resp_set_status(req, "204 No Content");
    httpd_resp_send(req, nullptr, 0);
    return ESP_OK;
}

}  // namespace

void LocalMcpServer::Start() {
    if (running_.exchange(true)) {
        return;
    }

    // Start 在 boot 的 main 任务(内部栈)执行,可安全读 NVS。把 PIN 载入 RAM 缓存,
    // 之后每个请求从缓存校验,不再每次开 NVS。
    {
        Settings settings("local_mcp", false);
        std::lock_guard<std::mutex> lock(pin_mutex_);
        pin_ = settings.GetString("token");
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 8080;
    config.ctrl_port = 32769;
    config.stack_size = 12288;
    config.max_uri_handlers = 8;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 10;

    httpd_handle_t server = nullptr;
    esp_err_t result = httpd_start(&server, &config);
    if (result != ESP_OK || server == nullptr) {
        running_.store(false);
        ESP_LOGE(TAG, "start failed on :8080: %s", esp_err_to_name(result));
        return;
    }
    server_ = server;

    httpd_uri_t post_uri = {};
    post_uri.uri = "/mcp";
    post_uri.method = HTTP_POST;
    post_uri.handler = PostHandler;
    post_uri.user_ctx = nullptr;
    httpd_uri_t get_uri = {};
    get_uri.uri = "/mcp";
    get_uri.method = HTTP_GET;
    get_uri.handler = GetHandler;
    get_uri.user_ctx = nullptr;
    httpd_uri_t options_uri = {};
    options_uri.uri = "/mcp";
    options_uri.method = HTTP_OPTIONS;
    options_uri.handler = OptionsHandler;
    options_uri.user_ctx = nullptr;
    httpd_uri_t delete_uri = {};
    delete_uri.uri = "/mcp";
    delete_uri.method = HTTP_DELETE;
    delete_uri.handler = DeleteHandler;
    delete_uri.user_ctx = nullptr;

    result = httpd_register_uri_handler(server, &post_uri);
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(server, &get_uri);
    }
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(server, &options_uri);
    }
    if (result == ESP_OK) {
        result = httpd_register_uri_handler(server, &delete_uri);
    }
    if (result != ESP_OK) {
        httpd_stop(server);
        server_ = nullptr;
        running_.store(false);
        ESP_LOGE(TAG, "register /mcp failed");
        return;
    }
    ESP_LOGI(TAG, "resident HTTP MCP listening on port 8080 at /mcp");
}

void LocalMcpServer::Stop() {
    if (!running_.exchange(false)) {
        return;
    }
    auto server = static_cast<httpd_handle_t>(server_);
    if (server != nullptr) {
        httpd_stop(server);
        server_ = nullptr;
    }
}

// ---- 6 位数字 PIN 认证(需求一.5) ----

bool LocalMcpServer::AuthEnabled() {
    std::lock_guard<std::mutex> lock(pin_mutex_);
    return !pin_.empty();
}

std::string LocalMcpServer::GetPin() {
    std::lock_guard<std::mutex> lock(pin_mutex_);
    return pin_;
}

void LocalMcpServer::SetPin(const std::string& pin) {
    {
        std::lock_guard<std::mutex> lock(pin_mutex_);
        if (pin_ == pin) {
            return;
        }
        pin_ = pin;
    }
    // 落盘必须离开 LVGL worker(PSRAM 栈,写 flash 会关 cache 触发断言),调度到 main。
    // 缓存已先行更新,UI 立即生效;写盘是异步收尾。
    Application::GetInstance().Schedule([pin]() {
        Settings settings("local_mcp", true);
        settings.SetString("token", pin);
    });
}

void LocalMcpServer::SetAuthEnabled(bool enable) {
    if (enable) {
        if (AuthEnabled()) {
            return;   // 已有 PIN,保持
        }
        RegeneratePin();
        return;
    }
    SetPin("");   // 清空 = 关闭认证(放开)
}

void LocalMcpServer::RegeneratePin() {
    // %06u:6 位数字(含前导零),方便局域网内手抄填写,取代长 bearer token。
    char pin[8];
    snprintf(pin, sizeof(pin), "%06u", static_cast<unsigned>(esp_random() % 1000000));
    SetPin(pin);
    ESP_LOGI(TAG, "MCP auth PIN regenerated");
}
