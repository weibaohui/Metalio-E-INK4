// main/display/screen/book_screen/book_font_manager.h
#pragma once

#include <lvgl.h>
#include <string>
#include <vector>
#include "display/font/epdfont.h"
#include "reader/book_session.h"

namespace book_font {

struct ReadFontEntry {
    std::string file;
    int size_px = 0;
};

/** @brief 从 .ef 文件名（如 misans_25_2.ef）解析字号像素值 */
bool ParseEfSizePx(const std::string& name, int* size_px);

/** @brief 条目排序：按字号升序，字号相同时按文件名字典序 */
bool ReadFontEntryLess(const ReadFontEntry& a, const ReadFontEntry& b);

/** @brief 扫描 SD 卡 /fonts 目录下所有合法的 .ef 字体并排序 */
void ScanReadFonts(std::vector<ReadFontEntry>& out_entries);

/** @brief 删除指定的字体文件及可能的 .tmp 临时文件 */
bool DeleteReadFontFile(const std::string& file, std::string& err_out);

/** @brief 书库/详情等界面标题：固件 fontpack 30@2 */
const lv_font_t* ListFont();

/** @brief 书库列表 item：固件 fontpack 25@2 */
const lv_font_t* ItemFont();

/** @brief 仅正文内容：SD epdfont（NVS 所选），失败回退 fontpack UI */
const lv_font_t* BookFont();

/** @brief 按当前阅读偏好载入 SD epdfont 字体 */
void EnsureBookFont();

/** @brief 卸载当前已载入的 epdfont */
void ReleaseBookFont();

/** @brief 预热指定页文本中的字符字模位图 */
void PrewarmPageFont(const reader::Page* page);

/** @brief 获取当前打开的 epdfont 句柄（可能为空） */
epdfont_t* GetActiveEpdFont();

} // namespace book_font
