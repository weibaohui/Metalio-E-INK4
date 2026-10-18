// tools/info_app/main/info_main.c
// 系统信息 动态应用：.app Bundle 规范版
// 纯 C + LVGL，仅调用固件符号表导出接口。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "lvgl.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_body = NULL;

static void *info_create(void);

static void fmt_kb(uint64_t bytes, char *out, size_t out_len) {
    if (bytes >= 1024ULL * 1024ULL * 1024ULL) {
        snprintf(out, out_len, "%.1f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024ULL * 1024ULL) {
        snprintf(out, out_len, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else {
        snprintf(out, out_len, "%u KB", (unsigned)(bytes / 1024ULL));
    }
}

static void make_section_bar(lv_obj_t *parent, const char *title) {
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 448, 30);
    lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(bar, 8, 0);
    lv_obj_set_style_radius(bar, 4, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(bar);
    lv_label_set_text(lbl, title);
    const lv_font_t *f18 = fontpack_lv_font_get(18, 2);
    if (f18 == NULL) f18 = fontpack_lv_font_ui();
    if (f18 != NULL) lv_obj_set_style_text_font(lbl, f18, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
}

static void make_info_label(lv_obj_t *parent, const char *text) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    const lv_font_t *f = fontpack_lv_font_ui();
    if (f != NULL) lv_obj_set_style_text_font(lbl, f, 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_set_width(lbl, 440);
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
}

static void fill_body(void) {
    if (s_body == NULL) return;

    // 清空重绘
    lv_obj_clean(s_body);

    dynapp_sysinfo_t info;
    if (!dynapp_sysinfo_get(&info)) {
        make_info_label(s_body, "获取系统信息失败");
        return;
    }

    char line[160];
    char a[24], b[24], c[24], d[24];

    // ==================== 1. 内存 (RAM / PSRAM) ====================
    make_section_bar(s_body, "内存 (RAM / PSRAM)");

    // 内部 RAM
    fmt_kb(info.ram_free, a, sizeof(a));
    fmt_kb(info.ram_total, b, sizeof(b));
    fmt_kb(info.ram_largest, c, sizeof(c));
    fmt_kb(info.ram_min_free, d, sizeof(d));
    int ram_used_pct = info.ram_total > 0 ? (int)((info.ram_total - info.ram_free) * 100 / info.ram_total) : 0;
    snprintf(line, sizeof(line), "内部 RAM: 空闲 %s / 共 %s (已用 %d%%)", a, b, ram_used_pct);
    make_info_label(s_body, line);
    snprintf(line, sizeof(line), "  最大连续块 %s; 历史最低空闲 %s", c, d);
    make_info_label(s_body, line);

    // PSRAM
    fmt_kb(info.psram_free, a, sizeof(a));
    fmt_kb(info.psram_total, b, sizeof(b));
    fmt_kb(info.psram_largest, c, sizeof(c));
    fmt_kb(info.psram_min_free, d, sizeof(d));
    int psram_used_pct = info.psram_total > 0 ? (int)((info.psram_total - info.psram_free) * 100 / info.psram_total) : 0;
    snprintf(line, sizeof(line), "PSRAM: 空闲 %s / 共 %s (已用 %d%%)", a, b, psram_used_pct);
    make_info_label(s_body, line);
    snprintf(line, sizeof(line), "  最大连续块 %s; 历史最低空闲 %s", c, d);
    make_info_label(s_body, line);

    // ==================== 2. 存储 (ROM / SD) ====================
    make_section_bar(s_body, "存储 (ROM / SD)");

    fmt_kb(info.flash_size, a, sizeof(a));
    if (info.app_part_size > 0) {
        fmt_kb(info.app_part_size, b, sizeof(b));
        snprintf(line, sizeof(line), "Flash: %s | 当前分区: %s (%s)", a, b, info.app_part_label);
    } else {
        snprintf(line, sizeof(line), "Flash: %s", a);
    }
    make_info_label(s_body, line);

    if (info.sd_mounted) {
        fmt_kb(info.sd_total_bytes, a, sizeof(a));
        fmt_kb(info.sd_free_bytes, b, sizeof(b));
        snprintf(line, sizeof(line), "SD 卡: 已挂载, 共 %s, 可用 %s", a, b);
    } else {
        snprintf(line, sizeof(line), "SD 卡: 未挂载 / 未插入");
    }
    make_info_label(s_body, line);

    // ==================== 3. NVS (键值存储) ====================
    make_section_bar(s_body, "NVS (系统键值存储)");
    snprintf(line, sizeof(line), "条目: 已用 %u / 共 %u (命名空间 %u 个)",
             (unsigned)info.nvs_used, (unsigned)info.nvs_total, (unsigned)info.nvs_ns_count);
    make_info_label(s_body, line);

    // ==================== 4. 系统信息 ====================
    make_section_bar(s_body, "系统与硬件");
    snprintf(line, sizeof(line), "芯片型号: %s", info.chip_model);
    make_info_label(s_body, line);
    snprintf(line, sizeof(line), "ESP-IDF 版本: v%s", info.idf_ver);
    make_info_label(s_body, line);
    snprintf(line, sizeof(line), "Wi-Fi MAC 地址: %s", info.mac);
    make_info_label(s_body, line);

    int64_t up = info.uptime_seconds;
    snprintf(line, sizeof(line), "开机连续运行: %lld天 %02lld:%02lld:%02lld",
             (long long)(up / 86400), (long long)((up / 3600) % 24),
             (long long)((up / 60) % 60), (long long)(up % 60));
    make_info_label(s_body, line);
}

static void on_refresh_clicked(lv_event_t *e) {
    fill_body();
    dynapp_toast_show("信息已更新", 1000);
}

static void on_back_clicked(lv_event_t *e) {
    dynapp_screen_go_home();
}

static int info_on_vk_key(const char *key) {
    if (key == NULL) return 0;
    if (strcmp(key, "home") == 0 || strcmp(key, "prev") == 0 || strcmp(key, "vk_prev") == 0) {
        dynapp_screen_go_home();
        return 1;
    }
    return 0;
}

static void info_screen_delete_cb(lv_event_t *e) {
    s_scr = NULL;
    s_body = NULL;
}

static void *info_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 800);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_scr = scr;

    lv_obj_add_event_cb(scr, info_screen_delete_cb, LV_EVENT_DELETE, NULL);

    // 1. 系统顶部状态栏
    dynapp_screen_status_bar(scr);

    // 2. 虚拟键
    dynapp_vk_attach(scr, "info", info_create, info_on_vk_key);

    // 3. 顶部 Header (返回按钮 + 标题 + 刷新按钮)
    lv_obj_t *header = lv_obj_create(scr);
    lv_obj_remove_style_all(header);
    lv_obj_set_size(header, 480, 56);
    lv_obj_set_pos(header, 0, 36);

    lv_obj_t *back_btn = lv_obj_create(header);
    lv_obj_remove_style_all(back_btn);
    lv_obj_set_size(back_btn, 64, 48);
    lv_obj_set_pos(back_btn, 12, 4);
    lv_obj_add_flag(back_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(back_btn, on_back_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *back_lbl = lv_label_create(back_btn);
    lv_label_set_text(back_lbl, "‹ 返回");
    const lv_font_t *f22 = fontpack_lv_font_get(22, 2);
    if (f22 == NULL) f22 = fontpack_lv_font_ui();
    if (f22 != NULL) lv_obj_set_style_text_font(back_lbl, f22, 0);
    lv_obj_set_style_text_color(back_lbl, lv_color_black(), 0);
    lv_obj_center(back_lbl);

    lv_obj_t *title_lbl = lv_label_create(header);
    lv_label_set_text(title_lbl, "系统信息");
    const lv_font_t *f26 = fontpack_lv_font_get(26, 2);
    if (f26 == NULL) f26 = fontpack_lv_font_ui();
    if (f26 != NULL) lv_obj_set_style_text_font(title_lbl, f26, 0);
    lv_obj_set_style_text_color(title_lbl, lv_color_black(), 0);
    lv_obj_align(title_lbl, LV_ALIGN_CENTER, 0, 0);

    lv_obj_t *refresh_btn = lv_obj_create(header);
    lv_obj_remove_style_all(refresh_btn);
    lv_obj_set_size(refresh_btn, 72, 38);
    lv_obj_align(refresh_btn, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_set_style_bg_color(refresh_btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(refresh_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(refresh_btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(refresh_btn, 2, 0);
    lv_obj_set_style_radius(refresh_btn, 8, 0);
    lv_obj_add_flag(refresh_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(refresh_btn, on_refresh_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ref_lbl = lv_label_create(refresh_btn);
    lv_label_set_text(ref_lbl, "刷新");
    const lv_font_t *f18 = fontpack_lv_font_get(18, 0);
    if (f18 == NULL) f18 = fontpack_lv_font_ui();
    if (f18 != NULL) lv_obj_set_style_text_font(ref_lbl, f18, 0);
    lv_obj_set_style_text_color(ref_lbl, lv_color_black(), 0);
    lv_obj_center(ref_lbl);

    // 4. 滚动内容区域
    s_body = lv_obj_create(scr);
    lv_obj_remove_style_all(s_body);
    lv_obj_set_size(s_body, 456, 696);
    lv_obj_set_pos(s_body, 12, 96);
    lv_obj_set_flex_flow(s_body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_body, 8, 0);

    fill_body();

    return scr;
}

void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "info";
    out->name = "系统信息";
    out->icon = "icon.png";
    out->create = info_create;
}
