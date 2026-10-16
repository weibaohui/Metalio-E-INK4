// main/audio/sound_player.h
// 轻量级系统短音效播放器 (SoundPlayer)
// 统一整机音频架构: 直接走 AudioCodec + AudioFocusManager (Transient 焦点)
// 彻底摆脱重型 AudioService 常驻管线, 播完即走, 零内存常驻
#pragma once

#include <string_view>

class SoundPlayer {
public:
    static SoundPlayer& GetInstance();

    /**
     * @brief 异步播放 OGG/Opus 短音效 (直通 AudioCodec, 播放完毕自动释放资源)
     * @param sound OGG 音频数据
     */
    void Play(const std::string_view& sound);

    /**
     * @brief 停止正在播放的音效
     */
    void Stop();

    /**
     * @brief 是否正在播放音效
     */
    bool IsPlaying() const;

private:
    SoundPlayer() = default;
    ~SoundPlayer() = default;
    SoundPlayer(const SoundPlayer&) = delete;
    SoundPlayer& operator=(const SoundPlayer&) = delete;
};
