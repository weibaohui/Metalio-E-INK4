#include "system_upgrade_screen/system_upgrade_screen.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "board.h"
#include "display/modal_manager.h"
#include "display/status_bar_manager.h"
#include "display/screen/epd_widget_kit.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "ota_upgrade_screen/ota_upgrade_screen.h"
#include "reader/file_size.h"
#include "screen_common.h"
#include "services/system_upgrade/system_upgrade_service.h"
#include "vk_key_handler.h"

namespace {

constexpr const char* TAG = "SysUpgradeScreen";
constexpr const char* kScreenId = "sys_upgrade";

struct UpgradeUi {
    lv_obj_t* screen = nullptr;
    lv_obj_t* body = nullptr;
    EpdWidgetKit::EpdPageScaffold* scaffold = nullptr;

    // 状态卡片
    lv_obj_t* status_card = nullptr;
    lv_obj_t* cur_ver_lbl = nullptr;
    lv_obj_t* part_info_lbl = nullptr;

    // WiFi Web OTA 卡片
    lv_obj_t* web_card = nullptr;
    lv_obj_t* wifi_status_lbl = nullptr;
    lv_obj_t* url_lbl = nullptr;
    lv_obj_t* switch_btn = nullptr;
    lv_obj_t* switch_btn_lbl = nullptr;
    lv_obj_t* qr_box = nullptr;
    lv_obj_t* qr_obj = nullptr;
    lv_obj_t* qr_tip_lbl = nullptr;

    // SD 卡固件列表容器
    lv_obj_t* list_container = nullptr;
};

UpgradeUi s_ui;
bool s_alive = false;
bool s_flashing_busy = false;

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
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_all(card, pad, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 6, 0);
    DisableScroll(card);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_CLICKABLE);
    return card;
}

void MakeSection(lv_obj_t* parent, const char* title) {
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(bar, 6, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* lbl = lv_label_create(bar);
    lv_label_set_text(lbl, title);
    lv_obj_set_width(lbl, lv_pct(100));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lbl, UiFont(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
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

// 停止 Web 服务并清理
void CleanupUpgradeService() {
    system_upgrade::SystemUpgradeService::GetInstance().StopWebServer();
}

// 刷新状态与 Web 提示
void RefreshWebStatus() {
    if (!s_alive) return;
    auto& svc = system_upgrade::SystemUpgradeService::GetInstance();
    auto& board = Board::GetInstance();
    bool net_ready = board.IsNetworkReady();
    bool running = svc.IsWebServerRunning();

    if (s_ui.wifi_status_lbl != nullptr) {
        if (!net_ready) {
            lv_label_set_text(s_ui.wifi_status_lbl, "网络状态: Wi-Fi 未连接 (请先在「网络设置」连接)");
        } else if (running) {
            lv_label_set_text(s_ui.wifi_status_lbl, "服务状态: Web 升级服务运行中");
        } else {
            lv_label_set_text(s_ui.wifi_status_lbl, "服务状态: 服务未启动");
        }
    }

    if (s_ui.url_lbl != nullptr) {
        if (!net_ready) {
            lv_label_set_text(s_ui.url_lbl, "请连接 Wi-Fi 后开启无线刷机");
        } else if (running) {
            std::string url = svc.GetWebServerUrl();
            lv_label_set_text(s_ui.url_lbl, url.empty() ? "获取 IP 地址中..." : url.c_str());
        } else {
            lv_label_set_text(s_ui.url_lbl, "点击下方按钮启动 Web 升级服务");
        }
    }

    if (s_ui.switch_btn_lbl != nullptr) {
        lv_label_set_text(s_ui.switch_btn_lbl, running ? "关闭 Web 升级服务" : "启动 Web 升级服务");
    }

    if (s_ui.qr_box != nullptr && s_ui.qr_obj != nullptr) {
        if (net_ready && running) {
            std::string url = svc.GetWebServerUrl();
            if (!url.empty()) {
                lv_obj_clear_flag(s_ui.qr_box, LV_OBJ_FLAG_HIDDEN);
                lv_qrcode_update(s_ui.qr_obj, url.c_str(), url.length());
                if (s_ui.qr_tip_lbl) lv_obj_clear_flag(s_ui.qr_tip_lbl, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_ui.qr_box, LV_OBJ_FLAG_HIDDEN);
                if (s_ui.qr_tip_lbl) lv_obj_add_flag(s_ui.qr_tip_lbl, LV_OBJ_FLAG_HIDDEN);
            }
        } else {
            lv_obj_add_flag(s_ui.qr_box, LV_OBJ_FLAG_HIDDEN);
            if (s_ui.qr_tip_lbl) lv_obj_add_flag(s_ui.qr_tip_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void OnToggleWebServerClicked(lv_event_t* /*e*/) {
    HapticPulseIfEnabled();
    auto& svc = system_upgrade::SystemUpgradeService::GetInstance();
    if (svc.IsWebServerRunning()) {
        svc.StopWebServer();
        StatusBarManager::GetInstance().ShowToast("已停止 Web 升级服务", 1500);
    } else {
        if (!Board::GetInstance().IsNetworkReady()) {
            ModalManager::GetInstance().ShowAlert("提示", "当前 Wi-Fi 未连接，请先在「网络设置」连接网络！", "我知道了");
            return;
        }
        if (svc.StartWebServer(80)) {
            StatusBarManager::GetInstance().ShowToast("Web 升级服务已启动", 1500);
        } else {
            ModalManager::GetInstance().ShowAlert("错误", "启动 Web 升级服务失败", "确定");
        }
    }
    RefreshWebStatus();
}

// 刷新 SD 卡固件列表
void RefreshSdFirmwareList();

struct ActionDialogData {
    system_upgrade::FirmwareInfo info;
};

// 异步 SD 卡固件刷机任务
struct FlashTaskCtx {
    std::string path;
    std::string target_version;
};

void FlashSdWorkerTask(void* param) {
    auto* ctx = static_cast<FlashTaskCtx*>(param);
    const std::string path = ctx->path;
    const std::string target_ver = ctx->target_version;
    delete ctx;

    const std::string cur_ver = system_upgrade::SystemUpgradeService::GetInstance().GetCurrentVersion();

    // 在 worker 线程中安全调出全屏升级 UI 并锁闭按键
    OtaUpgradeScreen::Show(cur_ver.c_str(), target_ver.c_str());
    OtaUpgradeScreen::SetProgress(0);

    auto res = system_upgrade::SystemUpgradeService::GetInstance().FlashFromSd(path, [](int pct, size_t /*speed*/) {
        OtaUpgradeScreen::SetProgress(pct);
    });

    if (res.success) {
        OtaUpgradeScreen::SetProgress(100);
        OtaUpgradeScreen::SetHint("固件写入成功！设备即将自动重启...");
        vTaskDelay(pdMS_TO_TICKS(1500));
        Application::GetInstance().Reboot();
    } else {
        OtaUpgradeScreen::Dismiss();
        s_flashing_busy = false;

        struct ErrMsg {
            std::string err;
        };
        auto* em = new ErrMsg{ res.error_msg };
        ScreenLvAsync([](void* p) {
            auto* m = static_cast<ErrMsg*>(p);
            if (s_alive) {
                ModalManager::GetInstance().ShowAlert("升级失败", m->err.c_str(), "确定");
                RefreshSdFirmwareList();
            }
            delete m;
        }, em);
    }

    vTaskDelete(nullptr);
}

void StartSdFlash(const std::string& path, const std::string& version) {
    s_flashing_busy = true;
    auto* ctx = new FlashTaskCtx{ path, version };
    if (xTaskCreate(FlashSdWorkerTask, "sd_ota_task", 8192, ctx, 5, nullptr) != pdPASS) {
        delete ctx;
        s_flashing_busy = false;
        ModalManager::GetInstance().ShowAlert("错误", "创建刷机任务失败，系统内存不足", "确定");
    }
}

void OpenFirmwareActionDialog(const system_upgrade::FirmwareInfo& info) {
    if (!s_alive) return;

    ModalManager::GetInstance().PushCustomModal([info](lv_obj_t* card) {
        lv_obj_set_width(card, LV_HOR_RES - 40);
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_all(card, 16, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(card, 10, 0);

        lv_obj_t* title = lv_label_create(card);
        lv_label_set_text(title, "固件升级确认");
        const lv_font_t* tf = fontpack_lv_font_get(28, 2);
        lv_obj_set_style_text_font(title, tf ? tf : UiFont(), 0);
        lv_obj_set_style_text_color(title, lv_color_black(), 0);
        lv_obj_set_width(title, lv_pct(100));
        lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);

        lv_obj_t* fn_lbl = lv_label_create(card);
        lv_label_set_text(fn_lbl, info.filename.c_str());
        lv_obj_set_width(fn_lbl, lv_pct(100));
        lv_label_set_long_mode(fn_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(fn_lbl, UiFont(), 0);
        lv_obj_set_style_text_color(fn_lbl, lv_color_black(), 0);

        char sz_str[32];
        reader::FormatFileSize(sz_str, sizeof(sz_str), info.file_size);

        char detail_buf[256];
        if (info.is_valid_esp32_bin) {
            snprintf(detail_buf, sizeof(detail_buf),
                     "项目: %s\n目标版本: %s\n编译时间: %s %s\n文件大小: %s",
                     info.project_name.c_str(), info.version.c_str(),
                     info.build_date.c_str(), info.build_time.c_str(), sz_str);
        } else {
            snprintf(detail_buf, sizeof(detail_buf), "文件大小: %s\n⚠️ 未能读取到有效 ESP32 镜像特征", sz_str);
        }

        lv_obj_t* sub_lbl = lv_label_create(card);
        lv_label_set_text(sub_lbl, detail_buf);
        lv_obj_set_width(sub_lbl, lv_pct(100));
        lv_label_set_long_mode(sub_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(sub_lbl, UiFont(), 0);
        lv_obj_set_style_text_color(sub_lbl, lv_color_black(), 0);

        if (info.is_valid_esp32_bin) {
            auto* path_heap = new std::string(info.full_path);
            CreateCustomBtn(card, "立即刷入此固件并重启", lv_pct(100), 48, [](lv_event_t* e) {
                auto* p = static_cast<std::string*>(lv_event_get_user_data(e));
                std::string path = *p;
                delete p;
                ModalManager::GetInstance().PopModal();

                ModalManager::GetInstance().ShowDialog("安全警告",
                    "即将向目标分区写入固件并自动重启。请确保电量充足，烧录过程中切勿断电或拔卡！",
                    "确定烧录", "取消", [path](bool confirmed) {
                        if (confirmed) {
                            StartSdFlash(path, "SD 卡固件");
                        }
                    });
            }, path_heap);
        }

        auto* path_heap_del = new std::string(info.full_path);
        CreateCustomBtn(card, "删除此固件文件", lv_pct(100), 44, [](lv_event_t* e) {
            auto* p = static_cast<std::string*>(lv_event_get_user_data(e));
            std::string path = *p;
            delete p;
            ModalManager::GetInstance().PopModal();
            ModalManager::GetInstance().ShowDialog("删除确认", "确定要删除该固件文件吗？", "删除", "取消", [path](bool confirmed) {
                if (confirmed) {
                    bool ok = system_upgrade::SystemUpgradeService::GetInstance().DeleteFirmwareFile(path);
                    if (ok) {
                        StatusBarManager::GetInstance().ShowToast("已删除固件文件", 1500);
                    } else {
                        ModalManager::GetInstance().ShowAlert("错误", "删除文件失败", "确定");
                    }
                    RefreshSdFirmwareList();
                }
            });
        }, path_heap_del);

        CreateCustomBtn(card, "取消", lv_pct(100), 44, [](lv_event_t*) {
            ModalManager::GetInstance().PopModal();
        }, nullptr);
    });
}

void OnFirmwareCardClicked(lv_event_t* e) {
    HapticPulseIfEnabled();
    auto* data = static_cast<ActionDialogData*>(lv_event_get_user_data(e));
    if (data != nullptr) {
        OpenFirmwareActionDialog(data->info);
    }
}

void RefreshSdFirmwareList() {
    if (!s_alive || s_ui.list_container == nullptr) return;
    lv_obj_clean(s_ui.list_container);

    auto& svc = system_upgrade::SystemUpgradeService::GetInstance();
    if (!svc.IsSdMounted()) {
        lv_obj_t* tip = lv_label_create(s_ui.list_container);
        lv_label_set_text(tip, "未检测到 SD 卡，请插入 SD 卡存放固件");
        lv_obj_set_width(tip, lv_pct(100));
        lv_label_set_long_mode(tip, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(tip, 20, 0);
        lv_obj_set_style_text_font(tip, UiFont(), 0);
        lv_obj_set_style_text_color(tip, lv_color_black(), 0);
        if (s_ui.scaffold != nullptr && s_ui.scaffold->GetScrollView() != nullptr) {
            s_ui.scaffold->GetScrollView()->UpdatePageInfo();
        }
        return;
    }

    auto firmwares = svc.ScanSdFirmwares();
    if (firmwares.empty()) {
        lv_obj_t* tip = lv_label_create(s_ui.list_container);
        lv_label_set_text(tip, "未在 SD 卡找到 .bin 固件文件\n可将固件放入 SD 根目录或 metalio/e-ink/firmware/");
        lv_obj_set_width(tip, lv_pct(100));
        lv_label_set_long_mode(tip, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(tip, 20, 0);
        lv_obj_set_style_text_font(tip, UiFont(), 0);
        lv_obj_set_style_text_color(tip, lv_color_black(), 0);
        if (s_ui.scaffold != nullptr && s_ui.scaffold->GetScrollView() != nullptr) {
            s_ui.scaffold->GetScrollView()->UpdatePageInfo();
        }
        return;
    }

    const std::string cur_ver = svc.GetCurrentVersion();

    for (const auto& item : firmwares) {
        lv_obj_t* card = lv_obj_create(s_ui.list_container);
        lv_obj_remove_style_all(card);
        lv_obj_set_width(card, lv_pct(100));
        lv_obj_set_height(card, LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(card, 68, 0);
        lv_obj_set_style_bg_color(card, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(card, lv_color_black(), 0);
        lv_obj_set_style_border_width(card, 2, 0);
        lv_obj_set_style_radius(card, 10, 0);
        lv_obj_set_style_pad_all(card, 8, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(card, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        lv_obj_set_style_pad_row(card, 4, 0);
        DisableScroll(card);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(card);

        auto* dlg_data = new ActionDialogData{ item };
        lv_obj_add_event_cb(card, OnFirmwareCardClicked, LV_EVENT_CLICKED, dlg_data);
        lv_obj_add_event_cb(card, [](lv_event_t* e) {
            auto* d = static_cast<ActionDialogData*>(lv_event_get_user_data(e));
            delete d;
        }, LV_EVENT_DELETE, dlg_data);

        // 文件名
        lv_obj_t* name_lbl = lv_label_create(card);
        lv_label_set_text(name_lbl, item.filename.c_str());
        lv_obj_set_width(name_lbl, lv_pct(100));
        lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(name_lbl, UiFont(), 0);
        lv_obj_set_style_text_color(name_lbl, lv_color_black(), 0);
        lv_obj_clear_flag(name_lbl, LV_OBJ_FLAG_CLICKABLE);

        // 详细描述 (版本 / 大小 / 时间)
        char size_buf[32];
        reader::FormatFileSize(size_buf, sizeof(size_buf), item.file_size);
        char desc_buf[160];
        if (item.is_valid_esp32_bin) {
            const char* tag = (item.version == cur_ver) ? "[当前同版]" : "[可刷入]";
            snprintf(desc_buf, sizeof(desc_buf), "%s 版本: %s · %s · 编译: %s %s",
                     tag, item.version.c_str(), size_buf, item.build_date.c_str(), item.build_time.c_str());
        } else {
            snprintf(desc_buf, sizeof(desc_buf), "%s · 未识别为标准固件", size_buf);
        }

        lv_obj_t* meta_lbl = lv_label_create(card);
        lv_label_set_text(meta_lbl, desc_buf);
        lv_obj_set_width(meta_lbl, lv_pct(100));
        lv_label_set_long_mode(meta_lbl, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_font(meta_lbl, UiFont(), 0);
        lv_obj_set_style_text_color(meta_lbl, lv_color_black(), 0);
        lv_obj_clear_flag(meta_lbl, LV_OBJ_FLAG_CLICKABLE);
    }

    if (s_ui.scaffold != nullptr && s_ui.scaffold->GetScrollView() != nullptr) {
        s_ui.scaffold->GetScrollView()->UpdatePageInfo();
    }
}

void OnRefreshClicked(lv_event_t* /*e*/) {
    HapticPulseIfEnabled();
    RefreshWebStatus();
    RefreshSdFirmwareList();
    StatusBarManager::GetInstance().ShowToast("已刷新固件与状态", 1200);
}

void OnScreenDestroyed(lv_event_t* /*e*/) {
    s_alive = false;
    CleanupUpgradeService();
    ModalManager::GetInstance().DismissAll();
    s_ui = {};
}

}  // namespace

lv_obj_t* SystemUpgradeScreen::Create() {
    s_ui = {};
    s_alive = true;
    s_flashing_busy = false;
    ScreenSetIsHome(false);

    auto* scaffold = EpdWidgetKit::EpdPageScaffold::Create({
        .title = "系统升级",
        .show_status_bar = true,
        .show_back_btn = true,
        .scrollable = true,
        .pad_h = 12,
        .pad_v = 10,
        .row_gap = 10,
    });
    s_ui.scaffold = scaffold;
    s_ui.screen = scaffold->GetScreen();
    s_ui.body = scaffold->GetContentContainer();

    // 严密监听页面卸载与删除事件，彻底释放服务
    lv_obj_add_event_cb(s_ui.screen, OnScreenDestroyed, LV_EVENT_SCREEN_UNLOAD_START, nullptr);
    lv_obj_add_event_cb(s_ui.screen, OnScreenDestroyed, LV_EVENT_SCREEN_UNLOADED, nullptr);
    lv_obj_add_event_cb(s_ui.screen, OnScreenDestroyed, LV_EVENT_DELETE, nullptr);

    // 头部挂载刷新按钮
    lv_obj_t* head = scaffold->GetHeader();
    if (head != nullptr) {
        lv_obj_t* ref_btn = CreateCustomBtn(head, "刷新", 80, 38, OnRefreshClicked, nullptr);
        lv_obj_set_style_radius(ref_btn, LV_RADIUS_CIRCLE, 0);
    }

    const auto* title_font = fontpack_lv_font_get(26, 2);

    // 1. 卡片：当前设备系统状态
    lv_obj_t* status_card = MakeCard(s_ui.body, 12);
    s_ui.status_card = status_card;

    lv_obj_t* title1 = lv_label_create(status_card);
    lv_label_set_text(title1, "当前系统运行状态");
    lv_obj_set_width(title1, lv_pct(100));
    lv_label_set_long_mode(title1, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(title1, title_font ? title_font : UiFont(), 0);
    lv_obj_set_style_text_color(title1, lv_color_black(), 0);

    auto& svc = system_upgrade::SystemUpgradeService::GetInstance();
    s_ui.cur_ver_lbl = lv_label_create(status_card);
    char ver_buf[128];
    snprintf(ver_buf, sizeof(ver_buf), "当前固件: %s (%s)", svc.GetCurrentVersion().c_str(), svc.GetBuildTime().c_str());
    lv_label_set_text(s_ui.cur_ver_lbl, ver_buf);
    lv_obj_set_width(s_ui.cur_ver_lbl, lv_pct(100));
    lv_label_set_long_mode(s_ui.cur_ver_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_ui.cur_ver_lbl, UiFont(), 0);
    lv_obj_set_style_text_color(s_ui.cur_ver_lbl, lv_color_black(), 0);

    s_ui.part_info_lbl = lv_label_create(status_card);
    char part_buf[128];
    snprintf(part_buf, sizeof(part_buf), "活动分区: %s (下次刷入: %s)", svc.GetRunningPartition().c_str(), svc.GetNextPartition().c_str());
    lv_label_set_text(s_ui.part_info_lbl, part_buf);
    lv_obj_set_width(s_ui.part_info_lbl, lv_pct(100));
    lv_label_set_long_mode(s_ui.part_info_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_ui.part_info_lbl, UiFont(), 0);
    lv_obj_set_style_text_color(s_ui.part_info_lbl, lv_color_black(), 0);

    // 2. 卡片：WiFi 局域网网页无线刷机
    s_ui.web_card = MakeCard(s_ui.body, 12);
    lv_obj_t* title2 = lv_label_create(s_ui.web_card);
    lv_label_set_text(title2, "WiFi 网页无线升级");
    lv_obj_set_width(title2, lv_pct(100));
    lv_label_set_long_mode(title2, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(title2, title_font ? title_font : UiFont(), 0);
    lv_obj_set_style_text_color(title2, lv_color_black(), 0);

    s_ui.wifi_status_lbl = lv_label_create(s_ui.web_card);
    lv_obj_set_width(s_ui.wifi_status_lbl, lv_pct(100));
    lv_label_set_long_mode(s_ui.wifi_status_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(s_ui.wifi_status_lbl, UiFont(), 0);
    lv_obj_set_style_text_color(s_ui.wifi_status_lbl, lv_color_black(), 0);

    s_ui.url_lbl = lv_label_create(s_ui.web_card);
    const lv_font_t* big_f = fontpack_lv_font_get(28, 2);
    lv_obj_set_style_text_font(s_ui.url_lbl, big_f ? big_f : UiFont(), 0);
    lv_obj_set_style_text_color(s_ui.url_lbl, lv_color_black(), 0);
    lv_obj_set_width(s_ui.url_lbl, lv_pct(100));
    lv_label_set_long_mode(s_ui.url_lbl, LV_LABEL_LONG_WRAP);

    s_ui.switch_btn = CreateCustomBtn(s_ui.web_card, "启动 Web 升级服务", lv_pct(100), 42,
                                      OnToggleWebServerClicked, nullptr, &s_ui.switch_btn_lbl);

    // 二维码区域
    lv_obj_t* qr_box = lv_obj_create(s_ui.web_card);
    s_ui.qr_box = qr_box;
    lv_obj_remove_style_all(qr_box);
    lv_obj_set_size(qr_box, 200, 200);
    lv_obj_set_style_bg_color(qr_box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(qr_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(qr_box, lv_color_black(), 0);
    lv_obj_set_style_border_width(qr_box, 2, 0);
    lv_obj_set_style_radius(qr_box, 8, 0);
    lv_obj_clear_flag(qr_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_align(qr_box, LV_ALIGN_CENTER);

    s_ui.qr_obj = lv_qrcode_create(qr_box);
    lv_qrcode_set_size(s_ui.qr_obj, 186);
    lv_qrcode_set_dark_color(s_ui.qr_obj, lv_color_black());
    lv_qrcode_set_light_color(s_ui.qr_obj, lv_color_white());
    lv_obj_center(s_ui.qr_obj);

    s_ui.qr_tip_lbl = lv_label_create(s_ui.web_card);
    lv_label_set_text(s_ui.qr_tip_lbl, "手机微信/相机扫码直达网页，直接选 .bin 固件升级");
    lv_obj_set_width(s_ui.qr_tip_lbl, lv_pct(100));
    lv_label_set_long_mode(s_ui.qr_tip_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_ui.qr_tip_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_ui.qr_tip_lbl, UiFont(), 0);
    lv_obj_set_style_text_color(s_ui.qr_tip_lbl, lv_color_black(), 0);

    // 默认若网络就绪则自动开启 Web 升级服务
    if (Board::GetInstance().IsNetworkReady()) {
        svc.StartWebServer(80);
    }
    RefreshWebStatus();

    // 3. 分隔标题与 SD 卡固件列表
    MakeSection(s_ui.body, "SD 卡本地固件列表 (.bin)");

    s_ui.list_container = lv_obj_create(s_ui.body);
    lv_obj_remove_style_all(s_ui.list_container);
    lv_obj_set_width(s_ui.list_container, lv_pct(100));
    lv_obj_set_flex_flow(s_ui.list_container, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_ui.list_container, 8, 0);
    lv_obj_set_style_pad_bottom(s_ui.list_container, 12, 0);
    DisableScroll(s_ui.list_container);

    RefreshSdFirmwareList();

    VkKey_AttachScreen(s_ui.screen, kScreenId, VkKeyScreenDesc{SystemUpgradeScreen::Create, nullptr});
    return s_ui.screen;
}
