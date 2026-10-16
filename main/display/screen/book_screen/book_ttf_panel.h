#pragma once

#include <cstddef>
#include <lvgl.h>

namespace book_screen_detail {

constexpr int kTtfConvertSizeMin = 20;
constexpr int kTtfConvertSizeMax = 40;
constexpr int kTtfConvertSizeDefault = 25;

/** 关闭 TTF 覆盖面板（不停止后台转换） */
void TtfPanelClose();
/** 设置卡重建后若仍在 TTF 流程则恢复覆盖层 */
void TtfEnsurePanel();
/** 停止进度轮询定时器 */
void StopTtfPollTimer();
/** 设置卡控件缓存失效时清空 TTF 控件指针（不删对象） */
void ClearTtfWidgetRefs();
/** 「导入 TTF」按钮回调 */
void OnFontImportClick(lv_event_t* e);

/** book_screen 注册：转换结束（或静默收尾）后刷新 .ef 列表 */
using TtfFontsChangedFn = void (*)();
void SetTtfFontsChangedHandler(TtfFontsChangedFn fn);

/** book_screen 注册：当前字集模板 .ef 全路径写入 out */
using TtfEfTemplateFn = void (*)(char* out, size_t out_len);
void SetTtfEfTemplateHandler(TtfEfTemplateFn fn);

}  // namespace book_screen_detail
