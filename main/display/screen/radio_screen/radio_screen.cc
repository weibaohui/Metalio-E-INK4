// main/display/screen/radio_screen/radio_screen.cc —— 网络收音机界面。
// 内置 339 台目录(radio_catalog),触摸:上一台/下一台/随机/收藏/音量/列表
// 翻页;虚拟键 vk_prev/vk_next 同样翻列表页。离开页面即停播省电。
// 列表与首页同规矩:不滚动,整页重画。
#include "radio_screen/radio_screen.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <utility>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_random.h>

#include "audio_codec.h"
#include "application.h"
#include "board.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "screen_common.h"
#include "services/radio/radio_catalog.h"
#include "services/radio/radio_player.h"
#include "settings.h"
#include "vk_key_handler.h"

namespace {

constexpr const char* TAG = "RadioScreen";
constexpr const char* kScreenId = "radio";

constexpr lv_coord_t kPad = 16;
constexpr lv_coord_t kRowGap = 10;
constexpr lv_coord_t kCardPad = 12;
constexpr lv_coord_t kCardRadius = 18;
constexpr lv_coord_t kButtonH = 44;
constexpr lv_coord_t kListRowH = 32;
constexpr lv_coord_t kListRowGap = 4;
constexpr int kRowsPerPage = 10;  // 电台列表每页 10 行(需求:电台列表 6→10)
constexpr const char* kNvsNs = "radio";

struct RadioUi {
    lv_obj_t* screen = nullptr;
    lv_obj_t* station_lbl = nullptr;
    lv_obj_t* title_lbl = nullptr;
    lv_obj_t* state_lbl = nullptr;
    lv_obj_t* play_btn_lbl = nullptr;    // 停止/播放 文本
    lv_obj_t* fav_btn_lbl = nullptr;     // 收藏按钮文本(★/☆)
    lv_obj_t* page_lbl = nullptr;
    lv_obj_t* scope_btn_lbl = nullptr;   // 全部/收藏 切换文本
    lv_obj_t* list = nullptr;
    lv_obj_t* footer_lbl = nullptr;
};

RadioUi s_ui;
bool s_alive = false;
lv_timer_t* s_timer = nullptr;
int s_page = 0;
bool s_fav_only = false;          // 列表范围:全部 / 仅收藏
std::string s_playing;            // 当前台名
std::vector<std::string> s_favs;  // 收藏台名列表

const lv_font_t* UiFont() { return fontpack_lv_font_ui(); }

void DisableScroll(lv_obj_t* obj) { lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE); }

// ---- 收藏持久化:台名以 '\n' 连接存 NVS ----

// 本任务(LVGL worker)栈在 PSRAM,NVS 读写一律禁止 —— 全部走
// radio_player 的 RAM 缓存(开机 main 任务载入,写由内部栈短任务落地)。
void LoadFavs() { s_favs = RadioGetFavs(); }

bool IsFav(const std::string& name) {
    for (const auto& f : s_favs) {
        if (f == name) return true;
    }
    return false;
}

void ToggleFav(const std::string& name) {
    RadioToggleFav(name);
    s_favs = RadioGetFavs();   // 同步界面视图
}

void SaveLast(const std::string& name, const std::string& url) {
    RadioSetLast(name, url);
}

// ---- 目录/收藏视图 ----

int TotalCount() {
    return s_fav_only ? static_cast<int>(s_favs.size()) : radio_catalog_count();
}

// 视图下标 i → 目录下标(收藏视图里找不到目录的台跳过;返回 -1 忽略)
int ViewToCatalog(int i) {
    if (s_fav_only) {
        if (i < 0 || i >= static_cast<int>(s_favs.size())) return -1;
        const std::string& name = s_favs[i];
        for (int k = 0; k < radio_catalog_count(); ++k) {
            radio_station_t st;
            if (radio_catalog_get(k, &st) && name == st.name) return k;
        }
        return -1;
    }
    return i;
}

int PageCount() {
    const int total = TotalCount();
    return total <= 0 ? 1 : (total + kRowsPerPage - 1) / kRowsPerPage;
}

void ClampPage() {
    if (s_page < 0) s_page = 0;
    if (s_page >= PageCount()) s_page = PageCount() - 1;
}

// ---- 播放控制 ----

void PlayCatalog(int idx) {
    radio_station_t st;
    if (!radio_catalog_get(idx, &st)) return;
    s_playing = st.name;
    radio_play_indexed(st.name, st.url, idx);
    SaveLast(st.name, st.url);
}

// 上一台/下一台/随机都在"当前视图"(全部或仅收藏)里取台。
void StepStation(int delta) {
    const int total = TotalCount();
    if (total <= 0) return;
    int cur = 0;
    for (int i = 0; i < total; ++i) {
        const int ci = ViewToCatalog(i);
        radio_station_t st;
        if (ci >= 0 && radio_catalog_get(ci, &st) && s_playing == st.name) {
            cur = i;
            break;
        }
    }
    int next = delta >= 0 ? (cur + 1) % total : (cur + total - 1) % total;
    if (s_playing.empty() && delta == 0) next = 0;  // 首播
    const int ci = ViewToCatalog(next);
    if (ci >= 0) PlayCatalog(ci);
}

void RandomStation() {
    const int total = TotalCount();
    if (total <= 0) return;
    if (total == 1) { StepStation(1); return; }
    int pick = 0;
    for (int tries = 0; tries < 8; ++tries) {
        pick = esp_random() % total;
        const int ci = ViewToCatalog(pick);
        radio_station_t st;
        if (ci >= 0 && radio_catalog_get(ci, &st) && st.name != s_playing) break;
    }
    const int ci = ViewToCatalog(pick);
    if (ci >= 0) PlayCatalog(ci);
}

void OnPlayStopClicked(lv_event_t*) {
    radio_player_snap_t snap;
    radio_player_snapshot(&snap);
    if (snap.state == RADIO_CONNECTING || snap.state == RADIO_PLAYING) {
        radio_stop();
        s_playing.clear();
    } else {
        std::string name, url;
        RadioGetLast(name, url);
        if (!name.empty() && !url.empty()) {
            s_playing = name;
            radio_play(name.c_str(), url.c_str());
        } else {
            StepStation(1);
        }
    }
}

void SyncStateLabels();   // 前置声明:OnVolumeClicked 在定义之前就要刷新音量读数

void OnVolumeClicked(lv_event_t* e) {
    const uintptr_t delta = reinterpret_cast<uintptr_t>(lv_event_get_user_data(e));
    auto* codec = Board::GetInstance().GetAudioCodec();
    if (codec == nullptr) return;
    int vol = codec->output_volume() + ((delta != 0) ? 10 : -10);
    if (vol > 100) vol = 100;
    if (vol < 0) vol = 0;
    // SetOutputVolume 已是"立即更新成员 + NVS 去抖延迟落盘到 main"(audio_codec.cc),
    // LVGL worker(PSRAM 栈)直调安全;音量立即生效、可立刻读回 —— 不再开持久化任务。
    codec->SetOutputVolume(vol);
    SyncStateLabels();   // 立即刷新音量读数(状态行显示"音量 NN%")
}

void RefreshList() {
    if (s_ui.list == nullptr) return;
    ClampPage();
    while (lv_obj_get_child_cnt(s_ui.list) > 0) {
        lv_obj_del(lv_obj_get_child(s_ui.list, 0));
    }
    const int start = s_page * kRowsPerPage;
    for (int r = 0; r < kRowsPerPage; ++r) {
        const int view_idx = start + r;
        const int ci = ViewToCatalog(view_idx);
        if (ci < 0) continue;
        radio_station_t st;
        if (!radio_catalog_get(ci, &st)) continue;

        lv_obj_t* row = lv_obj_create(s_ui.list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), kListRowH);
        lv_obj_set_style_bg_color(row, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(row, lv_color_black(), 0);
        lv_obj_set_style_border_width(row, 2, 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_pad_left(row, 10, 0);
        lv_obj_set_style_pad_right(row, 10, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(row);
        lv_obj_add_event_cb(
            row, [](lv_event_t* e) {
                const intptr_t idx = reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
                PlayCatalog(static_cast<int>(idx));
            },
            LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(ci)));

        std::string text;
        if (s_playing == st.name) {
            text = "\xE2\x97\x8F ";  // ● 当前播放台
        } else if (IsFav(st.name)) {
            text = "\xE2\x98\x85 ";  // ★
        }
        text += st.name;
        lv_obj_t* lbl = lv_label_create(row);
        lv_label_set_text(lbl, text.c_str());
        lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(lbl, lv_pct(100));
        lv_obj_set_style_text_font(lbl, UiFont(), 0);
        lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
        lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    }

    if (s_ui.page_lbl != nullptr) {
        char buf[24];
        snprintf(buf, sizeof(buf), "%d/%d", s_page + 1, PageCount());
        lv_label_set_text(s_ui.page_lbl, buf);
    }
}

void OnPageClicked(lv_event_t* e) {
    const uintptr_t delta = reinterpret_cast<uintptr_t>(lv_event_get_user_data(e));
    s_page += (delta != 0) ? 1 : -1;
    ClampPage();
    RefreshList();
}

void OnScopeClicked(lv_event_t*) {
    s_fav_only = !s_fav_only;
    s_page = 0;
    if (s_ui.scope_btn_lbl != nullptr) {
        // 按钮固定显示"收藏夹"(功能名):点了在 全部列表 ↔ 收藏夹 之间切换,
        // 当前在哪个视图由列表内容与页码自明。
        lv_label_set_text(s_ui.scope_btn_lbl, "收藏夹");
    }
    RefreshList();
}

void OnFavClicked(lv_event_t*) {
    if (s_playing.empty()) return;
    ToggleFav(s_playing);
    RefreshList();
}

void OnPrevNextClicked(lv_event_t* e) {
    const uintptr_t delta = reinterpret_cast<uintptr_t>(lv_event_get_user_data(e));
    StepStation(delta != 0 ? 1 : -1);
}

void OnRandomClicked(lv_event_t*) { RandomStation(); }

// ---- UI 构建 ----

lv_obj_t* MakeButton(lv_obj_t* parent, const char* text, lv_coord_t width,
                     lv_event_cb_t cb, void* user_data) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, width, kButtonH);
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_width(lbl, width - 16);
    lv_obj_set_style_text_font(lbl, UiFont(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return lbl;
}

lv_obj_t* MakeRow(lv_obj_t* parent, lv_coord_t height) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), height);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    DisableScroll(row);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_CLICKABLE);
    return row;
}

void SyncStateLabels() {
    // 全量判空:任一 label 缺失就整体跳过,绝不把 NULL 喂给 lv_label_set_text
    // (NULL 会以垃圾坐标触发无限重绘,整机假死 —— 见 Create 里的注释)
    if (!s_alive || s_ui.station_lbl == nullptr || s_ui.title_lbl == nullptr ||
        s_ui.state_lbl == nullptr || s_ui.play_btn_lbl == nullptr ||
        s_ui.fav_btn_lbl == nullptr || s_ui.page_lbl == nullptr ||
        s_ui.scope_btn_lbl == nullptr) {
        return;
    }
    radio_player_snap_t snap;
    radio_player_snapshot(&snap);
    if (snap.station[0] != '\0') {
        lv_label_set_text(s_ui.station_lbl, snap.station);
        // MCP/语音路径切台不经过本文件的状态变量,这里对齐
        if (snap.state == RADIO_PLAYING || snap.state == RADIO_CONNECTING) {
            s_playing = snap.station;
        }
    }
    lv_label_set_text(s_ui.title_lbl, snap.title[0] != '\0' ? snap.title : " ");

    const char* state;
    if (snap.state == RADIO_PLAYING) {
        state = "播放中";
    } else if (snap.state == RADIO_CONNECTING) {
        state = "连接中…";
    } else if (snap.state == RADIO_ERROR) {
        state = snap.err[0] != '\0' ? snap.err : "出错";
    } else {
        state = "已停止";
    }
    // 状态行附带实时音量,让"音量变了、数字跟着变"(需求三.3)。
    // output_volume() 是纯成员读(无 flash),LVGL worker 上安全。
    int vol = 0;
    if (auto* codec = Board::GetInstance().GetAudioCodec()) {
        vol = codec->output_volume();
    }
    char state_buf[128];   // state 可能来自 snap.err(最长 95 字节),64 会截断触发 -Werror=format-truncation
    snprintf(state_buf, sizeof(state_buf), "%s · 音量 %d%%", state, vol);
    lv_label_set_text(s_ui.state_lbl, state_buf);
    lv_label_set_text(s_ui.play_btn_lbl,
                      (snap.state == RADIO_PLAYING || snap.state == RADIO_CONNECTING)
                          ? "停止" : "播放");
    lv_label_set_text(s_ui.fav_btn_lbl, IsFav(s_playing) ? "\xE2\x98\x85 已藏"
                                         : "\xE2\x98\x86 收藏");
    if (s_ui.footer_lbl != nullptr) {
        std::string foot;
        if (!Board::GetInstance().IsNetworkReady()) {
            foot = "未联网:进入设置连接 WiFi 后可收听";
        } else if (snap.state == RADIO_ERROR) {
            foot = std::string("播放失败:") + (snap.err[0] ? snap.err : "未知错误");
        } else if (snap.state == RADIO_PLAYING || snap.state == RADIO_CONNECTING) {
            foot = "正在收听 " + s_playing;
        } else {
            foot = "点「播放」续听上次电台,或从列表选台";
        }
        lv_label_set_text(s_ui.footer_lbl, foot.c_str());
    }
}

void OnTimer(lv_timer_t*) { SyncStateLabels(); }

bool OnVkKey(const char* key) {
    if (key == nullptr || !s_alive) return false;
    // 盖板触摸 vk_prev/vk_next 在收音机屏=切上一台/下一台(实体音量键仍调音量,
    // 不经此处);列表翻页用屏内 </> 按钮。
    // return true 消费掉,避免 vk_prev 落到 DispatchDefault 的"返回上级"。
    if (strcmp(key, "vk_prev") == 0) {
        StepStation(-1);
        return true;
    }
    if (strcmp(key, "vk_next") == 0) {
        StepStation(1);
        return true;
    }
    return false;
}

void OnScreenUnloaded(lv_event_t*) {
    s_alive = false;
    radio_stop();           // 先投停止请求,让流尽快结束(LVGL 线程上即可,无 flash 操作)
    radio_player_shutdown(); // 非阻塞置退出标志,任务异步自删释放 8K PSRAM 栈。
    // 同步置位很关键:unload 立刻置 s_quit,紧随其后的播放请求会在 radio_player_start
    // 里等到旧任务退出再重建 —— 不会丢请求。此处不再 Schedule(本身不阻塞)。
    s_playing.clear();
    if (s_timer != nullptr) {
        lv_timer_delete(s_timer);
        s_timer = nullptr;
    }
    s_ui = {};
}

}  // namespace

lv_obj_t* RadioScreen::Create() {
    s_ui = {};
    s_alive = true;
    s_page = 0;
    LoadFavs();
    // 导航框架靠这个标志区分"首页/子页"。所有子屏都调了(assistant/book/cloud/
    // glm/settings/wallpaper...),漏掉会让导航把它当首页处理 -> 点了进不去。
    ScreenSetIsHome(false);

    lv_obj_t* screen = lv_obj_create(nullptr);
    s_ui.screen = screen;
    lv_obj_remove_style_all(screen);
    lv_obj_set_size(screen, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);
    lv_obj_set_style_text_font(screen, UiFont(), 0);
    DisableScroll(screen);
    lv_obj_add_event_cb(screen, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);

    const EpdStatusBar status = ScreenCreateStatusBar(screen);
    lv_obj_t* body = lv_obj_create(screen);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, LV_VER_RES - status.height);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, status.height);
    lv_obj_set_style_pad_all(body, kPad, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, kRowGap, 0);
    DisableScroll(body);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);

    // 正在收听卡片
    lv_obj_t* card = lv_obj_create(body);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, 132);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, kCardRadius, 0);
    lv_obj_set_style_pad_all(card, kCardPad, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 6, 0);
    DisableScroll(card);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);

    s_ui.station_lbl = ScreenMakeLabel(card, "收音机", 30, false);
    s_ui.title_lbl = ScreenMakeLabel(card, " ", 25, false);
    s_ui.state_lbl = ScreenMakeLabel(card, "已停止", 25, false);

    // 换台行
    constexpr lv_coord_t kBtnW3 = 136;
    lv_obj_t* row1 = MakeRow(body, kButtonH);
    MakeButton(row1, "上一台", kBtnW3, OnPrevNextClicked, (void*)0);
    // MakeButton 返回的就是 label 本身,不能再 get_child(0)(label 无子对象,
    // 返回 NULL → 后续 lv_label_set_text(NULL) 把垃圾坐标喂进渲染器,收音机页
    // 一打开就陷入无限重绘,整机假死 —— 实测 OpenOCD 采样定位)。
    s_ui.play_btn_lbl = MakeButton(row1, "播放", kBtnW3, OnPlayStopClicked, nullptr);
    MakeButton(row1, "下一台", kBtnW3, OnPrevNextClicked, (void*)1);

    // 随机/收藏/音量行
    constexpr lv_coord_t kBtnW4 = 100;
    lv_obj_t* row2 = MakeRow(body, kButtonH);
    MakeButton(row2, "随机", kBtnW4, OnRandomClicked, nullptr);
    s_ui.fav_btn_lbl = MakeButton(row2, "\xE2\x98\x86 收藏", kBtnW4, OnFavClicked, nullptr);
    MakeButton(row2, "音量-", kBtnW4, OnVolumeClicked, (void*)0);
    MakeButton(row2, "音量+", kBtnW4, OnVolumeClicked, (void*)1);

    // 列表头:翻页 + 范围切换
    lv_obj_t* row3 = MakeRow(body, kButtonH);
    MakeButton(row3, "<", 90, OnPageClicked, (void*)0);
    // 页码是纯展示,不做成按钮(原来是个点了没反应的假按钮,易误以为坏了)。
    s_ui.page_lbl = lv_label_create(row3);
    lv_label_set_text(s_ui.page_lbl, "1/1");
    lv_obj_set_width(s_ui.page_lbl, 90);
    lv_obj_set_style_text_align(s_ui.page_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_ui.page_lbl, UiFont(), 0);
    lv_obj_set_style_text_color(s_ui.page_lbl, lv_color_black(), 0);
    MakeButton(row3, ">", 90, OnPageClicked, (void*)1);
    // 默认显示"全部"列表;按钮固定叫"收藏夹",点了在 全部 ↔ 收藏夹 间切换。
    s_ui.scope_btn_lbl = MakeButton(row3, "收藏夹", 110, OnScopeClicked, nullptr);

    // 电台列表(6 行整页重画,不滚动)
    s_ui.list = lv_obj_create(body);
    lv_obj_remove_style_all(s_ui.list);
    lv_obj_set_size(s_ui.list, lv_pct(100),
                    kRowsPerPage * kListRowH + (kRowsPerPage - 1) * kListRowGap);
    lv_obj_set_flex_flow(s_ui.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_ui.list, kListRowGap, 0);
    DisableScroll(s_ui.list);
    lv_obj_clear_flag(s_ui.list, LV_OBJ_FLAG_CLICKABLE);
    RefreshList();

    s_ui.footer_lbl = lv_label_create(body);
    lv_label_set_text(s_ui.footer_lbl, "");
    lv_obj_set_width(s_ui.footer_lbl, lv_pct(100));
    lv_obj_set_flex_grow(s_ui.footer_lbl, 1);
    lv_label_set_long_mode(s_ui.footer_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_ui.footer_lbl, UiFont(), 0);
    lv_obj_set_style_text_color(s_ui.footer_lbl, lv_color_black(), 0);
    lv_obj_clear_flag(s_ui.footer_lbl, LV_OBJ_FLAG_CLICKABLE);

    SyncStateLabels();
    s_timer = lv_timer_create(OnTimer, 1000, nullptr);
    // 进收音机页即停掉语音管线(唤醒词/AFE/opus/采麦任务),把内存让给收音机的
    // 流拉取与可能的 TLS。语音页/其它页再进时各自 EnsureAudioServiceRunning 重建。
    // StopAudioServiceIfRunning 内部 Deinitialize AFE 会碰 flash,必须走 main 任务
    // 内部栈;本回调在 LVGL worker(PSRAM 栈),故 Schedule 上去。
    Application::GetInstance().Schedule([]() {
        Application::GetInstance().StopAudioServiceIfRunning();
    });
    VkKey_AttachScreen(screen, kScreenId, VkKeyScreenDesc{RadioScreen::Create, OnVkKey});
    return screen;
}
