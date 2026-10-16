// main/audio/sound_player.cc
#include "sound_player.h"

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include <opus_decoder.h>
#include "audio_codec.h"
#include "audio_focus_manager.h"
#include "board.h"
#include "power_policy/power_hw.h"
#include "utils/psr_worker.h"

namespace {

constexpr const char* TAG = "SoundPlayer";

std::atomic<bool> s_is_playing{false};
std::atomic<bool> s_stop_requested{false};

struct SoundTaskContext {
    std::string data;
};

void SoundPlayerWorker(void* param) {
    auto* ctx = static_cast<SoundTaskContext*>(param);
    if (ctx == nullptr) {
        return;
    }
    s_is_playing.store(true);

    // 1. 申请 Transient 级别音频焦点 (会自动暂停正在后台播放的媒体流, 如网络收音机)
    AudioFocusManager::GetInstance().RequestFocus("sound", AudioClientType::Transient, nullptr);

    // 2. 确保外设总电源通电 (BT音频芯片上电以输出 I2S 时钟)
    if (!power_hw_main_rail_is_on()) {
        power_hw_main_rail_set(true);
    }

    // 3. 获取并启动 AudioCodec
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        ESP_LOGE(TAG, "AudioCodec is null, cannot play sound");
        AudioFocusManager::GetInstance().AbandonFocus("sound");
        s_is_playing.store(false);
        delete ctx;
        return;
    }

    codec->Start();
    codec->EnableOutput(true);

    const int target_rate = codec->output_sample_rate() > 0 ? codec->output_sample_rate() : 16000;
    OpusDecoderWrapper decoder(target_rate, 1, 60);

    const uint8_t* buf = reinterpret_cast<const uint8_t*>(ctx->data.data());
    const size_t size = ctx->data.size();
    size_t offset = 0;
    bool seen_head = false;
    bool seen_tags = false;

    auto find_page = [&](size_t start) -> size_t {
        for (size_t i = start; i + 4 <= size; ++i) {
            if (buf[i] == 'O' && buf[i + 1] == 'g' && buf[i + 2] == 'g' && buf[i + 3] == 'S') {
                return i;
            }
        }
        return static_cast<size_t>(-1);
    };

    while (!s_stop_requested.load()) {
        size_t pos = find_page(offset);
        if (pos == static_cast<size_t>(-1)) {
            break;
        }
        offset = pos;
        if (offset + 27 > size) {
            break;
        }

        const uint8_t* page = buf + offset;
        uint8_t page_segments = page[26];
        size_t seg_table_off = offset + 27;
        if (seg_table_off + page_segments > size) {
            break;
        }

        size_t body_size = 0;
        for (size_t i = 0; i < page_segments; ++i) {
            body_size += page[27 + i];
        }

        size_t body_off = seg_table_off + page_segments;
        if (body_off + body_size > size) {
            break;
        }

        size_t cur = body_off;
        size_t seg_idx = 0;
        while (seg_idx < page_segments && !s_stop_requested.load()) {
            size_t pkt_len = 0;
            size_t pkt_start = cur;
            bool continued = false;
            do {
                uint8_t l = page[27 + seg_idx++];
                pkt_len += l;
                cur += l;
                continued = (l == 255);
            } while (continued && seg_idx < page_segments);

            if (pkt_len == 0) continue;
            const uint8_t* pkt_ptr = buf + pkt_start;

            if (!seen_head) {
                if (pkt_len >= 19 && std::memcmp(pkt_ptr, "OpusHead", 8) == 0) {
                    seen_head = true;
                }
                continue;
            }
            if (!seen_tags) {
                if (pkt_len >= 8 && std::memcmp(pkt_ptr, "OpusTags", 8) == 0) {
                    seen_tags = true;
                }
                continue;
            }

            // 解码 Opus 并直接输出至 AudioCodec DMA
            std::vector<uint8_t> opus_pkt(pkt_ptr, pkt_ptr + pkt_len);
            std::vector<int16_t> pcm;
            if (decoder.Decode(std::move(opus_pkt), pcm) && !pcm.empty()) {
                codec->OutputData(pcm);
            }
        }

        offset = body_off + body_size;
    }

    // 4. 释放焦点 (若此前收音机在播放, AudioFocusManager 会自动触发收音机续播)
    AudioFocusManager::GetInstance().AbandonFocus("sound");

    // 5. 若无其他业务在使用音频, 关闭功放输出节省电量
    if (AudioFocusManager::GetInstance().GetCurrentHolder().empty()) {
        codec->EnableOutput(false);
    }

    delete ctx;
    s_is_playing.store(false);
}

}  // namespace

SoundPlayer& SoundPlayer::GetInstance() {
    static SoundPlayer instance;
    return instance;
}

void SoundPlayer::Play(const std::string_view& sound) {
    if (sound.empty()) {
        return;
    }

    // 如果当前正在播放, 先请求打断
    if (s_is_playing.load()) {
        Stop();
        int wait_count = 0;
        while (s_is_playing.load() && ++wait_count < 20) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    s_stop_requested.store(false);

    auto* ctx = new SoundTaskContext{std::string(sound)};
    if (!SpawnPsrWorker("sound_play", 4096, 5, SoundPlayerWorker, ctx)) {
        ESP_LOGE(TAG, "SpawnPsrWorker(sound_play) failed");
        delete ctx;
    }
}

void SoundPlayer::Stop() {
    s_stop_requested.store(true);
}

bool SoundPlayer::IsPlaying() const {
    return s_is_playing.load();
}
