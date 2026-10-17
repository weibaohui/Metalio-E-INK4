#include "wallpaper_screen/wallpaper_screen.h"

#include "services/wallpaper/wallpaper_active.h"

#include "boards/common/SdCardManager.hpp"
#include "boards/common/sd_paths.h"
#include "display/modal_manager.h"
#include "display/screen/epd_widget_kit.h"
#include "fontpack_lvgl.h"
#include "haptic_feedback.h"
#include "reader/image_util.h"
#include "reader_core/reader_types.h"
#include "screen_common.h"
#include "utils/psr_worker.h"
#include "vk_key_handler.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

namespace {

constexpr const char* TAG = "WallpaperScreen";
constexpr const char* kScreenId = "wallpaper";
constexpr const char* kPosixDir = SD_PATH_WALLPAPER;

constexpr int kColsPerPage = 3;
constexpr int kRowsPerPage = 2;
constexpr int kItemsPerPage = kColsPerPage * kRowsPerPage; // 6 个/页
constexpr lv_coord_t kCardW = 136;
constexpr lv_coord_t kCardH = 220;
constexpr lv_coord_t kThumbMaxW = 124;
constexpr lv_coord_t kThumbMaxH = 170;
constexpr int kPreviewMaxW = 340;
constexpr int kPreviewMaxH = 500;
constexpr uint32_t kThumbWorkerStack = 12 * 1024;

enum class UiMode : uint8_t { kList = 0, kPreview = 1 };

struct FileEntry {
    std::string name;
    std::string path;
    size_t size_bytes = 0;
};

struct ThumbDoneMsg {
    uint32_t token = 0;
    size_t file_index = 0;
    reader::RasterImage* img = nullptr;
};

struct UiState {
    lv_obj_t* screen = nullptr;
    EpdWidgetKit::EpdPageScaffold* scaffold = nullptr;
    lv_obj_t* content_area = nullptr;
    lv_obj_t* page_lbl = nullptr;
    lv_obj_t* preview_img = nullptr;
};

UiState s_ui;
bool s_alive = false;
UiMode s_mode = UiMode::kList;
int s_cur_page = 0;
int s_preview_idx = -1;
std::vector<FileEntry> s_files;
std::vector<std::unique_ptr<reader::RasterImage>> s_thumbs;
std::unique_ptr<reader::RasterImage> s_preview_raster;
std::atomic<uint32_t> s_thumb_token{0};
std::atomic<uint32_t> s_preview_token{0};

const lv_font_t* UiFont() { return fontpack_lv_font_ui(); }
void DisableScroll(lv_obj_t* o) { if (o) lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE); }

bool IsSupportedImage(const char* name) {
    if (name == nullptr || name[0] == '.' || name[0] == '\0') {
        return false;
    }
    const char* dot = std::strrchr(name, '.');
    if (dot == nullptr) {
        return false;
    }
    return (strcasecmp(dot, ".a2i1") == 0 ||
            strcasecmp(dot, ".png") == 0 ||
            strcasecmp(dot, ".jpg") == 0 ||
            strcasecmp(dot, ".jpeg") == 0 ||
            strcasecmp(dot, ".bin") == 0);
}

void ScanWallpapers() {
    s_files.clear();
    s_thumbs.clear();
    if (!SdCardManager::GetInstance().IsMounted()) {
        return;
    }
    DIR* d = opendir(kPosixDir);
    if (d == nullptr) {
        mkdir(kPosixDir, 0755);
        return;
    }
    dirent* ent = nullptr;
    while ((ent = readdir(d)) != nullptr) {
        if (!IsSupportedImage(ent->d_name)) {
            continue;
        }
        std::string full_path = std::string(kPosixDir) + "/" + ent->d_name;
        struct stat st {};
        if (stat(full_path.c_str(), &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
            FileEntry fe;
            fe.name = ent->d_name;
            fe.path = full_path;
            fe.size_bytes = static_cast<size_t>(st.st_size);
            s_files.push_back(std::move(fe));
        }
    }
    closedir(d);

    std::sort(s_files.begin(), s_files.end(), [](const FileEntry& a, const FileEntry& b) {
        const bool a_standby = wallpaper::IsStandbyFilename(a.name.c_str());
        const bool b_standby = wallpaper::IsStandbyFilename(b.name.c_str());
        if (a_standby != b_standby) return a_standby > b_standby;
        const bool a_shut = wallpaper::IsActiveFilename(a.name.c_str());
        const bool b_shut = wallpaper::IsActiveFilename(b.name.c_str());
        if (a_shut != b_shut) return a_shut > b_shut;
        return a.name < b.name;
    });

    s_thumbs.resize(s_files.size());
}

int TotalPages() {
    if (s_files.empty()) return 1;
    return (static_cast<int>(s_files.size()) + kItemsPerPage - 1) / kItemsPerPage;
}

void RenderCurrentView();

void OnThumbDoneAsync(void* p) {
    auto* msg = static_cast<ThumbDoneMsg*>(p);
    if (msg == nullptr) return;
    if (!s_alive || msg->token != s_thumb_token.load() || s_mode != UiMode::kList) {
        delete msg->img;
        delete msg;
        return;
    }
    if (msg->file_index < s_thumbs.size()) {
        s_thumbs[msg->file_index].reset(msg->img);
    } else {
        delete msg->img;
    }
    delete msg;
    // 异步解码完成后局部更新
    RenderCurrentView();
}

void ThumbWorker(void* arg) {
    const uint32_t token = s_thumb_token.load();
    const int start = s_cur_page * kItemsPerPage;
    const int end = std::min(start + kItemsPerPage, static_cast<int>(s_files.size()));

    for (int i = start; i < end; ++i) {
        if (token != s_thumb_token.load() || !s_alive) {
            break;
        }
        if (s_thumbs[static_cast<size_t>(i)] != nullptr && !s_thumbs[static_cast<size_t>(i)]->empty()) {
            continue;
        }
        auto* img = new reader::RasterImage();
        if (reader::DecodeImageFileToL8(s_files[static_cast<size_t>(i)].path.c_str(), kThumbMaxW, kThumbMaxH, *img) && !img->empty()) {
            img->BindDsc();
            auto* done = new ThumbDoneMsg{token, static_cast<size_t>(i), img};
            if (!ScreenLvAsync(OnThumbDoneAsync, done)) {
                delete done->img;
                delete done;
            }
        } else {
            delete img;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void StartThumbnailsLoad() {
    s_thumb_token.fetch_add(1);
    (void)SpawnPsrWorker("wp_thumb_load", kThumbWorkerStack, 5, ThumbWorker, nullptr, nullptr, 0);
}

void OpenPreview(int file_index) {
    s_mode = UiMode::kPreview;
    s_preview_idx = file_index;
    s_preview_raster.reset();
    RenderCurrentView();
}

void BackToList() {
    s_mode = UiMode::kList;
    s_preview_idx = -1;
    s_preview_raster.reset();
    RenderCurrentView();
}

void RenderListView() {
    if (s_ui.content_area == nullptr) return;
    lv_obj_clean(s_ui.content_area);

    if (s_files.empty()) {
        lv_obj_t* empty_box = lv_obj_create(s_ui.content_area);
        lv_obj_remove_style_all(empty_box);
        lv_obj_set_size(empty_box, lv_pct(100), 400);
        lv_obj_set_flex_flow(empty_box, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(empty_box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(empty_box, 14, 0);
        DisableScroll(empty_box);

        lv_obj_t* tip = lv_label_create(empty_box);
        lv_label_set_text(tip, "SD 卡暂无壁纸\n请将图片文件存入\n/sdcard/metalio/wallpaper 目录\n（支持 A2I1 / PNG / JPG）");
        lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(tip, UiFont(), 0);
        lv_obj_set_style_text_color(tip, lv_color_black(), 0);
        return;
    }

    const int total_pages = TotalPages();
    if (s_cur_page >= total_pages) s_cur_page = total_pages - 1;
    if (s_cur_page < 0) s_cur_page = 0;

    // 3 列网格布局
    lv_obj_t* grid = lv_obj_create(s_ui.content_area);
    lv_obj_remove_style_all(grid);
    lv_obj_set_width(grid, lv_pct(100));
    lv_obj_set_height(grid, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(grid, 12, 0);
    DisableScroll(grid);

    const int start = s_cur_page * kItemsPerPage;
    const int end = std::min(start + kItemsPerPage, static_cast<int>(s_files.size()));

    for (int i = start; i < end; ++i) {
        const auto& item = s_files[static_cast<size_t>(i)];
        const bool is_standby = wallpaper::IsStandbyFilename(item.name.c_str());
        const bool is_shut = wallpaper::IsActiveFilename(item.name.c_str());

        lv_obj_t* card = lv_obj_create(grid);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, kCardW, kCardH);
        lv_obj_set_style_bg_color(card, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(card, lv_color_black(), 0);
        lv_obj_set_style_border_width(card, (is_standby || is_shut) ? 3 : 2, 0);
        lv_obj_set_style_radius(card, 10, 0);
        lv_obj_set_style_pad_all(card, 4, 0);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        DisableScroll(card);
        HapticAttachClick(card);

        const int item_idx = i;
        lv_obj_add_event_cb(card, [](lv_event_t* e) {
            int idx = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
            OpenPreview(idx);
        }, LV_EVENT_CLICKED, reinterpret_cast<void*>(static_cast<intptr_t>(item_idx)));

        // 缩略图区域
        lv_obj_t* img_box = lv_obj_create(card);
        lv_obj_remove_style_all(img_box);
        lv_obj_set_size(img_box, lv_pct(100), kThumbMaxH);
        lv_obj_center(img_box);
        lv_obj_clear_flag(img_box, LV_OBJ_FLAG_CLICKABLE);
        DisableScroll(img_box);

        if (s_thumbs[static_cast<size_t>(i)] != nullptr && !s_thumbs[static_cast<size_t>(i)]->empty()) {
            lv_obj_t* img = lv_image_create(img_box);
            lv_image_set_src(img, &s_thumbs[static_cast<size_t>(i)]->dsc);
            lv_obj_center(img);
            lv_obj_clear_flag(img, LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_obj_t* ph = lv_label_create(img_box);
            lv_label_set_text(ph, "加载中...");
            lv_obj_set_style_text_font(ph, UiFont(), 0);
            lv_obj_set_style_text_color(ph, lv_color_black(), 0);
            lv_obj_center(ph);
        }

        // 徽标状态
        if (is_standby || is_shut) {
            lv_obj_t* tag = lv_label_create(card);
            std::string tag_str;
            if (is_standby && is_shut) tag_str = "待机/关机";
            else if (is_standby) tag_str = "待机";
            else tag_str = "关机";
            lv_label_set_text(tag, tag_str.c_str());
            lv_obj_set_style_bg_color(tag, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(tag, LV_OPA_COVER, 0);
            lv_obj_set_style_text_color(tag, lv_color_white(), 0);
            lv_obj_set_style_text_font(tag, UiFont(), 0);
            lv_obj_set_style_pad_hor(tag, 6, 0);
            lv_obj_set_style_pad_ver(tag, 2, 0);
            lv_obj_set_style_radius(tag, 4, 0);
            lv_obj_align(tag, LV_ALIGN_TOP_LEFT, 2, 2);
        }

        // 名称
        lv_obj_t* name_lbl = lv_label_create(card);
        lv_label_set_long_mode(name_lbl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name_lbl, lv_pct(100));
        lv_label_set_text(name_lbl, item.name.c_str());
        lv_obj_set_style_text_font(name_lbl, UiFont(), 0);
        lv_obj_set_style_text_color(name_lbl, lv_color_black(), 0);
        lv_obj_set_style_text_align(name_lbl, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(name_lbl, LV_ALIGN_BOTTOM_MID, 0, -2);
    }

    // 底部页码指示器
    if (s_ui.page_lbl != nullptr) {
        char pbuf[64];
        std::snprintf(pbuf, sizeof(pbuf), "第 %d / %d 页 (共 %d 张)", s_cur_page + 1, total_pages, static_cast<int>(s_files.size()));
        lv_label_set_text(s_ui.page_lbl, pbuf);
    }
}

void OnPreviewDoneAsync(void* p) {
    auto* img = static_cast<reader::RasterImage*>(p);
    if (img == nullptr) return;
    if (!s_alive || s_mode != UiMode::kPreview) {
        delete img;
        return;
    }
    s_preview_raster.reset(img);
    if (s_ui.preview_img != nullptr && lv_obj_is_valid(s_ui.preview_img)) {
        s_preview_raster->BindDsc();
        lv_image_set_src(s_ui.preview_img, &s_preview_raster->dsc);
        lv_obj_set_size(s_ui.preview_img, s_preview_raster->width, s_preview_raster->height);
        lv_obj_center(s_ui.preview_img);
        lv_obj_clear_flag(s_ui.preview_img, LV_OBJ_FLAG_HIDDEN);
    }
}

void PreviewWorker(void* arg) {
    const int idx = static_cast<int>(reinterpret_cast<intptr_t>(arg));
    if (idx < 0 || idx >= static_cast<int>(s_files.size())) return;

    auto* img = new reader::RasterImage();
    if (reader::DecodeImageFileToL8(s_files[static_cast<size_t>(idx)].path.c_str(), kPreviewMaxW, kPreviewMaxH, *img) && !img->empty()) {
        if (!ScreenLvAsync(OnPreviewDoneAsync, img)) {
            delete img;
        }
    } else {
        delete img;
    }
}

void RenderPreviewView() {
    if (s_ui.content_area == nullptr || s_preview_idx < 0 || s_preview_idx >= static_cast<int>(s_files.size())) {
        BackToList();
        return;
    }
    lv_obj_clean(s_ui.content_area);

    const auto& item = s_files[static_cast<size_t>(s_preview_idx)];
    const bool is_standby = wallpaper::IsStandbyFilename(item.name.c_str());
    const bool is_shut = wallpaper::IsActiveFilename(item.name.c_str());

    lv_obj_t* col = lv_obj_create(s_ui.content_area);
    lv_obj_remove_style_all(col);
    lv_obj_set_size(col, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(col, 12, 0);
    DisableScroll(col);

    // 图片大预览框
    lv_obj_t* frame = lv_obj_create(col);
    lv_obj_remove_style_all(frame);
    lv_obj_set_size(frame, kPreviewMaxW, 460);
    lv_obj_set_style_border_color(frame, lv_color_black(), 0);
    lv_obj_set_style_border_width(frame, 2, 0);
    lv_obj_set_style_radius(frame, 12, 0);
    lv_obj_set_style_bg_color(frame, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    DisableScroll(frame);

    s_ui.preview_img = lv_image_create(frame);
    lv_obj_add_flag(s_ui.preview_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_ui.preview_img, LV_OBJ_FLAG_CLICKABLE);

    // 触发异步大图解码
    (void)SpawnPsrWorker("wp_prev_load", kThumbWorkerStack, 5, PreviewWorker, reinterpret_cast<void*>(static_cast<intptr_t>(s_preview_idx)), nullptr, 0);

    // 文件信息文案
    char info_buf[128];
    std::snprintf(info_buf, sizeof(info_buf), "%s  (%zu KB)", item.name.c_str(), item.size_bytes / 1024);
    lv_obj_t* info_lbl = lv_label_create(col);
    lv_label_set_text(info_lbl, info_buf);
    lv_obj_set_style_text_font(info_lbl, UiFont(), 0);
    lv_obj_set_style_text_color(info_lbl, lv_color_black(), 0);

    // 按钮操作区 (Flex row)
    lv_obj_t* btn_row = lv_obj_create(col);
    lv_obj_remove_style_all(btn_row);
    lv_obj_set_size(btn_row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(btn_row, 10, 0);
    DisableScroll(btn_row);

    auto make_action_btn = [btn_row](const char* text, bool filled, lv_event_cb_t cb) -> lv_obj_t* {
        lv_obj_t* b = lv_obj_create(btn_row);
        lv_obj_remove_style_all(b);
        lv_obj_set_size(b, 136, 42);
        lv_obj_set_style_bg_color(b, filled ? lv_color_black() : lv_color_white(), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(b, lv_color_black(), 0);
        lv_obj_set_style_border_width(b, 2, 0);
        lv_obj_set_style_radius(b, 8, 0);
        lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
        HapticAttachClick(b);
        lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, nullptr);

        lv_obj_t* l = lv_label_create(b);
        lv_label_set_text(l, text);
        lv_obj_set_style_text_font(l, UiFont(), 0);
        lv_obj_set_style_text_color(l, filled ? lv_color_white() : lv_color_black(), 0);
        lv_obj_center(l);
        return b;
    };

    // 设为待机壁纸 / 取消待机
    make_action_btn(is_standby ? "取消待机壁纸" : "设为待机壁纸", is_standby, [](lv_event_t*) {
        if (s_preview_idx < 0 || s_preview_idx >= static_cast<int>(s_files.size())) return;
        const auto& it = s_files[static_cast<size_t>(s_preview_idx)];
        if (wallpaper::IsStandbyFilename(it.name.c_str())) {
            wallpaper::ClearStandbyWallpaper();
            EpdWidgetKit::EpdToast::Show("已取消待机壁纸");
        } else {
            std::string err;
            if (wallpaper::SetStandbyFromFile(it.name.c_str(), err)) {
                EpdWidgetKit::EpdToast::Show("待机壁纸设置成功");
            } else {
                EpdWidgetKit::EpdToast::Show("设置失败");
            }
        }
        RenderPreviewView();
    });

    // 设为关机壁纸 / 取消关机
    make_action_btn(is_shut ? "取消关机壁纸" : "设为关机壁纸", is_shut, [](lv_event_t*) {
        if (s_preview_idx < 0 || s_preview_idx >= static_cast<int>(s_files.size())) return;
        const auto& it = s_files[static_cast<size_t>(s_preview_idx)];
        if (wallpaper::IsActiveFilename(it.name.c_str())) {
            wallpaper::ClearShutdownWallpaper();
            EpdWidgetKit::EpdToast::Show("已取消关机壁纸");
        } else {
            std::string err;
            if (wallpaper::SetActiveFromFile(it.name.c_str(), err)) {
                EpdWidgetKit::EpdToast::Show("关机壁纸设置成功");
            } else {
                EpdWidgetKit::EpdToast::Show("设置失败");
            }
        }
        RenderPreviewView();
    });

    // 删除壁纸
    make_action_btn("删除此壁纸", false, [](lv_event_t*) {
        if (s_preview_idx < 0 || s_preview_idx >= static_cast<int>(s_files.size())) return;
        const std::string fn = s_files[static_cast<size_t>(s_preview_idx)].name;
        EpdWidgetKit::EpdDialog::Show("删除壁纸", "确定删除此壁纸吗？", "删除", [fn](bool confirm) {
            if (confirm) {
                std::string p = std::string(kPosixDir) + "/" + fn;
                unlink(p.c_str());
                wallpaper::ClearActiveIfMatches(fn.c_str());
                EpdWidgetKit::EpdToast::Show("壁纸已删除");
                ScanWallpapers();
                BackToList();
            }
        });
    });

    // 返回列表
    make_action_btn("返回列表", false, [](lv_event_t*) {
        BackToList();
    });
}

void SetHeaderTitle(const char* title) {
    if (s_ui.scaffold != nullptr && s_ui.scaffold->GetHeader() != nullptr) {
        lv_obj_t* title_lbl = lv_obj_get_child(s_ui.scaffold->GetHeader(), 1);
        if (title_lbl != nullptr) {
            lv_label_set_text(title_lbl, title != nullptr ? title : "");
        }
    }
}

void RenderCurrentView() {
    if (s_mode == UiMode::kList) {
        SetHeaderTitle("壁纸图库");
        RenderListView();
        StartThumbnailsLoad();
    } else {
        SetHeaderTitle("壁纸预览");
        RenderPreviewView();
    }
}

bool WallpaperOnVkKey(const char* key) {
    if (key == nullptr) return false;
    if (std::strcmp(key, "vk_home") == 0) {
        if (s_mode == UiMode::kPreview) {
            BackToList();
            return true;
        }
        ScreenRequestBack();
        return true;
    }
    if (std::strcmp(key, "vk_prev") == 0) {
        if (s_mode == UiMode::kPreview) {
            BackToList();
            return true;
        }
        if (s_cur_page > 0) {
            --s_cur_page;
            RenderCurrentView();
        }
        return true;
    }
    if (std::strcmp(key, "vk_next") == 0) {
        if (s_mode == UiMode::kPreview) {
            return true;
        }
        if (s_cur_page + 1 < TotalPages()) {
            ++s_cur_page;
            RenderCurrentView();
        }
        return true;
    }
    return false;
}

void OnScreenDeleted(lv_event_t*) {
    s_alive = false;
    s_thumb_token.fetch_add(1);
    s_preview_token.fetch_add(1);
    s_files.clear();
    s_thumbs.clear();
    s_preview_raster.reset();
    s_ui = UiState{};
}

}  // namespace

lv_obj_t* WallpaperScreen::Create() {
    s_alive = true;
    s_mode = UiMode::kList;
    s_cur_page = 0;
    s_preview_idx = -1;
    s_ui = UiState{};
    ScreenSetIsHome(false);

    wallpaper::HydrateFromNvsNow();
    ScanWallpapers();

    auto* scaffold = EpdWidgetKit::EpdPageScaffold::Create({
        .title = "壁纸图库",
        .show_header = true,
        .show_status_bar = true,
        .show_back_btn = true,
        .scrollable = false,
    });
    s_ui.scaffold = scaffold;
    lv_obj_t* scr = scaffold->GetScreen();
    s_ui.screen = scr;
    lv_obj_add_event_cb(scr, OnScreenDeleted, LV_EVENT_DELETE, nullptr);

    lv_obj_t* body = scaffold->GetBody();

    // 顶部内容区 (用于放缩略图网格或大图预览)
    s_ui.content_area = lv_obj_create(body);
    lv_obj_remove_style_all(s_ui.content_area);
    lv_obj_set_size(s_ui.content_area, lv_pct(100), 650);
    lv_obj_align(s_ui.content_area, LV_ALIGN_TOP_MID, 0, 0);
    DisableScroll(s_ui.content_area);

    // 底部控制与页码栏
    lv_obj_t* footer = lv_obj_create(body);
    lv_obj_remove_style_all(footer);
    lv_obj_set_size(footer, lv_pct(100), 50);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_border_side(footer, LV_BORDER_SIDE_TOP, 0);
    lv_obj_set_style_border_width(footer, 1, 0);
    lv_obj_set_style_border_color(footer, lv_color_make(200, 200, 200), 0);
    DisableScroll(footer);

    s_ui.page_lbl = lv_label_create(footer);
    lv_label_set_text(s_ui.page_lbl, "");
    lv_obj_set_style_text_font(s_ui.page_lbl, UiFont(), 0);
    lv_obj_set_style_text_color(s_ui.page_lbl, lv_color_black(), 0);
    lv_obj_center(s_ui.page_lbl);

    RenderCurrentView();

    VkKey_AttachScreen(scr, kScreenId, VkKeyScreenDesc{WallpaperScreen::Create, WallpaperOnVkKey, nullptr, nullptr});
    return scr;
}
