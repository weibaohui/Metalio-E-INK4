#ifndef _BT_AUDIO_CODEC_H
#define _BT_AUDIO_CODEC_H

#include "audio_codec.h"

#include <driver/gpio.h>
#include <driver/i2s_pdm.h>
#include <mutex>
#include <vector>

class BTAudioCodec : public AudioCodec {
protected:
    std::mutex data_if_mutex_;
    // I2S 写的交织立体声缓冲。做成成员复用容量:Write 以 ~16Hz 高频调用,
    // 每次临时 new/free 8KB 内部 RAM 会把本就只有 ~30KB 的内部堆撕碎,
    // 最终 lwIP 拿不到连续内存导致整个网络栈饿死(WiFi 仍关联但 ping/TCP 全死)。
    std::vector<int32_t> tx_buffer_;

    virtual int Write(const int16_t* data, int samples) override;
    virtual int Read(int16_t* dest, int samples) override;

public:
    virtual ~BTAudioCodec();
};

class BTAudioCodecDuplex : public BTAudioCodec {
public:
    BTAudioCodecDuplex(int input_sample_rate, int output_sample_rate, gpio_num_t bclk, gpio_num_t ws, gpio_num_t dout, gpio_num_t din);
};


#endif // _BT_AUDIO_CODEC_H
