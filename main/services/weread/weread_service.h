#pragma once

#include "weread_types.h"
#include "weread_auth_store.h"
#include "weread_client.h"

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

class WereadService {
public:
    static WereadService& GetInstance();

    using QrReadyCb = std::function<void(const std::string& qr_url)>;
    using LoginResultCb = std::function<void(bool success, const std::string& err)>;
    using ShelfDoneCb = std::function<void(const std::string& json_shelf, int status)>;
    using ChaptersDoneCb = std::function<void(const std::string& json_toc, int status)>;
    using TextDoneCb = std::function<void(const std::string& text, int status)>;

    bool IsLoggedIn();
    void Logout();

    bool StartQrLogin(QrReadyCb on_qr, LoginResultCb on_result);
    void CancelQrLogin();

    bool FetchShelfAsync(ShelfDoneCb cb);
    bool FetchChaptersAsync(const std::string& book_id, ChaptersDoneCb cb);
    bool FetchChapterTextAsync(const std::string& book_id, const std::string& chapter_uid, TextDoneCb cb);
    void SyncProgress(const std::string& book_id, const std::string& chapter_uid,
                      int chapter_idx, int chapter_offset, int percent, int read_seconds);

private:
    WereadService();
    ~WereadService() = default;

    WereadAuth auth_;
    WereadAuthStore auth_store_;
    std::mutex mutex_;
    TaskHandle_t qr_task_ = nullptr;
    bool qr_cancelled_ = false;
};
