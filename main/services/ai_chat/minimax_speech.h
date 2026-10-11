// main/services/ai_chat/minimax_speech.h
// MiniMax 语音功能独立模块:
// 1. 语音合成 (TTS): t2a_v2 接口, 支持音色/语速/音量/情绪设定, 自动通过 tts_decoder 解码为 16k PCM
// 2. 语音转文字 (STT): OpenAI 兼容的 audio/transcriptions 接口, 打包上传 16k WAV
// 3. 音色库与参数持久化: 统一管理常用音色列表、参数配置与 NVS 存储
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

namespace minimax_speech {

// 支持点选的声音信息
struct VoiceInfo {
    const char* id;
    const char* name;
    const char* desc;
};

// 获取支持的推荐中文音色列表 (12 种典型中文音色)
const std::vector<VoiceInfo>& GetSupportedVoices();

// 语音合成配置
struct VoiceConfig {
    std::string voice_id = "male-qn-qingse"; // 音色 ID (默认青涩青年)
    float speed = 1.0f;                      // 语速: 0.5 ~ 2.0 (默认 1.0)
    float vol = 1.0f;                        // 音量: 0.1 ~ 10.0 (默认 1.0)
    int pitch = 0;                           // 音调: -12 ~ 12 (默认 0)
    std::string emotion = "neutral";         // 情绪: neutral, happy, sad, angry
    std::string model = "speech-02-turbo";   // 模型: speech-02-turbo, speech-02-hd
};

// 读取与设置当前音色配置 (线程安全 RAM 缓存 + NVS 持久化)
VoiceConfig GetVoiceConfig();
void SetVoiceConfig(const VoiceConfig& cfg);
void SaveVoiceConfig(const VoiceConfig& cfg);
void InitVoiceConfig();

// MiniMax TTS 合成: 返回 16kHz 单声道 int16 PCM 样本
bool Synthesize(const std::string& key,
                const std::string& text,
                const VoiceConfig& voice_cfg,
                std::vector<int16_t>& pcm,
                std::string& err,
                const std::atomic<bool>* interrupt_flag = nullptr,
                const std::atomic<bool>* cancel_flag = nullptr);

// MiniMax STT 转写: 上传 16kHz mono WAV, 返回识别文本
bool Transcribe(const std::string& url,
                const std::string& key,
                const std::string& model,
                const std::vector<int16_t>& pcm,
                std::string& text,
                std::string& err);

}  // namespace minimax_speech
