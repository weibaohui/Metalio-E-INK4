// main/audio/audio_focus_manager.h
// 统一音频焦点管理器 (Audio Focus Manager):
// 协调系统各业务对板载音频编解码器 (I2S DMA) 的使用权限, 彻底消灭声音打架、杂音混叠与硬件争抢。
// 规则:
// 1. Exclusive (独占高优, 如 AI 语音、电话): 抢占时通知 Media 客户端暂停; 释放时恢复 Media 客户端续播。
// 2. Media (媒体流, 如网络收音机、音乐): 属于可被打断的长期后台音频。
// 3. Transient (瞬时提示音, 如按键反馈、系统蜂鸣): 短暂独占或压低媒体音。
#pragma once

#include <string>
#include <functional>
#include <mutex>
#include <vector>

enum class AudioClientType {
    Transient,  ///< 瞬时短音效 (系统提示音、按键音)
    Media,      ///< 背景媒体流 (网络收音机、音乐)
    Exclusive,  ///< 强独占业务 (AI 对话、通话)
};

enum class AudioFocusState {
    Gained,         ///< 获得焦点, 允许启动/恢复播放
    Lost,           ///< 永久失去焦点, 需停止播放并释放资源
    LossTransient,  ///< 暂时失去焦点, 需暂停播放 (后续可能被唤醒恢复)
};

using AudioFocusCallback = std::function<void(AudioFocusState)>;

class AudioFocusManager {
public:
    static AudioFocusManager& GetInstance();

    /**
     * @brief 申请音频焦点
     * @param client_id 业务唯一标识 (如 "ai_chat", "radio")
     * @param type 客户端类型
     * @param callback 焦点变化回调 (被抢占/恢复时触发)
     * @return true 申请成功 (已获得焦点)
     */
    bool RequestFocus(const std::string& client_id,
                      AudioClientType type,
                      AudioFocusCallback callback);

    /**
     * @brief 放弃/释放音频焦点 (播放结束或退出时调用)
     * @param client_id 业务唯一标识
     */
    void AbandonFocus(const std::string& client_id);

    /**
     * @brief 查询当前持有焦点的业务
     */
    std::string GetCurrentHolder() const;

private:
    AudioFocusManager() = default;
    ~AudioFocusManager() = default;
    AudioFocusManager(const AudioFocusManager&) = delete;
    AudioFocusManager& operator=(const AudioFocusManager&) = delete;

    struct FocusRecord {
        std::string client_id;
        AudioClientType type;
        AudioFocusCallback callback;
    };

    mutable std::recursive_mutex mutex_;
    std::vector<FocusRecord> focus_stack_;
};
