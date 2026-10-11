#pragma once

// main/services/ai_chat/mcp_bridge.h —— 设备 MCP 工具的程序化调用桥(AI 对话
// 通过 function-calling 调用设备自身提供的 self.* 工具)。
//
// 复用 McpServer 公开的 ParseMessage(json, ResponseSink):自注入 sink 捕获
// JSON-RPC 响应,不新增服务器内部 API。工具回调可能碰 NVS/flash,统一 Schedule
// 到 main 任务(内部栈)执行;管线任务(PSRAM 栈)信号量等待。
//
// 生命周期:与 settings.cc 的 SafeSettings 同款双方接力——超时方置 timed_out
// 立即脱手,迟到方收尾;正常路径双方 fetch_add 最后一个清理。main 15s 未执行
// (病态卡死)时整体泄漏换绝对无 UAF。工具不取消,迟到执行完仍会完成自身动作。

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>
#include <string>

#include <cJSON.h>
#include <esp_log.h>

#include "application.h"
#include "mcp_server.h"

namespace mcp_bridge {

constexpr const char* kTag = "mcp_bridge";
constexpr TickType_t kExecTimeoutTicks = pdMS_TO_TICKS(15000);
constexpr size_t kMaxCapturedBytes = 32 * 1024;   // JSON-RPC 响应捕获上限(tools/list 单页 8KB)

struct BridgeState {
    SemaphoreHandle_t done = nullptr;
    std::string req_json;
    std::string captured;
    std::atomic<bool> timed_out{false};
    std::atomic<int> finished{0};

    void FinishOne() {
        if (finished.fetch_add(1) == 1) {
            vSemaphoreDelete(done);
            delete this;
        }
    }
};

// 在 main 任务上同步执行一段 JSON-RPC 请求,捕获响应。返回 false = 超时
// (captured 不可信;状态交 main 侧回收)。
inline bool ExecOnMain(const std::string& req_json, std::string& captured) {
    auto* st = new (std::nothrow) BridgeState();
    if (st == nullptr) return false;
    st->req_json = req_json;
    st->done = xSemaphoreCreateBinary();
    if (st->done == nullptr) {
        delete st;
        return false;
    }
    Application::GetInstance().Schedule([st]() {
        cJSON* req = cJSON_Parse(st->req_json.c_str());
        if (req != nullptr) {
            McpServer::GetInstance().ParseMessage(req, [st](const std::string& resp) {
                if (st->captured.size() < kMaxCapturedBytes) st->captured += resp;
            });
            cJSON_Delete(req);
        }
        xSemaphoreGive(st->done);
        if (st->timed_out.load() || st->finished.fetch_add(1) == 1) {
            vSemaphoreDelete(st->done);
            delete st;
        }
    });
    if (xSemaphoreTake(st->done, kExecTimeoutTicks) == pdTRUE) {
        captured = st->captured;
        st->FinishOne();
        return true;
    }
    st->timed_out.store(true);
    ESP_LOGW(kTag, "main 15s 未完成 MCP 请求,状态交 main 侧回收");
    return false;
}

// tools/call:同步执行单个工具,返回结果文本(content[0].text);ok=false 时
// 返回值为错误说明。
inline std::string CallTool(const std::string& name, const std::string& args_json, bool& ok) {
    ok = false;
    const std::string req = std::string("{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"tools/call\","
                                        "\"params\":{\"name\":\"") + name +
                            "\",\"arguments\":" + (args_json.empty() ? "{}" : args_json) + "}}";
    std::string captured;
    if (!ExecOnMain(req, captured)) return "main 任务超时,本轮工具调用丢弃";

    std::string out;
    cJSON* resp = cJSON_Parse(captured.c_str());
    if (resp != nullptr) {
        const cJSON* result = cJSON_GetObjectItem(resp, "result");
        const cJSON* content = result ? cJSON_GetObjectItem(result, "content") : nullptr;
        const cJSON* first = cJSON_IsArray(content) ? cJSON_GetArrayItem(content, 0) : nullptr;
        const cJSON* text = first ? cJSON_GetObjectItem(first, "text") : nullptr;
        if (cJSON_IsString(text) && text->valuestring != nullptr) out = text->valuestring;
        const cJSON* error = cJSON_GetObjectItem(resp, "error");
        if (cJSON_IsObject(error)) {
            const cJSON* msg = cJSON_GetObjectItem(error, "message");
            out = cJSON_IsString(msg) && msg->valuestring ? msg->valuestring : "tool error";
        } else if (!out.empty()) {
            ok = true;
        }
        cJSON_Delete(resp);
    }
    if (out.size() > 2048) out = out.substr(0, 2048) + "…(截断)";
    if (!ok && out.empty()) out = "工具无输出";
    return out;
}

// tools/list:返回合并(含翻页)后的 OpenAI function-calling tools 数组 JSON:
// [{"type":"function","function":{"name":..,"description":..,"parameters":..}},...]
// exclude_prefix:跳过该前缀的工具(防 AI 对话递归调自己的配置工具)。
inline std::string ToolsListForOpenAi(const char* exclude_prefix) {
    std::string merged = "[";
    bool first = true;
    std::string cursor = "";
    for (int page = 0; page < 8; ++page) {   // 翻页上限,防异常循环
        std::string req = std::string("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/list\","
                                      "\"params\":{\"cursor\":\"") + cursor + "\"}}";
        std::string captured;
        if (!ExecOnMain(req, captured)) {
            ESP_LOGW(kTag, "tools/list 超时(页 %d)", page);
            break;
        }
        cJSON* resp = cJSON_Parse(captured.c_str());
        if (resp == nullptr) break;
        const cJSON* result = cJSON_GetObjectItem(resp, "result");
        const cJSON* tools = result ? cJSON_GetObjectItem(result, "tools") : nullptr;
        if (cJSON_IsArray(tools)) {
            const cJSON* t = nullptr;
            cJSON_ArrayForEach(t, tools) {
                const cJSON* name = cJSON_GetObjectItem(t, "name");
                const cJSON* desc = cJSON_GetObjectItem(t, "description");
                const cJSON* schema = cJSON_GetObjectItem(t, "inputSchema");
                if (!cJSON_IsString(name) || name->valuestring == nullptr) continue;
                if (exclude_prefix != nullptr &&
                    strncmp(name->valuestring, exclude_prefix, strlen(exclude_prefix)) == 0) {
                    continue;
                }
                if (!first) merged += ",";
                first = false;
                merged += "{\"type\":\"function\",\"function\":{\"name\":";
                char* n = cJSON_PrintUnformatted(name);
                merged += n;
                cJSON_free(n);
                if (cJSON_IsString(desc) && desc->valuestring != nullptr) {
                    merged += ",\"description\":";
                    char* d = cJSON_PrintUnformatted(desc);
                    merged += d;
                    cJSON_free(d);
                }
                if (cJSON_IsObject(schema)) {
                    merged += ",\"parameters\":";
                    char* sp = cJSON_PrintUnformatted(schema);
                    merged += sp;
                    cJSON_free(sp);
                }
                merged += "}}";
            }
        }
        const cJSON* next = result ? cJSON_GetObjectItem(result, "nextCursor") : nullptr;
        const bool has_next = cJSON_IsString(next) && next->valuestring != nullptr &&
                              next->valuestring[0] != '\0';
        if (has_next) {
            cursor = next->valuestring;
            cJSON_Delete(resp);
            continue;
        }
        cJSON_Delete(resp);
        break;
    }
    merged += "]";
    return merged;
}

}  // namespace mcp_bridge
