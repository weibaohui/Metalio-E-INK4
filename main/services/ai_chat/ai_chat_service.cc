// main/services/ai_chat/ai_chat_service.cc —— 见 ai_chat_service.h(新增 APP:AI 语音对话)。
#include "ai_chat_service.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cJSON.h>

#include "application.h"
#include "utils/psr_worker.h"
#include "audio_codec.h"
#include "board.h"
#include "mcp_server.h"
#include "settings.h"

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
};
std::mutex s_cfg_mutex;
Cfg s_cfg;
bool s_cfg_loaded = false;

const char* kDefaultSttUrl = "https://api.minimaxi.com/v1/audio/transcriptions";
const char* kDefaultLlmBase = "https://api.minimaxi.com/v1";

// ---- 运行状态 ----
std::mutex s_state_mutex;
AiChatState s_state = AiChatState::Idle;
std::string s_user_text;
std::string s_reply_text;
std::string s_error;

std::atomic<bool> s_record_flag{false};   // 按住=true
std::atomic<bool> s_cancel{false};
TaskHandle_t s_task = nullptr;

void SetState(AiChatState st, const std::string& err = "") {
    std::lock_guard<std::mutex> lock(s_state_mutex);
    s_state = st;
    if (!err.empty()) s_error = err;
    else if (st != AiChatState::Error) s_error.clear();
}

void SetUserText(const std::string& t) { std::lock_guard<std::mutex> l(s_state_mutex); s_user_text = t; }
void SetReplyText(const std::string& t) { std::lock_guard<std::mutex> l(s_state_mutex); s_reply_text = t; }

// ---- WAV 打包(16k 单声道 int16 → RIFF/WAVE) ----
void PutLE32(std::string& s, uint32_t v) { for (int i=0;i<4;i++) s += (char)((v>>(8*i))&0xff); }
void PutLE16(std::string& s, uint16_t v) { for (int i=0;i<2;i++) s += (char)((v>>(8*i))&0xff); }
std::string BuildWav(const std::vector<int16_t>& pcm) {
    const uint32_t data_bytes = (uint32_t)(pcm.size() * sizeof(int16_t));
    std::string out;
    out.reserve(44 + data_bytes);
    out += "RIFF"; PutLE32(out, 36 + data_bytes); out += "WAVE";
    out += "fmt "; PutLE32(out, 16); PutLE16(out, 1); PutLE16(out, 1);  // PCM, mono
    PutLE32(out, kSampleRate); PutLE32(out, kSampleRate * 2); PutLE16(out, 2); PutLE16(out, 16);
    out += "data"; PutLE32(out, data_bytes);
    out.append(reinterpret_cast<const char*>(pcm.data()), data_bytes);
    return out;
}

// ---- HTTP 助手(整段 SetContent,内部栈任务调用) ----
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
    resp = http->ReadAll();
    http->Close();
    ESP_LOGI(TAG, "http: done %u bytes", (unsigned)resp.size());
    return status;
}

// STT:multipart 上传 WAV,取 OpenAI 转写格式 {"text":...}。成功返回 true。
bool SttTranscribe(const Cfg& cfg, const std::vector<int16_t>& pcm, std::string& text, std::string& err) {
    const std::string wav = BuildWav(pcm);
    const std::string boundary = "----aichat_boundary";
    std::string body;
    body.reserve(wav.size() + 512);
    body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
    body.append(wav);
    body += "\r\n--" + boundary + "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n" +
            (cfg.stt_model.empty() ? std::string("whisper-1") : cfg.stt_model) + "\r\n";
    body += "--" + boundary + "--\r\n";
    std::string resp;
    const int status = HttpPost(cfg.stt_url, cfg.stt_key,
                                "multipart/form-data; boundary=" + boundary, body, resp);
    if (status < 200 || status >= 300) {
        err = "转写 HTTP " + std::to_string(status);
        return false;
    }
    cJSON* root = cJSON_Parse(resp.c_str());
    if (root == nullptr) { err = "转写响应非 JSON"; return false; }
    const cJSON* t = cJSON_GetObjectItem(root, "text");
    if (cJSON_IsString(t)) text = t->valuestring;
    // MiniMax 等可能包一层 {"base_resp":{...},"data":"..."} 或 {"text":...};再兜一层
    if (text.empty()) {
        const cJSON* data = cJSON_GetObjectItem(root, "data");
        if (cJSON_IsString(data)) text = data->valuestring;
    }
    cJSON_Delete(root);
    if (text.empty()) { err = "转写结果为空"; return false; }
    return true;
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

bool LlmChat(const Cfg& cfg, const std::string& user_text, std::string& reply, std::string& err) {
    const std::string url = cfg.llm_base + "/chat/completions";
    const std::string model = cfg.llm_model.empty() ? "gpt-4o-mini" : cfg.llm_model;
    const std::string body = std::string("{\"model\":\"") + model +
        "\",\"messages\":[{\"role\":\"user\",\"content\":\"" + EscapeJson(user_text) + "\"}]}";
    std::string resp;
    const int status = HttpPost(url, cfg.llm_key, "application/json", body, resp);
    if (status < 200 || status >= 300) {
        err = "对话 HTTP " + std::to_string(status);
        return false;
    }
    cJSON* root = cJSON_Parse(resp.c_str());
    if (root == nullptr) { err = "对话响应非 JSON"; return false; }
    const cJSON* choices = cJSON_GetObjectItem(root, "choices");
    const cJSON* first = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : nullptr;
    const cJSON* msg = first ? cJSON_GetObjectItem(first, "message") : nullptr;
    const cJSON* content = msg ? cJSON_GetObjectItem(msg, "content") : nullptr;
    if (cJSON_IsString(content)) reply = content->valuestring;
    cJSON_Delete(root);
    // MiniMax-M2 等推理模型带 <think>...</think> 思考块,剥掉再上屏
    if (reply.rfind("<think>", 0) == 0) {
        const size_t end = reply.find("</think>");
        reply = end == std::string::npos ? "" : reply.substr(end + 8);
    }
    // 首尾空白
    while (!reply.empty() && (reply.front() == '\n' || reply.front() == ' ')) reply.erase(reply.begin());
    while (!reply.empty() && (reply.back() == '\n' || reply.back() == ' ')) reply.pop_back();
    if (reply.empty()) { err = "对话结果为空"; return false; }
    return true;
}

// TTS:MiniMax t2a_v2,返回 16k 单声道 PCM 的 hex 编码 → 解码为样本。
// 常见小坑:audio 字段是 hex 不是 base64;长回复会到 MB 级 JSON,靠
// SPIRAM_USE_MALLOC 落 PSRAM。文本截断 300 字符控制耗时与内存。
bool TtsSynthesize(const Cfg& cfg, const std::string& text, std::vector<int16_t>& pcm, std::string& err) {
    std::string clip = text;
    if (clip.size() > 600) clip = clip.substr(0, 600);   // 300 汉字上限
    const std::string body = std::string(
        "{\"model\":\"speech-02-turbo\",\"text\":\"") + EscapeJson(clip) +
        "\",\"stream\":false,"
        "\"voice_setting\":{\"voice_id\":\"male-qn-qingse\",\"speed\":1.0,\"vol\":1.0},"
        "\"audio_setting\":{\"sample_rate\":16000,\"format\":\"pcm\",\"channel\":1}}";
    std::string resp;
    const int status = HttpPost("https://api.minimax.cn/v1/t2a_v2", cfg.llm_key,
                                "application/json", body, resp);
    if (status < 200 || status >= 300) { err = "TTS HTTP " + std::to_string(status); return false; }
    cJSON* root = cJSON_Parse(resp.c_str());
    if (root == nullptr) { err = "TTS 响应非 JSON"; return false; }
    const cJSON* data = cJSON_GetObjectItem(root, "data");
    const cJSON* audio = data ? cJSON_GetObjectItem(data, "audio") : nullptr;
    std::string hex;
    if (cJSON_IsString(audio) && audio->valuestring != nullptr) hex = audio->valuestring;
    cJSON_Delete(root);
    if (hex.size() < 2) { err = "TTS 音频为空"; return false; }
    auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    if (hex.size() % 2 != 0) hex.pop_back();
    pcm.reserve(hex.size() / 2 / sizeof(int16_t));
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        const int hi = hexval(hex[i]), lo = hexval(hex[i + 1]);
        if (hi < 0 || lo < 0) continue;
        const uint16_t v = (uint16_t)((hi << 8) | lo);
        int16_t sample;
        memcpy(&sample, &v, 2);
        pcm.push_back(sample);
    }
    if (pcm.empty()) { err = "TTS 解码为空"; return false; }
    return true;
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
    const int64_t deadline = esp_timer_get_time() + (int64_t)kMaxRecordSec * 1000000LL;
    while (s_record_flag.load() && !s_cancel.load() && esp_timer_get_time() < deadline) {
        std::vector<int16_t> raw(chunk);
        if (!codec->InputData(raw) || raw.empty()) {
            ESP_LOGW(TAG, "record: InputData failed (iter %d)", iters);
            break;
        }
        ++iters;
        if (two_ch) {
            for (size_t i = 0; i + 1 < raw.size(); i += 2) pcm.push_back(raw[i]);
        } else {
            pcm.insert(pcm.end(), raw.begin(), raw.end());
        }
    }
    // 能量统计 + 峰值归一化。本板 I2S 32→16 位 >>12 后语音峰值仅数百(实测 512),
    // 直接上传会被 ASR 判为无语音;基线 AudioCodec::SetInputGain 是从未生效的死钩子。
    int64_t sum_sq = 0;
    int16_t peak = 0;
    for (int16_t v : pcm) {
        sum_sq += (int64_t)v * v;
        int16_t a = v < 0 ? (int16_t)-v : v;
        if (a > peak) peak = a;
    }
    float gain = 1.0f;
    if (!pcm.empty() && peak > 0 && peak < 14000) {
        gain = 14000.0f / peak;
        if (gain > 24.0f) gain = 24.0f;   // 防把底噪抬爆
        for (auto& v : pcm) {
            int32_t sv = (int32_t)(v * gain + (v >= 0 ? 0.5f : -0.5f));
            if (sv > 32767) sv = 32767;
            if (sv < -32768) sv = -32768;
            v = (int16_t)sv;
        }
    }
    const unsigned rms = pcm.empty() ? 0 : (unsigned)sqrt((double)sum_sq / pcm.size());
    ESP_LOGI(TAG, "record done: %u samples (%d iters) rms=%u peak=%d gain=x%.1f",
             (unsigned)pcm.size(), iters, rms, peak, gain);
    return pcm;
}

// 管线:录音→STT→对话。在内部栈任务上(HTTPS/TLS 读 CA 证书碰 flash,禁 PSRAM 栈)。
void PipelineTask(void*) {
    ESP_LOGI(TAG, "pipeline start (hold to record)");
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) { SetState(AiChatState::Error, "无音频编解码器"); ESP_LOGE(TAG, "no codec"); goto done; }
    if (!codec->input_enabled()) codec->EnableInput(true);

    {
        std::vector<int16_t> pcm = RecordWhileHeld();
        if (s_cancel.load()) goto done;
        if (pcm.size() < kSampleRate / 4) {  // 太短(<0.25s)
            SetState(AiChatState::Error, "录音太短");
            goto done;
        }

        Cfg cfg;
        { std::lock_guard<std::mutex> l(s_cfg_mutex); cfg = s_cfg; }

        SetState(AiChatState::Transcribing);
        ESP_LOGI(TAG, "stt: %u bytes wav uploading", (unsigned)(pcm.size() * 2 + 44));
        std::string text, err;
        if (!SttTranscribe(cfg, pcm, text, err)) { ESP_LOGE(TAG, "stt fail: %s", err.c_str()); SetState(AiChatState::Error, err); goto done; }
        ESP_LOGI(TAG, "stt ok: %s", text.c_str());
        SetUserText(text);

        SetState(AiChatState::Thinking);
        std::string reply;
        if (!LlmChat(cfg, text, reply, err)) { ESP_LOGE(TAG, "llm fail: %s", err.c_str()); SetState(AiChatState::Error, err); goto done; }
        ESP_LOGI(TAG, "llm ok: %u chars", (unsigned)reply.size());
        SetReplyText(reply);
        SetState(AiChatState::Done);
        if (cfg.auto_tts) {
            SetState(AiChatState::Speaking);
            std::vector<int16_t> tts_pcm;
            if (TtsSynthesize(cfg, reply, tts_pcm, err)) {
                ESP_LOGI(TAG, "tts ok: %u samples", (unsigned)tts_pcm.size());
                PlayPcm(tts_pcm);
            } else {
                ESP_LOGE(TAG, "tts fail: %s", err.c_str());
            }
            SetState(AiChatState::Done);
        }
    }

done:
    s_task = nullptr;   // SpawnPsrWorker 包装层 return 即配对自删
}

Cfg LoadCfgFromNvs() {
    Settings st(kNvsNs, false);
    Cfg c;
    c.stt_url = st.GetString("stt_url", kDefaultSttUrl);
    c.stt_key = st.GetString("stt_key", "");
    c.stt_model = st.GetString("stt_model", "");
    c.llm_base = st.GetString("llm_base", kDefaultLlmBase);
    c.llm_key = st.GetString("llm_key", "");
    c.llm_model = st.GetString("llm_model", "");
    c.auto_tts = st.GetBool("auto_tts", false);
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
    ESP_LOGI(TAG, "AiChat 初始化 stt配置=%d llm配置=%d",
             (int)AiChat_SttConfigured(), (int)AiChat_LlmConfigured());
}

void AiChat_BeginRecord() {
    if (s_task != nullptr) return;   // 一轮未结束
    s_cancel.store(false);
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
    s_record_flag.store(false);   // 管线在下一个检查点直接结束,不跑 STT
}

AiChatState AiChat_GetState() { std::lock_guard<std::mutex> l(s_state_mutex); return s_state; }
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
                      Property("auto_tts", kPropertyTypeString, std::string(""))}),
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
            SetCfg(c);
            return std::string("AI 对话配置已保存:stt_key ") +
                   (c.stt_key.empty() ? "空" : "已设") + ", llm_key " +
                   (c.llm_key.empty() ? "空" : "已设") + ", llm_base=" + c.llm_base +
                   ", 自动发声=" + (c.auto_tts ? "开" : "关");
        });

    mcp.AddTool("self.aichat.status",
        "Get AI voice-chat config/state: whether STT/LLM keys are set, current state.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            return std::string("stt_key ") + (AiChat_SttConfigured() ? "已设" : "未设") +
                   " | llm_key " + (AiChat_LlmConfigured() ? "已设" : "未设") +
                   " | 自动发声 " + (AiChat_AutoTts() ? "开" : "关") +
                   " | 状态 " + AiChat_GetStateText();
        });

    ESP_LOGI(TAG, "aichat MCP tools registered");
}

void AiChat_BootRegister() {
    AiChat_Init();
    AiChat_RegisterMcpTools();
}
