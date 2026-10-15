#pragma once

#include <string>
#include <cctype>

namespace wifi_transfer {

/**
 * @brief URL 解码函数 (如将 %20 转为空格，UTF-8 字符还原)
 */
inline std::string UrlDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            char hex[3] = {in[i + 1], in[i + 2], '\0'};
            char* end = nullptr;
            long val = strtol(hex, &end, 16);
            if (end == hex + 2) {
                out.push_back(static_cast<char>(val));
                i += 2;
                continue;
            }
        } else if (in[i] == '+') {
            out.push_back(' ');
            continue;
        }
        out.push_back(in[i]);
    }
    return out;
}

/**
 * @brief 净化文件名，去除路径穿越、非法特殊字符以及两端点号和空格
 */
inline std::string SanitizeFilename(const std::string& name) {
    std::string s;
    s.reserve(name.size());
    for (char c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' || static_cast<unsigned char>(c) < 32) {
            continue;
        }
        s.push_back(c);
    }
    while (!s.empty() && (s.front() == '.' || s.front() == ' ')) s.erase(0, 1);
    while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();

    // 限制最大文件名长度，避免 FAT32 超限
    if (s.size() > 128) {
        size_t dot_pos = s.rfind('.');
        if (dot_pos != std::string::npos && dot_pos > 100) {
            std::string ext = s.substr(dot_pos);
            s = s.substr(0, 120) + ext;
        } else {
            s = s.substr(0, 128);
        }
    }
    return s;
}

}  // namespace wifi_transfer
