#pragma once

// main/services/ai_chat/mcp_bridge.h —— 设备 MCP 工具的程序化调用桥(AI 对话
// 通过 function-calling 调用设备自身提供的 self.* 工具)。
//
// 复用 McpServer 公开的 ParseMessage(json, ResponseSink):自注入 sink 捕获
// JSON-RPC 响应,不新增服务器内部 API。工具回调可能碰 NVS/flash,统一 Schedule
// 到 main 任务(内部栈)执行;管线任务(PSRAM 栈)信号量等待。
//
// 生命周期:shared_ptr 全权管理(2026-10-11 崩溃修复)。旧手写 timed_out/
// finished 双方接力协议有洞:DoToolCall 内部再 Schedule 的工具 lambda 持有
// sink 副本(即 st 指针),不被 finished 计数 —— 工具稍慢的时序下 st 已被
// delete 而 sink 仍执行 captured += resp,向已释放内存写入 = 堆破坏
// (实测 LoadProhibited 崩溃于本文件捕获 sink 处,与 2026-10-11 晨 aichat
// free(0x2b) 堆破坏同源)。现 main 侧 lambda 与 sink 全部按值持有 shared_ptr,
// 最后释放方析构状态(信号量在析构里删),超时路径无需特殊回收。

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <atomic>
#include <memory>
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

    ~BridgeState() {
        if (done != nullptr) {
            vSemaphoreDelete(done);
        }
    }
};

// 在 main 任务上同步执行一段 JSON-RPC 请求,捕获响应。返回 false = 超时
// (captured 不可信;main 侧持有的 shared_ptr 会自行释放,无泄漏)。
inline bool ExecOnMain(const std::string& req_json, std::string& captured) {
    auto st = std::make_shared<BridgeState>();
    st->req_json = req_json;
    st->done = xSemaphoreCreateBinary();
    if (st->done == nullptr) {
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
    });
    if (xSemaphoreTake(st->done, kExecTimeoutTicks) == pdTRUE) {
        captured = st->captured;
        return true;
    }
    ESP_LOGW(kTag, "main 15s 未完成 MCP 请求(shared_ptr 自动回收,无泄漏)");
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
