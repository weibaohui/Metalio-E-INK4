// tools/hello_app/main/hello_main.c
// Hello 动态应用标准示范：纯 C + LVGL，采用 .app Bundle 架构
// 演示功能：
// 1. 标准 Bundle 结构与入口定义
// 2. 状态栏（dynapp_screen_status_bar）与虚拟键（dynapp_vk_attach）挂接
// 3. 沙箱隔离数据存储（dynapp_fopen_data）持久化计数器
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

static void *hello_create(void);

// 虚拟键处理回调（esp_timer 线程上下文，通过 dynapp_lv_async 投递 UI 操作）
static int hello_on_vk_key(const char *key) {
    if (key == NULL) return 0;
    if (strcmp(key, "home") == 0 || strcmp(key, "prev") == 0) {
        dynapp_screen_go_home();
        return 1;
    }
    return 0;
}

// 屏幕生命周期清理回调
static void hello_screen_delete_cb(lv_event_t *e) {
    // 如有自定义定时器、后台任务或动态资源，在此释放
}

// 读取并更新应用专属 data/ 目录中的运行计数
static int hello_update_launch_count(void) {
    int count = 0;
    FILE *fp = (FILE *)dynapp_fopen_data("launch_count.txt", "r");
    if (fp != NULL) {
        if (fscanf(fp, "%d", &count) != 1) {
            count = 0;
        }
        fclose(fp);
    }
    count++;
    fp = (FILE *)dynapp_fopen_data("launch_count.txt", "w");
    if (fp != NULL) {
        fprintf(fp, "%d\n", count);
        fclose(fp);
    }
    return count;
}

// 创建 LVGL 屏幕，返回根对象。
static void *hello_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(scr, hello_screen_delete_cb, LV_EVENT_DELETE, NULL);

    // 1. 挂接系统顶部状态栏（电量、时钟、Wi-Fi）
    dynapp_screen_status_bar(scr);

    // 2. 挂接虚拟按键（返回键/主页键交互）
    dynapp_vk_attach(scr, "hello", hello_create, hello_on_vk_key);

    // 3. 读取沙箱持久化数据
    int launch_count = hello_update_launch_count();

    // 4. 界面元素
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Hello Metalio .app");
    const lv_font_t *font_large = fontpack_lv_font_get(28, 2);
    if (font_large != NULL) {
        lv_obj_set_style_text_font(title, font_large, 0);
    }
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_align(title, LV_ALIGN_CENTER, 0, -40);

    char buf[128];
    snprintf(buf, sizeof(buf), "Bundle: %s\nData Dir: %s\nLaunch Count: %d (saved in data/)",
             dynapp_get_bundle_path(), dynapp_get_data_path(), launch_count);

    lv_obj_t *desc = lv_label_create(scr);
    lv_label_set_text(desc, buf);
    const lv_font_t *font_small = fontpack_lv_font_get(18, 0);
    if (font_small != NULL) {
        lv_obj_set_style_text_font(desc, font_small, 0);
    }
    lv_obj_set_style_text_color(desc, lv_color_black(), 0);
    lv_obj_set_style_text_align(desc, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(desc, LV_ALIGN_CENTER, 0, 30);

    return scr;
}

// 固件入口点：返回应用描述信息
void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "hello";
    out->name = "Hello";
    out->icon = "icon.png";
    out->create = hello_create;
}

