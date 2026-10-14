// main/core/system_event_bus.h
// 轻量级系统事件总线 (SystemEventBus) - Pub/Sub 模式
// 用于在硬件抽象层、后台服务与各前端应用/页面之间实现彻底解耦。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

enum class SystemEventType : uint16_t {
    kBatteryState = 1,   ///< 电池电量、充放电状态更新
    kWifiState,          ///< WiFi 连接状态、信号强度与 SSID
    kAudioVolume,        ///< 系统音量变化与静音状态
    kAudioPlayback,      ///< 音频播放状态 (电台/TTS/AI 对话)
    kPowerState,         ///< 供电与待机休眠策略状态
    kTimeTick,           ///< 系统时钟心跳更新
};

enum class SystemAudioPlaybackState : uint8_t {
    kStopped = 0,
    kConnecting,
    kPlaying,
    kPaused,
    kError,
};

enum class SystemWifiState : uint8_t {
    kDisconnected = 0,
    kConnecting,
    kConnected,
};

struct SystemEvent {
    SystemEventType type;

    union {
        struct {
            int level;          ///< 0..100
            bool charging;
            bool discharging;
            bool critical;
        } battery;

        struct {
            int volume;         ///< 0..100
            bool muted;
        } audio_volume;

        struct {
            SystemAudioPlaybackState state;
        } audio_playback;

        struct {
            SystemWifiState state;
            int rssi;
        } wifi;
    };

    std::string extra_str;       ///< 附加字符串(如 SSID、业务 owner_id、时间 "HH:MM")

    // 便捷工厂构造函数
    static SystemEvent Battery(int level, bool charging, bool discharging, bool critical = false) {
        SystemEvent ev{};
        ev.type = SystemEventType::kBatteryState;
        ev.battery.level = level;
        ev.battery.charging = charging;
        ev.battery.discharging = discharging;
        ev.battery.critical = critical;
        return ev;
    }

    static SystemEvent Wifi(SystemWifiState state, int rssi = 0, const std::string& ssid = "") {
        SystemEvent ev{};
        ev.type = SystemEventType::kWifiState;
        ev.wifi.state = state;
        ev.wifi.rssi = rssi;
        ev.extra_str = ssid;
        return ev;
    }

    static SystemEvent AudioVolume(int volume, bool muted) {
        SystemEvent ev{};
        ev.type = SystemEventType::kAudioVolume;
        ev.audio_volume.volume = volume;
        ev.audio_volume.muted = muted;
        return ev;
    }

    static SystemEvent AudioPlayback(SystemAudioPlaybackState state, const std::string& owner_id = "") {
        SystemEvent ev{};
        ev.type = SystemEventType::kAudioPlayback;
        ev.audio_playback.state = state;
        ev.extra_str = owner_id;
        return ev;
    }

    static SystemEvent TimeTick(const std::string& time_str) {
        SystemEvent ev{};
        ev.type = SystemEventType::kTimeTick;
        ev.extra_str = time_str;
        return ev;
    }
};

using EventCallback = std::function<void(const SystemEvent&)>;
using SubscriptionToken = uint32_t;

class SystemEventBus {
public:
    static SystemEventBus& GetInstance();

    /**
     * @brief 订阅指定类型的系统事件
     * @param type 事件类型
     * @param cb 回调函数 (注意: 可能会在发布任务线程触发，如需操作 UI 请使用 ScreenLvAsync)
     * @return 订阅令牌 (用于取消订阅)
     */
    SubscriptionToken Subscribe(SystemEventType type, EventCallback cb);

    /**
     * @brief 取消订阅
     * @param token 订阅令牌
     */
    void Unsubscribe(SubscriptionToken token);

    /**
     * @brief 发布系统事件 (线程安全，支持任意 FreeRTOS 任务调用)
     * @param event 系统事件
     */
    void Publish(const SystemEvent& event);

    /**
     * @brief RAII 风格自动生命周期订阅句柄
     */
    class ScopedSubscription {
    public:
        ScopedSubscription() = default;
        ScopedSubscription(SystemEventType type, EventCallback cb) {
            token_ = SystemEventBus::GetInstance().Subscribe(type, std::move(cb));
        }
        ~ScopedSubscription() {
            Reset();
        }

        ScopedSubscription(const ScopedSubscription&) = delete;
        ScopedSubscription& operator=(const ScopedSubscription&) = delete;

        ScopedSubscription(ScopedSubscription&& other) noexcept : token_(other.token_) {
            other.token_ = 0;
        }
        ScopedSubscription& operator=(ScopedSubscription&& other) noexcept {
            if (this != &other) {
                Reset();
                token_ = other.token_;
                other.token_ = 0;
            }
            return *this;
        }

        void Reset() {
            if (token_ != 0) {
                SystemEventBus::GetInstance().Unsubscribe(token_);
                token_ = 0;
            }
        }

    private:
        SubscriptionToken token_ = 0;
    };

private:
    SystemEventBus() = default;
    ~SystemEventBus() = default;

    struct SubscriberRecord {
        SubscriptionToken token;
        SystemEventType type;
        EventCallback callback;
    };

    std::mutex mutex_;
    SubscriptionToken next_token_ = 1;
    std::unordered_map<SubscriptionToken, SubscriberRecord> subscribers_;
    std::unordered_map<SystemEventType, std::vector<SubscriptionToken>> type_index_;
};
