#ifndef _APPLICATION_H_
#define _APPLICATION_H_

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
#include <esp_timer.h>

#include <string>
#include <mutex>
#include <deque>
#include <memory>
#include <atomic>
#include <vector>

#include "ota.h"
#include "audio_service.h"
#include "device_state_event.h"


#define MAIN_EVENT_SCHEDULE (1 << 0)
#define MAIN_EVENT_SEND_AUDIO (1 << 1)
#define MAIN_EVENT_VAD_CHANGE (1 << 3)
#define MAIN_EVENT_ERROR (1 << 4)
#define MAIN_EVENT_CLOCK_TICK (1 << 6)


enum AecMode {
    kAecOff,
    kAecOnDeviceSide,
    kAecOnServerSide,
};

class Application {
public:
    static Application& GetInstance() {
        static Application instance;
        return instance;
    }
    // 删除拷贝构造函数和赋值运算符
    Application(const Application&) = delete;
    Application& operator=(const Application&) = delete;

    void Start();
    void MainEventLoop();
    DeviceState GetDeviceState() const { return device_state_; }
    bool IsVoiceDetected() const { return audio_service_.IsVoiceDetected(); }
    void Schedule(std::function<void()> callback);
    void SetDeviceState(DeviceState state);
    void Alert(const char* status, const char* message, const char* emotion = "", const std::string_view& sound = "");
    void DismissAlert();
    void Reboot();
    bool UpgradeFirmware(Ota& ota, const std::string& url = "");
    bool UpgradeFirmwareUrl(const std::string& url, const std::string& current_version,
                            const std::string& new_version);
    void SetAecMode(AecMode mode);
    AecMode GetAecMode() const { return aec_mode_; }
    void PlaySound(const std::string_view& sound);
    AudioService& GetAudioService() { return audio_service_; }

    // 开机主流程是否已结束（联网/OTA 等）。
    bool IsBootReady() const { return boot_ready_.load(std::memory_order_acquire); }
    void SetBootReady(bool ready) { boot_ready_.store(ready, std::memory_order_release); }
    void StartMainEventLoop();

    // 按需启动音频服务
    void EnsureAudioServiceRunning();
    void StopAudioServiceIfRunning();

private:
    Application();
    ~Application();

    std::mutex mutex_;
    std::deque<std::function<void()>> main_tasks_;
    EventGroupHandle_t event_group_ = nullptr;
    esp_timer_handle_t clock_timer_handle_ = nullptr;
    volatile DeviceState device_state_ = kDeviceStateUnknown;
    AecMode aec_mode_ = kAecOff;
    std::string last_error_message_;
    AudioService audio_service_;

    bool has_server_time_ = false;
    std::string current_version_;
    bool audio_initialized_ = false;
    bool audio_running_ = false;
    std::atomic<bool> boot_ready_{false}; // Start() 跑完后置 true
    int clock_ticks_ = 0;
    TaskHandle_t main_event_loop_task_handle_ = nullptr;
    // Idle 状态栏：允许时钟页面刷 HH:MM；固定文案页改用 GetIdleStatusFixedText。
    void ApplyIdleStatusBar();
};


class TaskPriorityReset {
public:
    TaskPriorityReset(BaseType_t priority) {
        original_priority_ = uxTaskPriorityGet(NULL);
        vTaskPrioritySet(NULL, priority);
    }
    ~TaskPriorityReset() {
        vTaskPrioritySet(NULL, original_priority_);
    }

private:
    BaseType_t original_priority_;
};

#endif // _APPLICATION_H_
