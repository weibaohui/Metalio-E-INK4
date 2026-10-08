// main/services/radio/radio_mcp.cc —— 收音机的 MCP 工具(移植自 aipassport-radio
// 的 radio_mcp.c,按本机 MCP 框架(McpServer/Property)与目录形态(内置 339 台 +
// NVS 收藏,无自定义清单)改写)。
// 工具跑在 MCP 服务任务上下文;radio_play/radio_stop 本就线程安全,当前台从
// 播放器快照实时反查,与触摸/虚拟键两条播放路径天然一致。
#include "radio_mcp.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>

#include <esp_log.h>
#include <esp_random.h>

#include "mcp_server.h"
#include "radio_player.h"
#include "services/radio/radio_catalog.h"

namespace {

constexpr const char* TAG = "radio_mcp";

// 读 RAM 缓存(NVS 在开机时由 main 任务载入;统一走缓存与界面一致)。
std::vector<std::string> LoadFavs() { return RadioGetFavs(); }



// 收藏名 → 目录下标;-1 表示不在目录里(脏数据容忍:跳过)。
int FavToCatalog(const std::string& name) {
    for (int k = 0; k < radio_catalog_count(); ++k) {
        radio_station_t st;
        if (radio_catalog_get(k, &st) && name == st.name) return k;
    }
    return -1;
}

// 当前播放中的台在目录里的下标(未在播 -1)。
// 用快照里显式记录的下标,不从显示台名反查 —— 反查在"连接中"窗口会拿到旧台名,
// 表现为连点两次 next 停在同一个台上。
int CurrentIndex() {
    radio_player_snap_t s;
    radio_player_snapshot(&s);
    if (s.state != RADIO_PLAYING && s.state != RADIO_CONNECTING) return -1;
    return s.catalog_index;
}

void PlayAndReply(int idx, std::string& reply) {
    radio_station_t st;
    if (!radio_catalog_get(idx, &st)) {
        reply = "台目读取失败";
        return;
    }
    radio_play_indexed(st.name, st.url, idx);
    // 与 radio_screen 一致:记为"上次收听"(RAM 缓存 + 内部栈持久化)
    RadioSetLast(st.name, st.url);
    reply = std::string("已切台:第 ") + std::to_string(idx + 1) + " 台 " + st.name +
            "(连接中,几秒后 get_state 确认)";
}

}  // namespace

void RadioRegisterMcpTools() {
    // 本函数由 Application::Start 在 main 任务调用 —— 唯一的 NVS 读窗口。
    RadioCacheInit();
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool("self.radio.play_index",
        "Play a built-in radio station by its 1-based index in the catalog.",
        PropertyList({Property("index", kPropertyTypeInteger, 1, radio_catalog_count())}),
        [](const PropertyList& properties) -> ReturnValue {
            std::string reply;
            PlayAndReply(properties["index"].value<int>() - 1, reply);
            return reply;
        });

    mcp.AddTool("self.radio.play_name",
        "Play a built-in radio station by name (exact match). Use self.radio.search first if unsure.",
        PropertyList({Property("name", kPropertyTypeString)}),
        [](const PropertyList& properties) -> ReturnValue {
            const std::string name = properties["name"].value<std::string>();
            const int idx = FavToCatalog(name);
            if (idx < 0) {
                return std::string("没找到叫「") + name + "」的台;可先用 self.radio.search 模糊搜";
            }
            std::string reply;
            PlayAndReply(idx, reply);
            return reply;
        });

    mcp.AddTool("self.radio.next",
        "Switch to the next radio station in the catalog.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            const int total = radio_catalog_count();
            const int cur = CurrentIndex();
            std::string reply;
            PlayAndReply(cur < 0 ? 0 : (cur + 1) % total, reply);
            return reply;
        });

    mcp.AddTool("self.radio.prev",
        "Switch to the previous radio station in the catalog.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            const int total = radio_catalog_count();
            const int cur = CurrentIndex();
            std::string reply;
            PlayAndReply(cur < 0 ? total - 1 : (cur + total - 1) % total, reply);
            return reply;
        });

    mcp.AddTool("self.radio.random",
        "Play a random radio station (never the current one).",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            const int total = radio_catalog_count();
            if (total <= 0) return std::string("台目为空");
            const int cur = CurrentIndex();
            int idx = static_cast<int>(esp_random() % static_cast<unsigned>(total));
            if (cur >= 0 && total > 1) {
                while (idx == cur) {
                    idx = static_cast<int>(esp_random() % static_cast<unsigned>(total));
                }
            }
            std::string reply;
            PlayAndReply(idx, reply);
            return reply;
        });

    mcp.AddTool("self.radio.stop",
        "Stop radio playback.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            radio_stop();
            return std::string("已停止");
        });

    mcp.AddTool("self.radio.set_volume",
        "Set the speaker volume while listening to the radio (0-100).",
        PropertyList({Property("level", kPropertyTypeInteger, 0, 100)}),
        [](const PropertyList& properties) -> ReturnValue {
            radio_set_volume(properties["level"].value<int>());
            return std::string("音量已设为 ") +
                   std::to_string(properties["level"].value<int>()) + "%";
        });

    mcp.AddTool("self.radio.get_state",
        "Get the radio player state: playing/connecting/stopped/error, current station and ICY title, sample rate.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            radio_player_snap_t s;
            radio_player_snapshot(&s);
            static const char* K[] = {"未在收听", "正在连接", "正在收听", "出错"};
            std::string out = std::string(K[s.state & 3]) + " " + s.station;
            if (s.title[0]) {
                out += " | 正在播:" + std::string(s.title);
            }
            out += " | 采样率 " + std::to_string(s.sample_rate) + " Hz";
            if (s.state == RADIO_ERROR && s.err[0]) {
                out += std::string(" | 错误:") + s.err;
            }
            return out;
        });

    mcp.AddTool("self.radio.list",
        "List built-in radio stations. Args: from(0-based, default 0), count(1-50, default 10).",
        PropertyList({Property("from", kPropertyTypeInteger, 0),
                      Property("count", kPropertyTypeInteger, 10)}),
        [](const PropertyList& properties) -> ReturnValue {
            int from = properties["from"].value<int>();
            int count = properties["count"].value<int>();
            if (count < 1 || count > 50) count = 10;
            const int total = radio_catalog_count();
            if (from < 0) from = 0;
            if (from >= total) {
                return std::string("起点越界:共 ") + std::to_string(total) + " 台";
            }
            std::string out =
                "共 " + std::to_string(total) + " 台,第 " + std::to_string(from + 1) + " 台起:";
            radio_station_t st;
            for (int i = from; i < total && count > 0; ++i, --count) {
                if (!radio_catalog_get(i, &st)) continue;
                out += std::string("\n") + std::to_string(i + 1) + ". " + st.name;
            }
            return out;
        });

    mcp.AddTool("self.radio.search",
        "Search built-in radio stations by name substring. Args: keyword, limit(1-50, default 10).",
        PropertyList({Property("keyword", kPropertyTypeString),
                      Property("limit", kPropertyTypeInteger, 10)}),
        [](const PropertyList& properties) -> ReturnValue {
            const std::string key = properties["keyword"].value<std::string>();
            int limit = properties["limit"].value<int>();
            if (limit < 1 || limit > 50) limit = 10;
            radio_station_t st;
            int found = 0;
            std::string out;
            for (int i = 0; i < radio_catalog_count() && found < limit; ++i) {
                if (!radio_catalog_get(i, &st)) continue;
                if (strstr(st.name, key.c_str()) == nullptr) continue;
                out += (found ? std::string("\n") : std::string()) +
                       std::to_string(i + 1) + ". " + st.name;
                ++found;
            }
            if (found == 0) {
                return std::string("没有名字含「") + key + "」的台";
            }
            return out;
        });

    mcp.AddTool("self.radio.fav_list",
        "List favorite radio stations.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            const auto favs = LoadFavs();
            if (favs.empty()) return std::string("收藏为空");
            std::string out = "收藏 " + std::to_string(favs.size()) + " 台:";
            for (size_t i = 0; i < favs.size(); ++i) {
                out += std::string("\n") + std::to_string(i + 1) + ". " + favs[i];
            }
            return out;
        });

    mcp.AddTool("self.radio.fav_play",
        "Play a favorite radio station by its 1-based position in the favorites list.",
        PropertyList({Property("index", kPropertyTypeInteger)}),
        [](const PropertyList& properties) -> ReturnValue {
            const auto favs = LoadFavs();
            const int one = properties["index"].value<int>();
            if (one < 1 || one > static_cast<int>(favs.size())) {
                return std::string("下标越界:收藏共 ") + std::to_string(favs.size()) + " 台";
            }
            const int idx = FavToCatalog(favs[one - 1]);
            if (idx < 0) {
                return std::string("「") + favs[one - 1] + "」不在内置目录里,无法播放";
            }
            std::string reply;
            PlayAndReply(idx, reply);
            return reply;
        });

    mcp.AddTool("self.radio.fav_add",
        "Add a station (by catalog index, 1-based) to favorites.",
        PropertyList({Property("index", kPropertyTypeInteger)}),
        [](const PropertyList& properties) -> ReturnValue {
            const int one = properties["index"].value<int>();
            radio_station_t st;
            if (one < 1 || one > radio_catalog_count() || !radio_catalog_get(one - 1, &st)) {
                return std::string("下标越界");
            }
            if (!RadioFavAdd(st.name)) {
                return std::string("已在收藏:") + st.name;
            }
            return std::string("已收藏 ") + st.name + "(共 " +
                   std::to_string(RadioGetFavs().size()) + " 台)";
        });

    mcp.AddTool("self.radio.fav_remove",
        "Remove a station (by favorites list index, 1-based) from favorites.",
        PropertyList({Property("index", kPropertyTypeInteger)}),
        [](const PropertyList& properties) -> ReturnValue {
            auto favs = LoadFavs();
            const int one = properties["index"].value<int>();
            if (one < 1 || one > static_cast<int>(favs.size())) {
                return std::string("下标越界:收藏共 ") + std::to_string(favs.size()) + " 台";
            }
            const std::string name = favs[one - 1];
            RadioFavRemove(name);
            return std::string("已取消收藏 ") + name;
        });

    ESP_LOGI(TAG, "radio MCP tools registered");
}
