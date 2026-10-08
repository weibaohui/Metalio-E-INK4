// main/services/radio/radio_player.cc —— 见 radio_player.h。
// 播放主循环移植自 aipassport-radio(真机踩坑版),按本机差异改造:
//   - 传输层用 framework 的 appfw_stream(ICY 剥离/重定向/HLS/http 分身回退);
//   - 板载 codec 输出速率固定 16kHz 单声道(AudioCodec::OutputData),不同电台
//     解码出的 44.1k/48k 立体声在下混后重采样,而不是像 C3 版那样重配 I2S;
//   - S3 带 Octal PSRAM,不需要解码器预留块/环桶戏法,普通缓冲即可。
#include "radio_player.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include <vector>

#include <esp_heap_caps.h>
#include <esp_timer.h>
#include <esp_log.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>

#include "appfw_stream.h"
#include "application.h"
#include "audio_codec.h"
#include "boards/common/power_policy/power_policy.h"
#include "board.h"
#include "radio_catalog.h"
#include "settings.h"

extern "C" {
#include "decoder/esp_audio_dec_default.h"
#include "simple_dec/esp_audio_simple_dec.h"
#include "simple_dec/esp_audio_simple_dec_default.h"
}

static const char* TAG = "RadioPlayer";

namespace {

constexpr int kOutputRate = 16000;   // 与板级 AUDIO_OUTPUT_SAMPLE_RATE 一致
constexpr int kFeedBytes = 4096;     // 每轮从流里读的音频字节数
constexpr int kMaxBadFrames = 64;    // 连续解码失败上限
constexpr uint32_t kMonoCap = 8192;  // PCM 解码缓冲(一帧最多 4608)

struct PlayReq {
    std::string name;
    std::string url;
    uint64_t seq = 0;      // 请求序号:任务只执行最新一次
    bool stop = false;     // true=只停不播
    int catalog_index = -1;
};

std::mutex s_req_mutex;
// pending:待执行请求(被任务取走后清空)。gen 单调递增,用于判断"我这一轮
// 是否已被更新的请求取代"。不用布尔标志,避免快连切台时把新请求吞掉。
PlayReq s_pending;
bool s_pending_valid = false;
uint64_t s_gen = 0;           // 仅在 s_req_mutex 下自增
std::atomic<bool> s_quit{false};
std::atomic<bool> s_audio_ensured{false};   // 音频服务(含 i2s 使能)只初始化一次
// 首次播放的初始化同步信号:EnsureAudioServiceRunning 被投递到 main 任务执行
std::atomic<bool> s_audio_boot_done{false};

// ---- NVS 持久化数据(收藏/上次收听)的 RAM 缓存 ----
// NVS 读写会关 flash cache;LVGL worker(PSRAM 栈)上做即断言崩机。
// 读:开机 main 任务一次性载入;写:内部 RAM 栈的短命任务落地。
constexpr const char* kNvsNs = "radio";
std::mutex s_cache_mutex;
std::vector<std::string> s_favs_cache;
std::string s_last_name;
std::string s_last_url;

void PersistFavsTask(void* arg) {
    auto* raw = static_cast<std::string*>(arg);
    Settings settings(kNvsNs, true);
    settings.SetString("favs", *raw);
    delete raw;
    vTaskDelete(nullptr);
}

void PersistLastTask(void* arg) {
    auto* pair = static_cast<std::pair<std::string, std::string>*>(arg);
    Settings settings(kNvsNs, true);
    settings.SetString("last_name", pair->first);
    settings.SetString("last_url", pair->second);
    delete pair;
    vTaskDelete(nullptr);
}

void SpawnPersistTask(void (*fn)(void*), const char* name, void* payload) {
    // 4096 内部栈,必成功;失败则丢持久化(RAM 缓存仍生效),不崩机
    if (xTaskCreate(fn, name, 4096, payload, 4, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "persist task %s alloc failed", name);
    }
}

void PersistFavsLocked() {
    // 调用时须持 s_cache_mutex;raw 拷贝后立刻放锁
    std::string raw;
    for (size_t i = 0; i < s_favs_cache.size(); ++i) {
        if (i > 0) raw += '\n';
        raw += s_favs_cache[i];
    }
    SpawnPersistTask(PersistFavsTask, "radio_favs", new std::string(raw));
}
// 播放期间向电源策略硬占网(Acquire 可重入,这里用标志防重复计数)
bool s_net_held = false;

void NetHold(bool hold) {
    if (hold == s_net_held) return;
    s_net_held = hold;
    if (hold) {
        PowerPolicy::GetInstance().Acquire(PowerNeed::RadioPlayback);
    } else {
        PowerPolicy::GetInstance().Release(PowerNeed::RadioPlayback);
    }
}
TaskHandle_t s_task = nullptr;
bool s_decoders_registered = false;

// 快照跨三个任务读写(收音任务写 / LVGL 读 / MCP 读),必须用可挂起的互斥量:
// portENTER_CRITICAL 是关中断的自旋锁,任一侧在临界区里被抢占即双核死锁;
// 且临界区里不得调 libc(snprintf 会碰 flash cache)。格式化一律在锁外做。
std::mutex s_snap_mutex;
radio_player_snap_t s_snap = {};

void SetSnapState(radio_state_t st, const char* err) {
    char err_buf[sizeof(s_snap.err)] = {};
    if (err != nullptr) {
        snprintf(err_buf, sizeof(err_buf), "%s", err);
    }
    std::lock_guard<std::mutex> lock(s_snap_mutex);
    s_snap.state = st;
    if (err != nullptr) {
        memcpy(s_snap.err, err_buf, sizeof(s_snap.err));
    } else if (st != RADIO_ERROR) {
        s_snap.err[0] = '\0';
    }
}

// 统一在锁内按字段写入。调用方传文本,不传 s_snap 内部指针 —— 传内部指针会
// 在持锁期间再写同一结构,既易错又让"谁在保护什么"说不清。
void SetSnapStation(const char* text) {
    char buf[sizeof(s_snap.station)] = {};
    snprintf(buf, sizeof(buf), "%s", text != nullptr ? text : "");
    std::lock_guard<std::mutex> lock(s_snap_mutex);
    memcpy(s_snap.station, buf, sizeof(s_snap.station));
}

void SetSnapTitle(const char* text) {
    char buf[sizeof(s_snap.title)] = {};
    snprintf(buf, sizeof(buf), "%s", text != nullptr ? text : "");
    std::lock_guard<std::mutex> lock(s_snap_mutex);
    memcpy(s_snap.title, buf, sizeof(s_snap.title));
}

// ---- 重采样:任意输入率 → 16k 单声道。----
// 结构:先做 win=round(ratio) 点的滑动平均压高频(轻量抗混叠),再线性插值。
// 小喇叭+语音/广播内容下够用;追求音质可换 polyphase FIR。
class MonoResampler {
public:
    void Configure(int in_rate) {
        if (in_rate <= 0) in_rate = kOutputRate;
        in_rate_ = in_rate;
        step_ = (double)in_rate / kOutputRate;
        win_ = (int)(step_ + 0.5);
        if (win_ < 1) win_ = 1;
        if (win_ > 8) win_ = 8;
        pos_ = 0.0;
        hist_valid_ = 0;
    }
    int in_rate() const { return in_rate_; }

    // in: 单声道 int16;输出追加到 out。
    void Process(const int16_t* in, size_t n, std::vector<int16_t>& out) {
        if (in_rate_ == kOutputRate) {
            out.insert(out.end(), in, in + n);
            return;
        }
        // 带 1 点历史(与上一块的最后一个样本拼合),块边界不丢相位。
        std::vector<int16_t> buf(hist_valid_ + n);
        if (hist_valid_ > 0) buf[0] = hist_;
        memcpy(buf.data() + hist_valid_, in, n * sizeof(int16_t));
        const size_t m = buf.size();
        if (m < 2) {
            hist_ = buf[0];
            hist_valid_ = 1;
            return;
        }

        // 滑动平均(前向,窗口 win_):原地。
        if (win_ > 1) {
            int64_t acc = 0;
            size_t lo = 0;
            for (size_t i = 0; i < m; ++i) {
                acc += buf[i];
                if ((int)(i - lo + 1) > win_) {
                    acc -= buf[lo++];
                }
                buf[i] = (int16_t)(acc / (int64_t)(i - lo + 1));
            }
        }

        // 线性插值下采样。pos_ 是上一块遗留的相位(输入样本坐标)。
        while (pos_ < (double)(m - 1)) {
            const size_t i0 = (size_t)pos_;
            const double f = pos_ - (double)i0;
            const int32_t a = buf[i0];
            const int32_t b = buf[i0 + 1];
            out.push_back((int16_t)(a + (int32_t)((b - a) * f)));
            pos_ += step_;
        }
        pos_ -= (double)m;   // 折回本块坐标
        if (pos_ < -1.0) pos_ = 0.0;  // 防御:极端比率下不发散
        hist_ = buf[m - 1];
        hist_valid_ = 1;
    }

private:
    int in_rate_ = kOutputRate;
    double step_ = 1.0;
    double pos_ = 0.0;
    int win_ = 1;
    int16_t hist_ = 0;
    int hist_valid_ = 0;
};

// 把 16k 单声道 PCM 送板载 codec。
// 输出暂存:内部 RAM + TTS 同尺寸分片。
// 实测二分结论:用 PSRAM 里的 16KB 大缓冲整块 i2s_channel_write 时,设备会在
// 开播 ~10s 后网络栈死掉(WiFi 仍关联、ping/TCP 全断、无 panic)。百问 TTS 在
// 同一块板上长期稳定,差别只在:TTS 用内部 RAM 的小块(~960 样本=60ms)喂 codec。
// 这里按 TTS 的方式分片输出。
constexpr size_t kStageSamples = 960;   // 60ms @16k,与 OPUS_FRAME_DURATION_MS 对齐
std::vector<int16_t> s_stage;           // 内部 RAM(小容量不会进 PSRAM)

void OutputPcm(std::vector<int16_t>& pcm) {
    if (pcm.empty()) return;
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) {
        pcm.clear();
        return;
    }
    // 关键:必须让 AudioService 知道输出还在用。否则它的电源状态机会在
    // AUDIO_POWER_TIMEOUT_MS(15s)后 EnableOutput(false),而此刻本任务正阻塞
    // 在 i2s_channel_write(portMAX_DELAY) 上持着 codec 锁,写永不返回 ->
    // 锁永久占用 -> LVGL 刷新拿不到锁 -> 界面与 MCP 一起无响应。
    Application::GetInstance().GetAudioService().KeepOutputAlive();
    size_t off = 0;
    while (off < pcm.size()) {
        const size_t n = std::min(kStageSamples, pcm.size() - off);
        s_stage.assign(pcm.begin() + off, pcm.begin() + off + n);
        codec->OutputData(s_stage);
        off += n;
    }
    pcm.clear();
}

// 喂一段 MP3 给解码器;解出的 PCM 经下混+重采样进 codec。返回 0 继续其它错误码。
int Mp3Feed(esp_audio_simple_dec_handle_t dec, uint8_t* pcm_buf, uint32_t pcm_cap,
            MonoResampler& resam, std::vector<int16_t>& out16k,
            const uint8_t* data, uint32_t len, uint32_t& rate_out, int& ch_out) {
    esp_audio_simple_dec_raw_t raw = {
        .buffer = const_cast<uint8_t*>(data),
        .len = len,
        .eos = false,
        .consumed = 0,
    };
    int bad = 0;
    while (raw.len > 0) {
        esp_audio_simple_dec_out_t out = {
            .buffer = pcm_buf, .len = pcm_cap,
            .needed_size = 0, .decoded_size = 0,
        };
        const esp_audio_err_t dr = esp_audio_simple_dec_process(dec, &raw, &out);
        if (dr == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            return 1;  // 本机 pcm_cap 8192 足够一帧;尺寸异常时放弃这块
        }
        if (dr != ESP_AUDIO_ERR_OK) {
            if (++bad > kMaxBadFrames) return 2;
            if (raw.consumed == 0) break;  // 保证有进展
        } else {
            bad = 0;
        }
        if (out.decoded_size > 0) {
            esp_audio_simple_dec_info_t info = {};
            if (esp_audio_simple_dec_get_info(dec, &info) == ESP_AUDIO_ERR_OK &&
                info.sample_rate > 0) {
                if (info.sample_rate != rate_out || (int)info.channel != ch_out) {
                    rate_out = info.sample_rate;
                    ch_out = (int)info.channel;
                    resam.Configure(rate_out);
                    {
                        std::lock_guard<std::mutex> lock(s_snap_mutex);
                        s_snap.sample_rate = rate_out;
                    }
                    ESP_LOGI(TAG, "流采样率 %u Hz ch%u", (unsigned)rate_out, ch_out);
                }
            }
            if (rate_out > 0) {
                // 立体声原地下混单声道
                size_t mono_n = out.decoded_size / 2;
                const int16_t* in = (const int16_t*)out.buffer;
                if (ch_out >= 2) {
                    const size_t frames = out.decoded_size / 4;
                    int16_t* mono = (int16_t*)out.buffer;
                    for (size_t i = 0; i < frames; ++i) {
                        mono[i] = (int16_t)(((int32_t)in[2 * i] + in[2 * i + 1]) / 2);
                    }
                    mono_n = frames;
                }
                resam.Process((const int16_t*)out.buffer, mono_n, out16k);
                if (out16k.size() >= 1024) {
                    OutputPcm(out16k);
                }
            }
        }
        if (raw.consumed == 0) break;
        raw.buffer += raw.consumed;
        raw.len -= raw.consumed;
    }
    return 0;
}

void StreamTitleCb(void* user, const char* title) {
    (void)user;
    SetSnapTitle(title);
}

// 一轮完整收听;返回前 s_quit 或换台请求生效。
// 返回 true=本轮因新请求被取代(不应重试);false=流自己断了(调用方可重试)。
bool RunOneStream(const PlayReq& req, uint64_t seq) {
    char* playlist = nullptr;
    uint8_t* pcm = nullptr;
    uint8_t* feed = nullptr;
    esp_audio_simple_dec_handle_t dec = nullptr;
    appfw_stream_t* stream = nullptr;
    std::vector<int16_t> out16k;
    uint32_t rate = 0;
    int ch = 0;
    MonoResampler resam;
    resam.Configure(kOutputRate);

    do {
        if (s_quit.load()) return true;   // 退出中:调用方不再重试
        esp_audio_simple_dec_cfg_t dec_cfg = {
            .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
            .dec_cfg = nullptr,
            .cfg_size = 0,
            .use_frame_dec = false,   // 流式:内部自己拼跨块半帧
        };
        if (esp_audio_simple_dec_open(&dec_cfg, &dec) != ESP_AUDIO_ERR_OK) {
            SetSnapState(RADIO_ERROR, "解码器初始化失败");
            return false;   // 允许走自动重连(内存紧张时重开常能成功)
        }
        // 内部 RAM 已近枯竭(实测历史最小 9KB)：解码/流缓冲一律放 PSRAM
        pcm = (uint8_t*)heap_caps_malloc(kMonoCap, MALLOC_CAP_SPIRAM);
        feed = (uint8_t*)heap_caps_malloc(kFeedBytes, MALLOC_CAP_SPIRAM);
        playlist = (char*)heap_caps_malloc(2048, MALLOC_CAP_SPIRAM);
        if (pcm == nullptr || feed == nullptr || playlist == nullptr) {
            SetSnapState(RADIO_ERROR, "内存不足");
            break;
        }

        SetSnapState(RADIO_CONNECTING, nullptr);
        // 已知同时提供 http 明文源的台系,优先走明文:
        // 本机内部 RAM 实测仅剩 ~30KB、最大连续块 ~10KB,而一次 TLS 握手要
        // ~40KB 内部连续内存。https 流首次尚能连上,切台时再开一条就会把
        // lwIP/WiFi 栈一起饿死(症状:切台后网络 100% 丢包、MCP 无响应,而
        // CPU 仍在跑)。仅对已实测确认有明文源的域名降级,其余保持 https。
        std::string play_url = req.url;
        if (play_url.rfind("https://", 0) == 0) {
            static const char* kHttpTwinHosts[] = {
                "lhttp.qtfm.cn", "lhttp-hw.qtfm.cn", "lhttp.qingting.fm",
            };
            const std::string host_part = play_url.substr(8);
            for (const char* h : kHttpTwinHosts) {
                const size_t hn = strlen(h);
                if (host_part.compare(0, hn, h) == 0 &&
                    (host_part.size() == hn || host_part[hn] == '/' ||
                     host_part[hn] == ':')) {
                    play_url = "http://" + host_part;
                    break;
                }
            }
        }
        const appfw_stream_cfg_t scfg = {
            .url = play_url.c_str(),
            .playlist = playlist,
            .playlist_cap = 2048,
            .on_title = StreamTitleCb,
            .user = nullptr,
        };
        const appfw_stream_err_t oerr = appfw_stream_open(&stream, &scfg);
        if (oerr != APPFW_STREAM_ERR_NONE) {
            ESP_LOGW(TAG, "建连失败 err=%d 内部剩余=%u 最大块=%u", (int)oerr,
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            SetSnapState(RADIO_ERROR, oerr == APPFW_STREAM_ERR_URL ? "地址无效"
                                     : oerr == APPFW_STREAM_ERR_HTTP ? "服务端拒绝"
                                                                     : "连接失败");
            break;
        }
        SetSnapState(RADIO_PLAYING, nullptr);
        ESP_LOGI(TAG, "开播: %s (%s)", req.name.c_str(), appfw_stream_url(stream));

        int err_streak = 0;
        uint64_t total_bytes = 0;
        int64_t last_flow_log = 0;
        while (!s_quit.load()) {
            {   // 已被更新的请求取代就中止本轮
                std::lock_guard<std::mutex> lock(s_req_mutex);
                if (s_pending_valid || s_gen != seq) {
                    break;
                }
            }
            const int n = appfw_stream_read(stream, feed, kFeedBytes);
            if (n < 0) {
                ESP_LOGW(TAG, "流结束/故障(已收 %uKB)", (unsigned)(total_bytes/1024));
                break;
            }
            if (n == 0) {  // 暂时无数据(HLS 换段/分身重连),再读即可
                ESP_LOGW(TAG, "读空转 err_streak=%d(已收 %uKB)",
                         err_streak + 1, (unsigned)(total_bytes/1024));
                if (++err_streak > 32) break;
                // 让出 CPU:既给 WiFi/lwIP 任务留时间,也保证换台检查能按时回到
                vTaskDelay(pdMS_TO_TICKS(120));
                continue;
            }
            err_streak = 0;
            total_bytes += n;
            if (esp_timer_get_time() - last_flow_log >= 10 * 1000000LL) {
                last_flow_log = esp_timer_get_time();
                ESP_LOGI(TAG, "收流正常 累计 %uKB", (unsigned)(total_bytes/1024));
            }
            if (Mp3Feed(dec, pcm, kMonoCap, resam, out16k, feed, (uint32_t)n, rate,
                        ch) != 0) {
                SetSnapState(RADIO_ERROR, "解码失败");
                break;
            }
        }
    } while (false);

    if (stream != nullptr) appfw_stream_close(stream);
    if (dec != nullptr) esp_audio_simple_dec_close(dec);
    free(pcm);
    free(feed);
    free(playlist);
    OutputPcm(out16k);   // 尾部不足 1024 的残余也放完
    out16k.clear();

    // CDN 掐断连接是常态(qtfm 系实测播十几秒后掉):只要不是被新请求取代,
    // 调用方就应自动重试同一台。
    {
        std::lock_guard<std::mutex> lock(s_req_mutex);
        if (s_pending_valid || s_gen != seq) {
            return true;
        }
    }
    return false;
}

void RadioTask(void*) {
    int retry = 0;   // 当前台连续自动重连次数
    while (!s_quit.load()) {
        PlayReq req;
        bool have = false;
        {
            std::lock_guard<std::mutex> lock(s_req_mutex);
            if (s_pending_valid) {
                req = s_pending;
                s_pending_valid = false;   // 取走即消费,不会被重复执行
                have = true;
            }
        }
        if (!have) {
            retry = 0;
            NetHold(false);   // 空闲:释放保网需求,让电源策略正常进待机省电
            SetSnapState(RADIO_STOPPED, nullptr);
            vTaskDelay(pdMS_TO_TICKS(150));
            continue;
        }
        if (req.stop) {
            NetHold(false);   // 停止:释放保网
            SetSnapState(RADIO_STOPPED, nullptr);
            {
                std::lock_guard<std::mutex> lock(s_snap_mutex);
                s_snap.catalog_index = -1;
            }
            continue;
        }
        NetHold(true);        // 播放:硬占网。否则无交互 ~90s 后电源策略进待机
                              // 并停掉 WiFi(state: run -> init),直播流随之断掉
        // 收音机只需 i2s 输出,不需要整条语音管线(唤醒词/AFE/opus/采麦任务)。
        // 走"只开输出"路径:停掉可能正跑的语音管线(释放 AFE+opus+任务,但不动
        // i2s —— AudioService::Stop 不调 i2s_channel_disable),再幂等 codec->Start()
        // 保证 i2s TX 使能即可。不再调 EnsureAudioServiceRunning(那会把整条管线
        // 拉起来白白占内存,且与 TLS 抢内部 RAM)。
        // NVS/flash 操作必须走 main 任务内部栈(PSRAM 栈上做即 cache 断言崩机)。
        if (!s_audio_ensured.load()) {
            ESP_LOGI(TAG, "使能 i2s 输出(首次播放,投递到 main 任务)...");
            s_audio_boot_done.store(false);
            Application::GetInstance().Schedule([]() {
                // 若语音管线在跑(用户刚用过语音页),先停掉释放内存。
                // StopAudioServiceIfRunning 内部 Deinitialize AFE 也在 main 栈执行,flash 安全。
                Application::GetInstance().StopAudioServiceIfRunning();
                // 幂等使能 i2s(AudioCodec::Start 已加 started_ 守卫,二次调不 abort)。
                Board::GetInstance().GetAudioCodec()->Start();
                // WiFi 退出省电:PS 模式突发收包被压到几 KB/s,直播流必断。
                // esp_wifi_* 会读 flash 的 PHY 数据,必须在 main 任务(内部栈)执行;
                // 设置一次全局生效,无需每次开流重复。
                esp_wifi_set_ps(WIFI_PS_NONE);
                s_audio_boot_done.store(true);
            });
            int waited = 0;
            while (!s_audio_boot_done.load() && waited < 20000) {
                vTaskDelay(pdMS_TO_TICKS(100));
                waited += 100;
            }
            if (!s_audio_boot_done.load()) {
                ESP_LOGW(TAG, "i2s 使能未完成,本轮跳过");
                SetSnapState(RADIO_ERROR, "音频初始化超时");
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }
            s_audio_ensured.store(true);
            ESP_LOGI(TAG, "i2s 输出就绪");
        }
        // 台名/下标已在 radio_play_indexed() 里同步发布,这里不再覆盖:
        // 否则任务处理"旧"请求时会把界面刚显示的新台名改回旧的。
        bool superseded = RunOneStream(req, req.seq);
        if (!superseded && !s_quit.load() && retry < 3) {
            // CDN 掐断自动重连(与 aipassport-radio 同策略:断流短重连)
            ++retry;
            ESP_LOGW(TAG, "流断开,2s 后自动重连(%d/3): %s", retry, req.name.c_str());
            vTaskDelay(pdMS_TO_TICKS(2000));
            {
                std::lock_guard<std::mutex> lock(s_req_mutex);
                // 2s 等待期间用户可能已切台(s_pending_valid=true)。若无条件覆盖,
                // 新请求被吞、又跳回已断流的旧台 —— 表现为"切台没反应"。
                // 仅在没有更新请求时才重投旧台。
                if (!s_pending_valid) {
                    s_pending = req;          // 同代际重投
                    s_pending_valid = true;
                }
            }
            continue;
        }
        retry = 0;
        if (!s_quit.load()) {
            // 非换台失败给个短暂停,避免错误流死循环打日志
            bool errored;
            {
                std::lock_guard<std::mutex> lock(s_snap_mutex);
                errored = (s_snap.state == RADIO_ERROR);
            }
            if (errored) vTaskDelay(pdMS_TO_TICKS(600));
        }
    }
    s_task = nullptr;
    // 任务由 xTaskCreateWithCaps(PSRAM 栈)创建,必须用 vTaskDeleteWithCaps 配对释放,
    // 否则 PSRAM 栈释放路径错误。(此前 s_quit 恒为 false、此分支不可达,是潜伏死代码;
    // 现 shutdown 使其可达,故须配对。)
    vTaskDeleteWithCaps(nullptr);
}

}  // namespace

extern "C" int radio_player_start(void) {
    // 若上一个任务正被 shutdown(s_quit=true)但尚未自删,先等它退出再重建。
    // 否则会"看到旧任务还在就 return 0、结果旧任务随即因 s_quit 退出、新播放
    // 请求没人执行"。不持 s_req_mutex 等待 —— 任务循环每轮都短暂持锁自检,
    // 持锁等会死锁。
    if (s_task != nullptr && s_quit.load()) {
        ESP_LOGI(TAG, "start: 上一个收音任务正在退出,等待其收尾...");
        int waited = 0;
        while (s_task != nullptr && waited < 3000) {
            vTaskDelay(pdMS_TO_TICKS(50));
            waited += 50;
        }
        if (s_task != nullptr) {
            ESP_LOGE(TAG, "start: 旧任务 3s 未退出(可能卡在 i2s 写),放弃本次重建");
            return -1;
        }
        s_quit.store(false);
    }
    if (s_task != nullptr) return 0;   // 正常在跑:幂等,新请求会经 s_pending 被它拾取
    // 解码器注册是全局一次性动作，放在任务创建前(启动路径)完成
    if (!s_decoders_registered) {
        if (esp_audio_dec_register_default() != ESP_AUDIO_ERR_OK ||
            esp_audio_simple_dec_register_default() != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "解码器注册失败");
            return -1;
        }
        s_decoders_registered = true;
    }
    s_quit.store(false);
    // 栈放 PSRAM：本机内部 RAM 余量太小，8KB 内部栈会直接压垮其它任务
    if (xTaskCreateWithCaps(RadioTask, "radio", 8192, nullptr, 5, &s_task,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        s_task = nullptr;
        return -1;
    }
    return 0;
}

// 请求由其它任务发起时调用:标记打断 + 关闭当前流,使阻塞中的读立即返回。
// 只做标记不关句柄也可以,但等 2s 读超时用户能感知到"没反应",故直接关。
extern "C" void radio_play_indexed(const char* name, const char* url, int catalog_index) {
    if (name == nullptr || url == nullptr) return;
    radio_player_start();
    // 请求一落即发布"目标台":收音任务可能还阻塞在上一条流的读上(最长 10s),
    // 若等它轮到才更新,界面与 MCP 会显示上一台 —— 表现为切台"没反应"。
    SetSnapStation(name);
    SetSnapTitle("");
    {
        std::lock_guard<std::mutex> lock(s_snap_mutex);
        s_snap.catalog_index = catalog_index;
        s_snap.state = RADIO_CONNECTING;
        s_snap.err[0] = '\0';
    }
    std::lock_guard<std::mutex> lock(s_req_mutex);
    s_pending.name = name;
    s_pending.url = url;
    s_pending.catalog_index = catalog_index;
    s_pending.seq = ++s_gen;
    s_pending.stop = false;
    s_pending_valid = true;
}

extern "C" void radio_play(const char* name, const char* url) {
    radio_play_indexed(name, url, -1);
}

extern "C" void radio_stop(void) {
    std::lock_guard<std::mutex> lock(s_req_mutex);
    // 始终发一个"停"请求:代际前进可中止正在执行的流,也保证自己会被执行
    s_pending.stop = true;
    s_pending.seq = ++s_gen;
    s_pending_valid = true;
}

extern "C" void radio_player_shutdown(void) {
    // 非阻塞:只置退出标志(可在 LVGL worker 上直调)。任务的读循环与外层循环
    // 检测到 s_quit 即自行退出并 vTaskDelete 自删,释放 8K PSRAM 栈。
    // 最坏等待 ≈ 重连 vTaskDelay(2s) 或读超时 1s;期间任务仍活着(s_task!=nullptr)。
    // 同步置位(而非异步调度)很关键:这样 unload 后立刻置 s_quit,紧随其后的播放
    // 请求在 radio_player_start 里会看到 dying 任务并等其退出后重建 —— 不会丢请求。
    // s_quit 不复位:留在 true 也无妨,radio_player_start 重建时会复位(并先等旧任务死)。
    if (s_task != nullptr) {
        s_quit.store(true);
        ESP_LOGI(TAG, "shutdown: 已置退出标志,收音任务将自行退出并释放 8K 栈");
    }
}

extern "C" void radio_set_volume(int percent) {
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec != nullptr) {
        codec->SetOutputVolume(percent);
    }
}

extern "C" void radio_player_snapshot(radio_player_snap_t* out) {
    if (out == nullptr) return;
    std::lock_guard<std::mutex> lock(s_snap_mutex);
    *out = s_snap;
}

// ==================== RAM 缓存公开 API(见 radio_player.h 铁律注释) ====================

void RadioCacheInit() {
    Settings settings("radio", false);   // main 任务,内部栈,flash 安全
    const std::string raw = settings.GetString("favs", "");
    std::vector<std::string> favs;
    size_t start = 0;
    while (start < raw.size()) {
        size_t end = raw.find('\n', start);
        if (end == std::string::npos) end = raw.size();
        if (end > start) favs.push_back(raw.substr(start, end - start));
        start = end + 1;
    }
    const std::string ln = settings.GetString("last_name", "");
    const std::string lu = settings.GetString("last_url", "");
    std::lock_guard<std::mutex> lock(s_cache_mutex);
    s_favs_cache = std::move(favs);
    s_last_name = ln;
    s_last_url = lu;
    ESP_LOGI(TAG, "缓存载入: 收藏 %d 台, 上次收听 %s",
             static_cast<int>(s_favs_cache.size()), s_last_name.c_str());
}

std::vector<std::string> RadioGetFavs() {
    std::lock_guard<std::mutex> lock(s_cache_mutex);
    return s_favs_cache;
}

void RadioToggleFav(const std::string& name) {
    std::lock_guard<std::mutex> lock(s_cache_mutex);
    auto it = std::find(s_favs_cache.begin(), s_favs_cache.end(), name);
    if (it != s_favs_cache.end()) {
        s_favs_cache.erase(it);
    } else {
        s_favs_cache.push_back(name);
    }
    PersistFavsLocked();
}

bool RadioFavAdd(const std::string& name) {
    std::lock_guard<std::mutex> lock(s_cache_mutex);
    if (std::find(s_favs_cache.begin(), s_favs_cache.end(), name) != s_favs_cache.end()) {
        return false;
    }
    s_favs_cache.push_back(name);
    PersistFavsLocked();
    return true;
}

bool RadioFavRemove(const std::string& name) {
    std::lock_guard<std::mutex> lock(s_cache_mutex);
    auto it = std::find(s_favs_cache.begin(), s_favs_cache.end(), name);
    if (it == s_favs_cache.end()) return false;
    s_favs_cache.erase(it);
    PersistFavsLocked();
    return true;
}

void RadioGetLast(std::string& name, std::string& url) {
    std::lock_guard<std::mutex> lock(s_cache_mutex);
    name = s_last_name;
    url = s_last_url;
}

void RadioSetLast(const std::string& name, const std::string& url) {
    {
        std::lock_guard<std::mutex> lock(s_cache_mutex);
        if (s_last_name == name && s_last_url == url) return;
        s_last_name = name;
        s_last_url = url;
    }
    SpawnPersistTask(PersistLastTask, "radio_last",
                     new std::pair<std::string, std::string>(name, url));
}
