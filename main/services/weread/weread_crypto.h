#pragma once

#include <string>
#include <utility>
#include <vector>

namespace weread_crypto {

std::string EncodeId(const std::string& value);
std::string Sign(const std::string& query);
std::string SortedQuery(const std::vector<std::pair<std::string, std::string>>& params);
std::vector<std::pair<std::string, std::string>> ContentParams(
    const std::string& book_id, const std::string& chapter_uid,
    const std::string& psvts);
bool DecodeShards(const std::vector<std::string>& shards, std::string* output);
std::string StripMarkup(const std::string& input);
std::string Sha256Hex(const std::string& input);

}  // namespace weread_crypto
