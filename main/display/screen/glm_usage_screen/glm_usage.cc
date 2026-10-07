#include "glm_usage_screen/glm_usage.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cJSON.h"

namespace {

int GetInt(const cJSON* obj, const char* key, int missing = -1) {
    const cJSON* value = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(value) ? value->valueint : missing;
}

}  // namespace

bool GlmUsageParse(const std::string& body, GlmUsage& out) {
    out = GlmUsage{};
    if (body.empty()) {
        return false;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return false;
    }

    bool parsed = false;
    if (const cJSON* msg = cJSON_GetObjectItemCaseSensitive(root, "msg");
        cJSON_IsString(msg) && msg->valuestring != nullptr) {
        out.msg = msg->valuestring;
    }
    if (const cJSON* code = cJSON_GetObjectItemCaseSensitive(root, "code");
        cJSON_IsNumber(code)) {
        out.business_code = code->valueint;
        parsed = true;
    }

    const cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (cJSON_IsObject(data)) {
        if (const cJSON* level = cJSON_GetObjectItemCaseSensitive(data, "level");
            cJSON_IsString(level) && level->valuestring != nullptr) {
            out.level = level->valuestring;
            parsed = true;
        }

        struct Window {
            int64_t reset = -1;
            int pct = -1;
            int remaining = -1;
        };
        Window w5;
        Window week;
        int window_count = 0;

        if (const cJSON* limits = cJSON_GetObjectItemCaseSensitive(data, "limits");
            cJSON_IsArray(limits)) {
            const cJSON* item = nullptr;
            cJSON_ArrayForEach(item, limits) {
                const cJSON* type = cJSON_GetObjectItemCaseSensitive(item, "type");
                if (!cJSON_IsString(type) || type->valuestring == nullptr) {
                    continue;
                }
                if (std::strcmp(type->valuestring, "TIME_LIMIT") == 0) {
                    out.mcp_used = GetInt(item, "currentValue");
                    out.mcp_total = GetInt(item, "usage");
                    out.mcp_remaining = GetInt(item, "remaining");
                    parsed = true;
                    continue;
                }
                const bool is_window = std::strcmp(type->valuestring, "TOKENS_LIMIT") == 0 ||
                                       std::strcmp(type->valuestring, "CREDIT_LIMIT") == 0;
                if (!is_window) {
                    continue;
                }

                Window window;
                if (const cJSON* reset = cJSON_GetObjectItemCaseSensitive(item, "nextResetTime");
                    cJSON_IsNumber(reset)) {
                    window.reset = static_cast<int64_t>(reset->valuedouble);
                }
                window.pct = GetInt(item, "percentage");
                window.remaining = GetInt(item, "remaining");

                // The earlier reset is always the five-hour window.
                if (window_count == 0) {
                    w5 = window;
                } else if (window_count == 1) {
                    if (window.reset >= 0 && w5.reset >= 0 && window.reset < w5.reset) {
                        week = w5;
                        w5 = window;
                    } else {
                        week = window;
                    }
                } else if (window.reset >= 0) {
                    if (w5.reset >= 0 && window.reset < w5.reset) {
                        week = w5;
                        w5 = window;
                    } else if (week.reset < 0 || window.reset < week.reset) {
                        week = window;
                    }
                }
                ++window_count;
            }
        }

        if (window_count > 0) {
            out.tokens_5h_reset_ms = w5.reset;
            out.tokens_5h_used_pct = w5.pct;
            out.tokens_5h_remaining = w5.remaining;
            out.tokens_week_reset_ms = week.reset;
            out.tokens_week_used_pct = week.pct;
            out.tokens_week_remaining = week.remaining;
            parsed = true;
        }
    }

    cJSON_Delete(root);
    out.ok = out.business_code >= 200 && out.business_code < 300;
    return parsed;
}

bool GlmResetsParse(const std::string& body, int& five_hour, int& week) {
    five_hour = -1;
    week = -1;
    if (body.empty()) {
        return false;
    }
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        return false;
    }

    bool ok = false;
    const cJSON* data = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (cJSON_IsObject(data)) {
        const char* keys[] = {"fiveHourResets", "weekResets"};
        int* outputs[] = {&five_hour, &week};
        for (size_t i = 0; i < 2; ++i) {
            const cJSON* array = cJSON_GetObjectItemCaseSensitive(data, keys[i]);
            if (!cJSON_IsArray(array)) {
                continue;
            }
            int count = 0;
            const cJSON* item = nullptr;
            cJSON_ArrayForEach(item, array) {
                const cJSON* available = cJSON_GetObjectItemCaseSensitive(item, "available");
                if (!cJSON_IsBool(available) || cJSON_IsTrue(available)) {
                    ++count;
                }
            }
            *outputs[i] = count;
            ok = true;
        }
    }

    cJSON_Delete(root);
    return ok;
}

bool GlmUsageIsAuthError(const GlmUsage& usage) {
    return usage.business_code == 401 || usage.business_code == 403 ||
           usage.business_code == 1001;
}

std::string GlmFormatResetMs(int64_t reset_ms) {
    if (reset_ms <= 0) {
        return "--";
    }

    // The quota console is aimed at China, so display resets in UTC+8 like the
    // original AI Passport application.
    const int64_t seconds = reset_ms / 1000 + 8 * 3600;
    const int64_t days = seconds / 86400;
    const int64_t remainder = seconds % 86400;
    if (days < 0) {
        return "--";
    }

    // Howard Hinnant's civil-from-days algorithm.
    const int64_t z = days + 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t day = doy - (153 * mp + 2) / 5 + 1;
    const int64_t month = mp < 10 ? mp + 3 : mp - 9;
    char text[48];
    std::snprintf(text, sizeof(text), "%02lld-%02lld %02lld:%02lld",
                  static_cast<long long>(month), static_cast<long long>(day),
                  static_cast<long long>(remainder / 3600),
                  static_cast<long long>((remainder % 3600) / 60));
    return text;
}
