#pragma once

#include "http.h"
#include "system_info.h"

#include <memory>
#include <string>

#include <esp_heap_caps.h>

// 统一 HTTP 请求头：业务 API 请求先走 ApplyCommonHeaders / ApplyJsonHeaders。
// 自动带上 Device-Id（设备 MAC）。
namespace api {

// TLS(https)握手前的内部 RAM 预检(公共件;原收音机/ICS 订阅各写一份,阈值不一)。
// mbedtls 握手要 SSL_IN_CONTENT_LEN 16KB 连续内部块 + 工作区,还得给 lwIP/WiFi 留
// 余量;不足时硬开 TLS 会把网络栈饿崩(整机软死,实测)。建议在 Open("...", https)
// 之前调用;false = 内存不足,调用方降级(http 明文)或干净报错,别硬连。
// ESP32-S3 内部 RAM 全部 8-bit 可用,INTERNAL|8BIT 与 INTERNAL 等价。
inline bool TlsPreflightOk(size_t min_largest_block, size_t min_free_total = 0) {
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const size_t free_b = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    return largest >= min_largest_block && free_b >= min_free_total;
}

inline const std::string& DeviceId() {
    static const std::string kId = SystemInfo::GetMacAddress();
    return kId;
}

// 所有业务请求的公共头（对应 curl --header 'Device-Id: ...'）。
inline void ApplyCommonHeaders(Http* http) {
    if (http == nullptr) {
        return;
    }
    http->SetHeader("Device-Id", DeviceId());
    http->SetHeader("Accept", "*/*");
    http->SetHeader("Connection", "close");
}

inline void ApplyCommonHeaders(const std::unique_ptr<Http>& http) {
    ApplyCommonHeaders(http.get());
}

// JSON 请求额外 Content-Type。
inline void ApplyJsonHeaders(Http* http) {
    ApplyCommonHeaders(http);
    if (http != nullptr) {
        http->SetHeader("Content-Type", "application/json");
    }
}

inline void ApplyJsonHeaders(const std::unique_ptr<Http>& http) {
    ApplyJsonHeaders(http.get());
}

}  // namespace api
