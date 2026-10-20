#pragma once

/**
 * @file fs_utils.h
 * @brief 共享的文件名/路径净化工具
 *
 * 原先 WiFi 传书(wifi_transfer/transfer_utils.h)与 WiFi 文件管理(wifi_file)
 * 各自维护一份 URL 解码与文件名净化,行为并不一致(前者丢弃非法字符且限长
 * 128,后者把非法字符替换成 '_' 且不限长),合并后统一到本文件:
 *   - 非法字符替换为 '_'(保留可读性,不静默吞字)
 *   - 剥离首尾点号/空格(FAT32 目录项不允许尾随点与空格)
 *   - 长度上限 128 字节(FAT32 单文件名 255,留出扩展名余量)
 */

#include <cstdlib>
#include <string>

namespace fs_utils {

/**
 * @brief URL 解码(%20 → 空格,UTF-8 多字节还原,'+' → 空格)
 */
inline std::string UrlDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            char hex[3] = {in[i + 1], in[i + 2], '\0'};
            char* end = nullptr;
            long val = std::strtol(hex, &end, 16);
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
 * @brief 净化文件名:非法字符替换为 '_',剥离首尾点号/空格,限长 128
 * @return 净化结果;结果为空表示原名完全非法
 */
inline std::string SanitizeFilename(const std::string& name) {
    std::string s;
    s.reserve(name.size());
    for (char c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' ||
            c == '"' || c == '<' || c == '>' || c == '|' ||
            static_cast<unsigned char>(c) < 0x20) {
            s.push_back('_');
        } else {
            s.push_back(c);
        }
    }
    while (!s.empty() && (s.front() == '.' || s.front() == ' ')) s.erase(0, 1);
    while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();

    // 限制最大文件名长度,避免 FAT32 超限
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

/**
 * @brief 判断 abs_path 是否位于 dir 目录之下(含 dir 本身)
 */
inline bool IsUnderDir(const std::string& abs_path, const std::string& dir) {
    if (dir.empty()) return false;
    if (abs_path.compare(0, dir.size(), dir) != 0) return false;
    if (abs_path.size() == dir.size()) return true;
    // 必须是目录分隔符边界,避免 /books2 命中 /books
    return abs_path[dir.size()] == '/';
}

}  // namespace fs_utils
