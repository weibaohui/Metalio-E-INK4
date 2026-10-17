#include "wifi_file_screen/wifi_file_screen.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <esp_log.h>

#include "board.h"
#include "display/modal_manager.h"
#include "display/status_bar_manager.h"
#include "display/screen/epd_widget_kit.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "screen_common.h"
#include "boards/common/SdCardManager.hpp"
#include "services/wifi_file/wifi_file_service.h"
#include "reader_core/file_size.h"
#include "vk_key_handler.h"
#include "core/system_event_bus.h"
#include "ff.h"

namespace {

constexpr const char* TAG = "WifiFileScreen";
constexpr const char* kScreenId = "wifi_file";

struct UiState {
    lv_obj_t* screen = nullptr;
    EpdWidgetKit::EpdPageScaffold* scaffold = nullptr;
    lv_obj_t* wifi_status_lbl = nullptr;
    lv_obj_t* url_lbl = nullptr;
    lv_obj_t* switch_btn_lbl = nullptr;
    lv_obj_t* qr_obj = nullptr;
    lv_obj_t* qr_tip_lbl = nullptr;
    lv_obj_t* log_lbl = nullptr;
    lv_obj_t* storage_lbl = nullptr;
};

UiState s_ui;
bool s_alive = false;
SubscriptionToken s_wifi_sub_token = 0;
std::string s_recent_log = "服务就绪，点击下方按钮启动管理";

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
    auto& svc = wifi_file::WifiFileService::GetInstance();
    if (svc.IsRunning()) {
        svc.Stop();
        s_recent_log = "文件管理服务已停止";
        StatusBarManager::GetInstance().ShowToast("WiFi 文件服务已停止", 1500);
    } else {
        if (!Board::GetInstance().IsNetworkReady()) {
            ModalManager::GetInstance().ShowAlert("提示", "当前设备未连接 WiFi，请先进入设置连接 WiFi 后再启动文件管理服务。", "确定");
            return;
        }
        if (svc.Start()) {
            s_recent_log = "服务已启动，请在浏览器中输入网址访问";
            StatusBarManager::GetInstance().ShowToast("WiFi 文件服务已启动", 1500);
        } else {
            ModalManager::GetInstance().ShowAlert("错误", "启动服务失败，端口 8080 可能已被占用", "确定");
        }
    }
    RefreshUi();
}

void RefreshUi() {
    if (!s_alive || s_ui.screen == nullptr) return;

    auto& board = Board::GetInstance();
    auto& svc = wifi_file::WifiFileService::GetInstance();
    const bool is_net = board.IsNetworkReady();
    const bool running = svc.IsRunning();
    std::string ip = svc.GetLocalIp();
    std::string url = svc.GetServerUrl();

    // 1. WiFi 状态与 URL
    if (s_ui.wifi_status_lbl != nullptr) {
        if (is_net && !ip.empty()) {
            char buf[64];
            std::snprintf(buf, sizeof(buf), "WiFi 状态: 已连接 (IP: %s)", ip.c_str());
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
        lv_label_set_text(s_ui.switch_btn_lbl, running ? "停止文件管理服务" : "启动文件管理服务");
    }

    // 2. 二维码展示
    if (s_ui.qr_obj != nullptr) {
        if (running && !url.empty()) {
            lv_qrcode_update(s_ui.qr_obj, url.c_str(), url.size());
            lv_obj_clear_flag(s_ui.qr_obj, LV_OBJ_FLAG_HIDDEN);
            if (s_ui.qr_tip_lbl != nullptr) {
                lv_label_set_text(s_ui.qr_tip_lbl, "手机微信 / 浏览器相机扫码打开");
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
                std::snprintf(line, sizeof(line), "SD 卡容量: 可用 %s / 总共 %s", free_str, total_str);
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

void CleanupWifiFile() {
    if (!s_alive) return;
    s_alive = false;
    ESP_LOGI(TAG, "WiFi 文件管理应用关闭，停用服务并释放资源");
    if (s_wifi_sub_token != 0) {
        SystemEventBus::GetInstance().Unsubscribe(s_wifi_sub_token);
        s_wifi_sub_token = 0;
    }
    wifi_file::WifiFileService::GetInstance().Stop();
    wifi_file::WifiFileService::GetInstance().SetActivityCallback(nullptr);
    s_ui = UiState{};
}

void OnScreenLifecycleCb(lv_event_t* /*e*/) {
    CleanupWifiFile();
}

bool WifiFileOnVkKey(const char* key) {
    if (key == nullptr) return false;
    if (std::strcmp(key, "vk_home") == 0) {
        ScreenRequestBack();
        return true;
    }
    return false;
}

}  // namespace

lv_obj_t* WifiFileScreen::Create() {
    s_alive = true;
    s_ui = UiState{};
    ScreenSetIsHome(false);

    auto* scaffold = EpdWidgetKit::EpdPageScaffold::Create({
        .title = "WiFi 文件管理",
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

    lv_obj_add_event_cb(s_ui.screen, OnScreenLifecycleCb, LV_EVENT_SCREEN_UNLOADED, nullptr);
    lv_obj_add_event_cb(s_ui.screen, OnScreenLifecycleCb, LV_EVENT_SCREEN_UNLOAD_START, nullptr);
    lv_obj_add_event_cb(s_ui.screen, OnScreenLifecycleCb, LV_EVENT_DELETE, nullptr);

    // 1. 访问卡片
    lv_obj_t* access_card = MakeCard(body, 14);
    s_ui.wifi_status_lbl = lv_label_create(access_card);
    lv_label_set_text(s_ui.wifi_status_lbl, "WiFi 状态: 检测中...");
    lv_obj_set_style_text_font(s_ui.wifi_status_lbl, UiFont(), 0);
    lv_obj_set_style_text_color(s_ui.wifi_status_lbl, lv_color_black(), 0);

    lv_obj_t* url_tip = lv_label_create(access_card);
    lv_label_set_text(url_tip, "浏览器管理网址 (支持电脑与手机):");
    lv_obj_set_style_text_font(url_tip, UiFont(), 0);
    lv_obj_set_style_text_color(url_tip, lv_color_make(100, 100, 100), 0);

    s_ui.url_lbl = lv_label_create(access_card);
    lv_label_set_text(s_ui.url_lbl, "http://...");
    lv_obj_set_style_text_font(s_ui.url_lbl, UiFont(), 0);
    lv_obj_set_style_text_color(s_ui.url_lbl, lv_color_black(), 0);
    lv_obj_set_style_pad_ver(s_ui.url_lbl, 2, 0);

    // 开关按钮
    CreateCustomBtn(access_card, "启动文件管理服务", lv_pct(100), 42,
                    OnToggleServerClicked, nullptr, &s_ui.switch_btn_lbl);

    // 2. 二维码卡片
    lv_obj_t* qr_card = MakeCard(body, 12);
    lv_obj_set_width(qr_card, lv_pct(100));
    lv_obj_set_flex_align(qr_card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(qr_card, 6, 0);

    lv_obj_t* qr_box = lv_obj_create(qr_card);
    lv_obj_remove_style_all(qr_box);
    lv_obj_set_size(qr_box, 180, 180);
    lv_obj_set_style_bg_color(qr_box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(qr_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(qr_box, lv_color_black(), 0);
    lv_obj_set_style_border_width(qr_box, 2, 0);
    lv_obj_set_style_radius(qr_box, 8, 0);
    lv_obj_clear_flag(qr_box, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.qr_obj = lv_qrcode_create(qr_box);
    lv_qrcode_set_size(s_ui.qr_obj, 166);
    lv_qrcode_set_dark_color(s_ui.qr_obj, lv_color_black());
    lv_qrcode_set_light_color(s_ui.qr_obj, lv_color_white());
    lv_obj_center(s_ui.qr_obj);

    s_ui.qr_tip_lbl = lv_label_create(qr_card);
    lv_label_set_text(s_ui.qr_tip_lbl, "服务未运行，暂无二维码");
    lv_obj_set_style_text_font(s_ui.qr_tip_lbl, UiFont(), 0);
    lv_obj_set_style_text_color(s_ui.qr_tip_lbl, lv_color_make(100, 100, 100), 0);

    // 3. 说明与动态
    lv_obj_t* info_card = MakeCard(body, 14);
    lv_obj_t* info_title = lv_label_create(info_card);
    lv_label_set_text(info_title, "功能提示与动态");
    lv_obj_set_style_text_font(info_title, UiFont(), 0);
    lv_obj_set_style_text_color(info_title, lv_color_black(), 0);

    lv_obj_t* info_desc = lv_label_create(info_card);
    lv_label_set_text(info_desc, "• 支持全盘浏览、新建文件夹、文件上传/下载/删除\n• 壁纸请存入: /sdcard/metalio/wallpaper\n• 电子书存入: /sdcard/metalio/books");
    lv_obj_set_style_text_font(info_desc, UiFont(), 0);
    lv_obj_set_style_text_color(info_desc, lv_color_make(80, 80, 80), 0);

    s_ui.log_lbl = lv_label_create(info_card);
    lv_label_set_text(s_ui.log_lbl, s_recent_log.c_str());
    lv_obj_set_style_text_font(s_ui.log_lbl, UiFont(), 0);
    lv_obj_set_style_text_color(s_ui.log_lbl, lv_color_black(), 0);
    lv_label_set_long_mode(s_ui.log_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_ui.log_lbl, lv_pct(100));

    s_ui.storage_lbl = lv_label_create(info_card);
    lv_label_set_text(s_ui.storage_lbl, "SD 卡容量: 读取中...");
    lv_obj_set_style_text_font(s_ui.storage_lbl, UiFont(), 0);
    lv_obj_set_style_text_color(s_ui.storage_lbl, lv_color_make(100, 100, 100), 0);

    // 注册网络与活动监听
    s_wifi_sub_token = SystemEventBus::GetInstance().Subscribe(SystemEventType::kWifiState, [](const SystemEvent& /*ev*/) {
        ScreenLvAsync([](void*) {
            if (s_alive) {
                RefreshUi();
            }
        }, nullptr);
    });

    wifi_file::WifiFileService::GetInstance().SetActivityCallback([](const std::string& action, const std::string& path) {
        ScreenLvAsync([](void* p) {
            auto* s = static_cast<std::string*>(p);
            s_recent_log = *s;
            delete s;
            RefreshUi();
        }, new std::string(action + ": " + path));
    });

    // 默认自动启动服务（如果网络就绪）
    if (Board::GetInstance().IsNetworkReady()) {
        wifi_file::WifiFileService::GetInstance().Start();
    }

    RefreshUi();

    VkKey_AttachScreen(s_ui.screen, kScreenId, VkKeyScreenDesc{WifiFileScreen::Create, WifiFileOnVkKey, nullptr, nullptr});
    return s_ui.screen;
}
