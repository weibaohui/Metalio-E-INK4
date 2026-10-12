// main/services/ai_chat/tts_decoder.h
// TTS 响应自动探测与自适应音频解码器:
// 1. 容器探测: 支持 JSON(MiniMax/DashScope/讯飞/百度) 与 直接二进制音频流(OpenAI)
// 2. 文本解码: 自动探测 Hex 与 Base64 编码并解码为二进制字节
// 3. 音频解析: 自动探测 WAV(RIFF) 头、提取真实采样率与声道、跳过头数据防爆音,
//    若非 16kHz 自动插值重采样至 16kHz;若无头则按 16位小端裸 PCM 处理。
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <cmath>

#include <cJSON.h>
#include <esp_log.h>

namespace tts_decoder {

constexpr const char* TAG = "TtsDecoder";
constexpr int kTargetSampleRate = 16000;

// ---- 1. 基础编解码工具 ----

inline bool DecodeHex(const std::string& hex, std::vector<uint8_t>& out) {
    auto hexval = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.reserve(hex.size() / 2);
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        int hi = hexval(hex[i]);
        int lo = hexval(hex[i + 1]);
        if (hi < 0 || lo < 0) continue;
        out.push_back((uint8_t)((hi << 4) | lo));
    }
    return !out.empty();
}

inline bool DecodeBase64(const std::string& b64, std::vector<uint8_t>& out) {
    static const int8_t table[256] = {
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,62,-1,-1,-1,63,
        52,53,54,55,56,57,58,59,60,61,-1,-1,-1,-1,-1,-1,
        -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,
        15,16,17,18,19,20,21,22,23,24,25,-1,-1,-1,-1,-1,
        -1,26,27,28,29,30,31,32,33,34,35,36,37,38,39,40,
        41,42,43,44,45,46,47,48,49,50,51,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,
        -1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1,-1
    };
    uint32_t val = 0;
    int valb = -8;
    out.reserve(b64.size() * 3 / 4);
    for (uint8_t c : b64) {
        if (c == '=') break;
        int d = table[c];
        if (d < 0) continue;
        val = (val << 6) | d;
        valb += 6;
        if (valb >= 0) {
            out.push_back((uint8_t)((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return !out.empty();
}

inline bool IsProbablyBase64(const std::string& str) {
    // 检查前 256 字符，若出现 g-z, G-Z, +, /, = 则判定为 Base64
    size_t check_len = std::min(str.size(), (size_t)256);
    for (size_t i = 0; i < check_len; ++i) {
        char c = str[i];
        if (c == '+' || c == '/' || c == '=' ||
            (c >= 'g' && c <= 'z') || (c >= 'G' && c <= 'Z')) {
            return true;
        }
    }
    return false;
}

// 线性插值重采样至 16000 Hz 单声道
inline void ResampleTo16k(const int16_t* in, size_t in_samples, int in_rate, std::vector<int16_t>& out) {
    if (in_rate == kTargetSampleRate || in_rate <= 0) {
        out.insert(out.end(), in, in + in_samples);
        return;
    }
    const double step = (double)in_rate / (double)kTargetSampleRate;
    double pos = 0.0;
    out.reserve(out.size() + (size_t)(in_samples / step) + 16);
    while (pos + 1.0 < (double)in_samples) {
        const size_t i0 = (size_t)pos;
        const double frac = pos - (double)i0;
        const int32_t a = in[i0];
        const int32_t b = in[i0 + 1];
        out.push_back((int16_t)(a + (int32_t)((b - a) * frac)));
        pos += step;
    }
}

// ---- 2. 音频格式解析 ----

inline bool ParseWavAndExtract(const uint8_t* data, size_t len, std::vector<int16_t>& pcm_out, std::string& err_out) {
    if (len < 44 || memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0) {
        return false;
    }
    // 遍历 RIFF chunks
    size_t offset = 12;
    int channels = 1;
    int sample_rate = 16000;
    int bits_per_sample = 16;
    bool fmt_found = false;
    bool data_found = false;
    size_t data_offset = 0;
    size_t data_size = 0;

    while (offset + 8 <= len) {
        char chunk_id[5] = {0};
        memcpy(chunk_id, data + offset, 4);
        uint32_t chunk_len = (uint32_t)(data[offset + 4] | (data[offset + 5] << 8) |
                                       (data[offset + 6] << 16) | (data[offset + 7] << 24));
        if (memcmp(chunk_id, "fmt ", 4) == 0 && offset + 8 + 16 <= len) {
            uint16_t format_tag = (uint16_t)(data[offset + 8] | (data[offset + 9] << 8));
            if (format_tag != 1) {
                err_out = "不支持非 PCM 格式的 WAV (format=" + std::to_string(format_tag) + ")";
                return false;
            }
            channels = (int)(data[offset + 10] | (data[offset + 11] << 8));
            sample_rate = (int)(data[offset + 12] | (data[offset + 13] << 8) |
                                (data[offset + 14] << 16) | (data[offset + 15] << 24));
            bits_per_sample = (int)(data[offset + 22] | (data[offset + 23] << 8));
            fmt_found = true;
        } else if (memcmp(chunk_id, "data", 4) == 0) {
            data_offset = offset + 8;
            data_size = std::min((size_t)chunk_len, len - data_offset);
            data_found = true;
            break;
        }
        offset += 8 + ((chunk_len + 1) & ~1u);  // 按偶数字节对齐
    }

    if (!fmt_found || !data_found || bits_per_sample != 16) {
        err_out = "WAV 参数不支持 (fmt=" + std::to_string(fmt_found) + " bits=" + std::to_string(bits_per_sample) + ")";
        return false;
    }

    ESP_LOGI(TAG, "探测到 WAV 音频: %d Hz, %d 声道, %u 字节 PCM", sample_rate, channels, (unsigned)data_size);

    const size_t total_samples = data_size / 2;
    std::vector<int16_t> mono_raw;
    mono_raw.reserve(channels == 2 ? total_samples / 2 : total_samples);

    if (channels == 2) {
        for (size_t i = 0; i + 3 < data_size; i += 4) {
            int16_t l = (int16_t)(data[data_offset + i] | (data[data_offset + i + 1] << 8));
            int16_t r = (int16_t)(data[data_offset + i + 2] | (data[data_offset + i + 3] << 8));
            mono_raw.push_back((int16_t)(((int32_t)l + r) / 2));
        }
    } else {
        for (size_t i = 0; i + 1 < data_size; i += 2) {
            int16_t s = (int16_t)(data[data_offset + i] | (data[data_offset + i + 1] << 8));
            mono_raw.push_back(s);
        }
    }

    ResampleTo16k(mono_raw.data(), mono_raw.size(), sample_rate, pcm_out);
    return true;
}

// 裸 16位小端 PCM (无头) 解析
inline bool ParseRawPcm(const uint8_t* data, size_t len, std::vector<int16_t>& pcm_out) {
    if (len < 2) return false;
    const size_t num_samples = len / 2;
    pcm_out.reserve(pcm_out.size() + num_samples);
    for (size_t i = 0; i + 1 < len; i += 2) {
        int16_t s = (int16_t)(data[i] | (data[i + 1] << 8));
        pcm_out.push_back(s);
    }
    ESP_LOGI(TAG, "当作裸 16kHz 16位 PCM 解析: %u 样本", (unsigned)num_samples);
    return true;
}

// ---- 3. 顶层自动探测与解码主入口 ----
inline bool DecodeTtsResponse(const std::string& raw_resp_in, std::vector<int16_t>& pcm_out, std::string& err_out) {
    if (raw_resp_in.empty()) {
        err_out = "TTS 响应为空";
        return false;
    }
    // 拷贝成本换来"调用方可立即释放原响应"的时机:调用方(Synthesize)在把
    // 响应交给本函数后即释放自己的 resp,峰值从 2 份降为 1 份。
    std::string raw_resp_copy = raw_resp_in;
    std::string& raw_resp = raw_resp_copy;

    // A. 探测容器格式: JSON vs 直接二进制流
    size_t first_non_space = raw_resp.find_first_not_of(" \t\r\n");
    std::vector<uint8_t> binary_data;

    if (first_non_space != std::string::npos && raw_resp[first_non_space] == '{') {
        // 是 JSON 响应
        cJSON* root = cJSON_Parse(raw_resp.c_str());
        if (root == nullptr) {
            err_out = "TTS 响应 JSON 解析失败";
            return false;
        }

        // 尝试提取音频字符串字段 (优先级: data.audio > output.audio > audio > data)
        std::string audio_str;
        const cJSON* data = cJSON_GetObjectItem(root, "data");
        if (cJSON_IsObject(data)) {
            const cJSON* a = cJSON_GetObjectItem(data, "audio");
            if (cJSON_IsString(a) && a->valuestring != nullptr) audio_str = a->valuestring;
        }
        if (audio_str.empty()) {
            const cJSON* output = cJSON_GetObjectItem(root, "output");
            if (cJSON_IsObject(output)) {
                const cJSON* a = cJSON_GetObjectItem(output, "audio");
                if (cJSON_IsString(a) && a->valuestring != nullptr) audio_str = a->valuestring;
            }
        }
        if (audio_str.empty()) {
            const cJSON* a = cJSON_GetObjectItem(root, "audio");
            if (cJSON_IsString(a) && a->valuestring != nullptr) audio_str = a->valuestring;
        }
        if (audio_str.empty() && cJSON_IsString(data) && data->valuestring != nullptr) {
            audio_str = data->valuestring;
        }

        if (audio_str.empty()) {
            // 尝试读取服务端错误原因
            const cJSON* base_resp = cJSON_GetObjectItem(root, "base_resp");
            const cJSON* msg = base_resp ? cJSON_GetObjectItem(base_resp, "status_msg") : nullptr;
            if (!msg) msg = cJSON_GetObjectItem(root, "message");
            if (!msg) msg = cJSON_GetObjectItem(root, "error");
            if (cJSON_IsString(msg) && msg->valuestring != nullptr) {
                err_out = "TTS 报错: " + std::string(msg->valuestring);
            } else {
                err_out = "TTS JSON 未含已知 audio 载荷";
            }
            cJSON_Delete(root);
            return false;
        }
        cJSON_Delete(root);

        // B. 探测文本编码: Base64 vs Hex。
        // 内存纪律:长回复的 audio 载荷可达 MB 级,与 raw_resp/二进制/PCM 多份并存
        // 会把 PSRAM 打爆(bad_alloc → 未捕获异常 → abort,2026-10-12 实测)。
        // 解码出二进制后立即清 audio_str 释放一份。
        if (IsProbablyBase64(audio_str)) {
            ESP_LOGI(TAG, "探测到 Base64 编码音频 (%u 字符)", (unsigned)audio_str.size());
            if (!DecodeBase64(audio_str, binary_data)) {
                err_out = "Base64 解码失败";
                return false;
            }
        } else {
            ESP_LOGI(TAG, "探测到 Hex 编码音频 (%u 字符)", (unsigned)audio_str.size());
            if (!DecodeHex(audio_str, binary_data)) {
                err_out = "Hex 解码失败";
                return false;
            }
        }
    } else {
        // 直接二进制流 (如 OpenAI /v1/audio/speech)
        ESP_LOGI(TAG, "探测到直接二进制流 (%u 字节)", (unsigned)raw_resp.size());
        binary_data.assign(raw_resp.begin(), raw_resp.end());
    }

    if (binary_data.size() < 2) {
        err_out = "提取音频二进制过短";
        return false;
    }

    // 内存纪律:二进制已就绪,释放原始响应副本(MB 级,与 PCM 并存即爆);
    // JSON 路径的 audio_str 文本副本在其块内随二进制解码完成即结束作用域
    std::string().swap(raw_resp_copy);

    // C. 探测音频封装格式: WAV (RIFF) vs 裸 PCM
    pcm_out.clear();
    if (binary_data.size() >= 44 && memcmp(binary_data.data(), "RIFF", 4) == 0) {
        if (!ParseWavAndExtract(binary_data.data(), binary_data.size(), pcm_out, err_out)) {
            return false;
        }
    } else {
        if (!ParseRawPcm(binary_data.data(), binary_data.size(), pcm_out)) {
            err_out = "裸 PCM 样本过短";
            return false;
        }
    }

    if (pcm_out.empty()) {
        err_out = "解码后 PCM 样本数为 0";
        return false;
    }
    return true;
}

}  // namespace tts_decoder
