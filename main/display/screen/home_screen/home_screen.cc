#include "home_screen.h"
#include "ic_app_home_store.h"

#include "app_manager.h"
#include "application.h"
#include "home_screen/home_hero.h"
#include "assets/lang_config.h"
#include "haptic_feedback.h"
#include "lv_adapter_display.h"
#include "screen_common.h"
#include "settings.h"
#include "services/wifi_transfer/wifi_transfer_service.h"
#include "services/system_upgrade/system_upgrade_service.h"
#include "vk_key_handler.h"
#include "vk_page_repeat.h"

#include <cstdio>
#include <cstring>
#include <string>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "fontpack_lvgl.h"


// ---- 首页布局（逻辑分辨率约 480x800，旋转 270°）----
#define HOME_STATUS_BAR_H 45          // 状态栏高度 (px)
#define HOME_APPS_PAD_TOP 10          // 应用区顶部内边距 (px)；贴底时仅作内容间距
#define HOME_APPS_PAD_BOT 10          // 应用区底部内边距 (px)
#define HOME_APPS_LIFT 48             // 宫格整体相对屏底上移 (px)
#define HOME_APP_CARD_W 116           // 应用图标卡片宽 (px)
#define HOME_APP_CARD_H 136           // 应用图标卡片高 (px)
#define HOME_APP_CARD_MARGIN_H 38     // 每行应用卡片距左右边距 (px)
#define HOME_APP_CARD_GAP_COL 28      // 同行相邻应用卡片间距 (px)
#define HOME_APP_CARD_GAP_ROW 36      // 上下两行应用卡片间距 (px)
#define HOME_APP_CARD_ICON_PAD_TOP 10 // 卡片内图标距卡片顶部 (px)，水平居中
#define HOME_APP_CARD_NAME_PAD_BOT 10 // 卡片内名称距卡片底部 (px)，水平居中
#define HOME_APP_CARD_BORDER_W 4      // 描边样式时黑色描边宽度 (px)
#define HOME_PAGE_INDICATOR_H 18      // 多页时底部圆点指示高度 (px);圆点取代"1/2"文字,压矮抽屉
// NVS 无记录时的默认卡片样式：0=白底，1=网点灰底，2=白底+黑色描边
#define HOME_APP_CARD_STYLE_DEFAULT HomeScreen::kCardStyleBorder

namespace
{

    constexpr const char *TAG = "HomeScreen";
    constexpr const char *kScreenId = "home";
    constexpr lv_coord_t kIconSize = 70;
    constexpr lv_coord_t kCardRadius = 20;
    constexpr int kColsPerRow = 3;
    constexpr int kRowsPerPage = 3;   // 每页 3 行(贴合 800 竖屏:hero+状态栏后恰好 3 行)
    constexpr int kAppsPerPage = kColsPerRow * kRowsPerPage; // 每页 9 个

    // I1 无真灰阶：#AAAAAA 会量化成白。共用 8x8 L8 平铺（64B），稀疏网点近似浅灰。
    constexpr int kDitherW = 8;
    constexpr int kDitherH = 8;
    // 4x4 Bayer：黑点越少越淡；阈值 2 → 2/16=12.5% 黑
    constexpr uint8_t kBayer4[4][4] = {
        {0, 8, 2, 10},
        {12, 4, 14, 6},
        {3, 11, 1, 9},
        {15, 7, 13, 5},
    };
    constexpr int kBayerThreshold = 2; // 0..15，<2 为黑 → 2/16=12.5%
    uint8_t s_gray_dither_l8[kDitherW * kDitherH];
    lv_image_dsc_t s_gray_dither_img;
    bool s_gray_dither_ready = false;

    const lv_image_dsc_t *GrayDitherImg()
    {
        if (!s_gray_dither_ready)
        {
            for (int y = 0; y < kDitherH; ++y)
            {
                for (int x = 0; x < kDitherW; ++x)
                {
                    const bool black = kBayer4[y & 3][x & 3] < kBayerThreshold;
                    s_gray_dither_l8[y * kDitherW + x] = black ? 0x00 : 0xFF;
                }
            }
            s_gray_dither_img.header.magic = LV_IMAGE_HEADER_MAGIC;
            s_gray_dither_img.header.cf = LV_COLOR_FORMAT_L8;
            s_gray_dither_img.header.flags = 0;
            s_gray_dither_img.header.w = kDitherW;
            s_gray_dither_img.header.h = kDitherH;
            s_gray_dither_img.header.stride = kDitherW;
            s_gray_dither_img.data_size = sizeof(s_gray_dither_l8);
            s_gray_dither_img.data = s_gray_dither_l8;
            s_gray_dither_ready = true;
        }
        return &s_gray_dither_img;
    }

    void ApplyCardStyle(lv_obj_t *cell, int style)
    {
        if (style == HomeScreen::kCardStyleGray)
        {
            lv_obj_set_style_bg_opa(cell, LV_OPA_TRANSP, 0);
            lv_obj_set_style_bg_image_src(cell, GrayDitherImg(), 0);
            lv_obj_set_style_bg_image_tiled(cell, true, 0);
            lv_obj_set_style_border_width(cell, 0, 0);
            return;
        }

        lv_obj_set_style_bg_color(cell, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(cell, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_image_src(cell, nullptr, 0);
        if (style == HomeScreen::kCardStyleBorder)
        {
            lv_obj_set_style_border_width(cell, HOME_APP_CARD_BORDER_W, 0);
            lv_obj_set_style_border_color(cell, lv_color_black(), 0);
            lv_obj_set_style_border_opa(cell, LV_OPA_COVER, 0);
        }
        else
        {
            lv_obj_set_style_border_width(cell, 0, 0);
        }
    }

    lv_obj_t *s_apps = nullptr;
    lv_obj_t *s_page_dots = nullptr;
    lv_obj_t *s_home_scr = nullptr;
    int s_home_page = 0; // 离开子页再回首页时保持页码
    lv_obj_t *CreateAppCell(lv_obj_t *parent, AppModule &entry, int card_style);

    int PageCount()
    {
        const int enabled = AppManager::GetInstance().GetEnabledAppCount();
        return (enabled + kAppsPerPage - 1) / kAppsPerPage;
    }

    // 圆点页码指示刷新:pages>1 时按页数重建小圆点(当前页实心,其余空心)。
    // FillAppsPage 每次翻页都会调;圆点不可点,此处重建无"删被点对象"问题。
    void UpdatePageDots()
    {
        if (s_page_dots == nullptr)
        {
            return;
        }
        const int pages = PageCount();
        if (pages <= 1)
        {
            lv_obj_add_flag(s_page_dots, LV_OBJ_FLAG_HIDDEN);
            return;
        }
        lv_obj_clear_flag(s_page_dots, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clean(s_page_dots);
        for (int i = 0; i < pages; ++i)
        {
            lv_obj_t *dot = lv_obj_create(s_page_dots);
            lv_obj_remove_style_all(dot);
            lv_obj_set_size(dot, 10, 10);
            lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
            if (i == s_home_page)
            {
                lv_obj_set_style_bg_color(dot, lv_color_black(), 0);
                lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
            }
            else
            {
                lv_obj_set_style_bg_color(dot, lv_color_white(), 0);
                lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
                lv_obj_set_style_border_color(dot, lv_color_black(), 0);
                lv_obj_set_style_border_width(dot, 1, 0);
            }
            lv_obj_clear_flag(dot, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
        }
    }

    void FillAppsPage()
    {
        if (s_apps == nullptr)
        {
            return;
        }
        const int pages = PageCount();
        if (pages < 1)
        {
            return;
        }
        if (s_home_page < 0)
        {
            s_home_page = 0;
        }
        if (s_home_page >= pages)
        {
            s_home_page = pages - 1;
        }

        lv_obj_clean(s_apps);
        const int card_style = HomeScreen::LoadCardStyle();
        auto enabled = AppManager::GetInstance().GetEnabledApps();
        const int enabled_cnt = static_cast<int>(enabled.size());
        const int start = s_home_page * kAppsPerPage;
        int end = start + kAppsPerPage;
        if (end > enabled_cnt)
        {
            end = enabled_cnt;
        }
        for (int i = start; i < end; ++i)
        {
            CreateAppCell(s_apps, *enabled[i], card_style);
        }

        UpdatePageDots();
        ESP_LOGI(TAG, "home page %d/%d apps %d..%d of %d", s_home_page + 1, pages, start, end - 1,
                 enabled_cnt);
    }

    ScreenPaintCoalesce s_home_paint{};

    void RequestFillAppsPage() {
        if (s_home_paint.paint == nullptr) {
            s_home_paint.paint = FillAppsPage;
        }
        ScreenPaintCoalesceRequest(&s_home_paint);
    }

    bool OnVkKey(const char *key)
    {
        if (key == nullptr)
        {
            return false;
        }
        // vk_home 走默认（已在首页 no-op）
        if (std::strcmp(key, "vk_prev") == 0)
        {
            if (s_home_page > 0)
            {
                --s_home_page;
                RequestFillAppsPage();
            }
            return true;
        }
        if (std::strcmp(key, "vk_next") == 0)
        {
            if (s_home_page + 1 < PageCount())
            {
                ++s_home_page;
                RequestFillAppsPage();
            }
            return true;
        }
        return false;
    }

    bool HomePageRepeatStep(int page_delta)
    {
        if (page_delta == 0)
        {
            return false;
        }
        const int last = PageCount() > 0 ? PageCount() - 1 : 0;
        int next = s_home_page + page_delta;
        if (next < 0)
        {
            next = 0;
        }
        else if (next > last)
        {
            next = last;
        }
        if (next == s_home_page)
        {
            return false;
        }
        s_home_page = next;
        RequestFillAppsPage();
        return page_delta < 0 ? s_home_page > 0 : s_home_page < last;
    }

    // 翻页(供手势/虚拟键复用):delta=+1 下一页,-1 上一页,到边界不动
    void HomeGoPageDelta(int delta)
    {
        const int last = PageCount() > 0 ? PageCount() - 1 : 0;
        int next = s_home_page + delta;
        if (next < 0) next = 0;
        else if (next > last) next = last;
        if (next == s_home_page) return;
        s_home_page = next;
        RequestFillAppsPage();
    }

    // 原生 LVGL 滑动手势(需求:触摸手势 + 首页多屏)。LVGL 9 的 LV_EVENT_GESTURE 默认
    // 启用(无需改配置),触摸子系统已上报连续 press/move/release,满足 50px 手势阈值。
    // 约定:左滑(内容左移)= 下一页,右滑 = 上一页(类似安卓);上/下滑暂不翻页。
    void OnHomeGesture(lv_event_t *e)
    {
        lv_indev_t *indev = lv_indev_active();
        if (indev == nullptr) return;
        // 手势已被本屏认领(位移超手势阈值):claim-then-cancel(issue #43,
        // Android ACTION_CANCEL / iOS touchesCancelled 同义)。LVGL 9 的 release
        // 路径只被真 scroll 抑制 CLICKED,纯手势后照样派发 → 这里必须主动取消:
        // 1) lv_indev_reset 清 act_obj 并给原按压对象发 INDEV_RESET;
        // 2) lv_indev_wait_release 冻结后续 move 样本的重命中(reset 后 act_obj
        //    为空,indev_proc_press 会重新搜索手指下对象,不冻结则换目标再点)。
        lv_indev_reset(indev, nullptr);
        lv_indev_wait_release(indev);
        switch (lv_indev_get_gesture_dir(indev))
        {
            case LV_DIR_LEFT:  HomeGoPageDelta(+1); break;
            case LV_DIR_RIGHT: HomeGoPageDelta(-1); break;
            case LV_DIR_TOP:
            case LV_DIR_BOTTOM:
            default: break;
        }
    }

    bool OnVkKeyLongPress(const char *key)
    {
        return VkPageRepeatTryStart(key, HomePageRepeatStep);
    }

    bool OnVkKeyPressUp(const char *key)
    {
        return VkPageRepeatOnPressUp(key);
    }

    // BOOT 长按快捷进百问(助手模块已移除)
    bool OnBootLongPress()
    {
        ESP_LOGI(TAG, "boot long -> ignore (assistant removed)");
        return false;
    }

    bool s_launching = false;

    void OnHomeDeleted(lv_event_t *e)
    {
        // ScreenLoadReplace 会 async 删旧屏；勿清掉新首页的指针
        if (lv_event_get_target(e) != s_home_scr)
        {
            return;
        }
        home_hero::Teardown();
        s_home_scr = nullptr;
        s_apps = nullptr;
        s_page_dots = nullptr;
        s_launching = false;
    }

    void LaunchAppAsync(void *user_data)
    {
        auto *entry = static_cast<AppModule *>(user_data);
        if (entry == nullptr)
        {
            s_launching = false;
            return;
        }
        ESP_LOGI(TAG, "launch %s", entry->GetName());
        auto factory = entry->GetScreenFactory();
        if (factory) {
            ScreenNavigateTo(factory);
        } else {
            ScreenSetIsHome(false);
            ScreenLoadReplace(entry->CreateUI(nullptr));
        }
    }

    void OnAppClicked(lv_event_t *e)
    {
        if (s_launching)
        {
            return;
        }
        lv_obj_t *obj = static_cast<lv_obj_t *>(lv_event_get_target(e));
        AppModule *entry = nullptr;
        while (obj != nullptr)
        {
            entry = static_cast<AppModule *>(lv_obj_get_user_data(obj));
            if (entry != nullptr)
            {
                break;
            }
            obj = lv_obj_get_parent(obj);
        }
        if (entry == nullptr)
        {
            entry = static_cast<AppModule *>(lv_event_get_user_data(e));
        }
        if (entry == nullptr)
        {
            return;
        }
        s_launching = true;
        // 事件回调里删当前屏不安全，延后一拍再切（与返回键一致）
        lv_async_call(LaunchAppAsync, entry);
    }

    lv_obj_t *CreateAppCell(lv_obj_t *parent, AppModule &entry, int card_style)
    {
        // 整卡为唯一点击热区；样式来自设置→主题（NVS）。
        lv_obj_t *cell = lv_obj_create(parent);
        lv_obj_remove_style_all(cell);
        lv_obj_set_size(cell, HOME_APP_CARD_W, HOME_APP_CARD_H);
        lv_obj_set_style_pad_all(cell, 0, 0);
        ApplyCardStyle(cell, card_style);
        lv_obj_set_style_radius(cell, kCardRadius, 0);
        lv_obj_set_style_clip_corner(cell, true, 0);
        lv_obj_set_style_layout(cell, LV_LAYOUT_NONE, 0);
        lv_obj_clear_flag(cell, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(cell, LV_OBJ_FLAG_CLICKABLE);
        // 手势冒泡:在卡片上起手的左右滑动要冒泡到宫格容器,由屏的手势处理器翻页
        lv_obj_add_flag(cell, LV_OBJ_FLAG_GESTURE_BUBBLE);
        HapticAttachClick(cell);
        lv_obj_set_user_data(cell, &entry);
        lv_obj_add_event_cb(cell, OnAppClicked, LV_EVENT_CLICKED, &entry);
        lv_obj_t *icon = lv_image_create(cell);
        if (std::strcmp(entry.GetId(), "store") == 0) {
            lv_image_set_src(icon, &ic_app_home_store_dsc);
        } else {
            lv_image_set_src(icon, entry.GetIconPath());
        }
        lv_obj_set_size(icon, kIconSize, kIconSize);
        lv_obj_align(icon, LV_ALIGN_TOP_MID, 0, HOME_APP_CARD_ICON_PAD_TOP);
        lv_obj_clear_flag(icon, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *name = lv_label_create(cell);
        lv_label_set_text(name, entry.GetName());
        lv_obj_set_width(name, HOME_APP_CARD_W);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_font(name, fontpack_lv_font_ui(), 0);
        lv_obj_set_style_text_color(name, lv_color_black(), 0);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(name, LV_ALIGN_BOTTOM_MID, 0, -HOME_APP_CARD_NAME_PAD_BOT);
        lv_obj_clear_flag(name, LV_OBJ_FLAG_CLICKABLE);

        return cell;
    }

    // LVGL worker 栈在 PSRAM：不可在该任务里直接 NVS 写 flash（会关 cache 触发断言）。
    int s_card_style = HOME_APP_CARD_STYLE_DEFAULT;
    bool s_card_style_ready = false;

    int NormalizeCardStyle(int style)
    {
        if (style == HomeScreen::kCardStyleWhite || style == HomeScreen::kCardStyleGray ||
            style == HomeScreen::kCardStyleBorder)
        {
            return style;
        }
        return HOME_APP_CARD_STYLE_DEFAULT;
    }

    void PersistCardStyleTask(void *arg)
    {
        const int style = static_cast<int>(reinterpret_cast<intptr_t>(arg));
        Settings settings("display", true);
        settings.SetInt("card_style", style);
        ESP_LOGI(TAG, "nvs card_style=%d", style);
        vTaskDelete(nullptr);
    }

    void LoadCardStyleFromNvs()
    {
        Settings settings("display", false);
        s_card_style =
            NormalizeCardStyle(static_cast<int>(settings.GetInt("card_style", HOME_APP_CARD_STYLE_DEFAULT)));
        s_card_style_ready = true;
    }

} // namespace

int HomeScreen::LoadCardStyle()
{
    if (!s_card_style_ready)
    {
        // 首次读取：只读 NVS；之后一律走内存缓存，避免 LVGL 任务反复碰 flash。
        LoadCardStyleFromNvs();
    }
    return s_card_style;
}

void HomeScreen::SaveCardStyle(int style)
{
    style = NormalizeCardStyle(style);
    s_card_style = style;
    s_card_style_ready = true;
    // 立刻更新内存；写 NVS 放到内部 RAM 栈任务（与网络切换同理）。
    if (xTaskCreate(PersistCardStyleTask, "card_style", 4096,
                    reinterpret_cast<void *>(static_cast<intptr_t>(style)), 5, nullptr) != pdPASS)
    {
        ESP_LOGE("HomeScreen", "xTaskCreate(card_style) failed");
    }
}

// ---- APP 开关公开实现（转由 AppManager 统一管理）----

void HomeScreen::InitAppToggles()
{
    AppManager::GetInstance().Init();
}

int HomeScreen::GetAppToggleCount()
{
    return AppManager::GetInstance().GetAppCount();
}

const char *HomeScreen::GetAppToggleName(int index)
{
    return AppManager::GetInstance().GetAppNameByIndex(index);
}

bool HomeScreen::IsAppEnabledByIndex(int index)
{
    return AppManager::GetInstance().IsAppEnabledByIndex(index);
}

bool HomeScreen::IsAppMandatoryByIndex(int index)
{
    return AppManager::GetInstance().IsAppMandatoryByIndex(index);
}

bool HomeScreen::IsAppEnabledById(const char *id)
{
    return AppManager::GetInstance().IsAppEnabled(id);
}

void HomeScreen::RegisterEnabledAppMcpTools()
{
    AppManager::GetInstance().RegisterEnabledMcpTools();
}

void HomeScreen::SetAppEnabled(int index, bool enabled)
{
    AppManager::GetInstance().SetAppEnabled(index, enabled);
}

lv_obj_t *HomeScreen::Create()
{
    // 确保回到首页时，传书与升级服务彻底停止，释放所有端口与传输缓存
    wifi_transfer::WifiTransferService::GetInstance().Stop();
    system_upgrade::SystemUpgradeService::GetInstance().StopWebServer();

    lv_obj_t *scr = lv_obj_create(nullptr);
    s_home_scr = scr;
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_font(scr, fontpack_lv_font_ui(), 0);
    lv_obj_set_style_text_color(scr, lv_color_black(), 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    EpdStatusBar status = ScreenCreateStatusBar(scr);
    status.height = HOME_STATUS_BAR_H;
    if (status.bar != nullptr)
    {
        lv_obj_set_size(status.bar, LV_HOR_RES, HOME_STATUS_BAR_H);
    }
    if (status.overlay != nullptr)
    {
        lv_obj_set_size(status.overlay, LV_HOR_RES, HOME_STATUS_BAR_H);
    }

    const int card_style = LoadCardStyle();
    const int total_apps = AppManager::GetInstance().GetAppCount();
    const int pages = PageCount();
    ESP_LOGI(TAG, "layout apps=%d pages=%d per_page=%d style=%d", total_apps, pages,
             kAppsPerPage, card_style);

    // 上半：时分 + 日期天气（历史经典待机上半区；不含待办）
    home_hero::Mount(scr, status.height);
    home_hero::Start();

    // 多屏分页(需求:触摸手势+多屏):宫格容器固定为"单页视口"(最多 kRowsPerPage 行,
    // 贴合屏幕),超出的 APP 落到下一页,由左右滑动/盖板虚拟键切换 + 页码指示。
    // 不再是"行数随 app 总数无限长高"(那样超一屏会溢出且无分页可达)。
    const int total_rows = (total_apps + kColsPerRow - 1) / kColsPerRow;
    const int apps_rows = total_rows < kRowsPerPage ? total_rows : kRowsPerPage;
    const lv_coord_t apps_content_h =
        HOME_APPS_PAD_TOP + apps_rows * HOME_APP_CARD_H +
        (apps_rows > 1 ? (apps_rows - 1) * HOME_APP_CARD_GAP_ROW : 0) +
        HOME_APPS_PAD_BOT;
    // 圆点指示画在宫格下方的 HOME_APPS_LIFT 空余带内(y=-2 贴底),不额外
    // 抬高宫格——否则多页时宫格上移 18px 压住大字钟表(实测钟表显示不全)。
    const lv_coord_t apps_bottom_inset = HOME_APPS_LIFT;

    s_apps = lv_obj_create(scr);
    lv_obj_remove_style_all(s_apps);
    lv_obj_set_size(s_apps, LV_HOR_RES, apps_content_h);
    lv_obj_align(s_apps, LV_ALIGN_BOTTOM_MID, 0, -apps_bottom_inset);
    lv_obj_set_style_bg_color(s_apps, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_apps, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_top(s_apps, HOME_APPS_PAD_TOP, 0);
    lv_obj_set_style_pad_bottom(s_apps, HOME_APPS_PAD_BOT, 0);
    lv_obj_set_style_pad_left(s_apps, HOME_APP_CARD_MARGIN_H, 0);
    lv_obj_set_style_pad_right(s_apps, HOME_APP_CARD_MARGIN_H, 0);
    lv_obj_set_style_pad_column(s_apps, HOME_APP_CARD_GAP_COL, 0);
    lv_obj_set_style_pad_row(s_apps, HOME_APP_CARD_GAP_ROW, 0);
    lv_obj_set_flex_flow(s_apps, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(s_apps, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_clear_flag(s_apps, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_apps, LV_OBJ_FLAG_CLICKABLE);
    // 手势冒泡:宫格空白处起手的滑动冒泡到屏;卡片上的滑动由卡片自身的冒泡上来
    lv_obj_add_flag(s_apps, LV_OBJ_FLAG_GESTURE_BUBBLE);

    // 圆点页码指示(替换"1/2"文字):一排小圆点,当前页实心黑,其余空心描边。
    // 高度 18px(原 36px),多页时少占底部,不再遮挡大字时钟。
    s_page_dots = lv_obj_create(scr);
    lv_obj_remove_style_all(s_page_dots);
    lv_obj_set_size(s_page_dots, LV_SIZE_CONTENT, HOME_PAGE_INDICATOR_H);
    lv_obj_align(s_page_dots, LV_ALIGN_BOTTOM_MID, 0, -2);
    lv_obj_set_flex_flow(s_page_dots, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_page_dots, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_page_dots, 8, 0);
    lv_obj_clear_flag(s_page_dots, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_page_dots, LV_OBJ_FLAG_CLICKABLE);
    if (pages <= 1)
    {
        lv_obj_add_flag(s_page_dots, LV_OBJ_FLAG_HIDDEN);
    }

    FillAppsPage();

    // 原生 LVGL 滑动手势:左右滑切宫格页(多屏,需求)。订阅在屏根对象;卡片与宫格容器
    // 都开了 GESTURE_BUBBLE,手势冒泡到这里统一处理(单点,不重复触发)。
    lv_obj_add_event_cb(scr, OnHomeGesture, LV_EVENT_GESTURE, nullptr);

    lv_obj_add_event_cb(scr, OnHomeDeleted, LV_EVENT_DELETE, nullptr);
    ScreenSetIsHome(true);
    VkKey_AttachScreen(scr, kScreenId,
                       VkKeyScreenDesc{HomeScreen::Create, OnVkKey, nullptr, OnBootLongPress,
                                       nullptr, nullptr, OnVkKeyLongPress, OnVkKeyPressUp});
    return scr;
}

void HomeScreenRequestAppsRefresh() {
    if (s_apps == nullptr) {
        return;  // 首页未存活（不在首页或尚未创建）
    }
    // 线程安全投递：调用方可能在主循环（SD 推送重扫上下文）
    ScreenLvAsync([](void*) { RequestFillAppsPage(); }, nullptr);
}
