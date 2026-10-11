// main/services/ai_chat/ai_chat_service.cc —— 见 ai_chat_service.h(新增 APP:AI 语音对话)。
#include "ai_chat_service.h"

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cJSON.h>

#include "SdCardManager.hpp"
#include "application.h"
#include "boards/common/sd_paths.h"
#include "mcp_bridge.h"
#include "utils/psr_worker.h"
#include "audio_codec.h"
#include "board.h"
#include "power_policy.h"
#include "mcp_server.h"
#include "settings.h"
#include "tts_decoder.h"
#include "minimax_speech.h"

namespace {

constexpr const char* TAG = "AiChat";
constexpr const char* kNvsNs = "aichat";
constexpr int kMaxRecordSec = 15;          // 单次录音上限
constexpr int kSampleRate = 16000;         // 板载 mic 输入采样率

// ---- 配置(NVS + RAM 缓存) ----
struct Cfg {
    std::string stt_url;    // 语音转文字端点(OpenAI 兼容 multipart 转写)
    std::string stt_key;    // MiniMax API Key
    std::string stt_model;  // 转写模型
    std::string llm_base;   // OpenAI 兼容 base(如 https://api.openai.com/v1)
    std::string llm_key;    // 通用 OpenAI 格式 Key
    std::string llm_model;  // 对话模型
    bool auto_tts = false;  // 回复自动发声(MiniMax TTS)
    int history_rounds = 5; // 多轮对话历史轮数(默认 5 轮, 存储在 PSRAM)
};
std::mutex s_cfg_mutex;
Cfg s_cfg;
bool s_cfg_loaded = false;

const char* kDefaultSttUrl = "https://api.minimax.cn/v1/speech_to_text";  // curl 实测 200(2026-10-11);/v1/audio/transcriptions 两域名均 404
const char* kDefaultLlmBase = "https://api.minimax.cn/v1";
const char* kDefaultSttModel = "asr-1.0";  // speech_to_text 唯一合法模型(服务端 400 实证)
const char* kDefaultLlmModel = "MiniMax-M3.1-Flash-Preview";  // MiniMax 默认对话模型;空值曾兜底 gpt-4o-mini 致 400(2026-10-11)

// ---- 运行状态 ----
std::mutex s_state_mutex;
AiChatState s_state = AiChatState::Idle;
std::string s_user_text;
std::string s_reply_text;
std::string s_error;

std::atomic<bool> s_record_flag{false};   // 按住=true
std::atomic<bool> s_cancel{false};
std::atomic<bool> s_interrupt_tts{false};   // 播报中按住说话=打断播报(需求)
std::atomic<bool> s_barge_record{false};    // 本轮录音由"播报中按下"发起(区分单击/按住)
TaskHandle_t s_task = nullptr;

void SetState(AiChatState st, const std::string& err = "") {
    std::lock_guard<std::mutex> lock(s_state_mutex);
    s_state = st;
    if (!err.empty()) s_error = err;
    else if (st != AiChatState::Error) s_error.clear();
}

void SetUserText(const std::string& t) { std::lock_guard<std::mutex> l(s_state_mutex); s_user_text = t; }
void SetReplyText(const std::string& t) { std::lock_guard<std::mutex> l(s_state_mutex); s_reply_text = t; }

// ---- HTTP 助手(整段 SetContent,内部栈任务调用,用于大模型对话) ----
// 返回 HTTP 状态码;resp 填响应体。失败返回 <=0。
int HttpPost(const std::string& url, const std::string& bearer,
             const std::string& content_type, const std::string& body, std::string& resp) {
    if (url.empty()) return -1;
    ESP_LOGI(TAG, "http: create (%u bytes body)", (unsigned)body.size());
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    if (!http) { ESP_LOGE(TAG, "http: create failed"); return -1; }
    http->SetTimeout(20000);
    if (!bearer.empty()) http->SetHeader("Authorization", "Bearer " + bearer);
    http->SetHeader("Content-Type", content_type);
    http->SetContent(std::string(body));   // 整段 Content-Length POST
    ESP_LOGI(TAG, "http: open...");
    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "http: open failed err=0x%x", http->GetLastError());
        http->Close();
        return -2;
    }
    const int status = http->GetStatusCode();
    ESP_LOGI(TAG, "http: status %d, reading", status);
    char chunk[2048];
    while (true) {
        if (s_interrupt_tts.load() || s_cancel.load()) {
            ESP_LOGI(TAG, "http: read interrupted by user");
            break;
        }
        int r = http->Read(chunk, sizeof(chunk));
        if (r <= 0) break;
        resp.append(chunk, r);
    }
    http->Close();
    ESP_LOGI(TAG, "http: done %u bytes", (unsigned)resp.size());
    return status;
}

// STT: 委托给独立 minimax_speech 模块转写 (OpenAI 兼容 multipart WAV 上传)
bool SttTranscribe(const Cfg& cfg, const std::vector<int16_t>& pcm, std::string& text, std::string& err) {
    return minimax_speech::Transcribe(cfg.stt_url, cfg.stt_key, cfg.stt_model, pcm, text, err);
}

// 对话:OpenAI 兼容 chat completions。成功返回 true。
std::string EscapeJson(const std::string& in) {
    std::string out;
    for (char c : in) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': break;
            case '\t': out += "\\t"; break;
            default: out += c; break;
        }
    }
    return out;
}

// ---- 多轮对话历史管理 (严格保存在 PSRAM 中, 杜绝占用宝贵的内部 SRAM) ----
struct ChatHistoryTurn {
    char* user = nullptr;
    char* reply = nullptr;
};

class ChatHistoryManager {
public:
    static constexpr size_t kMaxCapacity = 10;

    ChatHistoryManager() = default;
    ~ChatHistoryManager() { Clear(); }

    void Clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        for (size_t i = 0; i < count_; ++i) {
            FreeTurn(turns_[i]);
        }
        count_ = 0;
    }

    void AddTurn(const std::string& user, const std::string& reply, size_t max_rounds = 5) {
        if (user.empty() || reply.empty()) return;
        if (max_rounds == 0) {
            Clear();
            return;
        }
        if (max_rounds > kMaxCapacity) max_rounds = kMaxCapacity;

        // 截断过长文本(防单轮超长膨胀上下文, 各截断前 1500 字符)
        std::string u_clip = user.size() > 1500 ? user.substr(0, 1500) : user;
        std::string r_clip = reply.size() > 1500 ? reply.substr(0, 1500) : reply;

        char* u_copy = AllocPsramStr(u_clip);
        char* r_copy = AllocPsramStr(r_clip);
        if (!u_copy || !r_copy) {
            if (u_copy) heap_caps_free(u_copy);
            if (r_copy) heap_caps_free(r_copy);
            ESP_LOGW(TAG, "history: PSRAM 分配失败");
            return;
        }

        std::lock_guard<std::mutex> lock(mutex_);
        if (count_ >= max_rounds) {
            // 队列已满: 释放最老的一轮, 其余前移
            FreeTurn(turns_[0]);
            for (size_t i = 0; i + 1 < count_; ++i) {
                turns_[i] = turns_[i + 1];
            }
            turns_[count_ - 1] = {u_copy, r_copy};
        } else {
            turns_[count_++] = {u_copy, r_copy};
        }
        ESP_LOGI(TAG, "history: 新增 1 轮, 当前在 PSRAM 中保留 %u/%u 轮",
                 (unsigned)count_, (unsigned)max_rounds);
    }

    // 格式化历史为 JSON 消息块:
    // {"role":"user","content":"..."},{"role":"assistant","content":"..."},
    std::string BuildMessagesJson(size_t max_rounds) const {
        if (max_rounds == 0) return "";
        std::lock_guard<std::mutex> lock(mutex_);
        if (count_ == 0) return "";

        size_t take = std::min(count_, max_rounds);
        size_t start_idx = count_ - take;

        std::string out;
        for (size_t i = start_idx; i < count_; ++i) {
            if (turns_[i].user && turns_[i].reply) {
                out += "{\"role\":\"user\",\"content\":\"" + EscapeJson(turns_[i].user) + "\"},";
                out += "{\"role\":\"assistant\",\"content\":\"" + EscapeJson(turns_[i].reply) + "\"},";
            }
        }
        return out;
    }

    size_t GetCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return count_;
    }

private:
    static char* AllocPsramStr(const std::string& s) {
        char* p = static_cast<char*>(heap_caps_malloc(s.size() + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (p) {
            memcpy(p, s.c_str(), s.size() + 1);
        }
        return p;
    }

    static void FreeTurn(ChatHistoryTurn& turn) {
        if (turn.user) {
            heap_caps_free(turn.user);
            turn.user = nullptr;
        }
        if (turn.reply) {
            heap_caps_free(turn.reply);
            turn.reply = nullptr;
        }
    }

    mutable std::mutex mutex_;
    ChatHistoryTurn turns_[kMaxCapacity] = {};
    size_t count_ = 0;
};

ChatHistoryManager s_history;

// 语音助手 system prompt:设备身份 + 工具使用规则 + 口语化约束。
// 没有它,模型不知道自己能调工具,容易用嘴"假装执行"(纯文本描述 curl 之类)。
const char* kSystemPrompt =
    "你是 Metalio 墨水屏设备上的语音助手,用户通过语音与你对话,你的回复会被语音播报。\n"
    "规则:\n"
    "1. 用户要求操作设备功能(日程/日历、收音机、对时、音量、系统信息、日志等)时,\n"
    "   必须调用对应工具(tool)完成,不允许只口头承诺或编造执行结果;\n"
    "2. 工具返回结果后,用简短口语告知用户结果;\n"
    "3. 回复要简短、口语化,不要用 markdown、表情符号、代码块;\n"
    "4. 用户说\"今天/明天/下周\"等相对时间时,先参照今天日期换算成 YYYY-MM-DD 再调工具。";

// 今天日期(本地历元模型,TZ=GMT+0;未对时则标注,让模型改问用户明确日期)
std::string TodayLine() {
    time_t now = time(nullptr);
    struct tm tmv = {};
    localtime_r(&now, &tmv);
    if (tmv.tm_year + 1900 < 2025) return "今天日期未知(设备未对时),相对时间请向用户问清具体日期。";
    static const char* kWday[] = {"日", "一", "二", "三", "四", "五", "六"};
    char buf[64];
    snprintf(buf, sizeof(buf), "今天是 %04d-%02d-%02d 周%s。",
             tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, kWday[tmv.tm_wday % 7]);
    return buf;
}

bool LlmChat(const Cfg& cfg, const std::string& user_text, std::string& reply, std::string& err) {
    const std::string url = cfg.llm_base + "/chat/completions";
    const std::string model = cfg.llm_model.empty() ? kDefaultLlmModel : cfg.llm_model;
    // 设备 MCP 工具表(排除 aichat 自身防递归);为空则退化为纯聊天
    const std::string tools = mcp_bridge::ToolsListForOpenAi("self.aichat");
    const bool has_tools = tools.size() > 2;
    if (has_tools) ESP_LOGI(TAG, "tools schema %u bytes", (unsigned)tools.size());

    struct ToolReq { std::string id, name, args; };
    // system 在前:设备身份 + 工具规则 + 今天日期(相对时间换算锚点)
    // 接着注入 PSRAM 中的最近 N 轮历史对话
    // 最后是当前用户的输入
    std::string messages = "[{\"role\":\"system\",\"content\":\"" +
                           EscapeJson(std::string(kSystemPrompt) + "\n" + TodayLine()) +
                           "\"}," +
                           s_history.BuildMessagesJson(cfg.history_rounds) +
                           "{\"role\":\"user\",\"content\":\"" + EscapeJson(user_text) + "\"}]";
    for (int round = 0; round < 4; ++round) {
        std::string body = std::string("{\"model\":\"") + model + "\",\"messages\":" + messages;
        if (has_tools) body += ",\"tools\":" + tools;
        body += "}";
        std::string resp;
        const int status = HttpPost(url, cfg.llm_key, "application/json", body, resp);
        if (status < 200 || status >= 300) {
            // 带出服务端错误体:400 的 message 直接写明原因(model/参数/JSON),
            // 只报状态码等于盲猜(2026-10-11 STT 400 排查教训)
            err = "对话 HTTP " + std::to_string(status) + ": " +
                  (resp.size() > 200 ? resp.substr(0, 200) : resp);
            ESP_LOGE(TAG, "llm http %d body=%u resp=%.200s",
                     status, (unsigned)body.size(), resp.c_str());
            return false;
        }
        cJSON* root = cJSON_Parse(resp.c_str());
        if (root == nullptr) { err = "对话响应非 JSON"; return false; }
        const cJSON* choices = cJSON_GetObjectItem(root, "choices");
        const cJSON* first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : nullptr;
        const cJSON* message = first ? cJSON_GetObjectItem(first, "message") : nullptr;
        const cJSON* tool_calls = message ? cJSON_GetObjectItem(message, "tool_calls") : nullptr;
        const cJSON* content = message ? cJSON_GetObjectItem(message, "content") : nullptr;

        // ---- root 存活期内完成全部 JSON 重组 ----
        std::vector<ToolReq> reqs;
        std::string assistant_json;   // 回填用 assistant 消息(含原样 tool_calls)
        if (cJSON_IsArray(tool_calls) && cJSON_GetArraySize(tool_calls) > 0) {
            std::string tcs = "[";
            bool tfirst = true;
            const cJSON* tc = nullptr;
            cJSON_ArrayForEach(tc, tool_calls) {
                char* tc_raw = cJSON_PrintUnformatted(tc);
                if (tc_raw != nullptr) {
                    if (!tfirst) tcs += ",";
                    tcs += tc_raw;
                    cJSON_free(tc_raw);
                    tfirst = false;
                }
                ToolReq r;
                const cJSON* fn = cJSON_GetObjectItem(tc, "function");
                const cJSON* tc_name = fn ? cJSON_GetObjectItem(fn, "name") : nullptr;
                const cJSON* tc_args = fn ? cJSON_GetObjectItem(fn, "arguments") : nullptr;
                const cJSON* tc_id = cJSON_GetObjectItem(tc, "id");
                if (cJSON_IsString(tc_name) && tc_name->valuestring != nullptr) {
                    r.name = tc_name->valuestring;
                    r.args = cJSON_IsString(tc_args) && tc_args->valuestring ? tc_args->valuestring : "{}";
                    if (cJSON_IsString(tc_id) && tc_id->valuestring != nullptr) r.id = tc_id->valuestring;
                    reqs.push_back(r);
                }
            }
            assistant_json = std::string("{\"role\":\"assistant\",\"content\":");
            if (cJSON_IsString(content)) {
                char* printed = cJSON_PrintUnformatted(content);
                assistant_json += printed;
                cJSON_free(printed);   // 不 free 会被工具轮数放大成泄漏
            } else {
                assistant_json += "null";
            }
            assistant_json += ",\"tool_calls\":" + tcs + "}";
        } else if (cJSON_IsString(content) && content->valuestring != nullptr) {
            reply = content->valuestring;
        }
        cJSON_Delete(root);   // 此后只使用上面重组出的字符串

        if (reqs.empty()) {
            // 无工具调用:最终回答
            if (reply.rfind("<think>", 0) == 0) {
                const size_t end = reply.find("</think>");
                reply = end == std::string::npos ? "" : reply.substr(end + 8);
            }
            while (!reply.empty() && (reply.front() == '\n' || reply.front() == ' ')) reply.erase(reply.begin());
            while (!reply.empty() && (reply.back() == '\n' || reply.back() == ' ')) reply.pop_back();
            if (reply.empty()) { err = "对话结果为空"; return false; }
            return true;
        }

        // ---- 执行工具并回填(纯字符串,与 root 生命周期解耦) ----
        std::string tool_results;
        int executed = 0;
        for (const auto& r : reqs) {
            if (++executed > 6) break;   // 单轮工具数上限
            bool ok = false;
            ESP_LOGI(TAG, "tool call: %s(%s)", r.name.c_str(), r.args.c_str());
            std::string result = mcp_bridge::CallTool(r.name, r.args, ok);
            ESP_LOGI(TAG, "tool %s -> %s: %u bytes", r.name.c_str(), ok ? "ok" : "err",
                     (unsigned)result.size());
            tool_results += "{\"role\":\"tool\",\"content\":\"" + EscapeJson(result) + "\"}";
            if (!r.id.empty()) {
                tool_results.insert(tool_results.size() - 1,
                                    ",\"tool_call_id\":\"" + EscapeJson(r.id) + "\"");
            }
            tool_results += ",";
        }
        if (!tool_results.empty() && tool_results.back() == ',') tool_results.pop_back();
        messages = "[" + messages.substr(1) + "," + assistant_json + "," + tool_results + "]";
    }
    err = "工具调用轮次超限";
    return false;
}

// 对话轮存档:JSONL 追加到 SD(/sdcard/metalio/e-ink/aichat/history.jsonl)。
// PSRAM 栈任务写 SD 合规(纪律只禁内部 flash/NVS);>512KB 轮转保留一代。
void SaveHistory(const std::string& user, const std::string& reply) {
    if (!SdCardManager::GetInstance().IsMounted()) {
        return;   // 无卡静默跳过
    }
    SdEnsureDir(SD_PATH_AICHAT);
    const std::string path = SD_PATH_AICHAT "/history.jsonl";
    struct stat st;
    if (stat(path.c_str(), &st) == 0 && st.st_size > 512 * 1024) {
        const std::string old = SD_PATH_AICHAT "/history.old.jsonl";
        unlink(old.c_str());
        rename(path.c_str(), old.c_str());
    }
    FILE* f = fopen(path.c_str(), "a");
    if (f == nullptr) {
        ESP_LOGW(TAG, "history open failed errno=%d", errno);
        return;
    }
    const std::string line = std::string("{\"ts\":") +
        std::to_string((long long)(esp_timer_get_time() / 1000000LL)) +
        ",\"user\":\"" + EscapeJson(user) +
        "\",\"reply\":\"" + EscapeJson(reply) + "\"}\n";
    fwrite(line.data(), 1, line.size(), f);
    fclose(f);
}

// TTS: 委托给独立 minimax_speech 模块合成 (使用选定的音色/语速/音量/情绪)
bool TtsSynthesize(const Cfg& cfg, const std::string& text, std::vector<int16_t>& pcm, std::string& err) {
    const std::string& key = !cfg.stt_key.empty() ? cfg.stt_key : cfg.llm_key;
    const auto voice_cfg = minimax_speech::GetVoiceConfig();
    return minimax_speech::Synthesize(key, text, voice_cfg, pcm, err, &s_interrupt_tts, &s_cancel);
}

// 播放:直写 codec 输出(16k 单声道)。进屏时已 Schedule codec->Start(),
// 音频服务已停,无争抢。OutputData 阻塞写 I2S,天然按 16k 节奏走。
void PlayPcm(std::vector<int16_t>& pcm) {
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) return;
    codec->EnableOutput(true);
    std::vector<int16_t> chunk;
    chunk.reserve(960);
    for (size_t i = 0; i < pcm.size(); i += 960) {
        // 打断(播报中按住说话)/取消(离屏):立即停播,别整段念完
        if (s_interrupt_tts.load() || s_cancel.load()) {
            ESP_LOGI(TAG, "tts playback interrupted at %u/%u samples",
                     (unsigned)i, (unsigned)pcm.size());
            return;
        }
        chunk.assign(pcm.begin() + i, pcm.begin() + std::min(i + 960, pcm.size()));
        codec->OutputData(chunk);
    }
}

// 录音(在管线任务上跑,内部栈):阻塞读 mic 直到 s_record_flag 清(松手)或超时。
std::vector<int16_t> RecordWhileHeld() {
    std::vector<int16_t> pcm;
    pcm.reserve(kSampleRate * kMaxRecordSec);
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) return pcm;
    const bool two_ch = codec->input_channels() == 2;   // mic + 回声参考,取 mic(偶数序)
    // InputData 契约:按 data.size() 读,必须预 resize(空 vector 读 0 样本即失败)
    const size_t chunk = kSampleRate * 60 / 1000 * (two_ch ? 2 : 1);   // 60ms
    int iters = 0;
    int64_t ch0_sq = 0, ch1_sq = 0;
    int16_t ch0_peak = 0, ch1_peak = 0;
    size_t ch_samples = 0;
    const int64_t deadline = esp_timer_get_time() + (int64_t)kMaxRecordSec * 1000000LL;
    while (s_record_flag.load() && !s_cancel.load() && esp_timer_get_time() < deadline) {
        std::vector<int16_t> raw(chunk);
        if (!codec->InputData(raw) || raw.empty()) {
            ESP_LOGW(TAG, "record: InputData failed (iter %d)", iters);
            break;
        }
        ++iters;
        if (two_ch) {
            for (size_t i = 0; i + 1 < raw.size(); i += 2) {
                int16_t s0 = raw[i];
                int16_t s1 = raw[i + 1];
                pcm.push_back(s0);
                ch0_sq += (int64_t)s0 * s0;
                int16_t a0 = s0 < 0 ? (int16_t)-s0 : s0;
                if (a0 > ch0_peak) ch0_peak = a0;

                ch1_sq += (int64_t)s1 * s1;
                int16_t a1 = s1 < 0 ? (int16_t)-s1 : s1;
                if (a1 > ch1_peak) ch1_peak = a1;
                ++ch_samples;
            }
        } else {
            for (int16_t v : raw) {
                pcm.push_back(v);
                ch0_sq += (int64_t)v * v;
                int16_t a0 = v < 0 ? (int16_t)-v : v;
                if (a0 > ch0_peak) ch0_peak = a0;
                ++ch_samples;
            }
        }
    }
    // 能量统计 + 峰值归一化。本板 I2S 32→16 位 >>12 后语音峰值仅数百(实测 512),
    // 直接上传会被 ASR 判为无语音;基线 AudioCodec::SetInputGain 是从未生效的死钩子。
    float gain = 1.0f;
    if (!pcm.empty() && ch0_peak > 0 && ch0_peak < 14000) {
        gain = 14000.0f / ch0_peak;
        if (gain > 24.0f) gain = 24.0f;   // 防把底噪抬爆
        for (auto& v : pcm) {
            int32_t sv = (int32_t)(v * gain + (v >= 0 ? 0.5f : -0.5f));
            if (sv > 32767) sv = 32767;
            if (sv < -32768) sv = -32768;
            v = (int16_t)sv;
        }
    }
    const unsigned rms0 = ch_samples == 0 ? 0 : (unsigned)sqrt((double)ch0_sq / ch_samples);
    if (two_ch) {
        const unsigned rms1 = ch_samples == 0 ? 0 : (unsigned)sqrt((double)ch1_sq / ch_samples);
        ESP_LOGI(TAG, "record done: %u samples (%d iters) ch0(rms=%u peak=%d gain=x%.1f) ch1(rms=%u peak=%d)",
                 (unsigned)pcm.size(), iters, rms0, ch0_peak, gain, rms1, ch1_peak);
    } else {
        ESP_LOGI(TAG, "record done: %u samples (%d iters) rms=%u peak=%d gain=x%.1f",
                 (unsigned)pcm.size(), iters, rms0, ch0_peak, gain);
    }
    return pcm;
}

// 管线:录音→STT→对话(→TTS 播报),循环体。播报中按住说话=打断播报并直接
// 开录(不新建任务,本任务循环回录音段)。在内部栈任务上(HTTPS/TLS 读 CA 证书
// 碰 flash,禁 PSRAM 栈)。
void PipelineTask(void*) {
    ESP_LOGI(TAG, "pipeline start (hold to record)");
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) { SetState(AiChatState::Error, "无音频编解码器"); ESP_LOGE(TAG, "no codec"); goto done; }
    if (!codec->input_enabled()) codec->EnableInput(true);

    while (!s_cancel.load()) {
        const bool barge = s_barge_record.exchange(false);
        std::vector<int16_t> pcm = RecordWhileHeld();
        if (s_cancel.load()) break;
        // 播报中按下但 <0.6s 就松手 = 单击:用户只想停止播报,静默回 Idle
        // (不进 STT、不报"录音太短");按住 ≥0.6s 才算"按住说话"
        if (barge && pcm.size() < (size_t)kSampleRate * 6 / 10) {
            ESP_LOGI(TAG, "barge tap: tts stopped only (%u samples)", (unsigned)pcm.size());
            SetState(AiChatState::Idle);
            break;
        }
        if (pcm.size() < kSampleRate / 4) {  // 太短(<0.25s)
            SetState(AiChatState::Error, "录音太短");
            break;
        }

        // 无网自动拉起(需求:没 WiFi 时不会自动连):拉不起再干净报错,
        // 别让 STT 直接撞"连接失败"。GLM 页同款 EnsureNetworkReady 先例。
        if (!Board::GetInstance().IsNetworkReady() &&
            !Board::GetInstance().EnsureNetworkReady(15000)) {
            SetState(AiChatState::Error, "无网络且自动连接失败,请检查 WiFi");
            break;
        }

        Cfg cfg;
        { std::lock_guard<std::mutex> l(s_cfg_mutex); cfg = s_cfg; }

        SetState(AiChatState::Transcribing);
        ESP_LOGI(TAG, "stt: %u bytes wav uploading", (unsigned)(pcm.size() * 2 + 44));
        std::string text, err;
        if (!SttTranscribe(cfg, pcm, text, err)) { ESP_LOGE(TAG, "stt fail: %s", err.c_str()); SetState(AiChatState::Error, err); break; }
        ESP_LOGI(TAG, "stt ok: %s", text.c_str());
        SetUserText(text);

        SetState(AiChatState::Thinking);
        std::string reply;
        if (!LlmChat(cfg, text, reply, err)) { ESP_LOGE(TAG, "llm fail: %s", err.c_str()); SetState(AiChatState::Error, err); break; }
        ESP_LOGI(TAG, "llm ok: %u chars", (unsigned)reply.size());
        SetReplyText(reply);
        s_history.AddTurn(text, reply, cfg.history_rounds);
        SaveHistory(text, reply);   // 先落盘再发声,TTS 慢不拖累存档
        SetState(AiChatState::Done);

        if (cfg.auto_tts && !s_cancel.load()) {
            SetState(AiChatState::Speaking);
            // 播报拉 PA(aichat 直写 codec 不进语音状态机,不拉就是"解码在走、
            // I2S 在写、功放关着"的无声;收音机 RadioPlayback 同款问题同款修法)。
            // 在合成前 Acquire:网络等待期正好覆盖功放爬坡,首字不被裁。
            PowerPolicy::GetInstance().Acquire(PowerNeed::AiChatTts);
            std::vector<int16_t> tts_pcm;
            if (TtsSynthesize(cfg, reply, tts_pcm, err)) {
                ESP_LOGI(TAG, "tts ok: %u samples", (unsigned)tts_pcm.size());
                PlayPcm(tts_pcm);   // 逐块查打断标志,播报中按住即停
            } else {
                ESP_LOGE(TAG, "tts fail: %s", err.c_str());
            }
            PowerPolicy::GetInstance().Release(PowerNeed::AiChatTts);
            s_interrupt_tts.store(false);
            SetState(AiChatState::Done);
        }

        // 播报中(或播报刚结束)用户已按住说话 → 循环回录音段直接开录,
        // 不等任务退出重建,打断跟手;未按住则正常收工。
        if (s_record_flag.load() && !s_cancel.load()) {
            SetState(AiChatState::Recording);
            continue;
        }
        break;
    }

done:
    s_task = nullptr;   // SpawnPsrWorker 包装层 return 即配对自删
}

Cfg LoadCfgFromNvs() {
    Settings st(kNvsNs, false);
    Cfg c;
    c.stt_url = st.GetString("stt_url", kDefaultSttUrl);
    c.stt_key = st.GetString("stt_key", "");
    c.stt_model = st.GetString("stt_model", kDefaultSttModel);
    c.llm_base = st.GetString("llm_base", kDefaultLlmBase);
    // 空字符串会覆盖默认值(GetString 语义):NVS 里存在过空写入(2026-10-11
    // 实测 stt_url 最新一代为空 → "STT URL 或 Key 为空")。凡空一律回退默认,
    // STT/LLM 端点是内置事实,不该被空值废掉。
    if (c.stt_url.empty()) c.stt_url = kDefaultSttUrl;
    if (c.llm_base.empty()) c.llm_base = kDefaultLlmBase;
    if (c.stt_model.empty()) c.stt_model = kDefaultSttModel;
    if (c.llm_model.empty()) c.llm_model = kDefaultLlmModel;
    // 域名迁移(2026-10-11,用户要求全部用国内站 minimax.cn):configure 曾把
    // 旧国际站域名无条件写进 NVS,只改默认值不生效 —— 读出含旧域名的存量值
    // 就地改写并回写一次(NVS 写在 main 任务/内部栈,本函数仅 AiChat_Init 调用)。
    bool migrated = false;
    if (c.stt_url.find("/v1/audio/transcriptions") != std::string::npos) {
        // 两域名该路径均 404(curl 实证);唯一可用的 STT 端点是 speech_to_text
        c.stt_url.replace(c.stt_url.find("/v1/audio/transcriptions"), 25, "/v1/speech_to_text");
        migrated = true;
    }
    if (c.stt_url.find("api.minimaxi.com") != std::string::npos) {
        c.stt_url.replace(c.stt_url.find("api.minimaxi.com"), 16, "api.minimax.cn");
        migrated = true;
    }
    if (c.llm_base.find("api.minimaxi.com") != std::string::npos) {
        c.llm_base.replace(c.llm_base.find("api.minimaxi.com"), 16, "api.minimax.cn");
        migrated = true;
    }
    if (migrated) {
        // Settings(ns,false) 是只读句柄——旧版在这里对它 SetString 静默失败,
        // 迁移从未落盘。必须开可写句柄(本函数仅在 main 任务 AiChat_Init 调用)。
        Settings w(kNvsNs, true);
        w.SetString("stt_url", c.stt_url);
        w.SetString("llm_base", c.llm_base);
        ESP_LOGI(TAG, "cfg migrated to minimax.cn: stt_url=%s llm_base=%s",
                 c.stt_url.c_str(), c.llm_base.c_str());
    }
    c.llm_key = st.GetString("llm_key", "");
    c.llm_model = st.GetString("llm_model", "");
    c.auto_tts = st.GetBool("auto_tts", false);
    c.history_rounds = st.GetInt("hist_rounds", 5);
    if (c.history_rounds < 0 || c.history_rounds > 10) c.history_rounds = 5;
    return c;
}

// 落盘(NVS)放内部栈短任务:MCP 回调/其它任务可能不在 main。先更新 RAM 缓存。
void PersistCfgTask(void* arg) {
    auto* c = static_cast<Cfg*>(arg);
    Settings st(kNvsNs, true);
    st.SetString("stt_url", c->stt_url);
    st.SetString("stt_key", c->stt_key);
    st.SetString("stt_model", c->stt_model);
    st.SetString("llm_base", c->llm_base);
    st.SetString("llm_key", c->llm_key);
    st.SetString("llm_model", c->llm_model);
    st.SetBool("auto_tts", c->auto_tts);
    st.SetInt("hist_rounds", c->history_rounds);
    delete c;
    vTaskDelete(nullptr);
}

void SetCfg(const Cfg& c) {
    {
        std::lock_guard<std::mutex> l(s_cfg_mutex);
        s_cfg = c;
    }
    auto* heap = new Cfg(c);
    if (xTaskCreate(PersistCfgTask, "aichat_nvs", 4096, heap, 4, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "persist task alloc failed");
        delete heap;
    }
}

}  // namespace

void AiChat_Init() {
    // main 任务(内部栈)读 NVS 到 RAM 缓存。codec 的 i2s 使能(codec->Start 含 NVS 读,
    // 须 main 任务)挪到进屏时由 AiVoiceChatScreen 经 Schedule 做 —— 不开此 APP 就不动
    // 音频硬件,符合"内存精贵/按需"。
    {
        std::lock_guard<std::mutex> l(s_cfg_mutex);
        s_cfg = LoadCfgFromNvs();
        s_cfg_loaded = true;
    }
    minimax_speech::InitVoiceConfig();
    ESP_LOGI(TAG, "AiChat 初始化 stt配置=%d llm配置=%d",
             (int)AiChat_SttConfigured(), (int)AiChat_LlmConfigured());
}

void AiChat_BeginRecord() {
    if (s_task != nullptr) {
        // 播报中按住说话:打断播报(TTS 逐块查标志即停),管线循环回录音段直接开录
        if (AiChat_GetState() == AiChatState::Speaking) {
            s_interrupt_tts.store(true);
            s_barge_record.store(true);   // 本轮源自播报中按下:松手太快视为单击停播
            s_record_flag.store(true);
            ESP_LOGI(TAG, "barge-in: interrupt tts, back to recording");
        }
        return;   // 其它阶段(转写/思考)一轮未结束,忽略
    }
    s_cancel.store(false);
    s_interrupt_tts.store(false);   // 清陈旧打断标志(新一轮)
    s_barge_record.store(false);
    s_record_flag.store(true);
    SetState(AiChatState::Recording);
    // 按下即起管线任务开录(内部栈;HTTPS/TLS 读 CA 证书碰 flash,禁 PSRAM 栈)。
    // 原设计在松手才建任务,而松手已清录音标志 → 循环瞬退,永远"录音太短"。
    // 栈放 PSRAM:8KB 内部栈在当前内 RAM 余量(~40KB 且碎片化)下常拿不到。
    // 管线自身不写 NVS/flash,与收音机/ICS 的 PSRAM 栈 TLS 同模式。
    if (!SpawnPsrWorker("aichat_pipe", 12288, 5, PipelineTask, nullptr, &s_task, 0)) {
        s_record_flag.store(false);
        SetState(AiChatState::Error, "任务创建失败(内存不足)");
    }
}

void AiChat_ReleaseRecord() {
    s_record_flag.store(false);   // 录音循环停下 → 管线转 STT+对话
}

void AiChat_AbortRecord() {
    s_cancel.store(true);
    s_interrupt_tts.store(true);   // 播报也立刻停(PlayPcm 逐块查)
    s_record_flag.store(false);   // 管线在下一个检查点直接结束,不跑 STT
}

AiChatState AiChat_GetState() { std::lock_guard<std::mutex> l(s_state_mutex); return s_state; }

bool AiChat_IsBusy() { return s_task != nullptr; }

void AiChat_ResetStaleState() {
    if (s_task != nullptr) return;   // 真在跑,别动
    const AiChatState st = AiChat_GetState();
    if (st == AiChatState::Recording || st == AiChatState::Transcribing ||
        st == AiChatState::Thinking || st == AiChatState::Speaking) {
        ESP_LOGW(TAG, "stale state %d without live task, reset to idle", (int)st);
        SetState(AiChatState::Idle);
    }
}
std::string AiChat_GetUserText() { std::lock_guard<std::mutex> l(s_state_mutex); return s_user_text; }
std::string AiChat_GetReplyText() { std::lock_guard<std::mutex> l(s_state_mutex); return s_reply_text; }
std::string AiChat_GetError() { std::lock_guard<std::mutex> l(s_state_mutex); return s_error; }

std::string AiChat_GetStateText() {
    switch (AiChat_GetState()) {
        case AiChatState::Idle: return "按住说话";
        case AiChatState::Recording: return "录音中… 松手发送";
        case AiChatState::Transcribing: return "语音转文字中…";
        case AiChatState::Thinking: return "思考中…";
        case AiChatState::Speaking: return "播报中…";
        case AiChatState::Done: return "完成";
        case AiChatState::Error: return "出错";
    }
    return "";
}

bool AiChat_SttConfigured() { std::lock_guard<std::mutex> l(s_cfg_mutex); return !s_cfg.stt_key.empty(); }

bool AiChat_AutoTts() { std::lock_guard<std::mutex> l(s_cfg_mutex); return s_cfg.auto_tts; }

void AiChat_SetAutoTts(bool enable) {
    Cfg c;
    { std::lock_guard<std::mutex> l(s_cfg_mutex); c = s_cfg; }
    if (c.auto_tts == enable) return;
    c.auto_tts = enable;
    SetCfg(c);   // 更新 RAM 缓存 + 内部栈短任务落盘(SafeSettings 兜底守卫)
}
bool AiChat_LlmConfigured() { std::lock_guard<std::mutex> l(s_cfg_mutex); return !s_cfg.llm_key.empty(); }

void AiChat_RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool("self.aichat.configure",
        "Configure AI voice-chat credentials. stt_key=MiniMax API key(语音转文字); llm_key=OpenAI-format key(对话大模型). Optional: stt_url, stt_model, llm_base, llm_model, auto_tts(bool,回复自动发声).",
        PropertyList({Property("stt_key", kPropertyTypeString, std::string("")),
                      Property("llm_key", kPropertyTypeString, std::string("")),
                      Property("stt_url", kPropertyTypeString, std::string("")),
                      Property("stt_model", kPropertyTypeString, std::string("")),
                      Property("llm_base", kPropertyTypeString, std::string("")),
                      Property("llm_model", kPropertyTypeString, std::string("")),
                      Property("auto_tts", kPropertyTypeString, std::string("")),
                      Property("history_rounds", kPropertyTypeString, std::string("")),
                      Property("voice_id", kPropertyTypeString, std::string("")),
                      Property("speed", kPropertyTypeString, std::string("")),
                      Property("emotion", kPropertyTypeString, std::string(""))}),
        [](const PropertyList& p) -> ReturnValue {
            Cfg c;
            { std::lock_guard<std::mutex> l(s_cfg_mutex); c = s_cfg; }
            std::string v;
            if (!(v = p["stt_key"].value<std::string>()).empty()) c.stt_key = v;
            if (!(v = p["llm_key"].value<std::string>()).empty()) c.llm_key = v;
            if (!(v = p["stt_url"].value<std::string>()).empty()) c.stt_url = v;
            if (!(v = p["stt_model"].value<std::string>()).empty()) c.stt_model = v;
            if (!(v = p["llm_base"].value<std::string>()).empty()) c.llm_base = v;
            if (!(v = p["llm_model"].value<std::string>()).empty()) c.llm_model = v;
            if (!(v = p["auto_tts"].value<std::string>()).empty()) {
                c.auto_tts = (v == "true" || v == "1" || v == "开");
            }
            if (!(v = p["history_rounds"].value<std::string>()).empty()) {
                c.history_rounds = std::max(0, std::min(10, std::atoi(v.c_str())));
            }
            SetCfg(c);

            auto vcfg = minimax_speech::GetVoiceConfig();
            bool v_changed = false;
            if (!(v = p["voice_id"].value<std::string>()).empty()) { vcfg.voice_id = v; v_changed = true; }
            if (!(v = p["speed"].value<std::string>()).empty()) { vcfg.speed = std::strtof(v.c_str(), nullptr); v_changed = true; }
            if (!(v = p["emotion"].value<std::string>()).empty()) { vcfg.emotion = v; v_changed = true; }
            if (v_changed) minimax_speech::SaveVoiceConfig(vcfg);

            return std::string("AI 对话配置已保存:stt_key ") +
                   (c.stt_key.empty() ? "空" : "已设") + ", llm_key " +
                   (c.llm_key.empty() ? "空" : "已设") +
                   "\nstt_url=" + c.stt_url +
                   "\nstt_model=" + c.stt_model +
                   "\nllm_base=" + c.llm_base +
                   "\nllm_model=" + c.llm_model +
                   "\n自动发声=" + (c.auto_tts ? "开" : "关") +
                   "\n音色=" + vcfg.voice_id +
                   ", 多轮历史=" + std::to_string(c.history_rounds) + "轮(PSRAM)";
        });

    mcp.AddTool("self.aichat.clear_history",
        "Clear in-memory/PSRAM multi-turn conversation history.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            AiChat_ClearHistory();
            return std::string("AI 对话多轮历史已清空 (PSRAM)");
        });

    mcp.AddTool("self.aichat.status",
        "Get AI voice-chat config/state: whether STT/LLM keys are set, current state.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            auto vcfg = minimax_speech::GetVoiceConfig();
            return std::string("stt_key ") + (AiChat_SttConfigured() ? "已设" : "未设") +
                   " | llm_key " + (AiChat_LlmConfigured() ? "已设" : "未设") +
                   " | 自动发声 " + (AiChat_AutoTts() ? "开" : "关") +
                   " | 音色 " + vcfg.voice_id +
                   " | 历史 " + std::to_string(AiChat_GetHistoryCount()) + "/" +
                   std::to_string(AiChat_GetHistoryRounds()) + " 轮(PSRAM)" +
                   " | 状态 " + AiChat_GetStateText();
        });

    ESP_LOGI(TAG, "aichat MCP tools registered");
}

size_t AiChat_GetHistoryCount() {
    return s_history.GetCount();
}

int AiChat_GetHistoryRounds() {
    std::lock_guard<std::mutex> l(s_cfg_mutex);
    return s_cfg.history_rounds;
}

void AiChat_ClearHistory() {
    s_history.Clear();
    ESP_LOGI(TAG, "history: cleared (PSRAM)");
}

void AiChat_BootRegister() {
    AiChat_Init();
    AiChat_RegisterMcpTools();
}
