#include "weread_crypto.h"

#include <esp_random.h>
#include <mbedtls/base64.h>
#include <mbedtls/md5.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <ctime>

namespace weread_crypto {
namespace {

std::string Hex(const unsigned char* data, size_t length) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result(length * 2, '0');
    for (size_t i = 0; i < length; ++i) {
        result[i * 2] = kHex[data[i] >> 4];
        result[i * 2 + 1] = kHex[data[i] & 15];
    }
    return result;
}

std::string Md5(const void* data, size_t length) {
    unsigned char digest[16] = {};
    mbedtls_md5(static_cast<const unsigned char*>(data), length, digest);
    return Hex(digest, sizeof(digest));
}

std::string Md5(const std::string& input) { return Md5(input.data(), input.size()); }

std::string UrlEncode(const std::string& input) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string output;
    for (unsigned char c : input) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            output.push_back(static_cast<char>(c));
        } else {
            output.push_back('%');
            output.push_back(kHex[c >> 4]);
            output.push_back(kHex[c & 15]);
        }
    }
    return output;
}

std::vector<int> SwapPositions(const std::string& encrypted) {
    std::vector<int> positions;
    const int length = static_cast<int>(encrypted.size());
    if (length < 4) return positions;
    if (length < 11) return {0, 2};
    const int tail = std::min(4, (length + 9) / 10);
    std::string digits;
    for (int i = length - 1; i >= length - tail; --i) {
        const uint8_t value = static_cast<uint8_t>(encrypted[i]);
        uint32_t expanded = 0;
        for (int bit = 0; bit < 8; ++bit) {
            if ((value >> bit) & 1U) expanded += 1U << (bit * 2);
        }
        digits += std::to_string(expanded);
    }
    const int modulus = length - tail - 2;
    if (modulus <= 0) return positions;
    const int width = static_cast<int>(std::to_string(modulus).size());
    for (int i = 0; positions.size() < 10 && i + width < static_cast<int>(digits.size()); i += width) {
        positions.push_back(std::stoi(digits.substr(i, width)) % modulus);
        positions.push_back(std::stoi(digits.substr(i + 1, width)) % modulus);
    }
    return positions;
}

void ReplaceAll(std::string* value, const std::string& from, const std::string& to) {
    size_t cursor = 0;
    while ((cursor = value->find(from, cursor)) != std::string::npos) {
        value->replace(cursor, from.size(), to);
        cursor += to.size();
    }
}

}  // namespace

std::string EncodeId(const std::string& value) {
    const std::string digest = Md5(value);
    const bool numeric = !value.empty() &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isdigit(c); });
    std::vector<std::string> chunks;
    if (numeric) {
        for (size_t i = 0; i < value.size(); i += 9) {
            const auto number = static_cast<unsigned long>(std::stoul(value.substr(i, 9)));
            char buffer[24] = {};
            std::snprintf(buffer, sizeof(buffer), "%lx", number);
            chunks.emplace_back(buffer);
        }
    } else {
        std::string bytes;
        char buffer[4] = {};
        for (unsigned char c : value) {
            std::snprintf(buffer, sizeof(buffer), "%x", c);
            bytes += buffer;
        }
        chunks.push_back(std::move(bytes));
    }

    std::string result = digest.substr(0, 3) + (numeric ? "3" : "4") + "2" + digest.substr(30);
    for (size_t i = 0; i < chunks.size(); ++i) {
        char length[4] = {};
        std::snprintf(length, sizeof(length), "%02x", static_cast<unsigned>(chunks[i].size()));
        result += length;
        result += chunks[i];
        if (i + 1 < chunks.size()) result += 'g';
    }
    if (result.size() < 20) result += digest.substr(0, 20 - result.size());
    return result + Md5(result).substr(0, 3);
}

std::string Sign(const std::string& query) {
    int64_t left = 0x15051505;
    int64_t right = left;
    int i = static_cast<int>(query.size());
    while (i > 1) {
        left = (left ^ (static_cast<int64_t>(static_cast<uint8_t>(query[i - 1])) <<
                ((query.size() - i + 1) % 30))) & 0x7fffffff;
        right = (right ^ (static_cast<int64_t>(static_cast<uint8_t>(query[i - 2])) <<
                 ((i - 1) % 30))) & 0x7fffffff;
        i -= 2;
    }
    char result[24] = {};
    std::snprintf(result, sizeof(result), "%lx", static_cast<unsigned long>(left + right));
    return result;
}

std::string SortedQuery(const std::vector<std::pair<std::string, std::string>>& params) {
    auto sorted = params;
    std::sort(sorted.begin(), sorted.end());
    std::string query;
    for (const auto& [key, value] : sorted) {
        if (key == "s") continue;
        if (!query.empty()) query += '&';
        query += key + "=" + UrlEncode(value);
    }
    return query;
}

std::vector<std::pair<std::string, std::string>> ContentParams(
        const std::string& book_id, const std::string& chapter_uid,
        const std::string& psvts) {
    long timestamp = static_cast<long>(std::time(nullptr));
    if (EncodeId(std::to_string(timestamp)) == psvts) ++timestamp;
    const uint32_t random = esp_random() % 10000;
    std::vector<std::pair<std::string, std::string>> params = {
        {"b", EncodeId(book_id)}, {"c", EncodeId(chapter_uid)},
        {"r", std::to_string(random * random)}, {"ct", std::to_string(timestamp)},
        {"ps", psvts}, {"pc", EncodeId(std::to_string(timestamp))}, {"sc", "1"},
        {"prevChapter", "false"}, {"st", "0"},
    };
    params.emplace_back("s", Sign(SortedQuery(params)));
    return params;
}

bool DecodeShards(const std::vector<std::string>& shards, std::string* output) {
    if (output == nullptr) return false;
    std::string payload;
    for (const auto& shard : shards) {
        if (shard.size() <= 32) continue;
        std::string expected = Md5(shard.data() + 32, shard.size() - 32);
        std::transform(expected.begin(), expected.end(), expected.begin(), ::toupper);
        if (shard.compare(0, 32, expected) == 0) payload.append(shard, 32, std::string::npos);
    }
    if (payload.size() < 2) return false;
    std::string encoded = payload.substr(1);
    const auto positions = SwapPositions(encoded);
    for (int i = static_cast<int>(positions.size()) - 1; i > 0; i -= 2) {
        for (int offset = 1; offset >= 0; --offset) {
            const int a = positions[i] + offset;
            const int b = positions[i - 1] + offset;
            if (a >= 0 && b >= 0 && a < encoded.size() && b < encoded.size()) std::swap(encoded[a], encoded[b]);
        }
    }
    std::string base64;
    for (char c : encoded) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '/') base64 += c;
    }
    while (base64.size() % 4) base64 += '=';
    size_t required = 0;
    mbedtls_base64_decode(nullptr, 0, &required,
                          reinterpret_cast<const unsigned char*>(base64.data()), base64.size());
    output->assign(required, '\0');
    size_t written = 0;
    if (mbedtls_base64_decode(reinterpret_cast<unsigned char*>(output->data()), output->size(), &written,
                              reinterpret_cast<const unsigned char*>(base64.data()), base64.size()) != 0) {
        output->clear();
        return false;
    }
    output->resize(written);
    return true;
}

std::string StripMarkup(const std::string& input) {
    std::string output;
    bool space = false;
    bool suppressed = false;
    for (size_t i = 0; i < input.size();) {
        if (input[i] == '<') {
            const size_t end = input.find('>', i + 1);
            if (end == std::string::npos) break;
            std::string tag = input.substr(i + 1, end - i - 1);
            std::transform(tag.begin(), tag.end(), tag.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            while (!tag.empty() && std::isspace(static_cast<unsigned char>(tag.front()))) tag.erase(tag.begin());
            const bool closing = !tag.empty() && tag.front() == '/';
            if (closing) tag.erase(tag.begin());
            const size_t name_end = tag.find_first_of(" \t\r\n/");
            const std::string name = tag.substr(0, name_end);
            if (name == "script" || name == "style") suppressed = !closing;
            const bool block = name == "p" || name == "div" || name == "br" ||
                name == "li" || name == "h1" || name == "h2" || name == "h3" ||
                name == "h4" || name == "blockquote" || name == "section";
            if (!suppressed && block && !output.empty() && output.back() != '\n') output += '\n';
            i = end + 1;
            continue;
        }
        const char c = input[i++];
        if (suppressed) continue;
        if (std::isspace(static_cast<unsigned char>(c))) {
            if (!space && !output.empty()) output += (c == '\n' ? '\n' : ' ');
            space = true;
        } else {
            output += c;
            space = false;
        }
    }
    ReplaceAll(&output, "&nbsp;", " ");
    ReplaceAll(&output, "&amp;", "&");
    ReplaceAll(&output, "&lt;", "<");
    ReplaceAll(&output, "&gt;", ">");
    ReplaceAll(&output, "&quot;", "\"");
    ReplaceAll(&output, "&apos;", "'");
    ReplaceAll(&output, "&#39;", "'");
    for (size_t pos = 0; (pos = output.find("&#", pos)) != std::string::npos;) {
        const size_t end = output.find(';', pos + 2);
        if (end == std::string::npos || end - pos > 10) { pos += 2; continue; }
        const bool hex = pos + 2 < end && (output[pos + 2] == 'x' || output[pos + 2] == 'X');
        const size_t digits = pos + (hex ? 3 : 2);
        char* parsed_end = nullptr;
        const unsigned long cp = std::strtoul(output.c_str() + digits, &parsed_end, hex ? 16 : 10);
        if (parsed_end != output.c_str() + end || cp == 0 || cp > 0x10ffff) {
            pos += 2;
            continue;
        }
        std::string encoded;
        if (cp <= 0x7f) encoded.push_back(static_cast<char>(cp));
        else if (cp <= 0x7ff) {
            encoded.push_back(static_cast<char>(0xc0 | (cp >> 6)));
            encoded.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else if (cp <= 0xffff) {
            encoded.push_back(static_cast<char>(0xe0 | (cp >> 12)));
            encoded.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            encoded.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        } else {
            encoded.push_back(static_cast<char>(0xf0 | (cp >> 18)));
            encoded.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3f)));
            encoded.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3f)));
            encoded.push_back(static_cast<char>(0x80 | (cp & 0x3f)));
        }
        output.replace(pos, end - pos + 1, encoded);
        pos += encoded.size();
    }
    return output;
}

std::string Sha256Hex(const std::string& input) {
    unsigned char digest[32] = {};
    mbedtls_sha256(reinterpret_cast<const unsigned char*>(input.data()), input.size(), digest, 0);
    return Hex(digest, sizeof(digest));
}

}  // namespace weread_crypto
