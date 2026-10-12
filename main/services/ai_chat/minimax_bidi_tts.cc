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

// 检查是否包含强断句终结标点 (句号、感叹号、问号、换行等)
bool HasSentenceTerminator(const std::string& s) {
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == '\n' || c == '.' || c == '!' || c == '?') {
            return true;
        }
        if (i + 2 < s.size()) {
            if (c == 0xEF && static_cast<unsigned char>(s[i+1]) == 0xBC) {
                unsigned char b2 = static_cast<unsigned char>(s[i+2]);
                if (b2 == 0x81 || b2 == 0x9F) return true; // ！ ？
            }
            if (c == 0xE3 && static_cast<unsigned char>(s[i+1]) == 0x80) {
                unsigned char b2 = static_cast<unsigned char>(s[i+2]);
                if (b2 == 0x82) return true; // 。
            }
        }
    }
    return false;
}

// 检查是否包含次级停顿标点 (逗号、顿号、分号、冒号等)
bool HasClausePunctuation(const std::string& s) {
    for (size_t i = 0; i < s.size(); ++i) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        if (c == ',' || c == ';' || c == ':') {
            return true;
        }
        if (i + 2 < s.size()) {
            if (c == 0xEF && static_cast<unsigned char>(s[i+1]) == 0xBC) {
                unsigned char b2 = static_cast<unsigned char>(s[i+2]);
                if (b2 == 0x8C || b2 == 0x9B || b2 == 0x9A) return true; // ， ； ：
            }
            if (c == 0xE3 && static_cast<unsigned char>(s[i+1]) == 0x80) {
                unsigned char b2 = static_cast<unsigned char>(s[i+2]);
                if (b2 == 0x81) return true; // 、
            }
        }
    }
    return false;
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
    is_flushed_.store(false);
    is_finished_.store(false);
    is_canceled_.store(false);
    session_active_.store(false);
    total_pcm_received_ = 0;

    {
        std::lock_guard<std::mutex> lk(send_buf_mutex_);
        send_buffer_.clear();
    }
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
            xEventGroupSetBits(event_group_, kBitTaskFinished | kBitTaskFlushed);
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
    last_active_us_.store(esp_timer_get_time());

    cJSON* root = cJSON_ParseWithLength(data, len);
    if (root == nullptr) {
        ESP_LOGD(TAG, "非 JSON 文本帧: %.64s", data);
        return;
    }

    // 检查 base_resp 中的状态码
    const cJSON* base_resp = cJSON_GetObjectItem(root, "base_resp");
    if (cJSON_IsObject(base_resp)) {
        const cJSON* code = cJSON_GetObjectItem(base_resp, "status_code");
        if (cJSON_IsNumber(code) && code->valueint != 0) {
            const cJSON* msg = cJSON_GetObjectItem(base_resp, "status_msg");
            ESP_LOGW(TAG, "MiniMax WSS 响应状态码 code=%d, msg=%s",
                     code->valueint,
                     (cJSON_IsString(msg) && msg->valuestring) ? msg->valuestring : "");
        }
    }

    const cJSON* event_item = cJSON_GetObjectItem(root, "event");
    const char* event_str = cJSON_IsString(event_item) ? event_item->valuestring : "";

    if (strcmp(event_str, "connected_success") == 0) {
        // 服务端确认建连, 立即发送 task_start
        char spd_buf[16], vol_buf[16];
        snprintf(spd_buf, sizeof(spd_buf), "%.1f", voice_cfg_.speed);
        snprintf(vol_buf, sizeof(vol_buf), "%.1f", voice_cfg_.vol);

        std::string model_name = voice_cfg_.model;
        if (model_name.empty() || model_name == "speech-02-turbo") {
            model_name = "speech-2.8-turbo";
        }
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
                    total_pcm_received_ += pcm.size();
                    PushPcm(pcm.data(), pcm.size());
                    ESP_LOGD(TAG, "收到音频块: %u 样本, 累计 %u 样本 (~%.1fs)",
                             (unsigned)pcm.size(), (unsigned)total_pcm_received_,
                             (float)total_pcm_received_ / 16000.0f);
                }
            }
        }
    } else if (strcmp(event_str, "sentence_start") == 0) {
        ESP_LOGI(TAG, "分句开始合成 (sentence_start)");
    } else if (strcmp(event_str, "sentence_end") == 0) {
        ESP_LOGI(TAG, "分句合成结束 (sentence_end), 累计已收音频 %.1fs",
                 (float)total_pcm_received_ / 16000.0f);
    } else if (strcmp(event_str, "task_flushed") == 0) {
        ESP_LOGI(TAG, "服务端确认所有音频帧已全部下发 (task_flushed), 累计 %.1fs",
                 (float)total_pcm_received_ / 16000.0f);
        is_flushed_.store(true);
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
        size_t overflow = (pcm_queue_.size() + count) - kMaxQueueSamples;
        pcm_queue_.erase(pcm_queue_.begin(), pcm_queue_.begin() + std::min(overflow, pcm_queue_.size()));
        ESP_LOGW(TAG, "PCM 缓冲溢出(>60s), 丢弃 %u 样本", (unsigned)overflow);
    }
    pcm_queue_.insert(pcm_queue_.end(), samples, samples + count);
}

bool MiniMaxBidiTts::DoSendContinue(const std::string& str) {
    if (!ws_ || !session_active_.load() || is_canceled_.load() || str.empty()) {
        return false;
    }
    std::string msg = "{\"event\":\"task_continue\",\"text\":\"" + JsonEscape(str) + "\"}";
    return ws_->Send(msg);
}

bool MiniMaxBidiTts::SendText(const std::string& text) {
    if (!ws_ || !session_active_.load() || is_canceled_.load()) {
        return false;
    }
    if (text.empty()) return true;

    std::string to_send;
    {
        std::lock_guard<std::mutex> lk(send_buf_mutex_);
        send_buffer_ += text;
        // 智能分句策略 (避免切碎短句，减少服务端分句断点开销):
        // 1. 强断句终结符 (。！？\n): 积攒 >=15 字节 (约 5 汉字) 即切分发送成句
        // 2. 次级从句标点 (，、；：): 需积攒满自然意群 (>=45 字节, 约 15 汉字) 才切分发送
        // 3. 长句保底: 若长达 75 字节 (约 25 汉字) 仍无标点，强制切分发送
        const bool term = HasSentenceTerminator(send_buffer_) && send_buffer_.size() >= 15;
        const bool clause = HasClausePunctuation(send_buffer_) && send_buffer_.size() >= 45;
        const bool overflow = send_buffer_.size() >= 75;

        if (term || clause || overflow) {
            to_send = std::move(send_buffer_);
            send_buffer_.clear();
        }
    }

    if (!to_send.empty()) {
        return DoSendContinue(to_send);
    }
    return true;
}

bool MiniMaxBidiTts::Flush() {
    if (!ws_ || !session_active_.load() || is_canceled_.load()) {
        return false;
    }

    // 1. 将暂存在客户端缓冲中的文本全部送出
    std::string remaining;
    {
        std::lock_guard<std::mutex> lk(send_buf_mutex_);
        remaining = std::move(send_buffer_);
        send_buffer_.clear();
    }
    if (!remaining.empty()) {
        DoSendContinue(remaining);
    }

    // 2. 发送 task_flush 催出服务端残留
    ESP_LOGI(TAG, "发送 task_flush 催出服务端残留文本...");
    std::string msg = "{\"event\":\"task_flush\"}";
    return ws_->Send(msg);
}

bool MiniMaxBidiTts::WaitFlushed(uint32_t timeout_ms) {
    if (is_flushed_.load() || is_canceled_.load()) return true;
    if (!session_active_.load()) return true;

    EventBits_t bits = xEventGroupWaitBits(event_group_,
                                          kBitTaskFlushed | kBitTaskFinished | kBitTaskFailed | kBitTaskCanceled,
                                          pdFALSE, pdFALSE,
                                          pdMS_TO_TICKS(timeout_ms));
    return (bits & (kBitTaskFlushed | kBitTaskFinished)) != 0;
}

bool MiniMaxBidiTts::WaitUntilDoneOrIdle(uint32_t idle_timeout_ms,
                                        const std::atomic<bool>* interrupt_flag,
                                        const std::atomic<bool>* cancel_flag) {
    if (is_flushed_.load() || is_finished_.load() || is_canceled_.load()) return true;
    if (!session_active_.load()) return true;

    while (session_active_.load() && !is_canceled_.load()) {
        if (is_flushed_.load() || is_finished_.load()) {
            return true;
        }
        if ((interrupt_flag && interrupt_flag->load()) || (cancel_flag && cancel_flag->load())) {
            ESP_LOGI(TAG, "WaitUntilDoneOrIdle: 收到打断/取消信号, 立即退出等待");
            Cancel();
            return false;
        }
        int64_t last = last_active_us_.load();
        if (last > 0) {
            int64_t idle = (esp_timer_get_time() - last) / 1000LL;
            if (idle > (int64_t)idle_timeout_ms) {
                ESP_LOGW(TAG, "WaitUntilDoneOrIdle: 服务端静默超时 %lld ms, 结束等待", (long long)idle);
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return is_flushed_.load() || is_finished_.load();
}

bool MiniMaxBidiTts::IsCanceled() const {
    return is_canceled_.load();
}

bool MiniMaxBidiTts::Finish() {
    if (!ws_ || is_canceled_.load()) {
        return false;
    }
    ESP_LOGI(TAG, "发送 task_finish 标记会话完结...");
    std::string msg = "{\"event\":\"task_finish\"}";
    return ws_->Send(msg);
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
    {
        std::lock_guard<std::mutex> lk(send_buf_mutex_);
        send_buffer_.clear();
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
    {
        std::lock_guard<std::mutex> lk(send_buf_mutex_);
        send_buffer_.clear();
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

bool MiniMaxBidiTts::IsFlushed() const {
    return is_flushed_.load();
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

        // 队列为空时: 若已收到 task_flushed 或 task_finished 且缓冲无数据, 则确认取完
        if (is_flushed_.load() || is_finished_.load()) {
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

size_t MiniMaxBidiTts::GetQueuedSamples() const {
    std::lock_guard<std::mutex> lk(pcm_mutex_);
    return pcm_queue_.size();
}

std::string MiniMaxBidiTts::GetLastError() const {
    std::lock_guard<std::mutex> lk(error_mutex_);
    return last_error_;
}

}  // namespace minimax_speech
