// tools/hello_app/main/hello_main.c
// Hello 动态应用试点：纯 C + LVGL，编译为 hello.so 拷入 SD 卡
// /sdcard/metalio/e-ink/apps/。
// 所有 lv_* 与 fontpack 符号在加载期由固件符号表解析（dlopen RTLD_NOW），
// .so 内不链接任何固件实现。交互：HOME 键返回桌面、vk_prev 返回（默认策略）。
#include <stdio.h>
#include <string.h>

#include "lvgl.h"
#include "dynamic_app_abi.h"
#include "fontpack_lvgl.h"

// 创建 LVGL 屏幕，返回根对象。仅使用固件符号表内导出的 API；
// 屏幕生命周期归 LVGL（导航离开时由固件删除）。
static void *hello_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);  // LVGL9: parent=NULL 即新屏幕
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Hello Dynamic App!");
    const lv_font_t *font = fontpack_lv_font_get(30, 2);
    if (font != NULL) {
        lv_obj_set_style_text_font(title, font, 0);
    }
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_center(title);

    lv_obj_t *sub = lv_label_create(scr);
    lv_label_set_text(sub, "loaded from SD card\n/sdcard/metalio/e-ink/apps/hello.so");
    lv_obj_set_style_text_color(sub, lv_color_black(), 0);
    lv_obj_align(sub, LV_ALIGN_CENTER, 0, 40);

    return scr;
}

// 固件 dlsym("metalio_app_entry") 的入口：填写应用描述符后返回。
// desc 全 POD / 字符串字面量，固件不持有需释放的指针。
void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "hello";
    out->name = "Hello";
    out->icon = "A:ic_app_home_clock.spng";
    out->create = hello_create;
}
