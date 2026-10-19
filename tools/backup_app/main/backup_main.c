// tools/backup_app/main/backup_main.c
// 备份与恢复 动态应用：全新三分层架构规范版
// 1. 系统基础配置 (System Config)
// 2. 应用配置与私有数据 (App Data & State)
// 3. 动态应用安装包 (App Bundles)
// 纯 C + LVGL，仅调用固件符号表导出接口。
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

#define ROWS_PER_PAGE 5

static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_sd_lbl = NULL;
static lv_obj_t *s_stats_lbl = NULL;
static lv_obj_t *s_page_lbl = NULL;
static lv_obj_t *s_list_box = NULL;
static lv_obj_t *s_modal = NULL;

static int s_page = 0;

static void *backup_create(void);

static const lv_font_t *get_font(int size, int weight) {
    const lv_font_t *f = fontpack_lv_font_get(size, weight);
    if (f == NULL) f = fontpack_lv_font_ui();
    return f;
}

static void on_back_clicked(lv_event_t *e) {
    (void)e;
    dynapp_screen_go_home();
}

static void close_modal(void) {
    if (s_modal) {
        lv_obj_del(s_modal);
        s_modal = NULL;
    }
}

static int backup_on_vk_key(const char* key) {
    if (key != NULL && (strcmp(key, "esc") == 0 || strcmp(key, "back") == 0 || strcmp(key, "vk_home") == 0 || strcmp(key, "home") == 0)) {
        if (s_modal != NULL) {
            close_modal();
            return 1;
        }
        dynapp_screen_go_home();
        return 1;
    }
    return 0;
}

// LV_EVENT_DELETE 自动释放 user_data。禁写 (lv_event_cb_t)free:那样 LVGL
// 删除对象时回调拿到的是 lv_event_t 本体,free 它等于喂给堆一个非法指针,
// 实测导出成功后 refresh_list 删卡片即堆腐败崩机。
static void free_user_data_cb(lv_event_t *e) {
    free(lv_event_get_user_data(e));
}

static lv_obj_t *make_btn(lv_obj_t *parent, const char *txt, lv_coord_t w, lv_coord_t h, lv_event_cb_t cb, void *user_data, lv_obj_t **out_lbl) {
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, txt);
    const lv_font_t *f16 = get_font(16, 2);
    if (f16) lv_obj_set_style_text_font(lbl, f16, 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    if (out_lbl) *out_lbl = lbl;
    return btn;
}

static void refresh_list(void);

// ==================== 1. 执行备份导出 ====================
static void do_export(int backup_type, const char *type_name) {
    if (!dynapp_backup_is_sd_mounted()) {
        dynapp_toast_show("SD 卡未挂载，无法备份", 2000);
        return;
    }

    dynapp_toast_show("正在生成备份，请稍候...", 2000);

    char out_name[64] = {};
    int ret = dynapp_backup_export(backup_type, out_name, sizeof(out_name));
    if (ret) {
        char msg[128];
        snprintf(msg, sizeof(msg), "%s完成:\n%s", type_name, out_name);
        dynapp_toast_show(msg, 3000);
        s_page = 0;
        refresh_list();
    } else {
        dynapp_toast_show("备份失败，请检查 SD 卡剩余空间", 2500);
    }
}

static void on_backup_full_clicked(lv_event_t *e) {
    (void)e;
    do_export(0, "完整快照备份");
}

static void on_backup_sys_clicked(lv_event_t *e) {
    (void)e;
    do_export(1, "系统配置备份");
}

static void on_backup_app_clicked(lv_event_t *e) {
    (void)e;
    do_export(2, "应用数据备份");
}

// ==================== 2. 执行恢复与重启 ====================
static void on_reboot_timer(lv_timer_t *tmr) {
    (void)tmr;
    dynapp_power_reboot();
}

static void execute_restore(const char *snapshot_path, int restore_mask, bool need_reboot) {
    dynapp_toast_show("正在恢复数据，请稍候...", 2500);
    int ret = dynapp_backup_import(snapshot_path, restore_mask);
    close_modal();

    if (ret) {
        if (need_reboot) {
            dynapp_toast_show("恢复成功！正在重启设备生效...", 3000);
            lv_timer_create(on_reboot_timer, 1500, NULL);
        } else {
            dynapp_toast_show("应用数据恢复成功！已重新加载", 2500);
            refresh_list();
        }
    } else {
        dynapp_toast_show("恢复失败，快照可能已损坏", 2500);
    }
}

// 恢复回调结构体
typedef struct {
    char path[128];
    int mask;
    bool reboot;
} restore_action_ctx_t;

static void on_restore_action_clicked(lv_event_t *e) {
    restore_action_ctx_t *ctx = (restore_action_ctx_t *)lv_event_get_user_data(e);
    if (ctx) {
        execute_restore(ctx->path, ctx->mask, ctx->reboot);
    }
}

static void on_delete_action_clicked(lv_event_t *e) {
    const char *path = (const char *)lv_event_get_user_data(e);
    if (path) {
        dynapp_backup_delete(path);
        dynapp_toast_show("快照已删除", 1500);
    }
    close_modal();
    refresh_list();
}

// ==================== 3. 恢复交互弹窗 ====================
static void show_snapshot_modal(const dynapp_backup_info_t *item) {
    close_modal();

    lv_obj_t *modal = lv_obj_create(s_scr);
    s_modal = modal;
    lv_obj_remove_style_all(modal);
    lv_obj_set_size(modal, 440, 520);
    lv_obj_center(modal);
    lv_obj_set_style_bg_color(modal, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(modal, lv_color_black(), 0);
    lv_obj_set_style_border_width(modal, 2, 0);
    lv_obj_set_style_radius(modal, 12, 0);
    lv_obj_set_style_pad_all(modal, 16, 0);
    lv_obj_set_flex_flow(modal, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(modal, 10, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);

    // 标题
    lv_obj_t *title = lv_label_create(modal);
    lv_label_set_text(title, "快照恢复与管理");
    const lv_font_t *f22 = get_font(22, 2);
    if (f22) lv_obj_set_style_text_font(title, f22, 0);

    // 快照详情信息
    lv_obj_t *desc = lv_label_create(modal);
    char buf[256];
    const char *tname = (item->backup_type == 1) ? "系统配置备份"
                        : (item->backup_type == 2) ? "应用数据备份" : "全量设备快照";
    snprintf(buf, sizeof(buf), "名称: %s\n类型: %s\n时间: %s\n内容: 系统配置 %d 项 | 应用配置 %d 项\n      数据应用 %d 个 | 安装包 %d 个",
             item->name, tname, item->created_at,
             item->system_nvs_count, item->app_nvs_count,
             item->app_data_count, item->app_bundle_count);
    lv_label_set_text(desc, buf);
    const lv_font_t *f16 = get_font(16, 0);
    if (f16) lv_obj_set_style_text_font(desc, f16, 0);
    lv_obj_set_width(desc, lv_pct(100));

    // 分隔线
    lv_obj_t *line = lv_obj_create(modal);
    lv_obj_remove_style_all(line);
    lv_obj_set_size(line, lv_pct(100), 1);
    lv_obj_set_style_bg_color(line, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);

    // 操作按钮区域
    if (item->backup_type == 0) { // 全量快照
        // 1. 恢复全部
        restore_action_ctx_t *ctx_all = (restore_action_ctx_t *)malloc(sizeof(restore_action_ctx_t));
        strncpy(ctx_all->path, item->full_path, sizeof(ctx_all->path) - 1);
        ctx_all->mask = 0x07; // ALL
        ctx_all->reboot = true;
        lv_obj_t *b1 = make_btn(modal, "● 恢复全部 (系统配置+应用数据+安装包)", lv_pct(100), 44, on_restore_action_clicked, ctx_all, NULL);
        lv_obj_add_event_cb(b1, free_user_data_cb, LV_EVENT_DELETE, ctx_all);

        // 2. 仅恢复应用数据
        restore_action_ctx_t *ctx_app = (restore_action_ctx_t *)malloc(sizeof(restore_action_ctx_t));
        strncpy(ctx_app->path, item->full_path, sizeof(ctx_app->path) - 1);
        ctx_app->mask = 0x02; // APP_DATA
        ctx_app->reboot = false;
        lv_obj_t *b2 = make_btn(modal, "○ 仅恢复应用数据 (存档/收藏，不改网络)", lv_pct(100), 40, on_restore_action_clicked, ctx_app, NULL);
        lv_obj_add_event_cb(b2, free_user_data_cb, LV_EVENT_DELETE, ctx_app);

        // 3. 仅恢复系统配置
        restore_action_ctx_t *ctx_sys = (restore_action_ctx_t *)malloc(sizeof(restore_action_ctx_t));
        strncpy(ctx_sys->path, item->full_path, sizeof(ctx_sys->path) - 1);
        ctx_sys->mask = 0x01; // SYSTEM
        ctx_sys->reboot = true;
        lv_obj_t *b3 = make_btn(modal, "○ 仅恢复系统配置 (Wi-Fi/音量/设置)", lv_pct(100), 40, on_restore_action_clicked, ctx_sys, NULL);
        lv_obj_add_event_cb(b3, free_user_data_cb, LV_EVENT_DELETE, ctx_sys);
    } else if (item->backup_type == 1) { // 仅系统配置
        restore_action_ctx_t *ctx_sys = (restore_action_ctx_t *)malloc(sizeof(restore_action_ctx_t));
        strncpy(ctx_sys->path, item->full_path, sizeof(ctx_sys->path) - 1);
        ctx_sys->mask = 0x01;
        ctx_sys->reboot = true;
        lv_obj_t *b = make_btn(modal, "● 恢复系统配置 (完成后将重启)", lv_pct(100), 44, on_restore_action_clicked, ctx_sys, NULL);
        lv_obj_add_event_cb(b, free_user_data_cb, LV_EVENT_DELETE, ctx_sys);
    } else { // 仅应用数据
        restore_action_ctx_t *ctx_app = (restore_action_ctx_t *)malloc(sizeof(restore_action_ctx_t));
        strncpy(ctx_app->path, item->full_path, sizeof(ctx_app->path) - 1);
        ctx_app->mask = 0x02;
        ctx_app->reboot = false;
        lv_obj_t *b = make_btn(modal, "● 恢复应用数据与状态", lv_pct(100), 44, on_restore_action_clicked, ctx_app, NULL);
        lv_obj_add_event_cb(b, free_user_data_cb, LV_EVENT_DELETE, ctx_app);
    }

    // 底部控制：[删除此快照] [关闭取消]
    lv_obj_t *btm = lv_obj_create(modal);
    lv_obj_remove_style_all(btm);
    lv_obj_set_size(btm, lv_pct(100), 42);
    lv_obj_set_flex_flow(btm, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(btm, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    char *path_copy = strdup(item->full_path);
    lv_obj_t *del_btn = make_btn(btm, "删除快照", 120, 38, on_delete_action_clicked, path_copy, NULL);
    lv_obj_add_event_cb(del_btn, free_user_data_cb, LV_EVENT_DELETE, path_copy);

    make_btn(btm, "关闭", 100, 38, (lv_event_cb_t)close_modal, NULL, NULL);
}

static void on_snapshot_row_clicked(lv_event_t *e) {
    dynapp_backup_info_t *info = (dynapp_backup_info_t *)lv_event_get_user_data(e);
    if (info) {
        show_snapshot_modal(info);
    }
}

// ==================== 4. 列表与主界面刷新 ====================
static void on_page_prev(lv_event_t *e) {
    (void)e;
    if (s_page > 0) {
        s_page--;
        refresh_list();
    }
}

static void on_page_next(lv_event_t *e) {
    (void)e;
    s_page++;
    refresh_list();
}

static void refresh_list(void) {
    if (!s_scr || !lv_obj_is_valid(s_scr)) return;

    // 1. 刷新状态栏
    char free_str[32] = "--", total_str[32] = "--";
    dynapp_backup_get_sd_capacity(free_str, sizeof(free_str), total_str, sizeof(total_str));
    char sd_txt[64];
    snprintf(sd_txt, sizeof(sd_txt), "SD 卡: 可用 %s / 总共 %s", free_str, total_str);
    lv_label_set_text(s_sd_lbl, sd_txt);

    int sys_nvs = 0, app_nvs = 0, inst_apps = 0;
    dynapp_backup_get_stats(&sys_nvs, &app_nvs, &inst_apps);
    char st_txt[128];
    snprintf(st_txt, sizeof(st_txt), "系统配置: %d 项 | 应用配置: %d 项 | 已装应用: %d 个", sys_nvs, app_nvs, inst_apps);
    lv_label_set_text(s_stats_lbl, st_txt);

    // 2. 刷新快照列表
    lv_obj_clean(s_list_box);
    int total_backups = dynapp_backup_list_count();

    int total_pages = (total_backups + ROWS_PER_PAGE - 1) / ROWS_PER_PAGE;
    if (total_pages <= 0) total_pages = 1;
    if (s_page >= total_pages) s_page = total_pages - 1;
    if (s_page < 0) s_page = 0;

    char pbuf[32];
    snprintf(pbuf, sizeof(pbuf), "%d / %d", s_page + 1, total_pages);
    lv_label_set_text(s_page_lbl, pbuf);

    if (total_backups == 0) {
        lv_obj_t *tip = lv_label_create(s_list_box);
        lv_label_set_text(tip, "暂无备份快照\n点击上方按钮即可创建备份");
        const lv_font_t *f18 = get_font(18, 0);
        if (f18) lv_obj_set_style_text_font(tip, f18, 0);
        lv_obj_set_width(tip, lv_pct(100));
        lv_obj_set_style_text_align(tip, LV_TEXT_ALIGN_CENTER, 0);
        return;
    }

    int start = s_page * ROWS_PER_PAGE;
    int end = start + ROWS_PER_PAGE;
    if (end > total_backups) end = total_backups;

    for (int i = start; i < end; ++i) {
        dynapp_backup_info_t *item = (dynapp_backup_info_t *)malloc(sizeof(dynapp_backup_info_t));
        if (!item) continue;
        if (!dynapp_backup_get_item(i, item)) {
            free(item);
            continue;
        }

        lv_obj_t *card = lv_obj_create(s_list_box);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, lv_pct(100), 54);
        lv_obj_set_style_bg_color(card, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(card, lv_color_black(), 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_radius(card, 8, 0);
        lv_obj_set_style_pad_hor(card, 12, 0);
        lv_obj_set_style_pad_ver(card, 6, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(card, 2, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_add_event_cb(card, on_snapshot_row_clicked, LV_EVENT_CLICKED, item);
        lv_obj_add_event_cb(card, free_user_data_cb, LV_EVENT_DELETE, item);

        // 第一行：[标签] + 时间
        lv_obj_t *r1 = lv_obj_create(card);
        lv_obj_remove_style_all(r1);
        lv_obj_set_size(r1, lv_pct(100), 22);
        lv_obj_set_flex_flow(r1, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(r1, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(r1, 8, 0);
        lv_obj_clear_flag(r1, LV_OBJ_FLAG_CLICKABLE);

        const char *tag_txt = (item->backup_type == 1) ? "[系统]"
                              : (item->backup_type == 2) ? "[应用]" : "[全量]";
        lv_obj_t *tag_lbl = lv_label_create(r1);
        lv_label_set_text(tag_lbl, tag_txt);
        const lv_font_t *f16b = get_font(16, 2);
        if (f16b) lv_obj_set_style_text_font(tag_lbl, f16b, 0);
        lv_obj_clear_flag(tag_lbl, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *time_lbl = lv_label_create(r1);
        lv_label_set_text(time_lbl, item->created_at);
        if (f16b) lv_obj_set_style_text_font(time_lbl, f16b, 0);
        lv_obj_clear_flag(time_lbl, LV_OBJ_FLAG_CLICKABLE);

        // 第二行：详细内容说明
        lv_obj_t *sub_lbl = lv_label_create(card);
        char sub[128];
        if (item->backup_type == 0) {
            snprintf(sub, sizeof(sub), "系统(%d项) · 应用数据(%d个应用) · 应用包(%d个)",
                     item->system_nvs_count, item->app_data_count, item->app_bundle_count);
        } else if (item->backup_type == 1) {
            snprintf(sub, sizeof(sub), "包含核心系统与网络配置 %d 项", item->system_nvs_count);
        } else {
            snprintf(sub, sizeof(sub), "包含应用配置 %d 项 · 应用私有数据 %d 个",
                     item->app_nvs_count, item->app_data_count);
        }
        lv_label_set_text(sub_lbl, sub);
        const lv_font_t *f14 = get_font(14, 0);
        if (f14) lv_obj_set_style_text_font(sub_lbl, f14, 0);
        lv_obj_clear_flag(sub_lbl, LV_OBJ_FLAG_CLICKABLE);
    }
}

static void on_screen_deleted(lv_event_t *e) {
    (void)e;
    close_modal();
    s_scr = NULL;
    s_sd_lbl = NULL;
    s_stats_lbl = NULL;
    s_page_lbl = NULL;
    s_list_box = NULL;
    s_modal = NULL;
}

static void *backup_create(void) {
    s_page = 0;

    lv_obj_t *scr = lv_obj_create(NULL);
    s_scr = scr;
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 800);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(scr, on_screen_deleted, LV_EVENT_DELETE, NULL);

    dynapp_vk_attach(scr, "backup", backup_create, backup_on_vk_key);

    // 主内容区 (无返回顶栏，由底部 vk_home 键统一控制返回)
    lv_obj_t *body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_size(body, 456, 750);
    lv_obj_set_pos(body, 12, 42);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 10, 0);
    lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

    // 1. 状态卡片
    lv_obj_t *card = lv_obj_create(body);
    lv_obj_remove_style_all(card);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_black(), 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_radius(card, 10, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 4, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    s_sd_lbl = lv_label_create(card);
    lv_label_set_text(s_sd_lbl, "SD 卡: 正在读取...");
    const lv_font_t *f16b = get_font(16, 2);
    if (f16b) lv_obj_set_style_text_font(s_sd_lbl, f16b, 0);

    s_stats_lbl = lv_label_create(card);
    lv_label_set_text(s_stats_lbl, "系统与应用状态统计中...");
    const lv_font_t *f14 = get_font(14, 0);
    if (f14) lv_obj_set_style_text_font(s_stats_lbl, f14, 0);

    // 2. 备份操作栏 (三分层备份入口)
    lv_obj_t *act_box = lv_obj_create(body);
    lv_obj_remove_style_all(act_box);
    lv_obj_set_size(act_box, lv_pct(100), 44);
    lv_obj_set_flex_flow(act_box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(act_box, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(act_box, LV_OBJ_FLAG_SCROLLABLE);

    make_btn(act_box, "★ 完整快照", 146, 42, on_backup_full_clicked, NULL, NULL);
    make_btn(act_box, "系统配置", 146, 42, on_backup_sys_clicked, NULL, NULL);
    make_btn(act_box, "应用数据", 146, 42, on_backup_app_clicked, NULL, NULL);

    // 3. 历史快照导航与翻页
    lv_obj_t *nav_bar = lv_obj_create(body);
    lv_obj_remove_style_all(nav_bar);
    lv_obj_set_size(nav_bar, lv_pct(100), 36);
    lv_obj_set_flex_flow(nav_bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(nav_bar, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(nav_bar, LV_OBJ_FLAG_SCROLLABLE);

    make_btn(nav_bar, "‹ 上页", 76, 32, on_page_prev, NULL, NULL);

    s_page_lbl = lv_label_create(nav_bar);
    lv_label_set_text(s_page_lbl, "1 / 1");
    if (f16b) lv_obj_set_style_text_font(s_page_lbl, f16b, 0);

    make_btn(nav_bar, "下页 ›", 76, 32, on_page_next, NULL, NULL);

    // 4. 快照列表容器
    s_list_box = lv_obj_create(body);
    lv_obj_remove_style_all(s_list_box);
    lv_obj_set_size(s_list_box, lv_pct(100), 470);
    lv_obj_set_flex_flow(s_list_box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list_box, 8, 0);
    lv_obj_clear_flag(s_list_box, LV_OBJ_FLAG_SCROLLABLE);

    refresh_list();

    return scr;
}

void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "backup";
    out->name = "备份与恢复";
    out->icon = "icon.png";
    out->create = backup_create;
}
