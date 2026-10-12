// main/services/ai_chat/minimax_bidi_tts.cc
#include "minimax_bidi_tts.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include <esp_log.h>
#include <esp_timer.h>
#include <cJSON.h>

#include "board.h"

namespace minimax_speech {

namespace {

constexpr const char* TAG = "MiniMaxBidiTts";

// 高效 HEX 字符转数值
inline int HexCharToInt(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// 快速解码小端 16-bit PCM (4 字符 hex -> 1 样本 int16)
void FastHexToPcm(const char* hex, size_t hex_len, std::vector<int16_t>& pcm_out) {
    if (hex == nullptr || hex_len < 4) return;
    const size_t num_samples = hex_len / 4;
    pcm_out.reserve(pcm_out.size() + num_samples);
    for (size_t i = 0; i + 3 < hex_len; i += 4) {
        int b0_hi = HexCharToInt(hex[i]);
        int b0_lo = HexCharToInt(hex[i + 1]);
        int b1_hi = HexCharToInt(hex[i + 2]);
        int b1_lo = HexCharToInt(hex[i + 3]);
        if (b0_hi < 0 || b0_lo < 0 || b1_hi < 0 || b1_lo < 0) continue;
        uint8_t lo = static_cast<uint8_t>((b0_hi << 4) | b0_lo);
        uint8_t hi = static_cast<uint8_t>((b1_hi << 4) | b1_lo);
        int16_t sample = static_cast<int16_t>(lo | (hi << 8));
        pcm_out.push_back(sample);
    }
}

std::string JsonEscape(const std::string& in) {
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

}  // namespace

MiniMaxBidiTts::MiniMaxBidiTts() {
    event_group_ = xEventGroupCreate();
}

MiniMaxBidiTts::~MiniMaxBidiTts() {
    Close();
    if (event_group_ != nullptr) {
        vEventGroupDelete(event_group_);
        event_group_ = nullptr;
    }
}

bool MiniMaxBidiTts::StartSession(const std::string& key,
                                 const VoiceConfig& voice_cfg,
                                 std::string& err,
                                 const std::string& ws_url) {
    if (key.empty()) {
        err = "MiniMax API Key 为空";
        return false;
    }

    Close();
    voice_cfg_ = voice_cfg;
    is_finished_.store(false);
    is_canceled_.store(false);
    session_active_.store(false);

    {
        std::lock_guard<std::mutex> lk(pcm_mutex_);
        pcm_queue_.clear();
    }
    xEventGroupClearBits(event_group_, 0x00FFFFFF);

    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        err = "无网络接口";
        return false;
    }

    // connect_id=2 避免与小智云端私有协议 (1) 或 HTTP (3) 冲突
    ws_ = network->CreateWebSocket(2);
    if (!ws_) {
        err = "创建 WebSocket 失败";
        return false;
    }

    // 设置接收缓冲为 64KB, 保证音频分包(通常每块 4KB~16KB hex)不触发溢出
    ws_->SetReceiveBufferSize(65536);
    ws_->SetHeader("Authorization", ("Bearer " + key).c_str());

    // 绑定数据回调
    ws_->OnData([this](const char* data, size_t len, bool binary) {
        this->OnWebSocketData(data, len, binary);
    });

    ws_->OnDisconnected([this]() {
        ESP_LOGI(TAG, "WebSocket disconnected callback");
        is_connected_.store(false);
        session_active_.store(false);
        is_finished_.store(true);
        if (event_group_) {
            xEventGroupSetBits(event_group_, kBitTaskFinished);
        }
    });

    ws_->OnError([this](int err_code) {
        ESP_LOGW(TAG, "WebSocket error code: %d", err_code);
        {
            std::lock_guard<std::mutex> lk(error_mutex_);
            last_error_ = "WS 错误码 " + std::to_string(err_code);
        }
        if (event_group_) {
            xEventGroupSetBits(event_group_, kBitTaskFailed);
        }
    });

    ESP_LOGI(TAG, "Connecting to %s ...", ws_url.c_str());
    const int64_t t0 = esp_timer_get_time();
    if (!ws_->Connect(ws_url.c_str())) {
        const int code = ws_->GetLastError();
        err = "连接 WSS 失败(code=" + std::to_string(code) + ")";
        ESP_LOGE(TAG, "%s in %lld ms", err.c_str(), (esp_timer_get_time() - t0) / 1000);
        ws_.reset();
        return false;
    }

    is_connected_.store(true);
    ESP_LOGI(TAG, "WSS 连接成功并完成握手, 耗时 %lld ms, 等待服务端 task_started ...",
             (esp_timer_get_time() - t0) / 1000);

    // 等待 task_started 事件 (包含服务端 connected_success 响应与 task_start 握手)
    EventBits_t bits = xEventGroupWaitBits(event_group_,
                                          kBitTaskStarted | kBitTaskFailed,
                                          pdFALSE, pdFALSE,
                                          pdMS_TO_TICKS(6000));
    if (bits & kBitTaskFailed) {
        std::lock_guard<std::mutex> lk(error_mutex_);
        err = last_error_.empty() ? "TTS 任务启动失败" : last_error_;
        Close();
        return false;
    }
    if (!(bits & kBitTaskStarted)) {
        err = "等待 task_started 超时";
        ESP_LOGW(TAG, "%s", err.c_str());
        Close();
        return false;
    }

    session_active_.store(true);
    ESP_LOGI(TAG, "双向流式 TTS 会话就绪, 可实时发送文本");
    return true;
}

void MiniMaxBidiTts::OnWebSocketData(const char* data, size_t len, bool binary) {
    if (binary || data == nullptr || len == 0) return;

    cJSON* root = cJSON_ParseWithLength(data, len);
    if (root == nullptr) {
        ESP_LOGD(TAG, "非 JSON 文本帧: %.64s", data);
        return;
    }

    const cJSON* event_item = cJSON_GetObjectItem(root, "event");
    const char* event_str = cJSON_IsString(event_item) ? event_item->valuestring : "";

    if (strcmp(event_str, "connected_success") == 0) {
        // 服务端确认建连, 立即发送 task_start
        char spd_buf[16], vol_buf[16];
        snprintf(spd_buf, sizeof(spd_buf), "%.1f", voice_cfg_.speed);
        snprintf(vol_buf, sizeof(vol_buf), "%.1f", voice_cfg_.vol);

        const std::string model_name = voice_cfg_.model.empty() ? "speech-02-turbo" : voice_cfg_.model;
        const std::string voice_id = voice_cfg_.voice_id.empty() ? "male-qn-qingse" : voice_cfg_.voice_id;
        const std::string emotion = voice_cfg_.emotion.empty() ? "neutral" : voice_cfg_.emotion;

        std::string start_req = "{\"event\":\"task_start\",\"model\":\"" + model_name + "\","
                                "\"voice_setting\":{"
                                "\"voice_id\":\"" + JsonEscape(voice_id) + "\","
                                "\"speed\":" + spd_buf + ","
                                "\"vol\":" + vol_buf + ","
                                "\"pitch\":" + std::to_string(voice_cfg_.pitch) + ","
                                "\"emotion\":\"" + JsonEscape(emotion) + "\"},"
                                "\"audio_setting\":{\"sample_rate\":16000,\"format\":\"pcm\",\"channel\":1}}";

        ESP_LOGI(TAG, "发送 task_start: voice=%s model=%s", voice_id.c_str(), model_name.c_str());
        if (ws_) ws_->Send(start_req);
        xEventGroupSetBits(event_group_, kBitConnected);
    } else if (strcmp(event_str, "task_started") == 0) {
        ESP_LOGI(TAG, "收到 task_started: 服务端开始接收文本");
        xEventGroupSetBits(event_group_, kBitTaskStarted);
    } else if (strcmp(event_str, "task_continued") == 0) {
        // 收到分句合成的音频片段
        const cJSON* data_obj = cJSON_GetObjectItem(root, "data");
        if (cJSON_IsObject(data_obj)) {
            const cJSON* audio_item = cJSON_GetObjectItem(data_obj, "audio");
            if (cJSON_IsString(audio_item) && audio_item->valuestring != nullptr) {
                const char* hex = audio_item->valuestring;
                const size_t hex_len = strlen(hex);
                std::vector<int16_t> pcm;
                FastHexToPcm(hex, hex_len, pcm);
                if (!pcm.empty()) {
                    PushPcm(pcm.data(), pcm.size());
                }
            }
        }
    } else if (strcmp(event_str, "sentence_start") == 0) {
        ESP_LOGD(TAG, "分句开始合成 (sentence_start)");
    } else if (strcmp(event_str, "sentence_end") == 0) {
        ESP_LOGD(TAG, "分句合成结束 (sentence_end)");
    } else if (strcmp(event_str, "task_flushed") == 0) {
        ESP_LOGI(TAG, "缓冲区残留文本合成已送出 (task_flushed)");
        xEventGroupSetBits(event_group_, kBitTaskFlushed);
    } else if (strcmp(event_str, "task_finished") == 0) {
        ESP_LOGI(TAG, "全部合成结束 (task_finished)");
        is_finished_.store(true);
        session_active_.store(false);
        xEventGroupSetBits(event_group_, kBitTaskFinished);
    } else if (strcmp(event_str, "task_canceled") == 0) {
        ESP_LOGI(TAG, "打断已生效 (task_canceled)");
        session_active_.store(false);
        xEventGroupSetBits(event_group_, kBitTaskCanceled);
    } else if (strcmp(event_str, "task_failed") == 0) {
        const cJSON* base_resp = cJSON_GetObjectItem(root, "base_resp");
        const cJSON* msg = base_resp ? cJSON_GetObjectItem(base_resp, "status_msg") : nullptr;
        std::string err_msg = (cJSON_IsString(msg) && msg->valuestring) ? msg->valuestring : "未知错误";
        ESP_LOGE(TAG, "TTS 任务失败: %s", err_msg.c_str());
        {
            std::lock_guard<std::mutex> lk(error_mutex_);
            last_error_ = err_msg;
        }
        session_active_.store(false);
        xEventGroupSetBits(event_group_, kBitTaskFailed);
    }

    cJSON_Delete(root);
}

void MiniMaxBidiTts::PushPcm(const int16_t* samples, size_t count) {
    if (samples == nullptr || count == 0) return;
    std::lock_guard<std::mutex> lk(pcm_mutex_);
    if (pcm_queue_.size() + count > kMaxQueueSamples) {
        // 达到队列上限 (5秒音频缓冲), 避免 OOM, 丢弃最老的一段
        size_t overflow = (pcm_queue_.size() + count) - kMaxQueueSamples;
        pcm_queue_.erase(pcm_queue_.begin(), pcm_queue_.begin() + std::min(overflow, pcm_queue_.size()));
        ESP_LOGW(TAG, "PCM 缓冲溢出, 丢弃 %u 样本", (unsigned)overflow);
    }
    pcm_queue_.insert(pcm_queue_.end(), samples, samples + count);
}

bool MiniMaxBidiTts::SendText(const std::string& text) {
    if (!ws_ || !session_active_.load() || is_canceled_.load()) {
        return false;
    }
    if (text.empty()) return true;

    // MiniMax 文档: {"event":"task_continue","text":"..."}
    std::string msg = "{\"event\":\"task_continue\",\"text\":\"" + JsonEscape(text) + "\"}";
    return ws_->Send(msg);
}

bool MiniMaxBidiTts::Flush() {
    if (!ws_ || !session_active_.load() || is_canceled_.load()) {
        return false;
    }
    ESP_LOGI(TAG, "发送 task_flush 催出残留文本...");
    std::string msg = "{\"event\":\"task_flush\"}";
    return ws_->Send(msg);
}

bool MiniMaxBidiTts::Finish() {
    if (!ws_ || is_canceled_.load()) {
        return false;
    }
    ESP_LOGI(TAG, "发送 task_finish 标记会话完结...");
    std::string msg = "{\"event\":\"task_finish\"}";
    bool ok = ws_->Send(msg);
    return ok;
}

void MiniMaxBidiTts::Cancel() {
    is_canceled_.store(true);
    session_active_.store(false);
    if (ws_ && is_connected_.load()) {
        ESP_LOGI(TAG, "发送 task_cancel 打断合成...");
        std::string msg = "{\"event\":\"task_cancel\"}";
        ws_->Send(msg);
    }
    // 立即清空本地待播缓冲, 扬声器瞬间静音
    {
        std::lock_guard<std::mutex> lk(pcm_mutex_);
        pcm_queue_.clear();
    }
}

void MiniMaxBidiTts::Close() {
    session_active_.store(false);
    is_connected_.store(false);
    if (ws_) {
        ws_->Close();
        ws_.reset();
    }
    {
        std::lock_guard<std::mutex> lk(pcm_mutex_);
        pcm_queue_.clear();
    }
}

bool MiniMaxBidiTts::IsConnected() const {
    return is_connected_.load() && ws_ && ws_->IsConnected();
}

bool MiniMaxBidiTts::IsSessionActive() const {
    return session_active_.load();
}

bool MiniMaxBidiTts::IsFinished() const {
    return is_finished_.load();
}

bool MiniMaxBidiTts::PopAudioPcm(std::vector<int16_t>& chunk, size_t max_samples, uint32_t wait_ms) {
    chunk.clear();
    const int64_t deadline = esp_timer_get_time() + (int64_t)wait_ms * 1000LL;

    while (true) {
        if (is_canceled_.load()) return false;

        {
            std::lock_guard<std::mutex> lk(pcm_mutex_);
            if (!pcm_queue_.empty()) {
                size_t count = std::min(max_samples, pcm_queue_.size());
                chunk.assign(pcm_queue_.begin(), pcm_queue_.begin() + count);
                pcm_queue_.erase(pcm_queue_.begin(), pcm_queue_.begin() + count);
                return true;
            }
        }

        // 队列为空时, 若已经收到 task_finished 且没有更多音频, 则收尾
        if (is_finished_.load()) {
            std::lock_guard<std::mutex> lk(pcm_mutex_);
            return !pcm_queue_.empty();
        }

        if (esp_timer_get_time() >= deadline) {
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(15));
    }
}

bool MiniMaxBidiTts::HasAudioPcm() const {
    std::lock_guard<std::mutex> lk(pcm_mutex_);
    return !pcm_queue_.empty();
}

std::string MiniMaxBidiTts::GetLastError() const {
    std::lock_guard<std::mutex> lk(error_mutex_);
    return last_error_;
}

}  // namespace minimax_speech
