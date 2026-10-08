#include "audio_codec.h"
#include "board.h"
#include "settings.h"

#include <esp_log.h>
#include <cstring>
#include <driver/i2s_common.h>

#define TAG "AudioCodec"

AudioCodec::AudioCodec() {
}

AudioCodec::~AudioCodec() {
}

void AudioCodec::OutputData(std::vector<int16_t>& data) {
    Write(data.data(), data.size());
}

bool AudioCodec::InputData(std::vector<int16_t>& data) {
    int samples = Read(data.data(), data.size());
    if (samples > 0) {
        return true;
    }
    return false;
}

void AudioCodec::Start() {
    {
        // 释放旧键条目(音量默认 70→5 改键 v2,旧键不再使用;NVS 16KB 很紧,
        // 曾因空间不足让音量键一次写入 abort 整机)
        Settings legacy("audio", true);
        legacy.EraseKey("output_volume");
    }
    Settings settings("audio", false);
    // 键名 v2:出厂默认从 70 改 5,换键让旧设备里存着的 70 不再盖掉新默认
    output_volume_ = settings.GetInt("output_volume_v2", output_volume_);
    if (output_volume_ <= 0) {
        ESP_LOGW(TAG, "Output volume value (%d) is too small, setting to default (10)", output_volume_);
        output_volume_ = 10;
    }

    // 幂等:i2s 通道已使能则跳过(二次 enable 返回 ESP_ERR_INVALID_STATE,
    // ESP_ERROR_CHECK 会 abort 整机)。收音机等只开输出的路径可安全重复调 Start()。
    if (!started_) {
        if (tx_handle_ != nullptr) {
            ESP_ERROR_CHECK(i2s_channel_enable(tx_handle_));
        }
        if (rx_handle_ != nullptr) {
            ESP_ERROR_CHECK(i2s_channel_enable(rx_handle_));
        }
        started_ = true;
    }

    EnableInput(true);
    EnableOutput(true);
    ESP_LOGI(TAG, "Audio codec started");
}

void AudioCodec::SetOutputVolume(int volume) {
    output_volume_ = volume;
    ESP_LOGI(TAG, "Set output volume to %d", output_volume_);
    
    Settings settings("audio", true);
    settings.SetInt("output_volume_v2", output_volume_);
}

void AudioCodec::SetInputGain(float gain) {
    input_gain_ = gain;
    ESP_LOGI(TAG, "Set input gain to %.1f", input_gain_);
}

void AudioCodec::EnableInput(bool enable) {
    if (enable == input_enabled_) {
        return;
    }
    input_enabled_ = enable;
    ESP_LOGI(TAG, "Set input enable to %s", enable ? "true" : "false");
}

void AudioCodec::EnableOutput(bool enable) {
    if (enable == output_enabled_) {
        return;
    }
    output_enabled_ = enable;
    ESP_LOGI(TAG, "Set output enable to %s", enable ? "true" : "false");
}
