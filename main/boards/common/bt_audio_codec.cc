#include "bt_audio_codec.h"

#include <esp_err.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <cmath>
#include <cstring>
#include "display/screen/bluetooth_screen/bluetooth_screen.h"

#define TAG "BTAudioCodec"

namespace {

// 软限幅(需求三.7 削爆音):逼近满幅时渐近压缩而非硬削顶。硬削顶(对超 INT32 的部分
// 直接 clamp)在小喇叭上是平顶失真/爆音的主因。80% 满幅以下线性(不动),之上平滑
// 压缩到 INT32_MAX。纯整数、无分配、有界耗时 —— Write 在持锁的音频路径上,必须如此。
int32_t SoftClip(int64_t x) {
    constexpr int64_t kFs = INT32_MAX;
    constexpr int64_t kKnee = kFs * 8 / 10;   // 80% 拐点
    if (x <= kKnee && x >= -kKnee) {
        return static_cast<int32_t>(x);
    }
    const int64_t sign = x < 0 ? -1 : 1;
    const int64_t a = x < 0 ? -x : x;
    const int64_t range = kFs - kKnee;         // 压缩区宽度
    const int64_t over = a - kKnee;            // 超出拐点的量
    // y = knee + range*over/(range+over):over→∞ 时渐近到 range(即 y→FS),平滑不削顶
    const int64_t y = kKnee + (range * over) / (range + over);
    return static_cast<int32_t>(sign * y);
}

}  // namespace

BTAudioCodec::~BTAudioCodec()
{
    if (rx_handle_ != nullptr)
    {
        ESP_ERROR_CHECK(i2s_channel_disable(rx_handle_));
    }
    if (tx_handle_ != nullptr)
    {
        ESP_ERROR_CHECK(i2s_channel_disable(tx_handle_));
    }
}


BTAudioCodecDuplex::BTAudioCodecDuplex(int input_sample_rate, int output_sample_rate, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din)
{
    duplex_ = true;
    input_sample_rate_ = input_sample_rate;
    output_sample_rate_ = output_sample_rate;

    input_reference_ = true;                    // 是否使用参考输入，实现回声消除
    input_channels_ = input_reference_ ? 2 : 1; // 输入通道数

    i2s_chan_config_t chan_cfg = {
        .id = I2S_NUM_0,
        .role = I2S_ROLE_SLAVE,
        .dma_desc_num = AUDIO_CODEC_DMA_DESC_NUM,
        .dma_frame_num = AUDIO_CODEC_DMA_FRAME_NUM,
        .auto_clear_after_cb = true,
        .auto_clear_before_cb = false,
        .intr_priority = 0,
    };
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle_, &rx_handle_));

    i2s_std_config_t std_cfg = {
        .clk_cfg = {
            .sample_rate_hz = (uint32_t)output_sample_rate_,
            .clk_src = I2S_CLK_SRC_DEFAULT,
            .mclk_multiple = I2S_MCLK_MULTIPLE_256,
#ifdef I2S_HW_VERSION_2
            .ext_clk_freq_hz = 0,
#endif

        },
        .slot_cfg = {.data_bit_width = I2S_DATA_BIT_WIDTH_32BIT, .slot_bit_width = I2S_SLOT_BIT_WIDTH_AUTO, .slot_mode = I2S_SLOT_MODE_STEREO, .slot_mask = I2S_STD_SLOT_BOTH, .ws_width = I2S_DATA_BIT_WIDTH_32BIT, .ws_pol = false, .bit_shift = true,
#ifdef I2S_HW_VERSION_2
                     .left_align = true,
                     .big_endian = false,
                     .bit_order_lsb = false
#endif

        },
        .gpio_cfg = {.mclk = I2S_GPIO_UNUSED, .bclk = bclk, .ws = ws, .dout = dout, .din = din, .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false}}};
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle_, &std_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle_, &std_cfg));
    ESP_LOGI(TAG, "Duplex channels created");
}

int BTAudioCodec::Write(const int16_t *data, int samples)
{
    std::lock_guard<std::mutex> lock(data_if_mutex_);
    // 双声道：每个采样点需要输出左右两个声道的数据。
    // 复用成员缓冲(容量只增不减),杜绝每次调用 8KB 内部堆的分配/释放抖动。
    if (static_cast<int>(tx_buffer_.size()) < samples * 2) {
        tx_buffer_.resize(samples * 2);
    }
    std::vector<int32_t>& buffer = tx_buffer_;

    // output_volume_: 0-100
    // volume_factor_: 0-65536
    int32_t volume_factor = pow(double(output_volume_) / 100.0, 2) * 65536;

    for (int i = 0; i < samples; i++)
    {
        int64_t temp = int64_t(data[i]) * volume_factor; // 使用 int64_t 进行乘法运算
        // 软限幅取代硬 clamp:逼近满幅平滑压缩,抑制削顶爆音(需求三.7)。
        int32_t processed_sample = SoftClip(temp);

        // 将处理后的采样点复制到左右两个声道（交错排列）
        buffer[i * 2] = processed_sample;     // 左声道
        buffer[i * 2 + 1] = processed_sample; // 右声道（复制相同的数据）
    }

    size_t bytes_written = 0;
    // 用有限超时而非 portMAX_DELAY:DMA 停摆时永久阻塞会一直占住 data_if_mutex_,
    // 导致 LVGL/MCP 拿不到锁整机假死;去掉 ESP_ERROR_CHECK:任何非 OK 返回(如
    // 通道被 disable 的 ESP_ERR_INVALID_STATE)不再 abort 整机。出错记日志并丢本块
    // (音频毛刺),远优于重启/假死。60ms 分片喂流,1000ms 超时留足 DMA 排空余量。
    esp_err_t err = i2s_channel_write(tx_handle_, buffer.data(), samples * 2 * sizeof(int32_t),
                                      &bytes_written, pdMS_TO_TICKS(1000));
    if (err != ESP_OK) {
        static int64_t s_last_recover_us = 0;
        const int64_t now_us = esp_timer_get_time();
        if (err == ESP_ERR_TIMEOUT && (now_us - s_last_recover_us > 3000000LL)) {
            s_last_recover_us = now_us;
            ESP_LOGW(TAG, "I2S write timeout (missing BCLK/WS clock from BT master), re-applying BT Mode 1...");
            BluetoothScreen::ApplyDefaultMode();
        }
        ESP_LOGE(TAG, "i2s_channel_write failed: %s (samples=%d)", esp_err_to_name(err), samples);
        return 0;
    }
    // 返回写入的采样点数量（以单声道计算）
    return bytes_written / (2 * sizeof(int32_t));
}

int BTAudioCodec::Read(int16_t *dest, int samples)
{
    size_t bytes_read = 0;

    // 复用成员缓冲(容量只增不减),对齐 Write/tx_buffer_:杜绝每次调用临时
    // vector 分配/释放把内部堆撕碎。
    if (static_cast<int>(rx_buffer_.size()) < samples) {
        rx_buffer_.resize(samples);
    }
    std::vector<int32_t>& bit32_buffer = rx_buffer_;

    // 有限超时对齐 Write(1000ms):portMAX_DELAY 在 DMA/对端停摆时会永久挂死
    // 录音路径;去掉硬失败即当失败返回 0,远优于整机假死。
    esp_err_t err = i2s_channel_read(rx_handle_, bit32_buffer.data(),
                                     samples * sizeof(int32_t), &bytes_read,
                                     pdMS_TO_TICKS(1000));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_read failed: %s (samples=%d)", esp_err_to_name(err), samples);
        return 0;
    }

    samples = bytes_read / sizeof(int32_t);
    for (int i = 0; i < samples; i++)
    {
        int32_t value = bit32_buffer[i] >> 12;
        dest[i] = (value > INT16_MAX) ? INT16_MAX : (value < -INT16_MAX) ? -INT16_MAX
                                                                         : (int16_t)value;
    }
    return samples;
}
