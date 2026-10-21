// main/services/dynamic_apps/dynamic_app_cloud.cc
// 动态应用云服务垫片：语音合成(TTS) + 异步 HTTP GET。
//
// 为什么给动态应用开这两个口子：
//   - TTS：电子墨水屏可以"熄屏只出声"——不烧屏、不消耗刷新电，
//     这是墨水屏相对手机/平板的独占能力（听书场景）。
//     固件里 MiniMax TTS(12 音色/语速/音调/情绪)与 I2S 播放链路本来就有，
//     这里只是把它暴露给动态应用用。
//   - HTTP：动态应用此前完全离线，做不了行情/天气/新闻这类"静态看板"
//     （墨水屏当第二屏的经典用法：一天刷几次即可）。
//
// 线程约定：
//   - dynapp_tts_speak() 立即返回，合成+播放在内部栈任务里做
//     （HTTPS 要用内部 RAM 的 TLS 栈，不能放 PSRAM 栈任务）
//   - dynapp_http_get_async() 立即返回，回调经 ScreenLvAsync 投递到 LVGL 线程，
//     应用回调里可以直接操作界面
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <string>
#include <vector>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "metalio/services.h"

#include "services/ai_chat/ai_chat_service.h"
#include "audio_codec.h"
#include "audio_focus_manager.h"
#include "board.h"
#include "display/screen/screen_common.h"
#include "services/ai_chat/minimax_speech.h"
#include "protocols/api_http.h"
#include "power_policy.h"

static const char* kTag = "DynCloud";

// ================= TTS =================
namespace {

std::mutex s_tts_mtx;
std::string s_tts_pending;
dynapp_tts_cb_t s_tts_cb = nullptr;
void* s_tts_cb_arg = nullptr;
std::atomic<bool> s_tts_stop{false};
TaskHandle_t s_tts_task = nullptr;

// 朗读结果回投 LVGL 线程（应用回调里可以直接改界面）
// pair = {回调, 用户参数, 错误串(堆上)}
struct TtsCbArg {
    dynapp_tts_cb_t cb = nullptr;
    void* user_data = nullptr;
    std::string err;
};

void TtsDeliver(void* arg) {
    std::unique_ptr<TtsCbArg> a(static_cast<TtsCbArg*>(arg));
    if (a->cb != nullptr) a->cb(1, a->err.c_str(), a->user_data);
}

void TtsDeliverFail(void* arg) {
    std::unique_ptr<TtsCbArg> a(static_cast<TtsCbArg*>(arg));
    if (a->cb != nullptr) a->cb(0, a->err.c_str(), a->user_data);
}

void TtsWorker(void* /*arg*/) {
    for (;;) {
        std::string text;
        {
            std::lock_guard<std::mutex> lk(s_tts_mtx);
            if (s_tts_pending.empty()) {
                s_tts_task = nullptr;
                vTaskDelete(nullptr);
                return; // 空闲即退出，别常驻占栈
            }
            text = s_tts_pending;
            s_tts_pending.clear();
        }
        s_tts_stop.store(false);

        const std::string key = AiChat_GetApiKey();
        if (key.empty()) {
            ESP_LOGW(kTag, "tts: 未配置 API Key，跳过");
            if (s_tts_cb != nullptr) {
                auto* a = new TtsCbArg();
                a->cb = s_tts_cb;
                a->user_data = s_tts_cb_arg;
                a->err = "未配置 MiniMax API Key";
                ScreenLvAsync(TtsDeliverFail, a);
            }
            continue;
        }
        std::vector<int16_t> pcm;
        std::string err;
        const auto voice = minimax_speech::GetVoiceConfig();
        // 云端合成要联网：必须显式占网，否则电源策略收网后 socket 直接被拒
        PowerPolicy::GetInstance().Acquire(PowerNeed::OtaDownload);
        // TLS 握手要 14KB 连续内部 RAM（Synthesize 里的预检），本任务自己占的
        // 内部栈也在抢这块；先等系统回收碎片，最多等 3 秒。
        for (int i = 0; i < 30; ++i) {
            if (api::CheckUrlTlsPreflight("https://api.minimax.cn/v1/t2a_v2", 14 * 1024)) break;
            vTaskDelay(pdMS_TO_TICKS(100));
        }
        const bool ok =
            minimax_speech::Synthesize(key, text, voice, pcm, err, nullptr, &s_tts_stop);
        PowerPolicy::GetInstance().Release(PowerNeed::OtaDownload);
        if (!ok) {
            ESP_LOGW(kTag, "tts 合成失败: %s", err.c_str());
            if (s_tts_cb != nullptr) {
                auto* a = new TtsCbArg();
                a->cb = s_tts_cb;
                a->user_data = s_tts_cb_arg;
                a->err = err;
                ScreenLvAsync(TtsDeliverFail, a);
            }
            continue;
        }
        ESP_LOGI(kTag, "tts 合成完成: %u 样本", (unsigned)pcm.size());
        if (pcm.empty()) {
            if (s_tts_cb != nullptr) {
                auto* a = new TtsCbArg();
                a->cb = s_tts_cb;
                a->user_data = s_tts_cb_arg;
                a->err = "合成结果为空";
                ScreenLvAsync(TtsDeliverFail, a);
            }
            continue;
        }
        // 播放：16k 单声道直写 codec（与 AI 对话播报同一条路）
        auto* codec = Board::GetInstance().GetAudioCodec();
        if (codec == nullptr) {
            continue;
        }
        AudioFocusManager::GetInstance().RequestFocus("dynapp_tts", AudioClientType::Exclusive, nullptr);
        PowerPolicy::GetInstance().Acquire(PowerNeed::AiChatTts);
        codec->EnableOutput(true);
        for (size_t i = 0; i < pcm.size(); i += 960) {
            if (s_tts_stop.load()) {
                ESP_LOGI(kTag, "tts 播报被打断");
                break;
            }
            const size_t end = std::min(i + 960, pcm.size());
            std::vector<int16_t> chunk(pcm.begin() + i, pcm.begin() + end);
            codec->OutputData(chunk);
        }
        PowerPolicy::GetInstance().Release(PowerNeed::AiChatTts);
        AudioFocusManager::GetInstance().AbandonFocus("dynapp_tts");
        ESP_LOGI(kTag, "tts 播报结束");
        if (s_tts_cb != nullptr) {
            auto* a = new TtsCbArg();
            a->cb = s_tts_cb;
            a->user_data = s_tts_cb_arg;
            ScreenLvAsync(TtsDeliver, a);
        }
    }
}

} // namespace

int dynapp_tts_speak_ex(const char* text, dynapp_tts_cb_t cb, void* user_data) {
    if (text == nullptr || text[0] == '\0') {
        return 0;
    }
    {
        std::lock_guard<std::mutex> lk(s_tts_mtx);
        s_tts_pending = text; // 新内容覆盖旧的，避免排队堆积
        s_tts_cb = cb;
        s_tts_cb_arg = user_data;
        s_tts_stop.store(false);
    }
    if (s_tts_task == nullptr) {
        // 内部栈 12KB：HTTPS/TLS 需要内部 RAM
        // 内部栈只给 6KB：TLS 握手需要 14KB 连续内部 RAM，任务自己别占太多
        if (xTaskCreate(TtsWorker, "dynapp_tts", 6144, nullptr,
                        tskIDLE_PRIORITY + 3, &s_tts_task) != pdPASS) {
            s_tts_task = nullptr;
            return 0;
        }
    }
    return 1;
}

int dynapp_tts_speak(const char* text) { return dynapp_tts_speak_ex(text, nullptr, nullptr); }

void dynapp_tts_stop(void) {
    s_tts_stop.store(true);
    std::lock_guard<std::mutex> lk(s_tts_mtx);
    s_tts_pending.clear();
}

int dynapp_tts_available(void) {
    return AiChat_GetApiKey().empty() ? 0 : 1;
}

// ================= HTTP =================
namespace {

constexpr int kHttpMaxBody = 16 * 1024;
constexpr int kHttpTimeoutMs = 10000;

struct HttpJob {
    std::string url;
    dynapp_http_resp_cb_t cb = nullptr;
    void* user_data = nullptr;
};

struct HttpResult {
    dynapp_http_resp_cb_t cb = nullptr;
    void* user_data = nullptr;
    char* data = nullptr;
    int len = 0;
    int status = 0;
};

void HttpDeliver(void* arg) {
    std::unique_ptr<HttpResult> r(static_cast<HttpResult*>(arg));
    if (r->cb != nullptr) {
        r->cb(r->data != nullptr ? r->data : "", r->len, r->status, r->user_data);
    }
    free(r->data);
}

void HttpWorker(void* arg) {
    std::unique_ptr<HttpJob> job(static_cast<HttpJob*>(arg));
    auto* res = new HttpResult();
    res->cb = job->cb;
    res->user_data = job->user_data;

    // 占网：否则电源策略收网后 socket 连不上（Connection failed, sock < 0）
    PowerPolicy::GetInstance().Acquire(PowerNeed::OtaDownload);
    // https 握手要 14KB 连续内部 RAM；本任务自己占的内部栈也在抢这块，
    // 内存碎片化时先等系统回收，最多等 3 秒。
    for (int i = 0; i < 30; ++i) {
        if (api::CheckUrlTlsPreflight(job->url, 14 * 1024)) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    // 走项目自己的 HttpFetch（network->CreateHttp）：固件的 HTTPS 证书链在
    // 那条路上（esp_crt_bundle_attach），裸 esp_http_client 没有挂证书包，
    // 握手会报 "No server verification option set in esp_tls_cfg_t"。
    api::HttpFetchOptions opts;
    opts.timeout_ms = kHttpTimeoutMs;
    opts.max_bytes = kHttpMaxBody;
    opts.send_common_headers = false;  // 第三方 URL，不带 Device-Id 等公共头
    opts.tls_min_largest_block = 14 * 1024;
    const api::HttpReply rep = api::HttpGetString(job->url, opts);
    PowerPolicy::GetInstance().Release(PowerNeed::OtaDownload);

    res->status = rep.status;
    if (rep.ok) {
        // 用 malloc：HttpDeliver 里是 free() 释放（不能混 new[]/free）
        res->data = static_cast<char*>(malloc(rep.body.size() + 1));
        if (res->data != nullptr) {
            memcpy(res->data, rep.body.data(), rep.body.size());
            res->data[rep.body.size()] = '\0';
            res->len = static_cast<int>(rep.body.size());
        }
    } else {
        ESP_LOGW(kTag, "http: %s -> %s", job->url.c_str(), rep.error.c_str());
    }

    ScreenLvAsync(HttpDeliver, res);
    vTaskDelete(nullptr);
}

} // namespace

int dynapp_http_get_async(const char* url, dynapp_http_resp_cb_t cb, void* user_data) {
    if (url == nullptr || url[0] == '\0') {
        return 0;
    }
    auto* job = new HttpJob();
    job->url = url;
    job->cb = cb;
    job->user_data = user_data;
    // 内部栈 6KB：TLS 握手需要 14KB 连续内部 RAM，任务自己别占太多
    if (xTaskCreate(HttpWorker, "dynapp_http", 6144, job, tskIDLE_PRIORITY + 3, nullptr) != pdPASS) {
        delete job;
        return 0;
    }
    return 1;
}
