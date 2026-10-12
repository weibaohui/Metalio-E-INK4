// main/services/ai_chat/minimax_bidi_tts.h
// MiniMax 双向流式语音合成 (WebSocket Bidi) 底层组件:
// 接口规范: wss://api.minimax.cn/ws/v1/t2a_v2_bidi
// 特性:
// 1. 面向大模型流式输出设计: 智能分句/断句发送 (task_continue), 由 MiniMax 服务端自动攒句合成。
// 2. 边生成边发声: 服务端分句返回音频分片 (task_continued, hex 编码 PCM 16kHz 单声道)。
// 3. 原生支持随时打断 (task_cancel), 立即停止服务端合成与清空本地待播队列。
// 4. 线程安全, 内部解耦网络接收与音频消费。
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include "minimax_speech.h"
#include "web_socket.h"

namespace minimax_speech {

class MiniMaxBidiTts {
public:
    MiniMaxBidiTts();
    ~MiniMaxBidiTts();

    // 禁用拷贝
    MiniMaxBidiTts(const MiniMaxBidiTts&) = delete;
    MiniMaxBidiTts& operator=(const MiniMaxBidiTts&) = delete;

    /**
     * @brief 启动双向流式合成会话
     * @param key MiniMax API Key
     * @param voice_cfg 音色与语音参数配置
     * @param err 失败时返回错误描述
     * @param ws_url WebSocket 地址, 默认 wss://api.minimax.cn/ws/v1/t2a_v2_bidi
     * @return true 握手成功且服务端已就绪 (task_started), 可以开始推文本
     */
    bool StartSession(const std::string& key,
                      const VoiceConfig& voice_cfg,
                      std::string& err,
                      const std::string& ws_url = "wss://api.minimax.cn/ws/v1/t2a_v2_bidi");

    /**
     * @brief 增量发送文本片段 (来自大模型 stream 输出的 token)
     * @param text 增量文本 (内部自动做标点断句缓冲, 避免暴洪式微帧)
     * @return true 发送成功
     */
    bool SendText(const std::string& text);

    /**
     * @brief 催出攒句缓冲区中的残留文本 (文本全部送完后调用, 使尾句立即合成出音频)
     */
    bool Flush();

    /**
     * @brief 等待服务端完成本轮所有发送文本的音频下发 (等待 task_flushed 事件)
     * @param timeout_ms 最长等待时间 (毫秒)
     * @return true 服务端确认全部音频下发完毕
     */
    bool WaitFlushed(uint32_t timeout_ms = 15000);

    /**
     * @brief 智能等待服务端完成全部合成下发：以静默活跃超时判定, 只要服务端持续在出分句音频即不超时
     * @param idle_timeout_ms 无任何新音频或事件到达的最大允许静默时间 (毫秒, 默认 15s)
     * @param interrupt_flag 外部打断标志, 置 true 时毫秒级退出等待并取消合成
     * @param cancel_flag 外部取消标志, 置 true 时立即退出等待
     * @return true 正常完成 (收到 task_flushed / task_finished)
     */
    bool WaitUntilDoneOrIdle(uint32_t idle_timeout_ms = 15000,
                             const std::atomic<bool>* interrupt_flag = nullptr,
                             const std::atomic<bool>* cancel_flag = nullptr);

    /**
     * @brief 正常结束本次合成任务 (服务端合成完残留音频后关闭会话)
     */
    bool Finish();

    /**
     * @brief 打断合成 (用户插话/离屏取消时调用, 立即丢弃未合成文本并停止输出)
     */
    void Cancel();

    /**
     * @brief 关闭 WebSocket 连接并释放资源
     */
    void Close();

    /**
     * @brief 连接与会话状态查询
     */
    bool IsConnected() const;
    bool IsSessionActive() const;
    bool IsFinished() const;
    bool IsFlushed() const;
    bool IsCanceled() const;

    /**
     * @brief 从内部音频缓冲队列中取出一批 16kHz 单声道 int16 PCM 样本
     * @param chunk 输出缓冲
     * @param max_samples 最多获取的样本数 (例如 960 个 = 60ms)
     * @param wait_ms 队列为空时的最大等待毫秒数
     * @return true 成功获取到样本; false 队列为空且会话已结束/被打断
     */
    bool PopAudioPcm(std::vector<int16_t>& chunk, size_t max_samples, uint32_t wait_ms = 50);

    /**
     * @brief 当前队列中是否还有未播放完的音频样本
     */
    bool HasAudioPcm() const;

    /**
     * @brief 当前队列中累积的音频采样点数
     */
    size_t GetQueuedSamples() const;

    /**
     * @brief 获取最后一次出错信息
     */
    std::string GetLastError() const;

private:
    void OnWebSocketData(const char* data, size_t len, bool binary);
    void PushPcm(const int16_t* samples, size_t count);
    bool DoSendContinue(const std::string& str);

    std::unique_ptr<WebSocket> ws_;
    std::string last_error_;
    mutable std::mutex error_mutex_;

    // 协议握手事件
    EventGroupHandle_t event_group_ = nullptr;
    static constexpr EventBits_t kBitConnected     = BIT0;
    static constexpr EventBits_t kBitTaskStarted   = BIT1;
    static constexpr EventBits_t kBitTaskFlushed   = BIT2;
    static constexpr EventBits_t kBitTaskFinished  = BIT3;
    static constexpr EventBits_t kBitTaskCanceled  = BIT4;
    static constexpr EventBits_t kBitTaskFailed    = BIT5;

    std::atomic<bool> is_connected_{false};
    std::atomic<bool> session_active_{false};
    std::atomic<bool> is_flushed_{false};
    std::atomic<bool> is_finished_{false};
    std::atomic<bool> is_canceled_{false};
    std::atomic<bool> first_sentence_sent_{false};
    std::atomic<int64_t> last_active_us_{0};

    // 客户端发送平滑缓冲 (按标点或长度分批发送, 防暴洪)
    std::mutex send_buf_mutex_;
    std::string send_buffer_;

    // 音频 PCM 队列缓冲 (保存在 PSRAM 中, 容量充足不丢帧)
    mutable std::mutex pcm_mutex_;
    std::vector<int16_t> pcm_queue_;
    static constexpr size_t kMaxQueueSamples = 16000 * 60; // 允许缓冲 60 秒音频 (约 1.9MB PSRAM)

    VoiceConfig voice_cfg_;
    std::string session_id_;
    size_t total_pcm_received_ = 0;
};

}  // namespace minimax_speech
