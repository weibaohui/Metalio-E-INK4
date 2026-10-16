#pragma once

#include "cloudzao_endpoints.h"

#include <cstring>
#include <string>

#include <esp_log.h>

// Enable extra request/response body logging in debug builds.
#ifndef API_HTTP_DEBUG
#define API_HTTP_DEBUG 0
#endif

namespace api {

namespace detail {

inline const char* HttpBodyForLog(const std::string& body) {
    return body.empty() ? "(empty)" : body.c_str();
}

}  // namespace detail

// Log requests at debug level without noise in normal builds.
inline void LogHttpRequest(const char* tag, const char* method, const std::string& url,
                           const std::string& body = std::string(), const char* extra = nullptr) {
    ESP_LOGD(tag, "[API_HTTP] >>> %s %s", method, url.c_str());
    if (extra != nullptr && extra[0] != '\0') {
        ESP_LOGD(tag, "[API_HTTP] >>> %s", extra);
    }
#if API_HTTP_DEBUG
    ESP_LOGD(tag, "[API_HTTP] >>> body=%s", detail::HttpBodyForLog(body));
#else
    (void)body;
#endif
}

inline void LogHttpResponse(const char* tag, int status, const std::string& body) {
#if API_HTTP_DEBUG
    ESP_LOGD(tag, "[API_HTTP] <<< status=%d body=%s", status, detail::HttpBodyForLog(body));
#else
    ESP_LOGD(tag, "[API_HTTP] <<< status=%d", status);
    (void)body;
#endif
}

inline void LogHttpBinaryRequest(const char* tag, const char* method, const std::string& url,
                                 size_t body_size, const char* extra = nullptr) {
    ESP_LOGD(tag, "[API_HTTP] >>> %s %s body_bytes=%u", method, url.c_str(),
             static_cast<unsigned>(body_size));
    if (extra != nullptr && extra[0] != '\0') {
        ESP_LOGD(tag, "[API_HTTP] >>> %s", extra);
    }
}

inline const char* const kHost = cloudzao_host;
inline const char* const kDefaultOtaUrl = cloudzao_ota_url;
inline const char* const kBookToolsUrl = cloudzao_book_tools_url;

inline const char* const kWeatherLatest = cloudzao_path_weather_latest;
inline const char* const kLibrarySync = cloudzao_path_library_sync;

// Host is configured; blank open-source builds leave it empty.
inline bool HasHost() {
    return kHost[0] != '\0';
}

// OTA URL is set when available.
inline bool HasDefaultOtaUrl() {
    return kDefaultOtaUrl[0] != '\0';
}

// Return empty string when host or path is unavailable.
inline std::string Url(const char* path) {
    if (!HasHost() || path == nullptr || path[0] == '\0') {
        return std::string();
    }
    return std::string(kHost) + path;
}

inline std::string WeatherLatestUrl() {
    return Url(kWeatherLatest);
}

inline std::string LibrarySyncUrl() {
    return Url(kLibrarySync);
}

// Redact URLs in log output when a host is configured.
inline std::string RedactClawUrlsForLog(const std::string& text) {
    if (!HasHost()) {
        return text;
    }
    std::string out = text;
    const size_t prefix_len = std::strlen(kHost);
    size_t pos = 0;
    while ((pos = out.find(kHost, pos)) != std::string::npos) {
        size_t end = out.find_first_of("\"' \t\r\n,}", pos + prefix_len);
        if (end == std::string::npos) {
            end = out.size();
        }
        out.replace(pos, end - pos, "[redacted]");
        pos += 10;
    }
    return out;
}

}  // namespace api
