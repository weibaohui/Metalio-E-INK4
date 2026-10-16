#include "book_mcp.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <esp_log.h>

#include "mcp_server.h"
#include "reader/book_library.h"
#include "reader/http_download_file.h"
#include "reader_core/file_size.h"
#include "SdCardManager.hpp"
#include "sd_paths.h"
#include "transfer_utils.h"

namespace {

constexpr const char* TAG = "BookMcp";

using wifi_transfer::SanitizeFilename;

}  // namespace

void BookRegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();

    // 1. self.book.save_text_book: 直接保存纯文本电子书或文章到 SD 卡图书目录
    mcp.AddTool("self.book.save_text_book",
        "Save text content, an article, or an essay directly as a .txt book on the device SD card (/sdcard/metalio/e-ink/books/). "
        "Args: filename (e.g. '三体.txt' or '人工智能简史.txt'), content (text content of the book).",
        PropertyList({Property("filename", kPropertyTypeString),
                      Property("content", kPropertyTypeString)}),
        [](const PropertyList& p) -> ReturnValue {
            auto& sd = SdCardManager::GetInstance();
            if (!sd.IsMounted()) {
                return std::string("操作失败：未检测到 SD 卡或 SD 卡未挂载");
            }

            std::string filename = SanitizeFilename(p["filename"].value<std::string>());
            if (filename.empty()) {
                return std::string("参数错误：文件名为空或包含非法字符");
            }
            if (filename.size() < 4 || filename.rfind(".txt") != filename.size() - 4) {
                filename += ".txt";
            }

            const std::string content = p["content"].value<std::string>();
            if (content.empty()) {
                return std::string("参数错误：图书正文内容为空");
            }

            mkdir(SD_APP_ROOT, 0755);
            mkdir(SD_PATH_BOOKS, 0755);

            std::string full_path = std::string(SD_PATH_BOOKS) + "/" + filename;
            FILE* fp = fopen(full_path.c_str(), "wb");
            if (fp == nullptr) {
                return std::string("写文件失败：无法在 SD 卡创建文件 " + full_path);
            }

            size_t written = fwrite(content.data(), 1, content.size(), fp);
            fclose(fp);

            reader::InvalidateBookLibraryCache();

            char buf[128];
            snprintf(buf, sizeof(buf), "已成功将图书「%s」存入设备书库 (大小: %u 字节)，可在阅读器中立即阅读。",
                     filename.c_str(), static_cast<unsigned int>(written));
            return std::string(buf);
        });

    // 2. self.book.download_book: 从网络公共 URL 下载电子书（.epub, .txt, .ebook）直接存入 SD 卡
    mcp.AddTool("self.book.download_book",
        "Download a book file (epub/txt/ebook) from a public HTTP/HTTPS URL and save it directly to the device SD card (/sdcard/metalio/e-ink/books/). "
        "Args: url (public web URL), filename (e.g. 'novel.epub' or 'story.txt').",
        PropertyList({Property("url", kPropertyTypeString),
                      Property("filename", kPropertyTypeString)}),
        [](const PropertyList& p) -> ReturnValue {
            auto& sd = SdCardManager::GetInstance();
            if (!sd.IsMounted()) {
                return std::string("操作失败：未检测到 SD 卡或 SD 卡未挂载");
            }

            const std::string url = p["url"].value<std::string>();
            std::string filename = SanitizeFilename(p["filename"].value<std::string>());
            if (url.empty() || filename.empty()) {
                return std::string("参数错误：URL 或文件名为空");
            }

            mkdir(SD_APP_ROOT, 0755);
            mkdir(SD_PATH_BOOKS, 0755);

            std::string dest_path = std::string(SD_PATH_BOOKS) + "/" + filename;
            std::string tmp_path = dest_path + ".download.tmp";

            std::string err_out;
            constexpr size_t kMaxBookBytes = 50 * 1024 * 1024; // 50MB
            bool ok = reader::DownloadHttpToFile(url.c_str(), tmp_path.c_str(), nullptr, 0,
                                                 kMaxBookBytes, nullptr, nullptr, err_out, nullptr);
            if (!ok) {
                unlink(tmp_path.c_str());
                return std::string("下载失败: " + err_out);
            }

            unlink(dest_path.c_str());
            if (rename(tmp_path.c_str(), dest_path.c_str()) != 0) {
                unlink(tmp_path.c_str());
                return std::string("重命名目标文件失败");
            }

            reader::InvalidateBookLibraryCache();

            struct stat st = {};
            uint32_t size_bytes = 0;
            if (stat(dest_path.c_str(), &st) == 0) {
                size_bytes = static_cast<uint32_t>(st.st_size);
            }
            char sz_str[32];
            reader::FormatFileSize(sz_str, sizeof(sz_str), size_bytes);

            char buf[160];
            snprintf(buf, sizeof(buf), "已成功从网络下载图书「%s」到设备书库 (大小: %s)，可在阅读器中立即阅读。",
                     filename.c_str(), sz_str);
            return std::string(buf);
        });

    // 3. self.book.list_books: 查询设备 SD 卡图书目录中的所有书籍
    mcp.AddTool("self.book.list_books",
        "List all books and articles currently stored on the device SD card (/sdcard/metalio/e-ink/books/).",
        PropertyList(),
        [](const PropertyList& /*p*/) -> ReturnValue {
            auto& sd = SdCardManager::GetInstance();
            if (!sd.IsMounted()) {
                return std::string("未检测到 SD 卡或 SD 卡未挂载");
            }

            std::vector<reader::BookInfo> books;
            reader::LoadBookLibrary(SD_PATH_BOOKS, books);
            if (books.empty()) {
                return std::string("当前设备 SD 卡图书目录下暂无图书。可通过 WiFi 传书或 MCP 工具添加图书。");
            }

            std::string result = "设备图书目录共 " + std::to_string(books.size()) + " 本书：\n";
            for (size_t i = 0; i < books.size(); ++i) {
                char sz_str[32];
                reader::FormatFileSize(sz_str, sizeof(sz_str), books[i].file_size);
                result += std::to_string(i + 1) + ". " + books[i].title + " (" + sz_str + ")\n";
            }
            return result;
        });

    // 4. self.book.delete_book: 删除指定的图书
    mcp.AddTool("self.book.delete_book",
        "Delete a book from the device SD card (/sdcard/metalio/e-ink/books/). Args: filename.",
        PropertyList({Property("filename", kPropertyTypeString)}),
        [](const PropertyList& p) -> ReturnValue {
            std::string filename = SanitizeFilename(p["filename"].value<std::string>());
            if (filename.empty()) {
                return std::string("参数错误：文件名为空");
            }

            std::string full_path = std::string(SD_PATH_BOOKS) + "/" + filename;
            if (unlink(full_path.c_str()) == 0) {
                unlink((full_path + ".pos").c_str());
                unlink((full_path + ".idx").c_str());
                reader::InvalidateBookLibraryCache();
                return std::string("已成功删除图书: " + filename);
            }
            return std::string("删除失败：找不到文件或文件无法删除");
        });
}
