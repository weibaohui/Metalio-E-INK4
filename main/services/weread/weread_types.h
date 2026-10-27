#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct WereadAuth {
    std::string vid;
    std::string skey;
    std::string gid;
    std::string refresh_token;

    bool IsLoggedIn() const {
        return !vid.empty() && (!skey.empty() || !gid.empty());
    }

    bool operator==(const WereadAuth& o) const {
        return vid == o.vid && skey == o.skey && gid == o.gid && refresh_token == o.refresh_token;
    }

    bool operator!=(const WereadAuth& o) const {
        return !(*this == o);
    }
};

struct WereadBook {
    std::string id;
    std::string title;
    std::string author;
    std::string cover;
    int progress = 0;
    std::string progress_chapter_uid;
    int progress_chapter_offset = 0;
    uint32_t update_time = 0;
};

struct WereadChapter {
    std::string uid;
    std::string title;
    int index = 0;
    int word_count = 0;
};
