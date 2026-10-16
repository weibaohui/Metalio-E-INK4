#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <vector>
#include <cstdint>

struct AudioStreamPacket {
    int sample_rate = 0;
    int frame_duration = 0;
    uint32_t timestamp = 0;
    std::vector<uint8_t> payload;
};

#endif // PROTOCOL_H
