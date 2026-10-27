#include "weread_service.h"

#include <cJSON.h>
#include <esp_log.h>
#include "display/screen/screen_common.h"
#include "utils/psr_worker.h"

namespace {
static const char* kTag = "WereadService";

struct DeliveryArg {
    std::function<void()> fn;
};

void DeliverThunk(void* p) {
    auto* arg = static_cast<DeliveryArg*>(p);
    if (arg && arg->fn) {
        arg->fn();
    }
    delete arg;
}

void DispatchToLvgl(std::function<void()> fn) {
    auto* arg = new DeliveryArg{std::move(fn)};
    if (!ScreenLvAsync(DeliverThunk, arg)) {
        ESP_LOGW(kTag, "ScreenLvAsync failed, dropping delivery");
        delete arg;
    }
}
} // namespace

WereadService& WereadService::GetInstance() {
    static WereadService instance;
    return instance;
}

WereadService::WereadService() {
    auth_store_.Load(&auth_);
}

bool WereadService::IsLoggedIn() {
    std::lock_guard<std::mutex> lock(mutex_);
    return auth_.IsLoggedIn();
}

void WereadService::Logout() {
    std::lock_guard<std::mutex> lock(mutex_);
    auth_ = WereadAuth();
    auth_store_.Clear();
    ESP_LOGI(kTag, "User logged out");
}

bool WereadService::StartQrLogin(QrReadyCb on_qr, LoginResultCb on_result) {
    std::lock_guard<std::mutex> lock(mutex_);
    CancelQrLogin();
    qr_cancelled_ = false;

    struct QrTaskArg {
        WereadService* self;
        QrReadyCb on_qr;
        LoginResultCb on_result;
    };
    auto* arg = new QrTaskArg{this, std::move(on_qr), std::move(on_result)};

    auto task_fn = [](void* param) {
        std::unique_ptr<QrTaskArg> ctx(static_cast<QrTaskArg*>(param));
        WereadClient client(&ctx->self->auth_);
        std::string uid, qr_url, err;

        if (!client.BeginQrLogin(&uid, &qr_url, &err)) {
            ESP_LOGE(kTag, "BeginQrLogin failed: %s", err.c_str());
            DispatchToLvgl([cb = ctx->on_result, err]() {
                if (cb) cb(false, err.empty() ? "无法获取登录二维码" : err);
            });
            ctx->self->qr_task_ = nullptr;
            return;
        }

        ESP_LOGI(kTag, "QR URL obtained: %s, uid=%s", qr_url.c_str(), uid.c_str());
        DispatchToLvgl([cb = ctx->on_qr, qr_url]() {
            if (cb) cb(qr_url);
        });

        int poll_count = 0;
        while (!ctx->self->qr_cancelled_ && poll_count < 120) { // 最多等待 3 分钟
            vTaskDelay(pdMS_TO_TICKS(1500));
            if (ctx->self->qr_cancelled_) break;

            WereadClient::QrLoginState state = WereadClient::QrLoginState::kPending;
            if (!client.PollQrLogin(uid, "", &state, &err)) {
                ESP_LOGW(kTag, "PollQrLogin error: %s", err.c_str());
            }

            if (state == WereadClient::QrLoginState::kComplete) {
                ESP_LOGI(kTag, "QR Login complete! vid=%s", ctx->self->auth_.vid.c_str());
                ctx->self->auth_store_.Save(ctx->self->auth_);
                DispatchToLvgl([cb = ctx->on_result]() {
                    if (cb) cb(true, "");
                });
                break;
            } else if (state == WereadClient::QrLoginState::kExpired) {
                ESP_LOGW(kTag, "QR code expired");
                DispatchToLvgl([cb = ctx->on_result]() {
                    if (cb) cb(false, "二维码已过期，请重试");
                });
                break;
            }
            poll_count++;
        }

        if (poll_count >= 120 && !ctx->self->qr_cancelled_) {
            DispatchToLvgl([cb = ctx->on_result]() {
                if (cb) cb(false, "登录超时");
            });
        }

        ctx->self->qr_task_ = nullptr;
    };

    if (!SpawnPsrWorker("weread_qr", 24576, tskIDLE_PRIORITY + 2, task_fn, arg, &qr_task_)) {
        delete arg;
        return false;
    }
    return true;
}

void WereadService::CancelQrLogin() {
    qr_cancelled_ = true;
    if (qr_task_ != nullptr) {
        // 等待任务自行退出或由其自然结束
        qr_task_ = nullptr;
    }
}

bool WereadService::FetchShelfAsync(ShelfDoneCb cb) {
    struct ShelfTaskArg {
        WereadService* self;
        ShelfDoneCb cb;
    };
    auto* arg = new ShelfTaskArg{this, std::move(cb)};

    auto task_fn = [](void* param) {
        std::unique_ptr<ShelfTaskArg> ctx(static_cast<ShelfTaskArg*>(param));
        WereadClient client(&ctx->self->auth_);
        std::vector<WereadBook> books;
        std::string err;

        bool ok = client.FetchShelf(&books, &err);
        if (!ok && err.find("过期") != std::string::npos) {
            ESP_LOGW(kTag, "Session may be expired, trying renew...");
            if (client.RenewSession(&err)) {
                ctx->self->auth_store_.Save(ctx->self->auth_);
                ok = client.FetchShelf(&books, &err);
            }
        }

        std::string json_res = "[]";
        if (ok) {
            cJSON* arr = cJSON_CreateArray();
            for (const auto& b : books) {
                cJSON* obj = cJSON_CreateObject();
                cJSON_AddStringToObject(obj, "id", b.id.c_str());
                cJSON_AddStringToObject(obj, "title", b.title.c_str());
                cJSON_AddStringToObject(obj, "author", b.author.c_str());
                cJSON_AddNumberToObject(obj, "progress", b.progress);
                cJSON_AddStringToObject(obj, "chapterUid", b.progress_chapter_uid.c_str());
                cJSON_AddNumberToObject(obj, "chapterOffset", b.progress_chapter_offset);
                cJSON_AddNumberToObject(obj, "updateTime", b.update_time);
                cJSON_AddItemToArray(arr, obj);
            }
            char* print_str = cJSON_PrintUnformatted(arr);
            if (print_str) {
                json_res = print_str;
                free(print_str);
            }
            cJSON_Delete(arr);
        }

        int status = ok ? 200 : -1;
        DispatchToLvgl([cb = ctx->cb, json_res, status]() {
            if (cb) cb(json_res, status);
        });
    };

    if (!SpawnPsrWorker("weread_shelf", 32768, tskIDLE_PRIORITY + 2, task_fn, arg)) {
        DispatchToLvgl([cb = arg->cb]() {
            if (cb) cb("[]", -1);
        });
        delete arg;
        return false;
    }
    return true;
}

bool WereadService::FetchChaptersAsync(const std::string& book_id, ChaptersDoneCb cb) {
    struct TocTaskArg {
        WereadService* self;
        std::string book_id;
        ChaptersDoneCb cb;
    };
    auto* arg = new TocTaskArg{this, book_id, std::move(cb)};

    auto task_fn = [](void* param) {
        std::unique_ptr<TocTaskArg> ctx(static_cast<TocTaskArg*>(param));
        WereadClient client(&ctx->self->auth_);
        std::vector<WereadChapter> chapters;
        std::string err;

        bool ok = client.FetchChapters(ctx->book_id, &chapters, &err);
        std::string json_res = "[]";
        if (ok) {
            cJSON* arr = cJSON_CreateArray();
            for (const auto& c : chapters) {
                cJSON* obj = cJSON_CreateObject();
                cJSON_AddStringToObject(obj, "uid", c.uid.c_str());
                cJSON_AddStringToObject(obj, "title", c.title.c_str());
                cJSON_AddNumberToObject(obj, "index", c.index);
                cJSON_AddNumberToObject(obj, "wordCount", c.word_count);
                cJSON_AddItemToArray(arr, obj);
            }
            char* print_str = cJSON_PrintUnformatted(arr);
            if (print_str) {
                json_res = print_str;
                free(print_str);
            }
            cJSON_Delete(arr);
        }

        int status = ok ? 200 : -1;
        DispatchToLvgl([cb = ctx->cb, json_res, status]() {
            if (cb) cb(json_res, status);
        });
    };

    if (!SpawnPsrWorker("weread_toc", 32768, tskIDLE_PRIORITY + 2, task_fn, arg)) {
        DispatchToLvgl([cb = arg->cb]() {
            if (cb) cb("[]", -1);
        });
        delete arg;
        return false;
    }
    return true;
}

bool WereadService::FetchChapterTextAsync(const std::string& book_id, const std::string& chapter_uid, TextDoneCb cb) {
    struct TextTaskArg {
        WereadService* self;
        std::string book_id;
        std::string chapter_uid;
        TextDoneCb cb;
    };
    auto* arg = new TextTaskArg{this, book_id, chapter_uid, std::move(cb)};

    auto task_fn = [](void* param) {
        std::unique_ptr<TextTaskArg> ctx(static_cast<TextTaskArg*>(param));
        WereadClient client(&ctx->self->auth_);
        WereadChapter chapter;
        chapter.uid = ctx->chapter_uid;
        std::string text, err;

        bool ok = client.FetchChapterText(ctx->book_id, chapter, &text, &err);
        int status = ok ? 200 : -1;
        if (!ok) {
            text = err.empty() ? "加载章节正文失败" : err;
        }

        DispatchToLvgl([cb = ctx->cb, text, status]() {
            if (cb) cb(text, status);
        });
    };

    if (!SpawnPsrWorker("weread_text", 49152, tskIDLE_PRIORITY + 2, task_fn, arg)) {
        DispatchToLvgl([cb = arg->cb]() {
            if (cb) cb("加载章节正文失败", -1);
        });
        delete arg;
        return false;
    }
    return true;
}

void WereadService::SyncProgress(const std::string& book_id, const std::string& chapter_uid,
                                  int chapter_idx, int chapter_offset, int percent, int read_seconds) {
    struct SyncArg {
        WereadService* self;
        std::string book_id;
        WereadChapter chapter;
        int chapter_offset;
        int percent;
        int read_seconds;
    };
    auto* arg = new SyncArg{this, book_id, {chapter_uid, "", chapter_idx, 0}, chapter_offset, percent, read_seconds};

    auto task_fn = [](void* param) {
        std::unique_ptr<SyncArg> ctx(static_cast<SyncArg*>(param));
        WereadClient client(&ctx->self->auth_);
        std::string err;
        client.UploadProgress(ctx->book_id, ctx->chapter, ctx->chapter_offset, ctx->percent, ctx->read_seconds, &err);
    };

    if (!SpawnPsrWorker("weread_sync", 16384, tskIDLE_PRIORITY + 1, task_fn, arg)) {
        delete arg;
    }
}
