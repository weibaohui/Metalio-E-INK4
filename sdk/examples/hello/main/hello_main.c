// sdk/examples/hello/main/hello_main.c
// Metalio 动态应用 SDK —— 最小示例(仅用 SDK 头文件,脱离固件源码可编译)
#include <string.h>

#include "lvgl.h"
#include "metalio/abi.h"
#include "metalio/services.h"
#include "fontpack_lvgl.h"

// 按键处理:vk_home 退出应用返回桌面(不注册走默认导航亦可,此处显式演示)
static int hello_on_vk_key(const char* key) {
    if (key != NULL && strcmp(key, "vk_home") == 0) {
        dynapp_screen_go_home();
        return 1;
    }
    return 0;
}

static void *hello_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 800);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    dynapp_screen_status_bar(scr);
    dynapp_vk_attach(scr, "hello", hello_create, hello_on_vk_key);

    lv_obj_t *lbl = lv_label_create(scr);
    lv_label_set_text(lbl, "Hello Metalio!\n\n这是一个用 SDK 独立编译的\n第三方动态应用示例。");
    // 字体 API 可能返回 NULL(字库按需载入):不设保护直接 set_style 会触发
    // LVGL 断言崩机——工程内所有应用均带此保护(实测教训)
    const lv_font_t *f = fontpack_lv_font_ui();
    if (f != NULL) {
        lv_obj_set_style_text_font(lbl, f, 0);
    }
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(lbl);

    return scr;
}

// 固件 dlsym("metalio_app_entry") 的入口:填 desc 后返回(无副作用)
void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "hello";
    out->name = "Hello SDK";
    out->icon = "icon.png";
    out->create = hello_create;
}
