#include "backup_screen/backup_screen.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "nvs_backup_service.h"
#include "reader/file_size.h"
#include "screen_common.h"
#include "sd_paths.h"
#include "vk_key_handler.h"

namespace {

constexpr const char* TAG = "BackupScreen";
constexpr const char* kScreenId = "backup_screen";

struct BackupUi {
    lv_obj_t* screen = nullptr;
    lv_obj_t* body = nullptr;
    lv_obj_t* status_card = nullptr;
    lv_obj_t* sd_status_lbl = nullptr;
    lv_obj_t* path_lbl = nullptr;
    lv_obj_t* nvs_info_lbl = nullptr;
    lv_obj_t* export_btn = nullptr;
    lv_obj_t* export_btn_lbl = nullptr;
    lv_obj_t* refresh_btn = nullptr;
    lv_obj_t* list_container = nullptr;
    lv_obj_t* tip_lbl = nullptr;

    // 模态对话框 / 浮层
    lv_obj_t* overlay = nullptr;
    lv_obj_t* overlay_card = nullptr;
    lv_obj_t* overlay_msg_lbl = nullptr;
    lv_timer_t* reboot_timer = nullptr;
    int reboot_remaining = 0;
};

BackupUi s_ui;
bool s_alive = false;
bool s_op_busy = false;

const lv_font_t* UiFont() { return fontpack_lv_font_ui(); }
void DisableScroll(lv_obj_t* o) { if (o) lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE); }

void CloseModal() {
    if (s_ui.reboot_timer != nullptr) {
        lv_timer_delete(s_ui.reboot_timer);
        s_ui.reboot_timer = nullptr;
    }
    if (s_ui.overlay != nullptr) {
        lv_obj_delete(s_ui.overlay);
        s_ui.overlay = nullptr;
        s_ui.overlay_card = nullptr;
        s_ui.overlay_msg_lbl = nullptr;
    }
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

void RefreshStatusHeader() {
    if (!s_alive) return;
    auto& svc = backup_restore::NvsBackupService::GetInstance();
    bool mounted = svc.IsSdMounted();

    if (s_ui.sd_status_lbl != nullptr) {
        if (mounted) {
            std::string free_str, total_str;
            svc.GetSdCapacityText(free_str, total_str);
            char buf[80];
            snprintf(buf, sizeof(buf), "SD 卡状态: 已挂载 (可用: %s / 共: %s)", free_str.c_str(), total_str.c_str());
            lv_label_set_text(s_ui.sd_status_lbl, buf);
        } else {
            lv_label_set_text(s_ui.sd_status_lbl, "SD 卡状态: 未检测到 SD 卡 / 未挂载");
        }
    }

    if (s_ui.nvs_info_lbl != nullptr) {
        int nvs_count = svc.GetLocalNvsEntryCount();
        char buf[64];
        snprintf(buf, sizeof(buf), "本机配置: 已用 NVS 条目 %d 项", nvs_count);
        lv_label_set_text(s_ui.nvs_info_lbl, buf);
    }
}

void ShowWaitModal(const char* text) {
    if (!s_alive || s_ui.screen == nullptr) return;
    CloseModal();

    lv_obj_t* mask = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(mask, 0, 0);
    ScreenApplyDotBackdrop(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    s_ui.overlay = mask;

    lv_obj_t* card = lv_obj_create(mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_HOR_RES - 60, 160);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 3, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    DisableScroll(card);
    s_ui.overlay_card = card;

    lv_obj_t* lbl = lv_label_create(card);
    lv_label_set_text(lbl, text);
    lv_obj_set_width(lbl, lv_pct(100));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lbl, UiFont(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl);
    s_ui.overlay_msg_lbl = lbl;
}

void ShowNoticeModal(const char* text) {
    if (!s_alive || s_ui.screen == nullptr) return;
    CloseModal();

    lv_obj_t* mask = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(mask, 0, 0);
    ScreenApplyDotBackdrop(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    s_ui.overlay = mask;

    lv_obj_t* card = lv_obj_create(mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_HOR_RES - 60, 200);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 3, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    DisableScroll(card);
    s_ui.overlay_card = card;

    lv_obj_t* lbl = lv_label_create(card);
    lv_label_set_text(lbl, text);
    lv_obj_set_width(lbl, lv_pct(100));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lbl, UiFont(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);

    CreateCustomBtn(card, "确定", 140, 44, [](lv_event_t*) {
        CloseModal();
    }, nullptr);
}

void StartRebootCountdown(const char* headline, int seconds = 2) {
    if (!s_alive || s_ui.screen == nullptr) return;
    CloseModal();

    lv_obj_t* mask = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(mask, 0, 0);
    ScreenApplyDotBackdrop(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    s_ui.overlay = mask;

    lv_obj_t* card = lv_obj_create(mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_HOR_RES - 60, 160);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 3, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    DisableScroll(card);
    s_ui.overlay_card = card;

    lv_obj_t* lbl = lv_label_create(card);
    s_ui.overlay_msg_lbl = lbl;
    lv_obj_set_width(lbl, lv_pct(100));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(lbl, UiFont(), 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl);

    s_ui.reboot_remaining = seconds;

    char msg[128];
    snprintf(msg, sizeof(msg), "%s\n%d 秒后自动重启...", headline, seconds);
    lv_label_set_text(lbl, msg);

    s_ui.reboot_timer = lv_timer_create([](lv_timer_t* t) {
        s_ui.reboot_remaining--;
        if (s_ui.reboot_remaining > 0) {
            char buf[128];
            snprintf(buf, sizeof(buf), "导入成功！\n%d 秒后自动重启...", s_ui.reboot_remaining);
            if (s_ui.overlay_msg_lbl) lv_label_set_text(s_ui.overlay_msg_lbl, buf);
        } else {
            lv_timer_delete(t);
            s_ui.reboot_timer = nullptr;
            if (s_ui.overlay_msg_lbl) lv_label_set_text(s_ui.overlay_msg_lbl, "正在重启设备…");
            Application::GetInstance().Schedule([]() {
                vTaskDelay(pdMS_TO_TICKS(300));
                Application::GetInstance().Reboot();
            });
        }
    }, 1000, nullptr);
}

// 刷新备份文件列表
void RefreshBackupList();

struct ActionDialogData {
    std::string filename;
    std::string full_path;
    uint32_t size_bytes = 0;
    int entry_count = 0;
};

// 执行恢复操作任务
struct ImportTaskCtx {
    std::string full_path;
    bool reboot_after = false;
};

void ImportWorkerTask(void* param) {
    auto* ctx = static_cast<ImportTaskCtx*>(param);
    auto res = backup_restore::NvsBackupService::GetInstance().ImportFromSd(ctx->full_path);
    const bool reboot = ctx->reboot_after;
    delete ctx;

    struct ResultMsg {
        backup_restore::ImportResult res;
        bool reboot = false;
    };
    auto* msg = new ResultMsg{res, reboot};

    ScreenLvAsync([](void* p) {
        s_op_busy = false;
        auto* m = static_cast<ResultMsg*>(p);
        if (s_alive) {
            if (m->res.success) {
                if (m->reboot) {
                    char h[80];
                    snprintf(h, sizeof(h), "成功导入 %d 项配置！", m->res.count);
                    StartRebootCountdown(h, 2);
                } else {
                    backup_restore::NvsBackupService::GetInstance().ReloadRuntimeCaches();
                    char h[128];
                    snprintf(h, sizeof(h), "导入成功！\n已覆盖 %d 项配置，已直接生效。", m->res.count);
                    ShowNoticeModal(h);
                    RefreshStatusHeader();
                    RefreshBackupList();
                }
            } else {
                char err[128];
                snprintf(err, sizeof(err), "导入失败: %s", m->res.error_msg.c_str());
                ShowNoticeModal(err);
            }
        }
        delete m;
    }, msg);

    vTaskDelete(nullptr);
}

void StartImportTask(const std::string& path, bool reboot) {
    s_op_busy = true;
    ShowWaitModal("正在导入配置并写入 NVS，请稍候…");
    auto* ctx = new ImportTaskCtx{path, reboot};
    if (xTaskCreate(ImportWorkerTask, "nvs_imp_task", 8192, ctx, 5, nullptr) != pdPASS) {
        delete ctx;
        s_op_busy = false;
        ShowNoticeModal("创建导入任务失败，内存不足");
    }
}

// 弹出文件详情及操作弹窗
void OpenActionDialog(const ActionDialogData& data) {
    if (!s_alive || s_ui.screen == nullptr) return;
    CloseModal();

    lv_obj_t* mask = lv_obj_create(s_ui.screen);
    lv_obj_remove_style_all(mask);
    lv_obj_set_size(mask, LV_HOR_RES, LV_VER_RES);
    lv_obj_set_pos(mask, 0, 0);
    ScreenApplyDotBackdrop(mask);
    lv_obj_add_flag(mask, LV_OBJ_FLAG_CLICKABLE);
    s_ui.overlay = mask;

    lv_obj_t* card = lv_obj_create(mask);
    lv_obj_remove_style_all(card);
    lv_obj_set_size(card, LV_HOR_RES - 40, 360);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 3, 0);
    lv_obj_set_style_radius(card, 16, 0);
    lv_obj_set_style_pad_all(card, 16, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 10, 0);
    DisableScroll(card);
    s_ui.overlay_card = card;

    lv_obj_t* title = lv_label_create(card);
    lv_label_set_text(title, "备份文件操作");
    const lv_font_t* tf = fontpack_lv_font_get(28, 2);
    lv_obj_set_style_text_font(title, tf ? tf : UiFont(), 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);

    lv_obj_t* fn_lbl = lv_label_create(card);
    lv_label_set_text(fn_lbl, data.filename.c_str());
    lv_obj_set_width(fn_lbl, lv_pct(100));
    lv_label_set_long_mode(fn_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(fn_lbl, UiFont(), 0);
    lv_obj_set_style_text_color(fn_lbl, lv_color_black(), 0);

    char info_buf[128];
    char sz_str[32];
    reader::FormatFileSize(sz_str, sizeof(sz_str), data.size_bytes);
    if (data.entry_count > 0) {
        snprintf(info_buf, sizeof(info_buf), "大小: %s · 约 %d 项配置", sz_str, data.entry_count);
    } else {
        snprintf(info_buf, sizeof(info_buf), "大小: %s", sz_str);
    }
    lv_obj_t* sub_lbl = lv_label_create(card);
    lv_label_set_text(sub_lbl, info_buf);
    lv_obj_set_style_text_font(sub_lbl, UiFont(), 0);
    lv_obj_set_style_text_color(sub_lbl, lv_color_black(), 0);

    auto* path_heap1 = new std::string(data.full_path);
    CreateCustomBtn(card, "导入覆盖并重启 (推荐)", lv_pct(100), 48, [](lv_event_t* e) {
        auto* p = static_cast<std::string*>(lv_event_get_user_data(e));
        std::string path = *p;
        delete p;
        CloseModal();
        StartImportTask(path, true);
    }, path_heap1);

    auto* path_heap2 = new std::string(data.full_path);
    CreateCustomBtn(card, "导入覆盖 (直接生效)", lv_pct(100), 48, [](lv_event_t* e) {
        auto* p = static_cast<std::string*>(lv_event_get_user_data(e));
        std::string path = *p;
        delete p;
        CloseModal();
        StartImportTask(path, false);
    }, path_heap2);

    auto* path_heap3 = new std::string(data.full_path);
    CreateCustomBtn(card, "删除此备份", lv_pct(100), 44, [](lv_event_t* e) {
        auto* p = static_cast<std::string*>(lv_event_get_user_data(e));
        std::string path = *p;
        delete p;
        CloseModal();
        bool ok = backup_restore::NvsBackupService::GetInstance().DeleteBackup(path);
        if (ok) {
            ShowNoticeModal("已成功删除该备份文件");
        } else {
            ShowNoticeModal("删除备份文件失败");
        }
        RefreshBackupList();
    }, path_heap3);

    CreateCustomBtn(card, "取消", lv_pct(100), 44, [](lv_event_t*) {
        CloseModal();
    }, nullptr);
}

void OnFileItemClicked(lv_event_t* e) {
    HapticPulseIfEnabled();
    auto* data = static_cast<ActionDialogData*>(lv_event_get_user_data(e));
    if (data != nullptr) {
        OpenActionDialog(*data);
    }
}

void RefreshBackupList() {
    if (!s_alive || s_ui.list_container == nullptr) return;
    lv_obj_clean(s_ui.list_container);

    auto& svc = backup_restore::NvsBackupService::GetInstance();
    if (!svc.IsSdMounted()) {
        lv_obj_t* tip = lv_label_create(s_ui.list_container);
        lv_label_set_text(tip, "请插入 SD 卡以进行备份与恢复");
        lv_obj_set_width(tip, lv_pct(100));
        lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(tip, 20, 0);
        lv_obj_set_style_text_font(tip, UiFont(), 0);
        lv_obj_set_style_text_color(tip, lv_color_black(), 0);
        return;
    }

    auto backups = svc.ListBackups();
    if (backups.empty()) {
        lv_obj_t* tip = lv_label_create(s_ui.list_container);
        lv_label_set_text(tip, "未在 SD 卡找到备份文件\n点击上方「导出本机配置」进行备份");
        lv_obj_set_width(tip, lv_pct(100));
        lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(tip, 20, 0);
        lv_obj_set_style_text_font(tip, UiFont(), 0);
        lv_obj_set_style_text_color(tip, lv_color_black(), 0);
        return;
    }

    for (const auto& item : backups) {
        lv_obj_t* card = lv_obj_create(s_ui.list_container);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, lv_pct(100), 68);
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

        auto* dlg_data = new ActionDialogData{item.filename, item.full_path, item.size_bytes, item.entry_count};
        lv_obj_add_event_cb(card, OnFileItemClicked, LV_EVENT_CLICKED, dlg_data);
        lv_obj_add_event_cb(card, [](lv_event_t* e) {
            auto* d = static_cast<ActionDialogData*>(lv_event_get_user_data(e));
            delete d;
        }, LV_EVENT_DELETE, dlg_data);

        // 文件名
        lv_obj_t* name_lbl = lv_label_create(card);
        lv_label_set_text(name_lbl, item.filename.c_str());
        lv_obj_set_width(name_lbl, lv_pct(100));
        lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(name_lbl, UiFont(), 0);
        lv_obj_set_style_text_color(name_lbl, lv_color_black(), 0);
        lv_obj_clear_flag(name_lbl, LV_OBJ_FLAG_CLICKABLE);

        // 信息行 (大小 + 时间 + 条目数)
        char size_buf[32];
        reader::FormatFileSize(size_buf, sizeof(size_buf), item.size_bytes);
        char desc_buf[128];
        struct tm tm_info = {};
        localtime_r(&item.mod_time, &tm_info);
        if (tm_info.tm_year + 1900 >= 2025) {
            if (item.entry_count > 0) {
                snprintf(desc_buf, sizeof(desc_buf), "%s · %04d-%02d-%02d %02d:%02d · 共 %d 项",
                         size_buf, tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                         tm_info.tm_hour, tm_info.tm_min, item.entry_count);
            } else {
                snprintf(desc_buf, sizeof(desc_buf), "%s · %04d-%02d-%02d %02d:%02d",
                         size_buf, tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                         tm_info.tm_hour, tm_info.tm_min);
            }
        } else {
            if (item.entry_count > 0) {
                snprintf(desc_buf, sizeof(desc_buf), "%s · 共 %d 项", size_buf, item.entry_count);
            } else {
                snprintf(desc_buf, sizeof(desc_buf), "%s", size_buf);
            }
        }

        lv_obj_t* meta_lbl = lv_label_create(card);
        lv_label_set_text(meta_lbl, desc_buf);
        lv_obj_set_style_text_font(meta_lbl, UiFont(), 0);
        lv_obj_set_style_text_color(meta_lbl, lv_color_black(), 0);
        lv_obj_clear_flag(meta_lbl, LV_OBJ_FLAG_CLICKABLE);
    }
}

// 导出任务执行
void ExportWorkerTask(void* /*arg*/) {
    auto res = backup_restore::NvsBackupService::GetInstance().ExportToSd();
    auto* msg = new backup_restore::ExportResult(res);

    ScreenLvAsync([](void* p) {
        s_op_busy = false;
        auto* m = static_cast<backup_restore::ExportResult*>(p);
        if (s_alive) {
            if (m->success) {
                char ok_text[160];
                snprintf(ok_text, sizeof(ok_text), "导出成功！\n已备份至: %s\n共导出 %d 项配置。",
                         m->filename.c_str(), m->count);
                ShowNoticeModal(ok_text);
                RefreshStatusHeader();
                RefreshBackupList();
            } else {
                char err_text[128];
                snprintf(err_text, sizeof(err_text), "导出失败: %s", m->error_msg.c_str());
                ShowNoticeModal(err_text);
            }
        }
        delete m;
    }, msg);

    vTaskDelete(nullptr);
}

void OnExportClicked(lv_event_t* /*e*/) {
    HapticPulseIfEnabled();
    if (s_op_busy) return;
    auto& svc = backup_restore::NvsBackupService::GetInstance();
    if (!svc.IsSdMounted()) {
        ShowNoticeModal("未检测到 SD 卡，请先插入 SD 卡！");
        return;
    }
    s_op_busy = true;
    ShowWaitModal("正在导出本机 NVS 配置到 SD 卡，请稍候…");
    if (xTaskCreate(ExportWorkerTask, "nvs_exp_task", 8192, nullptr, 5, nullptr) != pdPASS) {
        s_op_busy = false;
        ShowNoticeModal("创建导出任务失败，内存不足");
    }
}

void OnRefreshClicked(lv_event_t* /*e*/) {
    HapticPulseIfEnabled();
    RefreshStatusHeader();
    RefreshBackupList();
}

void OnScreenUnloaded(lv_event_t* /*e*/) {
    s_alive = false;
    CloseModal();
    s_ui = {};
}

}  // namespace

lv_obj_t* BackupScreen::Create() {
    s_ui = {};
    s_alive = true;
    s_op_busy = false;
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
    s_ui.body = body;
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, LV_HOR_RES, LV_VER_RES - status.height);
    lv_obj_align(body, LV_ALIGN_TOP_MID, 0, status.height);
    lv_obj_set_style_pad_all(body, 12, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 8, 0);
    DisableScroll(body);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_CLICKABLE);

    // 头部行：标题 + 刷新按钮
    lv_obj_t* head = lv_obj_create(body);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, lv_pct(100), 50);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    DisableScroll(head);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_CLICKABLE);

    CreateCustomBtn(head, "‹ 返回", 86, 42, [](lv_event_t*) {
        HapticPulseIfEnabled();
        ScreenRequestBack();
    }, nullptr);

    lv_obj_t* title = lv_label_create(head);
    lv_label_set_text(title, "备份恢复");
    const lv_font_t* title_font = fontpack_lv_font_get(30, 2);
    lv_obj_set_style_text_font(title, title_font ? title_font : UiFont(), 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_clear_flag(title, LV_OBJ_FLAG_CLICKABLE);

    s_ui.refresh_btn = CreateCustomBtn(head, "刷新", 86, 42, OnRefreshClicked, nullptr);

    // 状态概览卡片
    lv_obj_t* status_card = lv_obj_create(body);
    s_ui.status_card = status_card;
    lv_obj_remove_style_all(status_card);
    lv_obj_set_size(status_card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(status_card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(status_card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(status_card, lv_color_black(), 0);
    lv_obj_set_style_border_width(status_card, 2, 0);
    lv_obj_set_style_radius(status_card, 10, 0);
    lv_obj_set_style_pad_all(status_card, 10, 0);
    lv_obj_set_flex_flow(status_card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(status_card, 4, 0);
    DisableScroll(status_card);
    lv_obj_clear_flag(status_card, LV_OBJ_FLAG_CLICKABLE);

    s_ui.sd_status_lbl = lv_label_create(status_card);
    lv_obj_set_style_text_font(s_ui.sd_status_lbl, UiFont(), 0);
    lv_obj_set_style_text_color(s_ui.sd_status_lbl, lv_color_black(), 0);

    s_ui.path_lbl = lv_label_create(status_card);
    lv_label_set_text(s_ui.path_lbl, "备份目录: metalio/e-ink/backup/");
    lv_obj_set_style_text_font(s_ui.path_lbl, UiFont(), 0);
    lv_obj_set_style_text_color(s_ui.path_lbl, lv_color_black(), 0);

    s_ui.nvs_info_lbl = lv_label_create(status_card);
    lv_obj_set_style_text_font(s_ui.nvs_info_lbl, UiFont(), 0);
    lv_obj_set_style_text_color(s_ui.nvs_info_lbl, lv_color_black(), 0);

    // 主操作按钮：导出本机配置
    s_ui.export_btn = CreateCustomBtn(body, "导出本机配置到 SD 卡", lv_pct(100), 50, OnExportClicked, nullptr, &s_ui.export_btn_lbl);
    lv_obj_set_style_border_width(s_ui.export_btn, 3, 0);
    const lv_font_t* btn_f = fontpack_lv_font_get(26, 2);
    if (btn_f && s_ui.export_btn_lbl) {
        lv_obj_set_style_text_font(s_ui.export_btn_lbl, btn_f, 0);
    }

    // 分隔标题
    MakeSection(body, "SD 卡中的备份文件列表");

    // 滚动列表容器
    lv_obj_t* list = lv_obj_create(body);
    s_ui.list_container = list;
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, lv_pct(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_style_pad_bottom(list, 10, 0);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    RefreshStatusHeader();
    RefreshBackupList();

    VkKey_AttachScreen(screen, kScreenId, VkKeyScreenDesc{BackupScreen::Create, nullptr});
    return screen;
}
