#ifndef WXBOOK_WEREAD_CLIENT_H
#define WXBOOK_WEREAD_CLIENT_H

#include "weread_auth_store.h"
#include "weread_types.h"

#include <functional>
#include <string>
#include <vector>

class WereadClient {
public:
    using CancelCheck = std::function<bool()>;
    enum class QrLoginState : uint8_t {
        kPending = 0,
        kComplete,
        kNeedOtp,
        kOtpMismatch,
        kExpired,
    };

    explicit WereadClient(WereadAuth* auth) : auth_(auth) {}
    ~WereadClient();
    WereadClient(const WereadClient&) = delete;
    WereadClient& operator=(const WereadClient&) = delete;

    bool BeginQrLogin(std::string* uid, std::string* qr_url, std::string* error);
    bool PollQrLogin(const std::string& uid,
                     const std::string& otp,
                     QrLoginState* state,
                     std::string* error);
    bool RenewSession(std::string* error);
    bool FetchShelf(std::vector<WereadBook>* books, std::string* error);
    bool FetchChapters(const std::string& book_id,
                       std::vector<WereadChapter>* chapters,
                       std::string* error);
    bool FetchChapterText(const std::string& book_id,
                          const WereadChapter& chapter,
                          std::string* text,
                          std::string* error,
                          const CancelCheck& cancelled = {});
    bool UploadProgress(const std::string& book_id,
                        const WereadChapter& chapter,
                        int chapter_offset,
                        int percent,
                        int read_seconds,
                        std::string* error);

private:
    struct Response {
        int status = -1;
        bool overflow = false;
        std::string body;
        std::vector<std::string> set_cookies;
    };

    Response Request(const std::string& method,
                     const std::string& url,
                     const std::string& body = {},
                     const std::string& referer = {},
                     bool absorb_cookies = true);
    Response RequestOnce(const std::string& method,
                         const std::string& url,
                         const std::string& body,
                         const std::string& referer,
                         bool absorb_cookies);
    void AbsorbCookies(const Response& response);
    std::string CookieHeader() const;
    bool FetchReaderToken(const std::string& reader_url,
                          std::string* psvts,
                          int* status,
                          size_t* bytes_read,
                          std::string* error);
    bool ReaderTokens(const std::string& book_id,
                      const std::string& chapter_uid,
                      std::string* reader_url,
                      std::string* psvts,
                      std::string* error,
                      bool force_refresh = false);
    void CloseSession();

    WereadAuth* auth_ = nullptr;
    void* session_ = nullptr;
    Response* active_response_ = nullptr;
};

#endif
