#include "wifi_transfer_screen/wifi_transfer_screen.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <esp_log.h>
#include <esp_timer.h>

#include "board.h"
#include "display/modal_manager.h"
#include "display/status_bar_manager.h"
#include "display/screen/epd_widget_kit.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "screen_common.h"
#include "SdCardManager.hpp"
#include "sd_paths.h"
#include "services/wifi_transfer/wifi_transfer_service.h"
#include "reader/file_size.h"
#include "vk_key_handler.h"
#include "core/system_event_bus.h"
#include "ff.h"

namespace {

constexpr const char* TAG = "WifiTransferScreen";
constexpr const char* kScreenId = "wifi_transfer";

struct UiState {
    lv_obj_t* screen = nullptr;
    EpdWidgetKit::EpdPageScaffold* scaffold = nullptr;
    lv_obj_t* wifi_status_lbl = nullptr;
    lv_obj_t* url_lbl = nullptr;
    lv_obj_t* switch_btn = nullptr;
    lv_obj_t* switch_btn_lbl = nullptr;
    lv_obj_t* qr_card = nullptr;
    lv_obj_t* qr_obj = nullptr;
    lv_obj_t* qr_tip_lbl = nullptr;
    lv_obj_t* log_lbl = nullptr;
    lv_obj_t* storage_lbl = nullptr;
};

UiState s_ui;
bool s_alive = false;
SubscriptionToken s_wifi_sub_token = 0;
std::string s_recent_log = "服务就绪，等待文件传输...";

const lv_font_t* UiFont() { return fontpack_lv_font_ui(); }
void DisableScroll(lv_obj_t* o) { if (o) lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE); }

lv_obj_t* MakeCard(lv_obj_t* parent, lv_coord_t pad = 12) {
    lv_obj_t* card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 12, 0);
    lv_obj_set_style_pad_all(card, pad, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 6, 0);
    DisableScroll(card);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);
    return card;
}

lv_obj_t* CreateCustomBtn(lv_obj_t* parent, const char* text, lv_coord_t w, lv_coord_t h,
                          lv_event_cb_t cb, void* user_data, lv_obj_t** out_lbl = nullptr) {
    lv_obj_t* btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    HapticAttachClick(btn);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t* lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_font(lbl, UiFont(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    if (out_lbl) {
        *out_lbl = lbl;
    }
    return btn;
}

void RefreshUi();

void OnToggleServerClicked(lv_event_t* /*e*/) {
    HapticPulseIfEnabled();
    auto& svc = wifi_transfer::WifiTransferService::GetInstance();
    if (svc.IsRunning()) {
        svc.Stop();
        StatusBarManager::GetInstance().ShowToast("WiFi 传书服务已停止", 1500);
    } else {
        if (!Board::GetInstance().IsNetworkReady()) {
            ModalManager::GetInstance().ShowAlert("提示", "当前设备未连接 WiFi，请先进入设置连接 WiFi 后再启动传书服务。", "我知道了");
            return;
        }
        if (svc.Start()) {
            StatusBarManager::GetInstance().ShowToast("WiFi 传书服务已启动", 1500);
        } else {
            ModalManager::GetInstance().ShowAlert("错误", "启动传书服务失败，端口可能已被占用", "确定");
        }
    }
    RefreshUi();
}

void RefreshUi() {
    if (!s_alive || s_ui.screen == nullptr) return;

    auto& board = Board::GetInstance();
    auto& svc = wifi_transfer::WifiTransferService::GetInstance();
    const bool is_net = board.IsNetworkReady();
    const bool running = svc.IsRunning();
    std::string ip = svc.GetLocalIp();
    std::string url = svc.GetServerUrl();

    // 1. WiFi 状态与 URL
    if (s_ui.wifi_status_lbl != nullptr) {
        if (is_net && !ip.empty()) {
            char buf[64];
            snprintf(buf, sizeof(buf), "WiFi 状态: 已连接 (IP: %s)", ip.c_str());
            lv_label_set_text(s_ui.wifi_status_lbl, buf);
        } else {
            lv_label_set_text(s_ui.wifi_status_lbl, "WiFi 状态: 未连接网络 (请在设置中连接 WiFi)");
        }
    }

    if (s_ui.url_lbl != nullptr) {
        if (running && !url.empty()) {
            lv_label_set_text(s_ui.url_lbl, url.c_str());
        } else if (!is_net) {
            lv_label_set_text(s_ui.url_lbl, "未连接 WiFi");
        } else {
            lv_label_set_text(s_ui.url_lbl, "服务未运行 (请点击下方启动)");
        }
    }

    if (s_ui.switch_btn_lbl != nullptr) {
        lv_label_set_text(s_ui.switch_btn_lbl, running ? "停止传书服务" : "启动传书服务");
    }

    // 2. 二维码展示与更新
    if (s_ui.qr_obj != nullptr) {
        if (running && !url.empty()) {
            lv_qrcode_update(s_ui.qr_obj, url.c_str(), url.size());
            lv_obj_clear_flag(s_ui.qr_obj, LV_OBJ_FLAG_HIDDEN);
            if (s_ui.qr_tip_lbl != nullptr) {
                lv_label_set_text(s_ui.qr_tip_lbl, "手机微信 / 相机扫码打开");
            }
        } else {
            lv_obj_add_flag(s_ui.qr_obj, LV_OBJ_FLAG_HIDDEN);
            if (s_ui.qr_tip_lbl != nullptr) {
                lv_label_set_text(s_ui.qr_tip_lbl, "服务未运行，暂无二维码");
            }
        }
    }

    // 3. 动态日志
    if (s_ui.log_lbl != nullptr) {
        lv_label_set_text(s_ui.log_lbl, s_recent_log.c_str());
    }

    // 4. 存储容量
    if (s_ui.storage_lbl != nullptr) {
        auto& sd = SdCardManager::GetInstance();
        if (sd.IsMounted()) {
            FATFS* fs = nullptr;
            DWORD free_clusters = 0;
            if (f_getfree("0:", &free_clusters, &fs) == FR_OK && fs != nullptr) {
                constexpr DWORD kSectorSize = 512;
                uint64_t total_bytes = static_cast<uint64_t>(fs->n_fatent - 2) * fs->csize * kSectorSize;
                uint64_t free_bytes = static_cast<uint64_t>(free_clusters) * fs->csize * kSectorSize;
                char total_str[32];
                char free_str[32];
                reader::FormatFileSize(total_str, sizeof(total_str), total_bytes);
                reader::FormatFileSize(free_str, sizeof(free_str), free_bytes);
                char line[128];
                snprintf(line, sizeof(line), "SD 卡容量: 可用 %s / 总共 %s", free_str, total_str);
                lv_label_set_text(s_ui.storage_lbl, line);
            } else {
                lv_label_set_text(s_ui.storage_lbl, "SD 卡状态: 已挂载");
            }
        } else {
            lv_label_set_text(s_ui.storage_lbl, "SD 卡状态: 未检测到 SD 卡 / 未挂载");
        }
    }

    if (s_ui.scaffold != nullptr && s_ui.scaffold->GetScrollView() != nullptr) {
        s_ui.scaffold->GetScrollView()->UpdatePageInfo();
    }
}

void OnScreenUnloaded(lv_event_t* /*e*/) {
    s_alive = false;
    if (s_wifi_sub_token != 0) {
        SystemEventBus::GetInstance().Unsubscribe(s_wifi_sub_token);
        s_wifi_sub_token = 0;
    }
    // 离屏停止服务，释放所有内存和端口资源，做到 0 常驻内存
    wifi_transfer::WifiTransferService::GetInstance().Stop();
    wifi_transfer::WifiTransferService::GetInstance().SetUploadCallback(nullptr);
    s_ui = {};
}

}  // namespace

lv_obj_t* WifiTransferScreen::Create() {
    s_ui = {};
    s_alive = true;
    ScreenSetIsHome(false);

    auto* scaffold = EpdWidgetKit::EpdPageScaffold::Create({
        .title = "WiFi 传书",
        .show_status_bar = true,
        .show_back_btn = true,
        .scrollable = true,
        .pad_h = 14,
        .pad_v = 10,
        .row_gap = 10,
    });
    s_ui.scaffold = scaffold;
    s_ui.screen = scaffold->GetScreen();
    lv_obj_t* body = scaffold->GetContentContainer();
    lv_obj_add_event_cb(s_ui.screen, OnScreenUnloaded, LV_EVENT_SCREEN_UNLOADED, nullptr);

    // 头部右侧挂载刷新按钮
    lv_obj_t* head = scaffold->GetHeader();
    if (head != nullptr) {
        lv_obj_t* ref_btn = CreateCustomBtn(head, "刷新", 80, 38, [](lv_event_t*) {
            RefreshUi();
            StatusBarManager::GetInstance().ShowToast("已刷新状态", 1000);
        }, nullptr);
        lv_obj_set_style_radius(ref_btn, LV_RADIUS_CIRCLE, 0);
    }

    const lv_font_t* title_font = fontpack_lv_font_get(24, 2);

    // 顶部双列并排容器 (左侧输入网址卡片 475px，右侧扫码卡片 280px)
    lv_obj_t* top_row = lv_obj_create(body);
    lv_obj_remove_style_all(top_row);
    lv_obj_set_width(top_row, lv_pct(100));
    lv_obj_set_height(top_row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top_row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(top_row, LV_OBJ_FLAG_SCROLLABLE);

    // 1. 左侧卡片：电脑/手机浏览器网址访问
    lv_obj_t* left_card = MakeCard(top_row, 12);
    lv_obj_set_width(left_card, 475);
    lv_obj_set_style_pad_row(left_card, 5, 0);

    lv_obj_t* title_left = lv_label_create(left_card);
    lv_label_set_text(title_left, "电脑 / 手机浏览器访问");
    lv_obj_set_style_text_font(title_left, title_font ? title_font : UiFont(), 0);
    lv_obj_set_style_text_color(title_left, lv_color_black(), 0);

    s_ui.wifi_status_lbl = lv_label_create(left_card);
    lv_obj_set_style_text_font(s_ui.wifi_status_lbl, UiFont(), 0);
    lv_obj_set_style_text_color(s_ui.wifi_status_lbl, lv_color_black(), 0);

    lv_obj_t* tip_h = lv_label_create(left_card);
    lv_label_set_text(tip_h, "同局域网浏览器输入下方网址即可传书:");
    lv_obj_set_style_text_font(tip_h, UiFont(), 0);
    lv_obj_set_style_text_color(tip_h, lv_color_black(), 0);

    s_ui.url_lbl = lv_label_create(left_card);
    const lv_font_t* big_f = fontpack_lv_font_get(28, 2);
    lv_obj_set_style_text_font(s_ui.url_lbl, big_f ? big_f : UiFont(), 0);
    lv_obj_set_style_text_color(s_ui.url_lbl, lv_color_black(), 0);
    lv_obj_set_width(s_ui.url_lbl, lv_pct(100));
    lv_label_set_long_mode(s_ui.url_lbl, LV_LABEL_LONG_WRAP);

    s_ui.switch_btn = CreateCustomBtn(left_card, "启动传书服务", lv_pct(100), 40,
                                      OnToggleServerClicked, nullptr, &s_ui.switch_btn_lbl);
    lv_obj_set_style_border_width(s_ui.switch_btn, 2, 0);

    s_ui.storage_lbl = lv_label_create(left_card);
    lv_obj_set_width(s_ui.storage_lbl, lv_pct(100));
    lv_obj_set_style_text_font(s_ui.storage_lbl, UiFont(), 0);
    lv_obj_set_style_text_color(s_ui.storage_lbl, lv_color_black(), 0);

    lv_obj_t* fmt_lbl = lv_label_create(left_card);
    lv_label_set_text(fmt_lbl, "支持格式: .txt / .epub / .ebook / .pdf (直存图书库)");
    lv_obj_set_style_text_font(fmt_lbl, UiFont(), 0);
    lv_obj_set_style_text_color(fmt_lbl, lv_color_black(), 0);

    // 2. 右侧卡片：手机扫码直达 (直接相机/微信扫一扫打开网页上传)
    s_ui.qr_card = MakeCard(top_row, 10);
    lv_obj_set_width(s_ui.qr_card, 280);
    lv_obj_set_flex_align(s_ui.qr_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_ui.qr_card, 4, 0);

    lv_obj_t* qr_title = lv_label_create(s_ui.qr_card);
    lv_label_set_text(qr_title, "手机扫码直达");
    lv_obj_set_style_text_font(qr_title, title_font ? title_font : UiFont(), 0);
    lv_obj_set_style_text_color(qr_title, lv_color_black(), 0);

    s_ui.qr_tip_lbl = lv_label_create(s_ui.qr_card);
    lv_label_set_text(s_ui.qr_tip_lbl, "手机微信 / 相机扫码打开");
    lv_obj_set_style_text_font(s_ui.qr_tip_lbl, UiFont(), 0);
    lv_obj_set_style_text_color(s_ui.qr_tip_lbl, lv_color_black(), 0);

    // 二维码白底黑框包装容器
    lv_obj_t* qr_box = lv_obj_create(s_ui.qr_card);
    lv_obj_remove_style_all(qr_box);
    lv_obj_set_size(qr_box, 196, 196);
    lv_obj_set_style_bg_color(qr_box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(qr_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(qr_box, lv_color_black(), 0);
    lv_obj_set_style_border_width(qr_box, 2, 0);
    lv_obj_set_style_radius(qr_box, 8, 0);
    lv_obj_clear_flag(qr_box, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.qr_obj = lv_qrcode_create(qr_box);
    lv_qrcode_set_size(s_ui.qr_obj, 184);
    lv_qrcode_set_dark_color(s_ui.qr_obj, lv_color_black());
    lv_qrcode_set_light_color(s_ui.qr_obj, lv_color_white());
    lv_obj_center(s_ui.qr_obj);

    lv_obj_t* qr_subtip = lv_label_create(s_ui.qr_card);
    lv_label_set_text(qr_subtip, "免输网址 · 手机直接选书上传");
    lv_obj_set_style_text_font(qr_subtip, UiFont(), 0);
    lv_obj_set_style_text_color(qr_subtip, lv_color_black(), 0);

    // 3. 底部卡片：实时动态记录
    lv_obj_t* bottom_card = MakeCard(body, 10);
    lv_obj_set_width(bottom_card, lv_pct(100));
    lv_obj_set_style_pad_row(bottom_card, 4, 0);

    lv_obj_t* ltitle = lv_label_create(bottom_card);
    lv_label_set_text(ltitle, "实时传输动态");
    lv_obj_set_style_text_font(ltitle, title_font ? title_font : UiFont(), 0);
    lv_obj_set_style_text_color(ltitle, lv_color_black(), 0);

    s_ui.log_lbl = lv_label_create(bottom_card);
    lv_obj_set_width(s_ui.log_lbl, lv_pct(100));
    lv_label_set_long_mode(s_ui.log_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_ui.log_lbl, UiFont(), 0);
    lv_obj_set_style_text_color(s_ui.log_lbl, lv_color_black(), 0);

    lv_obj_t* hint = lv_label_create(bottom_card);
    lv_label_set_text(hint, "💡 提示: 退出本界面将自动关闭 Web 传书服务，零常驻后台内存。");
    lv_obj_set_width(hint, lv_pct(100));
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(hint, UiFont(), 0);
    lv_obj_set_style_text_color(hint, lv_color_black(), 0);

    // 注册上传事件回调
    wifi_transfer::WifiTransferService::GetInstance().SetUploadCallback([](const wifi_transfer::UploadEvent& ev) {
        struct EvMsg {
            std::string filename;
            size_t size;
        };
        auto* msg = new EvMsg{ev.filename, ev.size_bytes};
        ScreenLvAsync([](void* p) {
            auto* m = static_cast<EvMsg*>(p);
            if (s_alive) {
                char sz_str[32];
                reader::FormatFileSize(sz_str, sizeof(sz_str), m->size);
                char line[128];
                snprintf(line, sizeof(line), "✓ 成功接收: %s (%s)", m->filename.c_str(), sz_str);
                s_recent_log = line;
                if (s_ui.log_lbl) {
                    lv_label_set_text(s_ui.log_lbl, s_recent_log.c_str());
                }
                char toast[64];
                snprintf(toast, sizeof(toast), "已收到新书: %s", m->filename.c_str());
                StatusBarManager::GetInstance().ShowToast(toast, 2500);
            }
            delete m;
        }, msg);
    });

    // 订阅 WiFi 状态变化事件，实现网络连接后全自动拉起服务并生成二维码
    s_wifi_sub_token = SystemEventBus::GetInstance().Subscribe(SystemEventType::kWifiState, [](const SystemEvent& ev) {
        if (ev.wifi.state == SystemWifiState::kConnected) {
            ScreenLvAsync([](void*) {
                if (s_alive) {
                    auto& svc = wifi_transfer::WifiTransferService::GetInstance();
                    if (!svc.IsRunning()) {
                        svc.Start();
                    }
                    RefreshUi();
                }
            }, nullptr);
        } else if (ev.wifi.state == SystemWifiState::kDisconnected) {
            ScreenLvAsync([](void*) {
                if (s_alive) {
                    RefreshUi();
                }
            }, nullptr);
        }
    });

    // 默认进入页面自动启动服务
    if (Board::GetInstance().IsNetworkReady()) {
        wifi_transfer::WifiTransferService::GetInstance().Start();
    }

    RefreshUi();

    VkKey_AttachScreen(s_ui.screen, kScreenId, VkKeyScreenDesc{WifiTransferScreen::Create, nullptr});
    return s_ui.screen;
}
