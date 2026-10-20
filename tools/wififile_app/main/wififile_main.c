// tools/wififile_app/main/wififile_main.c
// WiFi 传输 动态应用：传书 + 传壁纸 + 全盘文件管理（合一的唯一 WiFi 应用）。
// 旧「WiFi 传书」(transfer) 应用已并入本应用，功能不再单独提供。
//
// 界面设计约束（墨水屏 480x800 单色）：
//   - 只用黑/白两档，"弱化"靠字号而非灰度
//   - URL 是用户唯一需要抄写的���息 → 独占一行大号字
//   - 状态用黑底白字胶囊徽标表达，一眼可辨，不靠读长句
//   - 二维码框常驻（未启动时显示占位文案），避免启停时整页跳动
//   - 2s 轮询只在内容真正变化时才 set_text / 重绘二维码，避免墨水屏无谓刷新
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

static lv_obj_t *s_scr = NULL;
static lv_obj_t *s_badge = NULL;
static lv_obj_t *s_badge_lbl = NULL;
static lv_obj_t *s_url_lbl = NULL;
static lv_obj_t *s_tip_lbl = NULL;
static lv_obj_t *s_switch_btn_lbl = NULL;
static lv_obj_t *s_qr_box = NULL;
static lv_obj_t *s_qr_obj = NULL;
static lv_obj_t *s_qr_placeholder = NULL;
static lv_obj_t *s_storage_lbl = NULL;
static lv_obj_t *s_log_lbl = NULL;
static lv_timer_t *s_timer = NULL;

static char s_recent_log[256] = "服务就绪，点击下方按钮启动";

// 记录上一次的 URL，二维码只在地址变化时重绘（省墨水屏刷新）
static char s_last_url[128] = {0};

// 地址区当前形态：0=未运行(小字) 1=运行中(大号 URL)
static int s_url_state = -1;

static const lv_font_t *s_f_ui = NULL;
static const lv_font_t *s_f_big = NULL;

static void *wififile_create(void);

// UI 字体：25@2（包内唯一的中文常规字号）。30@2 用于标题与 URL。
static const lv_font_t *ui_font(void) {
    const lv_font_t *f = fontpack_lv_font_ui();
    return f != NULL ? f : fontpack_lv_font_get(25, 2);
}

// 大号字：30@2（包内有汉字）。请求 28 等其它字号会回退到 25@2（见 fontpack_lvgl.h），
// 所以这里直接取 30@2，而不是请求 28。
static const lv_font_t *big_font(void) {
    const lv_font_t *f = fontpack_lv_font_get(30, 2);
    return f != NULL ? f : ui_font();
}

static int wififile_on_vk_key(const char* key) {
    if (key != NULL && (strcmp(key, "esc") == 0 || strcmp(key, "back") == 0 ||
                        strcmp(key, "vk_home") == 0 || strcmp(key, "home") == 0)) {
        dynapp_screen_go_home();
        return 1;
    }
    return 0;
}

// 统一风格的卡片：白底黑框圆角，纵向 flex
static lv_obj_t *make_card(lv_obj_t *parent, lv_coord_t pad) {
    lv_obj_t *card = lv_obj_create(parent);
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
    lv_obj_set_style_pad_row(card, 5, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return card;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char* text, const lv_font_t* font) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    lv_obj_set_width(lbl, lv_pct(100));
    lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    if (font) lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    return lbl;
}

// 细横线，用于分隔"动态"与"说明"
static lv_obj_t *make_divider(lv_obj_t *parent) {
    lv_obj_t *line = lv_obj_create(parent);
    lv_obj_remove_style_all(line);
    lv_obj_set_width(line, lv_pct(100));
    lv_obj_set_height(line, 1);
    lv_obj_set_style_bg_color(line, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    return line;
}

// 状态徽标：黑底白字胶囊
static lv_obj_t *make_badge(lv_obj_t *parent, const char* text, const lv_font_t* font) {
    lv_obj_t *badge = lv_obj_create(parent);
    lv_obj_remove_style_all(badge);
    lv_obj_set_size(badge, 104, 34);
    lv_obj_set_style_bg_color(badge, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(badge, 17, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *lbl = lv_label_create(badge);
    lv_label_set_text(lbl, text);
    if (font) lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_center(lbl);
    return badge;
}

// 文本变了才写回，避免 2s 轮询反复触发墨水屏重绘
static void set_text_if_changed(lv_obj_t* lbl, const char* text) {
    if (lbl == NULL) return;
    if (strcmp(lv_label_get_text(lbl), text) == 0) return;
    lv_label_set_text(lbl, text);
}

static void refresh_ui(void) {
    if (!s_scr || !lv_obj_is_valid(s_scr)) return;

    const int is_net = dynapp_is_network_ready();
    const int running = dynapp_wifi_file_is_running();
    char ip[32] = {0};
    const int has_ip = dynapp_get_ip_address(ip, sizeof(ip));
    char url[128] = {0};
    dynapp_wifi_file_get_url(url, sizeof(url));

    const int online = (is_net && has_ip && ip[0] != '\0');
    const int live = (running && url[0] != '\0');

    // 1. 状态徽标：未联网 / 待启动 / 运行中
    if (s_badge_lbl) {
        if (!online) {
            set_text_if_changed(s_badge_lbl, "未联网");
        } else if (!live) {
            set_text_if_changed(s_badge_lbl, "待启动");
        } else {
            set_text_if_changed(s_badge_lbl, "运行中");
        }
    }

    // 2. 地址区：运行时显示大号 URL；未运行时用小字引导，不跟大号 URL 抢视觉
    if (s_url_lbl) {
        const int want_state = live ? 1 : 0;
        if (s_url_state != want_state) {
            s_url_state = want_state;
            const lv_font_t* f = live ? s_f_big : s_f_ui;
            if (f) lv_obj_set_style_text_font(s_url_lbl, f, 0);
        }
        if (live) {
            set_text_if_changed(s_url_lbl, url);
        } else {
            set_text_if_changed(s_url_lbl, online ? "点击下方按钮开启服务" : "请先连接 WiFi");
        }
    }

    // 3. 辅助提示
    if (s_tip_lbl) {
        set_text_if_changed(s_tip_lbl, live ? "同局域网浏览器打开，或用手机扫码"
                                             : "开启后可传书、传壁纸、管文件");
    }

    // 4. 主按钮
    if (s_switch_btn_lbl) {
        set_text_if_changed(s_switch_btn_lbl, live ? "停止传输服务" : "启动传输服务");
    }

    // 5. 二维码：地址变化才重绘；未运行时框内显示占位文案，框不消失
    if (s_qr_box && s_qr_obj) {
        if (live) {
            if (strcmp(s_last_url, url) != 0) {
                snprintf(s_last_url, sizeof(s_last_url), "%s", url);
                lv_qrcode_update(s_qr_obj, url, strlen(url));
            }
            lv_obj_clear_flag(s_qr_obj, LV_OBJ_FLAG_HIDDEN);
            if (s_qr_placeholder) lv_obj_add_flag(s_qr_placeholder, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_qr_obj, LV_OBJ_FLAG_HIDDEN);
            if (s_qr_placeholder) lv_obj_clear_flag(s_qr_placeholder, LV_OBJ_FLAG_HIDDEN);
        }
    }

    // 6. 存储容量
    if (s_storage_lbl) {
        char free_str[32] = {0}, total_str[32] = {0};
        if (dynapp_backup_get_sd_capacity(free_str, sizeof(free_str), total_str, sizeof(total_str))) {
            char sbuf[96];
            snprintf(sbuf, sizeof(sbuf), "SD 可用 %s / 共 %s", free_str, total_str);
            set_text_if_changed(s_storage_lbl, sbuf);
        } else {
            set_text_if_changed(s_storage_lbl, "未检测到 SD 卡");
        }
    }

    // 7. 实时动态
    if (s_log_lbl) {
        set_text_if_changed(s_log_lbl, s_recent_log);
    }
}

static void on_toggle_clicked(lv_event_t* e) {
    (void)e;
    if (dynapp_wifi_file_is_running()) {
        dynapp_wifi_file_stop();
        snprintf(s_recent_log, sizeof(s_recent_log), "传输服务已停止");
        dynapp_toast_show("WiFi 传输服务已停止", 1500);
    } else {
        if (!dynapp_is_network_ready()) {
            dynapp_modal_alert("提示", "当前设备未连接 WiFi，请先进入设置连接 WiFi 后再启动传输服务。", "确定");
            return;
        }
        if (dynapp_wifi_file_start()) {
            snprintf(s_recent_log, sizeof(s_recent_log), "服务已启动，请在浏览器中输入网址访问");
            dynapp_toast_show("WiFi 传输服务已启动", 1500);
        } else {
            dynapp_modal_alert("错误", "启动服务失败，端口可能已被占用", "确定");
        }
    }
    refresh_ui();
}

// Web 端文件操作活动回传（传书/上传/删除/新建目录）
static void on_fs_activity_cb(const char* action, const char* path, void* user_data) {
    (void)user_data;
    if (!action) return;
    if (strcmp(action, "传书") == 0) {
        snprintf(s_recent_log, sizeof(s_recent_log), "✓ 已存入书库: %s", path ? path : "");
        dynapp_toast_show("已收到新书", 2500);
    } else if (strcmp(action, "上传") == 0) {
        snprintf(s_recent_log, sizeof(s_recent_log), "✓ 已上传: %s", path ? path : "");
    } else if (strcmp(action, "删除") == 0) {
        snprintf(s_recent_log, sizeof(s_recent_log), "已删除: %s", path ? path : "");
    } else if (strcmp(action, "新建目录") == 0) {
        snprintf(s_recent_log, sizeof(s_recent_log), "已新建目录: %s", path ? path : "");
    }
    if (s_log_lbl && lv_obj_is_valid(s_log_lbl)) {
        lv_label_set_text(s_log_lbl, s_recent_log);
    }
}

static void timer_cb(lv_timer_t* tmr) {
    (void)tmr;
    refresh_ui();
}

static void on_screen_deleted(lv_event_t* e) {
    (void)e;
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    dynapp_wifi_file_set_callback(NULL, NULL);
    dynapp_wifi_file_stop();

    s_scr = NULL;
    s_badge = NULL;
    s_badge_lbl = NULL;
    s_url_lbl = NULL;
    s_tip_lbl = NULL;
    s_switch_btn_lbl = NULL;
    s_qr_box = NULL;
    s_qr_obj = NULL;
    s_qr_placeholder = NULL;
    s_storage_lbl = NULL;
    s_log_lbl = NULL;
}

static void* wififile_create(void) {
    lv_obj_t* scr = lv_obj_create(NULL);
    s_scr = scr;
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 800);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(scr, on_screen_deleted, LV_EVENT_DELETE, NULL);

    dynapp_screen_status_bar(scr);
    dynapp_vk_attach(scr, "wififile", wififile_create, wififile_on_vk_key);

    const lv_font_t* f_ui = ui_font();
    const lv_font_t* f_big = big_font();
    s_f_ui = f_ui;
    s_f_big = f_big;
    s_url_state = -1;

    // 主滚动容器（无返回顶栏，由底部 vk_home 键统一控制返回）
    lv_obj_t* body = lv_obj_create(scr);
    lv_obj_remove_style_all(body);
    lv_obj_set_pos(body, 14, 42);
    lv_obj_set_size(body, 452, 750);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(body, 8, 0);

    // ===== 卡片 1：状态 + 地址 + 开关 =====
    lv_obj_t* card1 = make_card(body, 12);

    // 标题行：标题在左，状态徽标在右
    lv_obj_t* head = lv_obj_create(card1);
    lv_obj_remove_style_all(head);
    lv_obj_set_width(head, lv_pct(100));
    lv_obj_set_height(head, 36);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(head, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* title = lv_label_create(head);
    lv_label_set_text(title, "WiFi 传输");
    if (f_big) lv_obj_set_style_text_font(title, f_big, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);

    s_badge = make_badge(head, "待启动", f_ui);
    s_badge_lbl = lv_obj_get_child(s_badge, 0);
    lv_obj_align(s_badge, LV_ALIGN_RIGHT_MID, 0, 0);

    // 地址值：固定高度盒子，运行时放 URL，未运行时放引导语，布局不跳
    lv_obj_t* url_box = lv_obj_create(card1);
    lv_obj_remove_style_all(url_box);
    lv_obj_set_width(url_box, lv_pct(100));
    lv_obj_set_height(url_box, 44);
    lv_obj_clear_flag(url_box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_url_lbl = lv_label_create(url_box);
    lv_label_set_text(s_url_lbl, "点击下方按钮开启服务");
    lv_label_set_long_mode(s_url_lbl, LV_LABEL_LONG_WRAP);
    if (f_ui) lv_obj_set_style_text_font(s_url_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_url_lbl, lv_color_black(), 0);
    lv_obj_align(s_url_lbl, LV_ALIGN_LEFT_MID, 0, 0);

    s_tip_lbl = make_label(card1, "开启后可传书、传壁纸、管文件", f_ui);

    // 主操作按钮：黑底白字，视觉权重最高
    lv_obj_t* btn = lv_obj_create(card1);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, lv_pct(100), 48);
    lv_obj_set_style_bg_color(btn, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, on_toggle_clicked, LV_EVENT_CLICKED, NULL);

    s_switch_btn_lbl = lv_label_create(btn);
    lv_label_set_text(s_switch_btn_lbl, "启动传输服务");
    if (f_ui) lv_obj_set_style_text_font(s_switch_btn_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_switch_btn_lbl, lv_color_white(), 0);
    lv_obj_center(s_switch_btn_lbl);

    s_storage_lbl = make_label(card1, "正在读取 SD 卡容量...", f_ui);

    // ===== 卡片 2：二维码 =====
    lv_obj_t* card2 = make_card(body, 12);
    lv_obj_set_flex_align(card2, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t* qr_title = make_label(card2, "手机扫码直达", f_big);
    lv_obj_set_style_text_align(qr_title, LV_TEXT_ALIGN_CENTER, 0);

    s_qr_box = lv_obj_create(card2);
    lv_obj_remove_style_all(s_qr_box);
    lv_obj_set_size(s_qr_box, 200, 200);
    lv_obj_set_style_bg_color(s_qr_box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_qr_box, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_qr_box, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_qr_box, 2, 0);
    lv_obj_set_style_radius(s_qr_box, 8, 0);
    lv_obj_clear_flag(s_qr_box, LV_OBJ_FLAG_SCROLLABLE);

    s_qr_obj = lv_qrcode_create(s_qr_box);
    lv_qrcode_set_size(s_qr_obj, 186);
    lv_qrcode_set_dark_color(s_qr_obj, lv_color_black());
    lv_qrcode_set_light_color(s_qr_obj, lv_color_white());
    lv_obj_center(s_qr_obj);

    // 未启动时的占位文案（框常驻，避免启停时整页跳动）
    s_qr_placeholder = lv_label_create(s_qr_box);
    lv_label_set_text(s_qr_placeholder, "启动服务后\n显示二维码");
    lv_label_set_long_mode(s_qr_placeholder, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_qr_placeholder, 170);
    lv_obj_set_style_text_align(s_qr_placeholder, LV_TEXT_ALIGN_CENTER, 0);
    if (f_ui) lv_obj_set_style_text_font(s_qr_placeholder, f_ui, 0);
    lv_obj_set_style_text_color(s_qr_placeholder, lv_color_black(), 0);
    lv_obj_center(s_qr_placeholder);

    // ===== 卡片 3：实时动态 + 支持格式 =====
    lv_obj_t* card3 = make_card(body, 12);

    lv_obj_t* log_title = make_label(card3, "实时操作动态", f_big);

    s_log_lbl = make_label(card3, s_recent_log, f_ui);

    make_divider(card3);

    make_label(card3, "传书 .txt/.epub/.ebook/.pdf", f_ui);
    make_label(card3, "壁纸 .png/.jpg/.jpeg/.a2i1", f_ui);
    make_label(card3, "退出本界面将自动关闭服务", f_ui);

    // 注册文件操作活动回调（传书/上传/删除实时回显）
    dynapp_wifi_file_set_callback(on_fs_activity_cb, NULL);

    refresh_ui();

    s_timer = lv_timer_create(timer_cb, 2000, NULL);
    return scr;
}

void metalio_app_entry(metalio_app_desc_t* out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "wififile";
    out->name = "WiFi 传输";
    out->icon = "icon.png";
    out->create = wififile_create;
}
