#pragma once

#include <string>
#include <cstdint>

// GLM Coding Plan quota snapshot. Invalid values use -1/empty strings so the
// UI can degrade gracefully instead of showing fabricated numbers.
struct GlmUsage {
    bool ok = false;
    int business_code = -1;
    std::string msg;
    std::string level;

    int tokens_5h_used_pct = -1;
    int64_t tokens_5h_reset_ms = -1;
    int tokens_5h_remaining = -1;

    int tokens_week_used_pct = -1;
    int64_t tokens_week_reset_ms = -1;
    int tokens_week_remaining = -1;

    int mcp_used = -1;
    int mcp_total = -1;
    int mcp_remaining = -1;

    int five_hour_resets_left = -1;
    int week_resets_left = -1;
};

// Parse the public-but-unofficial quota endpoint response. Returns true when
// a recognizable business JSON was obtained (even if business code != 200).
bool GlmUsageParse(const std::string& body, GlmUsage& out);

// Parse the optional team-package reset-count response. Returns true when the
// reset arrays were recognized; unavailable entries are not counted.
bool GlmResetsParse(const std::string& body, int& five_hour, int& week);

bool GlmUsageIsAuthError(const GlmUsage& usage);

// Format a Unix-milliseconds reset time as MM-DD HH:MM in UTC+8.
std::string GlmFormatResetMs(int64_t reset_ms);
