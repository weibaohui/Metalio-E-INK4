// main/services/ai_chat/minimax_speech.cc
#include "minimax_speech.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cJSON.h>

#include "board.h"
#include "settings.h"
#include "tts_decoder.h"
#include "protocols/api_http.h"

namespace minimax_speech {

namespace {

constexpr const char* TAG = "MiniMaxSpeech";
constexpr const char* kNvsNs = "aichat";
constexpr int kSampleRate = 16000;

std::mutex s_cfg_mutex;
VoiceConfig s_voice_cfg;
bool s_cfg_initialized = false;

// 12 种精选热门中文音色
const std::vector<VoiceInfo> s_supported_voices = {
    {"male-qn-qingse", "青涩青年", "男声 · 清新亲切"},
    {"male-qn-jingying", "精英青年", "男声 · 干练沉稳"},
    {"male-qn-badao", "霸道青年", "男声 · 磁性深沉"},
    {"male-qn-daxuesheng", "活力男声", "男声 · 阳光朝气"},
    {"Chinese (Mandarin)_Gentleman", "沉稳绅士", "男声 · 儒雅大方"},
    {"Chinese (Mandarin)_Radio_Host", "电台主播", "男声 · 叙事播音"},
    {"female-shaonv", "少女音色", "女声 · 清脆活泼"},
    {"female-yujie", "御姐音色", "女声 · 成熟自信"},
    {"female-tianmei", "甜美女声", "女声 · 温柔治愈"},
    {"female-chengshu", "成熟女性", "女声 · 温婉端庄"},
    {"Chinese (Mandarin)_Warm_Bestie", "温柔闺蜜", "女声 · 轻松陪伴"},
    {"Chinese (Mandarin)_Wise_Women", "知性学者", "女声 · 博学睿智"},
};

// JSON 转义辅助
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

// WAV 打包辅助 (16k 单声道 int16 → RIFF/WAVE)
void PutLE32(std::string& s, uint32_t v) { for (int i = 0; i < 4; ++i) s += (char)((v >> (8 * i)) & 0xff); }
void PutLE16(std::string& s, uint16_t v) { for (int i = 0; i < 2; ++i) s += (char)((v >> (8 * i)) & 0xff); }

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

int HttpPostRaw(const std::string& url, const std::string& bearer,
                const std::string& content_type, const std::string& body, std::string& resp,
                const std::atomic<bool>* interrupt_flag, const std::atomic<bool>* cancel_flag) {
    if (url.empty()) return -1;
    if (!api::CheckUrlTlsPreflight(url, 14 * 1024)) {
        ESP_LOGE(TAG, "http: internal RAM too low for TLS handshake");
        return -3;
    }
    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    if (!http) { ESP_LOGE(TAG, "http: create failed"); return -1; }
    http->SetTimeout(20000);
    if (!bearer.empty()) http->SetHeader("Authorization", "Bearer " + bearer);
    http->SetHeader("Content-Type", content_type);
    http->SetContent(std::string(body));
    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "http: open failed err=0x%x", http->GetLastError());
        http->Close();
        return -2;
    }
    const int status = http->GetStatusCode();
    char chunk[2048];
    while (true) {
        if ((interrupt_flag && interrupt_flag->load()) || (cancel_flag && cancel_flag->load())) {
            ESP_LOGI(TAG, "http: interrupted by user");
            break;
        }
        int r = http->Read(chunk, sizeof(chunk));
        if (r <= 0) break;
        resp.append(chunk, r);
    }
    http->Close();
    return status;
}

}  // namespace

const std::vector<VoiceInfo>& GetSupportedVoices() {
    return s_supported_voices;
}

VoiceConfig GetVoiceConfig() {
    std::lock_guard<std::mutex> lock(s_cfg_mutex);
    if (!s_cfg_initialized) {
        // 自动懒初始化
        Settings st(kNvsNs, false);
        s_voice_cfg.voice_id = st.GetString("tts_voice", "male-qn-qingse");
        int speed_x10 = st.GetInt("tts_spd_x10", 10);
        if (speed_x10 < 5 || speed_x10 > 20) speed_x10 = 10;
        s_voice_cfg.speed = (float)speed_x10 / 10.0f;

        int vol_x10 = st.GetInt("tts_vol_x10", 10);
        if (vol_x10 < 1 || vol_x10 > 20) vol_x10 = 10;
        s_voice_cfg.vol = (float)vol_x10 / 10.0f;

        s_voice_cfg.pitch = st.GetInt("tts_pitch", 0);
        if (s_voice_cfg.pitch < -12 || s_voice_cfg.pitch > 12) s_voice_cfg.pitch = 0;

        s_voice_cfg.emotion = st.GetString("tts_emotion", "neutral");
        s_voice_cfg.model = st.GetString("tts_model", "speech-2.8-turbo");
        if (s_voice_cfg.model.empty() || s_voice_cfg.model == "speech-02-turbo") {
            s_voice_cfg.model = "speech-2.8-turbo";
        }
        s_cfg_initialized = true;
    }
    return s_voice_cfg;
}

void SetVoiceConfig(const VoiceConfig& cfg) {
    std::lock_guard<std::mutex> lock(s_cfg_mutex);
    s_voice_cfg = cfg;
    s_cfg_initialized = true;
}

void SaveVoiceConfig(const VoiceConfig& cfg) {
    SetVoiceConfig(cfg);
    struct SaveTaskArg { VoiceConfig c; };
    auto* arg = new SaveTaskArg{cfg};
    auto task = [](void* p) {
        auto* a = static_cast<SaveTaskArg*>(p);
        Settings st(kNvsNs, true);
        st.SetString("tts_voice", a->c.voice_id);
        st.SetInt("tts_spd_x10", (int)std::round(a->c.speed * 10.0f));
        st.SetInt("tts_vol_x10", (int)std::round(a->c.vol * 10.0f));
        st.SetInt("tts_pitch", a->c.pitch);
        st.SetString("tts_emotion", a->c.emotion);
        st.SetString("tts_model", a->c.model);
        delete a;
        vTaskDelete(nullptr);
    };
    if (xTaskCreate(task, "mm_nvs", 4096, arg, 4, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "mm_nvs task create failed");
        delete arg;
    }
}

void InitVoiceConfig() {
    GetVoiceConfig();
}

bool Synthesize(const std::string& key,
                const std::string& text,
                const VoiceConfig& voice_cfg,
                std::vector<int16_t>& pcm,
                std::string& err,
                const std::atomic<bool>* interrupt_flag,
                const std::atomic<bool>* cancel_flag) {
    if (key.empty()) {
        err = "MiniMax API Key 为空";
        return false;
    }
    std::string clip = text;
    if (clip.size() > 600) clip = clip.substr(0, 600); // 300 汉字上限控制耗时

    char num_buf[32];
    snprintf(num_buf, sizeof(num_buf), "%.1f", voice_cfg.speed);
    std::string speed_str = num_buf;
    snprintf(num_buf, sizeof(num_buf), "%.1f", voice_cfg.vol);
    std::string vol_str = num_buf;

    std::string body = std::string("{\"model\":\"") +
                       (voice_cfg.model.empty() ? "speech-02-turbo" : voice_cfg.model) +
                       "\",\"text\":\"" + EscapeJson(clip) +
                       "\",\"stream\":false,"
                       "\"voice_setting\":{"
                       "\"voice_id\":\"" + EscapeJson(voice_cfg.voice_id.empty() ? "male-qn-qingse" : voice_cfg.voice_id) + "\","
                       "\"speed\":" + speed_str + ","
                       "\"vol\":" + vol_str + ","
                       "\"pitch\":" + std::to_string(voice_cfg.pitch) + ","
                       "\"emotion\":\"" + EscapeJson(voice_cfg.emotion.empty() ? "neutral" : voice_cfg.emotion) + "\"}"
                       ",\"audio_setting\":{\"sample_rate\":16000,\"format\":\"pcm\",\"channel\":1}}";

    std::string resp;
    const int status = HttpPostRaw("https://api.minimax.cn/v1/t2a_v2", key,
                                   "application/json", body, resp,
                                   interrupt_flag, cancel_flag);
    if ((interrupt_flag && interrupt_flag->load()) || (cancel_flag && cancel_flag->load())) {
        err = "播报已打断";
        return false;
    }
    if (status < 200 || status >= 300) {
        err = "TTS HTTP " + std::to_string(status);
        return false;
    }

    bool ok = false;
    try {
        ok = tts_decoder::DecodeTtsResponse(resp, pcm, err);
    } catch (const std::bad_alloc&) {
        // 长回复的 MB 级载荷可能打爆 PSRAM;未捕获异常会直接 abort 整机
        err = "TTS 内存不足(回复过长)";
        ok = false;
    } catch (const std::exception& e) {
        err = std::string("TTS 解码异常: ") + e.what();
        ok = false;
    }
    std::string().swap(resp);   // 解码器已自持副本,立即释放调用方这份(MB 级)
    return ok;
}

bool TranscribeStream(const std::string& url,
                      const std::string& key,
                      const std::string& model,
                      const std::vector<int16_t>& pcm,
                      std::function<void(const std::string& delta)> on_delta,
                      std::string& text,
                      std::string& err,
                      const std::atomic<bool>* cancel_flag) {
    if (url.empty() || key.empty()) {
        err = "STT URL 或 Key 为空";
        return false;
    }
    const uint32_t data_bytes = (uint32_t)(pcm.size() * sizeof(int16_t));
    const std::string boundary = "----aichat_boundary";
    std::string body;
    body.reserve(data_bytes + 512);
    body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\nContent-Type: audio/wav\r\n\r\n";
    // 零拷贝优化：直接在 body 中组装 WAV 头与数据，避免生成完整的 wav 中间副本
    body += "RIFF"; PutLE32(body, 36 + data_bytes); body += "WAVE";
    body += "fmt "; PutLE32(body, 16); PutLE16(body, 1); PutLE16(body, 1);  // PCM, mono
    PutLE32(body, kSampleRate); PutLE32(body, kSampleRate * 2); PutLE16(body, 2); PutLE16(body, 16);
    body += "data"; PutLE32(body, data_bytes);
    body.append(reinterpret_cast<const char*>(pcm.data()), data_bytes);
    body += "\r\n--" + boundary + "\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n" +
            (model.empty() ? std::string("asr-1.0") : model) + "\r\n";
    body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"response_format\"\r\n\r\njson\r\n";
    body += "--" + boundary + "\r\nContent-Disposition: form-data; name=\"stream\"\r\n\r\ntrue\r\n";
    body += "--" + boundary + "--\r\n";

    if (!api::CheckUrlTlsPreflight(url, 14 * 1024)) {
        err = "内部内存不足，无法建立安全连接";
        return false;
    }

    auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
    if (!http) { err = "创建 HTTP 客户端失败"; return false; }
    http->SetTimeout(25000);
    http->SetHeader("Authorization", "Bearer " + key);
    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    http->SetContent(std::move(body));
    if (!http->Open("POST", url)) {
        err = "STT 连接失败 code=" + std::to_string(http->GetLastError());
        http->Close();
        return false;
    }
    const int status = http->GetStatusCode();
    if (status < 200 || status >= 300) {
        std::string err_body = http->ReadAll();
        http->Close();
        err = "转写 HTTP " + std::to_string(status) + ": " + (err_body.size() > 120 ? err_body.substr(0, 120) : err_body);
        return false;
    }

    text.clear();
    std::string line_buf;
    char chunk[1024];
    while (true) {
        if (cancel_flag && cancel_flag->load()) {
            err = "转写已取消";
            http->Close();
            return false;
        }
        int r = http->Read(chunk, sizeof(chunk) - 1);
        if (r <= 0) break;
        chunk[r] = '\0';
        line_buf.append(chunk, r);

        size_t pos = 0;
        while ((pos = line_buf.find('\n')) != std::string::npos) {
            std::string line = line_buf.substr(0, pos);
            line_buf.erase(0, pos + 1);

            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            while (!line.empty() && line.front() == ' ') line.erase(line.begin());

            if (line.rfind("data:", 0) == 0) {
                std::string payload = line.substr(5);
                while (!payload.empty() && payload.front() == ' ') payload.erase(payload.begin());
                if (payload.empty()) continue;

                cJSON* root = cJSON_Parse(payload.c_str());
                if (root) {
                    const cJSON* d = cJSON_GetObjectItem(root, "delta");
                    if (cJSON_IsString(d) && d->valuestring && strlen(d->valuestring) > 0) {
                        std::string delta = d->valuestring;
                        text += delta;
                        if (on_delta) on_delta(delta);
                    }
                    cJSON_Delete(root);
                }
            }
        }
    }
    http->Close();

    if (text.empty() && !line_buf.empty()) {
        cJSON* root = cJSON_Parse(line_buf.c_str());
        if (root) {
            const cJSON* t = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(t) && t->valuestring) {
                text = t->valuestring;
                if (on_delta) on_delta(text);
            }
            cJSON_Delete(root);
        }
    }

    if (text.empty()) {
        err = "转写结果为空";
        return false;
    }
    return true;
}

bool Transcribe(const std::string& url,
                const std::string& key,
                const std::string& model,
                const std::vector<int16_t>& pcm,
                std::string& text,
                std::string& err) {
    return TranscribeStream(url, key, model, pcm, nullptr, text, err, nullptr);
}

}  // namespace minimax_speech
