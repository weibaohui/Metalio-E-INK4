#pragma once

#include "api_endpoints.h"
#include "board.h"
#include "http.h"
#include "system_info.h"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

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

// ---- 统一 HTTP 请求收拢(分析文档 §2.2) ----
// 手写 CreateHttp+Open+ReadAll 模式曾散布 16 文件 24 处;超时/重定向/体积上限/
// TLS 预检/日志这里只写一遍。流式下载(落盘/刷写/multipart 上传)不在此范围,
// 仍走各自实现。

struct HttpReply {
    bool ok = false;       // 传输完成且状态码 2xx
    int status = 0;        // HTTP 状态码;0 = 传输层失败/未连上
    std::string body;      // ok 时的响应体;非 2xx 时不读 body,拿 status/error 即可
    std::string error;     // !ok 时的人读原因(可直接上屏/入日志)
};

struct HttpFetchOptions {
    int timeout_ms = 15000;
    size_t max_bytes = 256 * 1024;   // 响应上限,超限报错不截断(防大响应吃满内存)
    // 第三方/用户提供的 URL 置 false:不带 Device-Id 等公共头。本 API 永不自动
    // 携带鉴权头;鉴权头只能经 extra_headers 显式传入,且跨 host 重定向会被剥掉。
    bool send_common_headers = true;
    bool allow_redirect = true;      // 最多 1 跳(301/302/303/307/308)
    size_t tls_min_largest_block = 12 * 1024;  // https 握手前内部 RAM 预检阈值(ICS 实测值)
    std::function<bool()> cancel;    // 逐块轮询,返回 true=中止(下载独占/翻页打断用)
};

using HeaderList = std::vector<std::pair<std::string, std::string>>;

inline std::string HttpUrlHost(const std::string& url) {
    const size_t scheme = url.find("://");
    if (scheme == std::string::npos) {
        return "";
    }
    const size_t start = scheme + 3;
    const size_t end = url.find('/', start);
    return url.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

inline bool HttpIsAuthHeader(const std::string& key) {
    return key == "Authorization" || key == "Cookie";
}

inline HttpReply HttpFetch(const char* method, const std::string& url, std::string body,
                           const HttpFetchOptions& opts = {},
                           const HeaderList& extra_headers = {}) {
    HttpReply rep;
    constexpr const char* kTag = "api_http";
    if (url.empty()) {
        rep.error = "端点未配置";
        return rep;
    }
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) {
        rep.error = "无网络接口";
        ESP_LOGW(kTag, "%s %s -> %s", method, url.c_str(), rep.error.c_str());
        return rep;
    }
    // https 握手要 16KB 级连续内部块;不足时硬连会把网络栈饿崩(实测整机软死),
    // 这里统一预检,由调用方按 rep.error 降级或报错。
    if (url.rfind("https://", 0) == 0 && !TlsPreflightOk(opts.tls_min_largest_block)) {
        rep.error = "内部内存不足,暂无法 HTTPS";
        ESP_LOGW(kTag, "%s %s -> %s", method, url.c_str(), rep.error.c_str());
        return rep;
    }
    auto http = network->CreateHttp(0);
    if (http == nullptr) {
        rep.error = "创建 HTTP 客户端失败";
        ESP_LOGW(kTag, "%s %s -> %s", method, url.c_str(), rep.error.c_str());
        return rep;
    }
    http->SetTimeout(opts.timeout_ms);
    LogHttpRequest(kTag, method, url, body);
    // 成功走 D 级(api_endpoints 的 [API_HTTP] 日志,默认静默);失败统一 W 级,
    // 但"已取消"是正常打断(翻页/下载独占),不算失败。
    auto err_return = [&](std::string why) {
        rep.error = std::move(why);
        if (rep.error != "已取消") {
            ESP_LOGW(kTag, "%s %s -> %s", method, url.c_str(), rep.error.c_str());
        }
        http->Close();
        return rep;
    };

    const std::string origin_host = HttpUrlHost(url);
    std::string real_url = url;
    // 头在每次发送后会被实现清空,重定向逐跳重设;跨 host 跳转剥掉鉴权类头。
    auto apply_headers = [&](void) {
        if (opts.send_common_headers) {
            ApplyCommonHeaders(http.get());
        }
        for (const auto& kv : extra_headers) {
            if (HttpIsAuthHeader(kv.first) && HttpUrlHost(real_url) != origin_host) {
                ESP_LOGW(kTag, "跨 host 重定向,丢弃鉴权头 %s", kv.first.c_str());
                continue;
            }
            http->SetHeader(kv.first, kv.second);
        }
    };

    const int64_t t0 = esp_timer_get_time();
    for (int hop = 0; hop < 2; ++hop) {
        if (opts.cancel && opts.cancel()) {
            return err_return("已取消");
        }
        apply_headers();
        // 空 body 也要 SetContent(""):部分服务端对无 Content 的 POST/DELETE
        // 按 chunked 等待会挂死(task_screen 实测)。拷贝而非 move:重定向
        // 第二跳还要重发同一 body。
        http->SetContent(std::string(body));
        if (!http->Open(method, real_url)) {
            return err_return("连接失败");
        }
        rep.status = http->GetStatusCode();
        if (rep.status >= 200 && rep.status < 300) {
            break;
        }
        const bool is_redirect = rep.status == 301 || rep.status == 302 ||
                                 rep.status == 303 || rep.status == 307 ||
                                 rep.status == 308;
        if (!is_redirect) {
            return err_return("HTTP " + std::to_string(rep.status));
        }
        if (!opts.allow_redirect) {
            return err_return("HTTP " + std::to_string(rep.status));
        }
        const std::string loc = http->GetResponseHeader("Location");
        http->Close();
        if (loc.empty()) {
            return err_return("重定向无 Location");
        }
        if (hop == 1) {
            return err_return("重定向过多");
        }
        real_url = loc;
    }

    const size_t content_len = http->GetBodyLength();
    if (content_len > opts.max_bytes) {
        return err_return("响应过大(>" + std::to_string(opts.max_bytes / 1024) + "KB)");
    }
    rep.body.reserve(content_len < 64 * 1024 ? 64 * 1024 : content_len + 1);
    char chunk[2048];
    while (true) {
        if (opts.cancel && opts.cancel()) {
            return err_return("已取消");
        }
        const int r = http->Read(chunk, sizeof(chunk));
        if (r < 0) {
            return err_return("读取中断");
        }
        if (r == 0) {
            break;
        }
        if (rep.body.size() + static_cast<size_t>(r) > opts.max_bytes) {
            return err_return("响应过大(>" + std::to_string(opts.max_bytes / 1024) + "KB)");
        }
        rep.body.append(chunk, static_cast<size_t>(r));
    }
    http->Close();
    rep.ok = true;
    LogHttpResponse(kTag, rep.status, rep.body);
    ESP_LOGD(kTag, "%s %s -> %d (%u bytes, %lld ms)", method, url.c_str(), rep.status,
             static_cast<unsigned>(rep.body.size()),
             (long long)((esp_timer_get_time() - t0) / 1000));
    return rep;
}

inline HttpReply HttpGetString(const std::string& url, const HttpFetchOptions& opts = {},
                               const HeaderList& extra_headers = {}) {
    return HttpFetch("GET", url, std::string(), opts, extra_headers);
}

inline HttpReply HttpPostJson(const std::string& url, std::string json_body,
                              const HttpFetchOptions& opts = {},
                              const HeaderList& extra_headers = {}) {
    HeaderList headers = extra_headers;
    headers.emplace_back("Content-Type", "application/json");
    return HttpFetch("POST", url, std::move(json_body), opts, headers);
}

}  // namespace api
