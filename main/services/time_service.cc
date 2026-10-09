// main/services/time_service.cc —— 见 time_service.h(需求一.1 对时/时区/校准)。
#include "time_service.h"

#include <atomic>
#include <ctime>
#include <sys/time.h>

#include <esp_log.h>
#include <esp_netif_sntp.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include "application.h"
#include "board.h"
#include "settings.h"

namespace {

constexpr const char* TAG = "TimeService";
constexpr const char* kNvsNs = "time";
constexpr int kDefaultTzOffsetMin = 480;  // UTC+8(中国)

constexpr EventBits_t kBitNetReady = BIT0;  // 网络就绪,可启动/重启 SNTP
constexpr EventBits_t kBitSyncNow  = BIT1;  // 请求立即对时
constexpr EventBits_t kBitSynced   = BIT2;  // SNTP 刚同步成功,需回写 RTC

std::atomic<int> s_tz_offset_min{kDefaultTzOffsetMin};
std::atomic<bool> s_sntp_started{false};
std::atomic<bool> s_synced{false};
std::atomic<long> s_last_sync_epoch{0};
EventGroupHandle_t s_events = nullptr;
TaskHandle_t s_task = nullptr;

// SNTP 同步回调(lwip 任务上下文):esp_sntp 刚把系统时间设为 UTC。本机模型是
// "本地时刻作为 epoch"(TZ=GMT+0),故把 UTC 加上时区偏移再写回系统。
// 只做 settimeofday + 记账 + 置位;RTC(I2C)回写留给时间服务任务(内部栈)。
void OnSntpSynced(struct timeval* tv) {
    const time_t local_epoch = tv->tv_sec + (time_t)s_tz_offset_min.load() * 60;
    struct timeval ltv = { .tv_sec = local_epoch, .tv_usec = 0 };
    settimeofday(&ltv, nullptr);
    s_last_sync_epoch.store((long)local_epoch);
    s_synced.store(true);
    if (s_events != nullptr) {
        xEventGroupSetBits(s_events, kBitSynced);
    }
    ESP_LOGI(TAG, "SNTP 对时成功,本地时间=%ld", (long)local_epoch);
}

void StartSntp() {
    esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    config.start = false;             // 手动 start,便于 SyncNow 用 deinit/init 强制重同步
    config.sync_cb = OnSntpSynced;
    config.smooth_sync = false;       // 步进对时,一次性纠偏
    config.wait_for_sync = false;
    esp_err_t err = esp_netif_sntp_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "sntp init failed: %s", esp_err_to_name(err));
        return;
    }
    esp_netif_sntp_start();
    s_sntp_started.store(true);
    ESP_LOGI(TAG, "SNTP 已启动(ntp.aliyun.com)");
}

void RestartSntpForSyncNow() {
    // deinit + init + start 强制一次全新同步
    if (s_sntp_started.load()) {
        esp_netif_sntp_deinit();
        s_sntp_started.store(false);
    }
    StartSntp();
}

void TimeTask(void*) {
    bool started = false;
    for (;;) {
        // 等网络就绪或"立即对时"请求
        EventBits_t bits = xEventGroupWaitBits(
            s_events, kBitNetReady | kBitSyncNow | kBitSynced,
            pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
        (void)bits;

        if (!started && Board::GetInstance().IsNetworkReady()) {
            started = true;
            StartSntp();   // 启动后 SNTP 自行周期对时(LWIP_SNTP_UPDATE_DELAY=1h)
        }
        if (bits & kBitSyncNow) {
            if (Board::GetInstance().IsNetworkReady()) {
                RestartSntpForSyncNow();
            }
        }
        if (bits & kBitSynced) {
            // 把刚对好的系统时间写回 RTC 芯片(I2C;板级钩子=系统→RTC→读回)
            Board::GetInstance().OnNetworkTimeSynced();
        }
    }
}

}  // namespace

void TimeService_Init() {
    // main 任务(内部栈)读 NVS。
    {
        Settings settings(kNvsNs, false);
        s_tz_offset_min.store(settings.GetInt("tz_offset_min", kDefaultTzOffsetMin));
    }
    if (s_events == nullptr) {
        s_events = xEventGroupCreate();
    }
    if (s_task == nullptr && s_events != nullptr) {
        // 内部 RAM 栈(对时回调里可能经 RTC 写碰 I2C/后续 flash 相关,不用 PSRAM 栈)
        xTaskCreate(TimeTask, "time_svc", 4096, nullptr, 2, &s_task);
    }
    ESP_LOGI(TAG, "TimeService 初始化, 时区偏移=%d 分钟", s_tz_offset_min.load());
}

void TimeService_SyncNow() {
    if (s_events != nullptr) {
        xEventGroupSetBits(s_events, kBitSyncNow);
    }
}

int TimeService_GetTzOffsetMin() { return s_tz_offset_min.load(); }

void TimeService_SetTzOffsetMin(int minutes) {
    if (minutes < -12 * 60) minutes = -12 * 60;
    if (minutes > 14 * 60) minutes = 14 * 60;
    if (minutes == s_tz_offset_min.load()) return;
    s_tz_offset_min.store(minutes);
    // 落盘到 main 任务(设置页在 LVGL worker,禁直接写 flash)。
    Application::GetInstance().Schedule([minutes]() {
        Settings settings(kNvsNs, true);
        settings.SetInt("tz_offset_min", minutes);
    });
    // 立即重新对时,让显示按新偏移走
    TimeService_SyncNow();
}

bool TimeService_IsSynced() { return s_synced.load(); }
long TimeService_LastSyncEpoch() { return s_last_sync_epoch.load(); }

bool TimeService_SetManual(int year, int month, int day, int hour, int minute) {
    if (year < 2000 || year > 2099 || month < 1 || month > 12 ||
        day < 1 || day > 31 || hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return false;
    }
    struct tm tmv = {};
    tmv.tm_year = year - 1900;
    tmv.tm_mon = month - 1;
    tmv.tm_mday = day;
    tmv.tm_hour = hour;
    tmv.tm_min = minute;
    tmv.tm_sec = 0;
    // TZ=GMT+0:mktime 把"本地字段"直接当 epoch,正好符合本机 local-epoch 模型。
    time_t ts = mktime(&tmv);
    if (ts < 0) return false;
    struct timeval tv = { .tv_sec = ts, .tv_usec = 0 };
    settimeofday(&tv, nullptr);
    // 写回 RTC(走 main 任务,避免在调用方线程做 I2C/flash)。
    Application::GetInstance().Schedule([]() {
        Board::GetInstance().OnNetworkTimeSynced();
    });
    ESP_LOGI(TAG, "手动校准时间=%04d-%02d-%02d %02d:%02d", year, month, day, hour, minute);
    return true;
}

std::string TimeService_NowText() {
    time_t now = time(nullptr);
    struct tm local = {};
    localtime_r(&now, &local);
    if (local.tm_year + 1900 < 2025) {
        return "--";
    }
    char buf[24];
    snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d",
             local.tm_year + 1900, local.tm_mon + 1, local.tm_mday,
             local.tm_hour, local.tm_min);
    return buf;
}
