#include "application.h"
#include "board.h"
#include <esp_heap_caps.h>
#include "power_policy.h"
#include "display.h"
#include "system_info.h"
#include "audio_codec.h"
#include "websocket_protocol.h"
#include "assets/lang_config.h"
#include "mcp_server.h"
#include "services/glm_usage/glm_usage_service.h"
#include "services/radio/radio_player.h"
#include "services/calendar/calendar_ics.h"
#include "services/local_mcp/local_mcp_server.h"
#include "services/syslog_sender.h"
#include "services/time_service.h"
#include "services/mem_report/mem_report.h"
#include "assets.h"
#include "app_manager.h"
#include "boot_key_handler.h"
#include "fontpack_lvgl.h"
#include "settings.h"
#include "standby_screen/standby_screen.h"
#include "ota_upgrade_screen/ota_upgrade_screen.h"
#include "ota_confirm_dialog/ota_confirm_dialog.h"
#include "ota_ignore.h"
#include "reader/book_home_snapshot.h"
#include "reader/book_progress_sync.h"
#include "reader/book_library_warmup.h"
#include "api_endpoints.h"

#include <cstring>
#include <ctime>
#include <array>
#include <algorithm>
#include <thread>
#include <esp_log.h>
#include <esp_app_desc.h>
#include <cJSON.h>
#include <driver/gpio.h>
#include <arpa/inet.h>
#include <font_awesome.h>

#define TAG "Application"

namespace {

constexpr int kAssistantNetSliceMs = 200;
constexpr int kAssistantNetBudgetMs = 30000;

}  // namespace

static const char* const STATE_STRINGS[] = {
    "unknown",
    "starting",
    "configuring",
    "idle",
    "connecting",
    "listening",
    "speaking",
    "upgrading",
    "activating",
    "audio_testing",
    "fatal_error",
    "invalid_state"
};

Application::Application() {
    event_group_ = xEventGroupCreate();

#if CONFIG_USE_DEVICE_AEC && CONFIG_USE_SERVER_AEC
#error "CONFIG_USE_DEVICE_AEC and CONFIG_USE_SERVER_AEC cannot be enabled at the same time"
#elif CONFIG_USE_DEVICE_AEC
    aec_mode_ = kAecOnDeviceSide;
#elif CONFIG_USE_SERVER_AEC
    aec_mode_ = kAecOnServerSide;
#else
    aec_mode_ = kAecOff;
#endif

    esp_timer_create_args_t clock_timer_args = {
        .callback = [](void* arg) {
            Application* app = (Application*)arg;
            xEventGroupSetBits(app->event_group_, MAIN_EVENT_CLOCK_TICK);
        },
        .arg = this,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "clock_timer",
        .skip_unhandled_events = true
    };
    esp_timer_create(&clock_timer_args, &clock_timer_handle_);
}

Application::~Application() {
    if (clock_timer_handle_ != nullptr) {
        esp_timer_stop(clock_timer_handle_);
        esp_timer_delete(clock_timer_handle_);
    }
    vEventGroupDelete(event_group_);
}

bool Application::ShouldAllowBootAudio() const {
    switch (device_state_) {
        case kDeviceStateStarting:
        case kDeviceStateWifiConfiguring:
        case kDeviceStateAudioTesting:
        case kDeviceStateActivating:
        case kDeviceStateUpgrading:
            return true;
        default:
            return false;
    }
}

void Application::EnsureAudioServiceRunning() {
    if (!audio_initialized_) {
        auto codec = Board::GetInstance().GetAudioCodec();
        audio_service_.Initialize(codec);

        AudioServiceCallbacks callbacks;
        callbacks.on_send_queue_available = [this]() {
            xEventGroupSetBits(event_group_, MAIN_EVENT_SEND_AUDIO);
        };
        callbacks.on_vad_change = [this](bool speaking) {
            (void)speaking;
            xEventGroupSetBits(event_group_, MAIN_EVENT_VAD_CHANGE);
        };
        audio_service_.SetCallbacks(callbacks);
        audio_initialized_ = true;
    }

    if (!audio_running_) {
        audio_service_.Start();
        if (!audio_service_.IsStarted()) {
            ESP_LOGE(TAG, "Audio service Start failed");
            return;
        }
        audio_running_ = true;
        ESP_LOGI(TAG, "Audio service started");
    }
}

void Application::StopAudioServiceIfRunning() {
    if (!audio_running_) {
        return;
    }
    audio_service_.EnableVoiceProcessing(false);
    audio_service_.Stop();
    audio_running_ = false;
    ESP_LOGI(TAG, "Audio service stopped");
}

void Application::EnsureProtocolReady() {
    if (!api::HasHost()) {
        ESP_LOGI(TAG, "EnsureProtocolReady skipped: blank cloud endpoints");
        protocol_ready_ = false;
        protocol_.reset();
        return;
    }
    if (protocol_ready_ && protocol_) {
        return;
    }

    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(Lang::Strings::LOADING_PROTOCOL);

    protocol_ = std::make_unique<WebsocketProtocol>();

    protocol_->OnConnected([this]() {
        DismissAlert();
    });

    protocol_->OnNetworkError([this](const std::string& message) {
        last_error_message_ = message;
        xEventGroupSetBits(event_group_, MAIN_EVENT_ERROR);
    });
    protocol_->OnIncomingAudio([this](std::unique_ptr<AudioStreamPacket> packet) {
        if (device_state_ == kDeviceStateSpeaking) {
            audio_service_.PushPacketToDecodeQueue(std::move(packet));
        }
    });
    protocol_->OnAudioChannelOpened([this]() {
        auto& board = Board::GetInstance();
        auto codec = board.GetAudioCodec();
        PowerPolicy::GetInstance().Acquire(PowerNeed::AudioSession);
        if (protocol_->server_sample_rate() != codec->output_sample_rate()) {
            ESP_LOGW(TAG, "Server sample rate %d does not match device output sample rate %d, resampling may cause distortion",
                protocol_->server_sample_rate(), codec->output_sample_rate());
        }
    });
    protocol_->OnAudioChannelClosed([this]() {
        PowerPolicy::GetInstance().Release(PowerNeed::AudioSession);
        Schedule([this]() {
            auto display = Board::GetInstance().GetDisplay();
            display->SetChatMessage("system", "");
            SetDeviceState(kDeviceStateIdle);
        });
    });
    protocol_->OnIncomingJson([this](const cJSON* root) {
        auto display = Board::GetInstance().GetDisplay();
        auto type = cJSON_GetObjectItem(root, "type");
        if (strcmp(type->valuestring, "tts") == 0) {
            auto state = cJSON_GetObjectItem(root, "state");
            if (strcmp(state->valuestring, "start") == 0) {
                Schedule([this]() {
                    aborted_ = false;
                    if (device_state_ == kDeviceStateIdle || device_state_ == kDeviceStateListening) {
                        SetDeviceState(kDeviceStateSpeaking);
                    }
                });
            } else if (strcmp(state->valuestring, "stop") == 0) {
                Schedule([this]() {
                    if (device_state_ == kDeviceStateSpeaking) {
                        if (listening_mode_ == kListeningModeManualStop) {
                            SetDeviceState(kDeviceStateIdle);
                        } else {
                            SetDeviceState(kDeviceStateListening);
                        }
                    }
                });
            } else if (strcmp(state->valuestring, "sentence_start") == 0) {
                auto text = cJSON_GetObjectItem(root, "text");
                if (cJSON_IsString(text)) {
                    ESP_LOGI(TAG, "<< %s", text->valuestring);
                    Schedule([this, display, message = std::string(text->valuestring)]() {
                        display->SetChatMessage("assistant", message.c_str());
                    });
                }
            }
        } else if (strcmp(type->valuestring, "stt") == 0) {
            auto text = cJSON_GetObjectItem(root, "text");
            if (cJSON_IsString(text)) {
                ESP_LOGI(TAG, ">> %s", text->valuestring);
                Schedule([this, display, message = std::string(text->valuestring)]() {
                    display->SetChatMessage("user", message.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "llm") == 0) {
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(emotion)) {
                Schedule([this, display, emotion_str = std::string(emotion->valuestring)]() {
                    display->SetEmotion(emotion_str.c_str());
                });
            }
        } else if (strcmp(type->valuestring, "mcp") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            if (cJSON_IsObject(payload)) {
                McpServer::GetInstance().ParseMessage(payload);
            }
        } else if (strcmp(type->valuestring, "system") == 0) {
            auto command = cJSON_GetObjectItem(root, "command");
            if (cJSON_IsString(command)) {
                ESP_LOGI(TAG, "System command: %s", command->valuestring);
                if (strcmp(command->valuestring, "reboot") == 0) {
                    Schedule([this]() {
                        Reboot();
                    });
                } else {
                    ESP_LOGW(TAG, "Unknown system command: %s", command->valuestring);
                }
            }
        } else if (strcmp(type->valuestring, "alert") == 0) {
            auto status = cJSON_GetObjectItem(root, "status");
            auto message = cJSON_GetObjectItem(root, "message");
            auto emotion = cJSON_GetObjectItem(root, "emotion");
            if (cJSON_IsString(status) && cJSON_IsString(message) && cJSON_IsString(emotion)) {
                Alert(status->valuestring, message->valuestring, emotion->valuestring, Lang::Sounds::OGG_VIBRATION);
            } else {
                ESP_LOGW(TAG, "Alert command requires status, message and emotion");
            }
#if CONFIG_RECEIVE_CUSTOM_MESSAGE
        } else if (strcmp(type->valuestring, "custom") == 0) {
            auto payload = cJSON_GetObjectItem(root, "payload");
            ESP_LOGI(TAG, "Received custom message: %s", cJSON_PrintUnformatted(root));
            if (cJSON_IsObject(payload)) {
                Schedule([this, display, payload_str = std::string(cJSON_PrintUnformatted(payload))]() {
                    display->SetChatMessage("system", payload_str.c_str());
                });
            } else {
                ESP_LOGW(TAG, "Invalid custom message format: missing payload");
            }
#endif
        } else {
            ESP_LOGW(TAG, "Unknown message type: %s", type->valuestring);
        }
    });

    bool protocol_started = protocol_->Start();
    protocol_ready_ = true;

    if (protocol_started && !current_version_.empty()) {
        auto display = Board::GetInstance().GetDisplay();
        std::string message = std::string(Lang::Strings::VERSION) + current_version_;
        display->ShowNotification(message.c_str());
        display->SetChatMessage("system", "");
    }
}

void Application::TeardownProtocol() {
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    protocol_ready_ = false;
}

void Application::EnsureAssistantNetworkReadyAsync(std::function<void(bool)> callback) {
    callback(false);
}

void Application::StartAssistantNetworkWorker() {
}

void Application::CompleteAssistantNetworkReady(bool ready, bool cancelled) {
    (void)ready;
    (void)cancelled;
}

void Application::CancelAssistantNetworkWait() {
}

void Application::StartXiaozhiVoice(bool enter_listening) {
    (void)enter_listening;
}

void Application::StartXiaozhiVoiceImpl(bool enter_listening, int network_result) {
    (void)enter_listening;
    (void)network_result;
}

void Application::StopXiaozhiVoice() {
}

void Application::CheckAssetsVersion() {
    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto& assets = Assets::GetInstance();

    if (!assets.partition_valid()) {
        ESP_LOGW(TAG, "Assets partition is disabled for board %s", BOARD_NAME);
        return;
    }
    
    Settings settings("assets", true);
    // Check if there is a new assets need to be downloaded
    std::string download_url = settings.GetString("download_url");

    if (!download_url.empty()) {
        settings.EraseKey("download_url");

        char message[256];
        snprintf(message, sizeof(message), Lang::Strings::FOUND_NEW_ASSETS, download_url.c_str());
        Alert(Lang::Strings::LOADING_ASSETS, message, "cloud_arrow_down", Lang::Sounds::OGG_UPGRADE);
        
        // Wait for the audio service to be idle for 3 seconds
        vTaskDelay(pdMS_TO_TICKS(3000));
        SetDeviceState(kDeviceStateUpgrading);
        display->SetChatMessage("system", Lang::Strings::PLEASE_WAIT);

        bool success = assets.Download(download_url, [display](int progress, size_t speed) -> void {
            std::thread([display, progress, speed]() {
                char buffer[32];
                snprintf(buffer, sizeof(buffer), "%d%% %uKB/s", progress, speed / 1024);
                display->SetChatMessage("system", buffer);
            }).detach();
        });

        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!success) {
            Alert(Lang::Strings::ERROR, Lang::Strings::DOWNLOAD_ASSETS_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
            vTaskDelay(pdMS_TO_TICKS(2000));
            return;
        }
    }

    // Apply assets
    assets.Apply();
    display->SetChatMessage("system", "");
    display->SetEmotion("microchip_ai");
}

void Application::CheckNewVersion(Ota& ota) {
    const int MAX_RETRY = 10;
    int retry_count = 0;
    int retry_delay = 10; // 初始重试延迟为10秒

    activation_code_.clear();
    activation_message_.clear();

    auto& board = Board::GetInstance();
    // 开机未连上时不空等激活重试；下次 EnsureNetworkReady 后再走 OTA/激活
    if (!board.IsNetworkReady()) {
        ESP_LOGW(TAG, "skip CheckNewVersion: network not ready");
        xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
        return;
    }
    // Cloudzao 空白开源版必须无条件跳过 OTA / 激活检测；即使旧 NVS 里残留 ota_url 也不能继续请求。
    if (!api::HasDefaultOtaUrl()) {
        ESP_LOGW(TAG, "skip CheckNewVersion: Cloudzao blank endpoints");
        xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
        return;
    }
    if (ota.GetCheckVersionUrl().length() < 10) {
        ESP_LOGW(TAG, "skip CheckNewVersion: OTA URL not configured");
        xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
        return;
    }
    while (true) {
        SetDeviceState(kDeviceStateActivating);
        auto display = board.GetDisplay();
        display->SetStatus(Lang::Strings::CHECKING_NEW_VERSION);

        esp_err_t err = ota.CheckVersion();
        if (err != ESP_OK) {
            retry_count++;
            if (retry_count >= MAX_RETRY) {
                ESP_LOGE(TAG, "Too many retries, exit version check");
                return;
            }

            char error_message[128];
            snprintf(error_message, sizeof(error_message), "code=%d, url=%s", err, ota.GetCheckVersionUrl().c_str());
            char buffer[256];
            snprintf(buffer, sizeof(buffer), Lang::Strings::CHECK_NEW_VERSION_FAILED, retry_delay, error_message);
            Alert(Lang::Strings::ERROR, buffer, "cloud_slash", Lang::Sounds::OGG_EXCLAMATION);

            ESP_LOGW(TAG, "Check new version failed, retry in %d seconds (%d/%d)", retry_delay, retry_count, MAX_RETRY);
            for (int i = 0; i < retry_delay; i++) {
                if (device_state_ == kDeviceStateIdle) {
                    break;
                }
                if (DelayMsInterruptible(1000)) {
                    break;
                }
            }
            retry_delay *= 2; // 每次重试后延迟时间翻倍
            continue;
        }
        retry_count = 0;
        retry_delay = 10; // 重置重试延迟时间

        if (ota.HasNewVersion()) {
            const std::string& new_ver = ota.GetFirmwareVersion();
            if (OtaIgnore::IsIgnored(new_ver)) {
                ESP_LOGI(TAG, "firmware %s ignored in NVS; skip prompt", new_ver.c_str());
            } else {
                const auto choice = OtaConfirmDialog::ShowBlocking(
                    ota.GetCurrentVersion().c_str(), new_ver.c_str(), OtaConfirmDialog::Mode::Boot);
                if (choice == OtaConfirmDialog::Choice::Upgrade) {
                    if (UpgradeFirmware(ota)) {
                        return;  // reboot
                    }
                    // upgrade failed → fall through existing boot path
                } else if (choice == OtaConfirmDialog::Choice::IgnorePersist) {
                    OtaIgnore::Set(new_ver);
                }
                // RemindLater / ignore-after-set / upgrade-fail: same fall through as no new version
            }
        }

        // No new version, mark the current version as valid
        ota.MarkCurrentVersionValid();
        if (!ota.HasActivationCode() && !ota.HasActivationChallenge()) {
            activation_code_.clear();
            activation_message_.clear();
            xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
            // Exit the loop if done checking new version
            break;
        }

        display->SetStatus(Lang::Strings::ACTIVATION);
        // Activation code is shown to the user and waiting for the user to input
        if (ota.HasActivationCode()) {
            ShowActivationCode(ota.GetActivationCode(), ota.GetActivationMessage());
        }

        // This will block the loop until the activation is done or timeout
        for (int i = 0; i < 10; ++i) {
            ESP_LOGI(TAG, "Activating... %d/%d", i + 1, 10);
            esp_err_t err = ota.Activate();
            if (err == ESP_OK) {
                activation_code_.clear();
                activation_message_.clear();
                xEventGroupSetBits(event_group_, MAIN_EVENT_CHECK_NEW_VERSION_DONE);
                break;
            }
            if (device_state_ == kDeviceStateIdle) {
                break;
            }
            const int delay_ms = (err == ESP_ERR_TIMEOUT) ? 3000 : 10000;
            if (DelayMsInterruptible(delay_ms)) {
                continue;
            }
            if (device_state_ == kDeviceStateIdle) {
                break;
            }
        }
    }
}

void Application::ShowActivationCode(const std::string& code, const std::string& message) {
    activation_code_ = code;
    activation_message_ = message;
    // 只在顶部状态栏显示验证码，不语音播报；日志里不打印 host 文本，避免任何云域名残留。
    char status[64];
    std::snprintf(status, sizeof(status), Lang::Strings::ACTIVATION_CODE_FMT, code.c_str());

    ESP_LOGW(TAG, "Activation [%s]", status);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
}

bool Application::DelayMsInterruptible(int ms) {
    constexpr int kStepMs = 100;
    for (int elapsed = 0; elapsed < ms; elapsed += kStepMs) {
        if (device_state_ == kDeviceStateIdle) {
            return false;
        }
        if (activation_kick_.exchange(false, std::memory_order_acq_rel)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(kStepMs));
    }
    if (device_state_ == kDeviceStateIdle) {
        return false;
    }
    return activation_kick_.exchange(false, std::memory_order_acq_rel);
}

void Application::ResumeActivationAfterStandby() {
    Schedule([this]() {
        if (device_state_ != kDeviceStateActivating) {
            return;
        }
        auto display = Board::GetInstance().GetDisplay();
        display->SetStatus(activation_code_.empty() ? Lang::Strings::CHECKING_NEW_VERSION
                                                    : Lang::Strings::ACTIVATION);
        display->SetEmotion("neutral");
        if (!activation_code_.empty()) {
            ShowActivationCode(activation_code_, activation_message_);
        }
        activation_kick_.store(true, std::memory_order_release);
        ESP_LOGI(TAG, "resume activation after standby LP");
    });
}

void Application::Alert(const char* status, const char* message, const char* emotion, const std::string_view& sound) {
    ESP_LOGW(TAG, "Alert [%s] %s: %s", emotion, status, message);
    auto display = Board::GetInstance().GetDisplay();
    display->SetStatus(status);
    display->SetEmotion(emotion);
    display->SetChatMessage("system", message);
    if (!sound.empty()) {
        PlaySound(sound);
    }
}

void Application::ApplyIdleStatusBar() {
    auto display = Board::GetInstance().GetDisplay();
    // 固定文案页：不刷时钟。百问页已声明 kClock，会话 Idle 也走时钟，勿显示「待命」
    if (!display->AllowsIdleStatusClock()) {
        const char* text = display->GetIdleStatusFixedText();
        display->SetStatus((text != nullptr && text[0] != '\0') ? text : Lang::Strings::STANDBY);
        return;
    }

    // 未绑定：Idle 顶栏保持验证码，勿被时钟盖住（进百问会改 state，退回首页也立刻恢复）
    if (!activation_code_.empty()) {
        ShowActivationCode(activation_code_, activation_message_);
        return;
    }

    // 首页 / 百问 Idle：直接显示时钟
    time_t now = time(nullptr);
    struct tm tm_info = {};
    if (localtime_r(&now, &tm_info) != nullptr && tm_info.tm_year >= (2025 - 1900)) {
        char time_str[16];
        strftime(time_str, sizeof(time_str), "%H:%M", &tm_info);
        display->SetStatus(time_str);
    } else {
        display->SetStatus("--:--");
    }
}

void Application::DismissAlert() {
    if (device_state_ == kDeviceStateIdle) {
        auto display = Board::GetInstance().GetDisplay();
        ApplyIdleStatusBar();
        display->SetEmotion("neutral");
        display->SetChatMessage("system", "");
    }
}

void Application::StartListening() {
    if (device_state_ == kDeviceStateActivating) {
        SetDeviceState(kDeviceStateIdle);
        return;
    } else if (device_state_ == kDeviceStateWifiConfiguring) {
        // 原工程配网态长按 BOOT 会进本地录音环回；产品不要此行为
        ESP_LOGI(TAG, "StartListening ignored in wifi configuring (no audio testing)");
        return;
    }

    if (!boot_ready_.load(std::memory_order_acquire)) {
        pending_xiaozhi_voice_.store(true, std::memory_order_release);
        pending_xiaozhi_listen_.store(true, std::memory_order_release);
        ESP_LOGI(TAG, "StartListening deferred until boot ready");
        Board::GetInstance().GetDisplay()->SetStatus(Lang::Strings::PLEASE_WAIT);
        return;
    }

    if (!xiaozhi_voice_active_ || !protocol_) {
        ESP_LOGW(TAG, "StartListening ignored: Xiaozhi voice session not active");
        return;
    }
    const uint32_t generation = voice_session_generation_;
    Schedule([this, generation]() { StartListeningImpl(generation); });
}

void Application::StartListeningImpl(uint32_t session_generation, int network_result) {
    (void)session_generation;
    (void)network_result;
}

void Application::StopListening() {
    if (device_state_ == kDeviceStateAudioTesting) {
        audio_service_.EnableAudioTesting(false);
        SetDeviceState(kDeviceStateWifiConfiguring);
        return;
    }

    const std::array<int, 3> valid_states = {
        kDeviceStateListening,
        kDeviceStateSpeaking,
        kDeviceStateIdle,
    };
    // If not valid, do nothing
    if (std::find(valid_states.begin(), valid_states.end(), device_state_) == valid_states.end()) {
        return;
    }

    Schedule([this]() {
        if (device_state_ == kDeviceStateListening && protocol_) {
            protocol_->SendStopListening();
            SetDeviceState(kDeviceStateIdle);
        }
    });
}

void Application::AbortSpeakingToIdle() {
    // 仅百问会话 + Speaking：短按打断 → 待命。不 StartListening，避免闪「聆听中」。
    if (!xiaozhi_voice_active_ || device_state_ != kDeviceStateSpeaking) {
        return;
    }

    Schedule([this]() {
        if (!xiaozhi_voice_active_ || device_state_ != kDeviceStateSpeaking) {
            return;
        }
        AbortSpeaking(kAbortReasonNone);
        if (audio_running_) {
            audio_service_.ResetDecoder();
        }
        SetDeviceState(kDeviceStateIdle);
    });
}

void Application::FlushPendingXiaozhiVoice() {
}

void Application::Start() {
    // §2.8 boot 表驱动:开机流程拆 BootStep 表,每步打耗时日志——以后排查
    // "开机慢/某步崩"直接看日志表,不用顺着 150 行顺序调用猜。步骤顺序即
    // 原实现顺序;条件与注释原样保留在各步内。
    auto& board = Board::GetInstance();
    ESP_LOGI(TAG, "board initialization begin");
    board.Initialize();
    ESP_LOGI(TAG, "board initialization complete");
    auto display = board.GetDisplay();

    struct BootStep {
        const char* name;
        std::function<void()> fn;
    };
    const BootStep kSteps[] = {
        {"lang",
         [&] {
             // UI 语言：NVS ui/language，无则 LANG_DEFAULT_CODE（跟 CONFIG_LANGUAGE_*）
             Lang::InitFromNvs();
         }},
        {"device_starting",
         [&] {
             SetDeviceState(kDeviceStateStarting);
         }},
        {"fontpack",
         [&] {
             // fontpack mmap 须在内部 DRAM 栈任务上完成；开机预热 UI 25@2，避免各页首绘空字体
             fontpack_lv_ensure_ready();
             (void)fontpack_lv_font_ui();
         }},
        {"a2ui_cache",
         [&] {
         }},
        {"chat_store",
         [&] {
         }},
        {"chat_message",
         [&] {
             // Print board name/version info
             display->SetChatMessage("system", SystemInfo::GetUserAgent().c_str());
         }},
        {"main_loop",
         [&] {
             // 开机不启动音频 / 唤醒词 / 协议；主事件循环仍负责 Schedule 与状态栏
             // 栈须内部 DRAM：Schedule 路径含 NVS/flash（关 cache），SPIRAM 栈会 assert
             xTaskCreate([](void* arg) {
                 ((Application*)arg)->MainEventLoop();
                 vTaskDelete(NULL);
             }, "main_event_loop", 2048 * 4, this, 3, &main_event_loop_task_handle_);
             /* Start the clock timer to update the status bar */
             esp_timer_start_periodic(clock_timer_handle_, 1000000);
         }},
        {"reader_warmup",
         [&] {
             // 书库预热须在 StartNetwork 之前（仅创建后台任务）。
             reader::book_home_snapshot::HydrateFromNvs();
             reader::book_library_warmup::RequestBootWarmup();
         }},
        {"app_toggles",
         [&] {
             // APP 开关(需求一.3):开机把 NVS 开关载入 RAM 缓存(main 任务,内部栈)。
             // 必须早于:1) 下方 MCP 工具注册的开关门控;2) 首页首次渲染。
             AppManager::GetInstance().Init();
             // GLM 用量服务的 Init(NVS 读 + MCP 工具注册)必须在 StartNetwork 之前完成:
             // StartNetwork 会阻塞数十秒(慢路由连网),期间用户已可进入 GLM 用量屏;
             // 若 Init 迟到,屏幕侧 Start() 因 !inited_ 空手而归 → 无 worker 不刷新,
             // 卡"尚未查询"(2026-10-09 实测:22s 进屏报 start-before-init,125s Init 才跑)。
             // Init 幂等。应用开关关掉 GLM 用量时不初始化:不注册 MCP,也不载入状态。
             if (AppManager::GetInstance().IsAppEnabled("glm")) {
                 GlmUsageService::GetInstance().Init();
             }
             if (AppManager::GetInstance().IsAppEnabled("radio")) {
                 RadioPlayer::GetInstance().Init();
             }
         }},
        {"network",
         [&] {
             /* 有已存 WiFi：等保网时长；失败不 SoftAP。无 SSID 跳过联网，到设置→网络机上配网 */
             board.StartNetwork();
             // Update the status bar immediately to show the network state
             display->UpdateStatusBar(true);
         }},
        {"ota_url",
         [&] {
             // 清理 NVS 中残留的 ota_url 与激活状态
             Settings settings("wifi", true);
             settings.EraseKey("ota_url");
             activation_code_.clear();
             activation_message_.clear();
             ESP_LOGI(TAG, "cleared stale ota_url and activation state");
         }},
        {"ota_check",
         [&] {
             // 禁用开机 OTA 检查与激活轮询
             current_version_ = esp_app_get_description()->version;
             ESP_LOGI(TAG, "skip ota_check & activation (disabled)");
         }},
        {"post_net_jobs",
         [&] {
             // 阅读快照：显示初始化 / StartNetwork 前已灌；此处幂等补灌
             reader::book_home_snapshot::HydrateFromNvs();
             // 禁用天气拉取、云端待办同步、阅读进度上报与 WiFi 位置上报
             ESP_LOGI(TAG, "skip weather, checklist, book sync, and wifi-loc (disabled)");
         }},
        {"mcp_services",
         [&] {
             // MCP 工具可提前注册；真正连协议在进入 AI 聊天页时
             auto& mcp_server = McpServer::GetInstance();
             mcp_server.AddCommonTools();
             mcp_server.AddUserOnlyTools();
             // MCP 注册跟随应用开关,表驱动(关掉的 APP 连 MCP 工具都不注册,重启后生效)。
             // syslog/MCP 自身是系统服务非 APP,常注册;GLM 的 Init(含 MCP 注册)
             // 已按开关在 StartNetwork 前完成(见上)。
             AppManager::GetInstance().RegisterEnabledMcpTools();
             SyslogRegisterMcpTools();   // 需求一.2:syslog 的 MCP 配置接口(self.syslog.*)
             MemReport_RegisterMcpTools();  // 每任务内存占用查询(self.mem.tasks)
             LocalMcpServer::GetInstance().Start();
             // syslog 日志上报(需求一.2):读 NVS 配置,已启用且配了 IP 则启动发送任务。
             Syslog_Init();
             // 网络对时服务(SNTP,需求一.1):读时区偏移 + 起后台任务,网络就绪后自动对时。
             // Init 在 main 任务(内部栈)读 NVS,安全。
             TimeService_Init();
             // ICS 日历订阅:读 NVS 订阅 URL/最近同步时间(main 任务,内部栈,安全)。
             // 不联网下载——首次进日历屏时按过期策略自动同步。
             CalendarIcs_Init();
             // APP 开关(需求一.3):NVS→RAM 缓存已随显示初始化(SetupUI 首次渲染首页前)
             // 载入,开机第一屏就按开关过滤图标;此处幂等重载兜底,防启动路径变动回归。
             AppManager::GetInstance().Init();
         }},
        {"boot_ready",
         [&] {
             boot_ready_.store(true, std::memory_order_release);
             StopAudioServiceIfRunning();
             SetDeviceState(kDeviceStateIdle);
             ESP_LOGI(TAG, "Boot complete");
         }},
    };

    const int64_t boot_t0 = esp_timer_get_time();
    for (const auto& step : kSteps) {
        const int64_t t0 = esp_timer_get_time();
        step.fn();
        const int64_t cost_ms = (esp_timer_get_time() - t0) / 1000;
        // 本机 newlib 是 nano 格式,%lld 不支持,用 %lu
        ESP_LOGI(TAG, "boot[%s] %lu ms", step.name, static_cast<unsigned long>(cost_ms));
        if (cost_ms >= 1000) {
            // 慢步单独立 W 级,开机慢时一眼定位
            ESP_LOGW(TAG, "boot[%s] slow: %lu ms", step.name, static_cast<unsigned long>(cost_ms));
        }
    }
    ESP_LOGI(TAG, "boot total %lu ms",
             static_cast<unsigned long>((esp_timer_get_time() - boot_t0) / 1000));
    // 内存全景快照:各区域总量/最小水位一屏打尽,内存调优的对照基线
    heap_caps_print_heap_info(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    heap_caps_print_heap_info(MALLOC_CAP_SPIRAM);
    // 每任务占用:回答"400 多 KB 都谁用了";运行期随时用 self.mem.tasks 再查
    MemReport_PerTask(true);
}

// Add a async task to MainLoop
void Application::Schedule(std::function<void()> callback) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        main_tasks_.push_back(std::move(callback));
    }
    xEventGroupSetBits(event_group_, MAIN_EVENT_SCHEDULE);
}

// The Main Event Loop controls the chat state and websocket connection
// If other tasks need to access the websocket or chat state,
// they should use Schedule to call this function
void Application::MainEventLoop() {
    while (true) {
        auto bits = xEventGroupWaitBits(event_group_, MAIN_EVENT_SCHEDULE |
            MAIN_EVENT_SEND_AUDIO |
            MAIN_EVENT_VAD_CHANGE |
            MAIN_EVENT_CLOCK_TICK |
            MAIN_EVENT_ERROR, pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & MAIN_EVENT_ERROR) {
            SetDeviceState(kDeviceStateIdle);
            // 状态用错误文案本身，勿套「错误」前缀
            Alert(last_error_message_.c_str(), last_error_message_.c_str(), "cloud_slash",
                  Lang::Sounds::OGG_EXCLAMATION);
        }

        if (bits & MAIN_EVENT_SEND_AUDIO) {
            while (auto packet = audio_service_.PopPacketFromSendQueue()) {
                if (protocol_ && !protocol_->SendAudio(std::move(packet))) {
                    break;
                }
            }
        }

        if (bits & MAIN_EVENT_VAD_CHANGE) {
            if (device_state_ == kDeviceStateListening) {
                auto led = Board::GetInstance().GetLed();
                led->OnStateChanged();
            }
        }

        if (bits & MAIN_EVENT_SCHEDULE) {
            std::unique_lock<std::mutex> lock(mutex_);
            auto tasks = std::move(main_tasks_);
            lock.unlock();
            for (auto& task : tasks) {
                task();
            }
        }

        if (bits & MAIN_EVENT_CLOCK_TICK) {
            clock_ticks_++;
            auto display = Board::GetInstance().GetDisplay();
            display->UpdateStatusBar();
        
            // Print the debug info every 10 seconds
            if (clock_ticks_ % 10 == 0) {
                // SystemInfo::PrintTaskCpuUsage(pdMS_TO_TICKS(1000));
                // SystemInfo::PrintTaskList();
                // SystemInfo::PrintHeapStats();
            }
        }
    }
}

void Application::AbortSpeaking(AbortReason reason) {
    ESP_LOGI(TAG, "Abort speaking");
    aborted_ = true;
    if (protocol_) {
        protocol_->SendAbortSpeaking(reason);
    }
}

void Application::SetListeningMode(ListeningMode mode) {
    listening_mode_ = mode;
    SetDeviceState(kDeviceStateListening);
}

void Application::SetDeviceState(DeviceState state) {
    if (device_state_ == state) {
        return;
    }
    
    clock_ticks_ = 0;
    auto previous_state = device_state_;
    device_state_ = state;
    ESP_LOGI(TAG, "STATE: %s", STATE_STRINGS[device_state_]);

    // Send the state change event
    DeviceStateEventManager::GetInstance().PostStateChangeEvent(previous_state, state);

    auto& board = Board::GetInstance();
    auto display = board.GetDisplay();
    auto led = board.GetLed();
    led->OnStateChanged();
    switch (state) {
        case kDeviceStateUnknown:
        case kDeviceStateIdle:
            ApplyIdleStatusBar();
            display->SetEmotion("neutral");
            // 屏级会话：Idle 回落只停语音处理
            if (xiaozhi_voice_active_ && audio_running_) {
                audio_service_.EnableVoiceProcessing(false);
            }
            break;
        case kDeviceStateConnecting:
            display->SetStatus(Lang::Strings::CONNECTING);
            display->SetEmotion("neutral");
            display->SetChatMessage("system", "");
            break;
        case kDeviceStateListening:
            display->SetStatus(Lang::Strings::LISTENING);
            display->SetEmotion("neutral");

            // Make sure the audio processor is running
            if (xiaozhi_voice_active_ && audio_running_ && protocol_ &&
                !audio_service_.IsAudioProcessorRunning()) {
                // Send the start listening command
                protocol_->SendStartListening(listening_mode_);
                audio_service_.EnableVoiceProcessing(true);
            }
            break;
        case kDeviceStateSpeaking:
            display->SetStatus(Lang::Strings::SPEAKING);

            if (xiaozhi_voice_active_ && audio_running_) {
                if (listening_mode_ != kListeningModeRealtime) {
                    audio_service_.EnableVoiceProcessing(false);
                }
                audio_service_.ResetDecoder();
            }
            break;
        default:
            // Do nothing
            break;
    }

    PowerPolicy::GetInstance().NotifyDeviceState(state);
}

void Application::Reboot() {
    ESP_LOGI(TAG, "Rebooting...");
    // Disconnect the audio channel
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        protocol_->CloseAudioChannel();
    }
    protocol_.reset();
    protocol_ready_ = false;
    xiaozhi_voice_active_ = false;
    StopAudioServiceIfRunning();

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

bool Application::UpgradeFirmware(Ota& ota, const std::string& url) {
    // Use provided URL or get from OTA object
    std::string upgrade_url = url.empty() ? ota.GetFirmwareUrl() : url;
    std::string current_version = ota.GetCurrentVersion();
    if (current_version.empty()) {
        const esp_app_desc_t* app_desc = esp_app_get_description();
        current_version = (app_desc != nullptr && app_desc->version[0] != '\0') ? app_desc->version : "—";
    }
    std::string new_version = url.empty() ? ota.GetFirmwareVersion() : Lang::Strings::OTA_MANUAL;
    if (new_version.empty()) {
        new_version = "—";
    }
    return UpgradeFirmwareUrl(upgrade_url, current_version, new_version);
}

bool Application::UpgradeFirmwareUrl(const std::string& url, const std::string& current_version,
                                     const std::string& new_version) {
    std::string upgrade_url = url;
    std::string cur = current_version.empty() ? "—" : current_version;
    std::string next = new_version.empty() ? "—" : new_version;

    // Close audio channel if it's open
    if (protocol_ && protocol_->IsAudioChannelOpened()) {
        ESP_LOGI(TAG, "Closing audio channel before firmware upgrade");
        protocol_->CloseAudioChannel();
    }
    ESP_LOGD(TAG, "Starting firmware upgrade from URL: %s (cur=%s new=%s)", upgrade_url.c_str(),
             cur.c_str(), next.c_str());

    SetDeviceState(kDeviceStateUpgrading);

    // 全屏升级页：锁键 + 版本/进度；须在非 LVGL 线程调用
    if (!OtaUpgradeScreen::Show(cur.c_str(), next.c_str())) {
        ESP_LOGW(TAG, "OTA upgrade UI show incomplete; continuing with keys locked");
    }
    OtaUpgradeScreen::SetProgress(0);

    // 顶栏文案 + 提示音；进度改由全屏页展示（墨水屏 SetChatMessage 在非百问页无效）
    Alert(Lang::Strings::OTA_UPGRADE, Lang::Strings::UPGRADING, "download", Lang::Sounds::OGG_UPGRADE);
    vTaskDelay(pdMS_TO_TICKS(800));  // 给墨水首帧局刷时间

    PowerPolicy::GetInstance().Acquire(PowerNeed::OtaDownload);
    StopAudioServiceIfRunning();
    vTaskDelay(pdMS_TO_TICKS(500));

    if (!Board::GetInstance().EnsureNetworkReady()) {
        PowerPolicy::GetInstance().Release(PowerNeed::OtaDownload);
        ESP_LOGE(TAG, "Firmware upgrade aborted: network not ready");
        OtaUpgradeScreen::Dismiss();
        if (xiaozhi_voice_active_) {
            EnsureAudioServiceRunning();
        }
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    }

    Ota ota;
    bool upgrade_success = ota.StartUpgradeFromUrl(upgrade_url, [](int progress, size_t /*speed*/) {
        OtaUpgradeScreen::SetProgress(progress);
    });

    PowerPolicy::GetInstance().Release(PowerNeed::OtaDownload);

    if (!upgrade_success) {
        ESP_LOGE(TAG, "Firmware upgrade failed, continuing operation...");
        OtaUpgradeScreen::Dismiss();
        if (xiaozhi_voice_active_) {
            EnsureAudioServiceRunning();
        }
        Alert(Lang::Strings::ERROR, Lang::Strings::UPGRADE_FAILED, "circle_xmark", Lang::Sounds::OGG_EXCLAMATION);
        vTaskDelay(pdMS_TO_TICKS(3000));
        return false;
    }

    OtaIgnore::Clear();
    ESP_LOGI(TAG, "Firmware upgrade successful, rebooting...");
    OtaUpgradeScreen::SetProgress(100);
    OtaUpgradeScreen::SetHint(Lang::Strings::OTA_SUCCESS_REBOOT);
    vTaskDelay(pdMS_TO_TICKS(1200));  // 让 100% 有机会上屏
    // 成功路径直接重启，无需 Dismiss（复位后自然清空）
    Reboot();
    return true;
}

void Application::SendMcpMessage(const std::string& payload) {
    // protocol_ 可能在排队期间被 TeardownProtocol() 拆掉（离开助手页）。
    // 真正发送时必须再判空，避免 LoadProhibited。
    if (xTaskGetCurrentTaskHandle() == main_event_loop_task_handle_) {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
        return;
    }

    Schedule([this, payload]() {
        if (protocol_) {
            protocol_->SendMcpMessage(payload);
        }
    });
}

void Application::SetAecMode(AecMode mode) {
    aec_mode_ = mode;
    Schedule([this]() {
        if (!xiaozhi_voice_active_ || !audio_running_) {
            return;
        }
        auto& board = Board::GetInstance();
        auto display = board.GetDisplay();
        switch (aec_mode_) {
        case kAecOff:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_OFF);
            break;
        case kAecOnServerSide:
            audio_service_.EnableDeviceAec(false);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        case kAecOnDeviceSide:
            audio_service_.EnableDeviceAec(true);
            display->ShowNotification(Lang::Strings::RTC_MODE_ON);
            break;
        }

        // If the AEC mode is changed, close the audio channel
        if (protocol_ && protocol_->IsAudioChannelOpened()) {
            protocol_->CloseAudioChannel();
        }
    });
}

void Application::PlaySound(const std::string_view& sound) {
    if (sound.empty()) {
        return;
    }
    // AI 聊天会话内，或开机 OTA/配网阶段允许拉起音频播提示音
    if (!audio_running_) {
        if (xiaozhi_voice_active_ || ShouldAllowBootAudio()) {
            EnsureAudioServiceRunning();
        } else {
            ESP_LOGD(TAG, "PlaySound skipped: audio not running outside Xiaozhi session");
            return;
        }
    }
    audio_service_.PlaySound(sound);
}
