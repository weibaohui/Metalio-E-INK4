#pragma once
// main/services/radio/radio_player.h —— 网络收音机播放任务。
// 拉流(appfw_stream,ICY 剥离) → MP3 解码(esp_audio_simple_dec) →
// 下混单声道 → 重采样到设备输出 16kHz → 板载 codec OutputData。
// 播放任务独立于 LVGL;界面用 radio_player_snapshot() 读线程安全快照。
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RADIO_TITLE_MAX 96

typedef enum {
    RADIO_STOPPED = 0,
    RADIO_CONNECTING,
    RADIO_PLAYING,
    RADIO_ERROR,
} radio_state_t;

typedef struct {
    radio_state_t state;
    char station[96];   // 当前台名(显示)
    char title[RADIO_TITLE_MAX];  // ICY 正在播放
    char err[96];       // RADIO_ERROR 时的简述
    uint32_t sample_rate;
    int catalog_index;  // 当前台在 radio_catalog 中的下标;-1 = 未知/自定义
} radio_player_snap_t;

// 创建常驻播放任务(幂等)。返回 0 成功。
int radio_player_start(void);

// 切台(任何时刻可调;任务会中止上一轮)。url 必须是 http(s) 流地址。
// catalog_index 供上一台/下一台精确定位;-1 表示不在内置目录里。
void radio_play_indexed(const char* name, const char* url, int catalog_index);

// 兼容旧签名的便捷封装(index=-1)。
void radio_play(const char* name, const char* url);

// 停止收听。
void radio_stop(void);

// 请求收掉播放任务(释放 8K PSRAM 栈):非阻塞置 s_quit,任务循环检测到即自删。
// 离开收音机页时调用,实现"不播放即不占内存"。下次播放由 radio_player_start 幂等
// 重建(会先等旧任务退出)。
void radio_player_shutdown(void);

// 0..100;直接作用于系统输出音量(AudioCodec::SetOutputVolume)。
void radio_set_volume(int percent);

void radio_player_snapshot(radio_player_snap_t* out);

#ifdef __cplusplus
}

// ---- NVS 持久化数据(收藏/上次台)的 RAM 缓存 —— 仅 C++ ----
// 铁律:LVGL worker 栈在 PSRAM(lv_adapter_display.cc stack_in_psram=true),
// NVS 读写都要关 flash cache,在 PSRAM 栈任务上执行即 cache_utils 断言。
// 故 NVS 只允许出现在:(a) 开机时 main 任务加载,(b) 内部 RAM 栈的短命持久化任务。
// 界面/MCP 一律走这里的 RAM 缓存。
#include <string>
#include <vector>

void RadioCacheInit();                                   // 开机 main 任务调一次
std::vector<std::string> RadioGetFavs();
void RadioToggleFav(const std::string& name);            // 有则删,无则加
bool RadioFavAdd(const std::string& name);               // false=已存在
bool RadioFavRemove(const std::string& name);            // false=不存在
void RadioGetLast(std::string& name, std::string& url);  // 上次收听(可能为空)
void RadioSetLast(const std::string& name, const std::string& url);
#endif
