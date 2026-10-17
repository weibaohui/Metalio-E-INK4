#include "book_ui_state.h"
#include "display/screen/screen_common.h"
#include <algorithm>

namespace book_screen_detail {

BookUiState& State() {
    static BookUiState s;
    return s;
}

namespace {
struct CoverMissMsg {
    std::string path;
};

void AsyncRememberCoverMiss(void* user_data) {
    auto* msg = static_cast<CoverMissMsg*>(user_data);
    RememberCoverEmbedMiss(msg->path);
    delete msg;
}
}  // namespace

bool IsCoverEmbedMiss(const std::string& path) {
    if (path.empty()) {
        return false;
    }
    const auto& miss = State().cover_embed_miss;
    return std::find(miss.begin(), miss.end(), path) != miss.end();
}

void RememberCoverEmbedMiss(const std::string& path) {
    if (path.empty() || IsCoverEmbedMiss(path)) {
        return;
    }
    State().cover_embed_miss.push_back(path);
}

void PostCoverEmbedMiss(const std::string& path) {
    if (path.empty()) {
        return;
    }
    auto* msg = new CoverMissMsg{};
    msg->path = path;
    if (!ScreenLvAsync(AsyncRememberCoverMiss, msg)) {
        delete msg;
    }
}

}  // namespace book_screen_detail
