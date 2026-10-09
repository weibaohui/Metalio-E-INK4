// main/services/calendar/calendar_ics.cc —— 见 calendar_ics.h。
#include "calendar_ics.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/idf_additions.h>   // xTaskCreatePinnedToCoreWithCaps / vTaskDeleteWithCaps
#include <freertos/task.h>
#include <zlib.h>   // espressif__zlib 已随 reader 链接;苹果日历服务器恒回 gzip,须解压

#include "application.h"
#include "board.h"
#include "calendar_store.h"
#include "power_policy.h"
#include "services/time_service.h"
#include "settings.h"

namespace {

constexpr const char* TAG = "CalendarIcs";

// 订阅源体积上限:苹果法定节假日约 50KB;384KB 已极宽。防恶意/异常 URL 撑爆 RAM。
constexpr size_t kMaxIcsBytes = 384 * 1024;
// 跨天展开后总条数上限(法定节假日 3 年约 300 条)。
constexpr int kMaxEvents = 1200;
// 单个跨天日程最多展开天数(春节假期 8 天;31 兜底)。
constexpr int kMaxSpanDays = 31;
// 标题截断(日历屏一行放不下长标题)。
constexpr size_t kMaxTitleBytes = 60;
constexpr int kHttpTimeoutMs = 20000;
// HTTPS 握手的内部 RAM 需求:mbedtls 动态收发缓冲走 malloc 已落 PSRAM
// (SPIRAM_USE_MALLOC,>16B 即 PSRAM),内部 RAM 只需 lwIP 收包 pbuf 余量与
// 握手工作集,实测 12KB 起可成。30KB 的保守预检在开机碎片化下永远拦截
// (44KB 空闲但最大块 <30KB),导致 ICS 永远无法同步。失败路径安全:
// esp-tls 干净报错,不会把网络栈饿崩(收音机 TLS 守卫同因,失败不致命)。
constexpr size_t kTlsMinInternalBlock = 12 * 1024;
constexpr int64_t kAutoRefreshIntervalSec = 12 * 3600;   // 进日历屏时数据超 12h 自动同步

std::mutex s_mutex;
std::string s_url;
time_t s_last_fetched = 0;
std::string s_last_error;
bool s_fetching = false;
bool s_inited = false;
void (*s_changed_cb)() = nullptr;

// ---- 时间工具(设备为"本地历元"模型:TZ=GMT+0,epoch 即本地民用时) ----
void EpochToYmd(time_t t, int& y, int& m, int& d) {
    struct tm tmv = {};
    localtime_r(&t, &tmv);
    y = tmv.tm_year + 1900; m = tmv.tm_mon + 1; d = tmv.tm_mday;
}
time_t YmdToNoonEpoch(int y, int m, int d) {
    struct tm tmv = {};
    tmv.tm_year = y - 1900; tmv.tm_mon = m - 1; tmv.tm_mday = d; tmv.tm_hour = 12;
    return mktime(&tmv);
}
std::string YmdToStr(int y, int m, int d) {
    char b[40];   // 年份字段理论 11 字节,-Werror 防截断
    snprintf(b, sizeof(b), "%04d-%02d-%02d", y, m, d);
    return b;
}

// ---- ICS 解析 ----
// 折行展开:以空格/制表符开头的行是上一行的续行(RFC5545 §3.1)。
std::vector<std::string> UnfoldLines(const std::string& ics) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos <= ics.size()) {
        size_t nl = ics.find('\n', pos);
        if (nl == std::string::npos) nl = ics.size();
        std::string line = ics.substr(pos, nl - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && (line[0] == ' ' || line[0] == '\t')) {
            if (!lines.empty()) lines.back() += line.substr(1);
        } else if (!line.empty()) {
            lines.push_back(std::move(line));
        }
        if (nl >= ics.size()) break;
        pos = nl + 1;
    }
    return lines;
}

std::string UnescapeText(const std::string& v) {
    std::string out;
    out.reserve(v.size());
    for (size_t i = 0; i < v.size(); ++i) {
        if (v[i] == '\\' && i + 1 < v.size()) {
            const char n = v[++i];
            if (n == 'n' || n == 'N') { out += ' '; continue; }
            out += n;   // \, \; \\ 及未知转义:保留后字符
            continue;
        }
        out += v[i];
    }
    return out;
}

// UTF-8 安全截断:不在多字节字符中间切断。
void TruncateTitle(std::string& s) {
    if (s.size() <= kMaxTitleBytes) return;
    size_t cut = kMaxTitleBytes;
    while (cut > 0 && (static_cast<uint8_t>(s[cut]) & 0xC0) == 0x80) --cut;
    s.resize(cut);
}

bool ParseYmd(const std::string& v, int& y, int& m, int& d) {
    if (v.size() < 8) return false;
    for (int i = 0; i < 8; ++i) if (!isdigit((uint8_t)v[i])) return false;
    y = (v[0]-'0')*1000 + (v[1]-'0')*100 + (v[2]-'0')*10 + (v[3]-'0');
    m = (v[4]-'0')*10 + (v[5]-'0');
    d = (v[6]-'0')*10 + (v[7]-'0');
    return y >= 2000 && y <= 2200 && m >= 1 && m <= 12 && d >= 1 && d <= 31;
}

struct RawEvent {
    bool all_day = false;
    bool utc = false;        // 值带 Z 后缀
    int sy = 0, sm = 0, sd = 0;
    int sh = 0, smin = 0;
    bool has_end = false;
    bool end_all_day = false;
    int ey = 0, em = 0, ed = 0;
    int eh = 0, emin = 0;
    // RRULE 仅支持 FREQ=YEARLY(苹果法定节假日用 YEARLY;COUNT=6 表达元旦/妇女节等
    // 固定日节日);其余频率(RRULE_FREQ_WEEKLY 等)不支持,整条跳过。
    int yearly_count = 0;      // 0=无 RRULE;>0=含首发生共重复次数
    int yearly_interval = 1;
    time_t yearly_until = 0;   // 0=无 UNTIL
    std::string summary;
};

// 解析 RRULE 值(形如 "FREQ=YEARLY;COUNT=6" / "FREQ=YEARLY;UNTIL=20291231T235959Z")。
// 返回 false=不支持的频率/写法,调用方整条事件跳过。
bool ParseRrule(const std::string& v, RawEvent& ev) {
    bool yearly = false;
    int count = 0, interval = 1;
    time_t until = 0;
    size_t pos = 0;
    while (pos <= v.size()) {
        const size_t semi = v.find(';', pos);
        const std::string kv = v.substr(pos, semi == std::string::npos ? semi : semi - pos);
        const size_t eq = kv.find('=');
        if (eq != std::string::npos) {
            const std::string key = kv.substr(0, eq);
            const std::string val = kv.substr(eq + 1);
            if (key == "FREQ") yearly = (val == "YEARLY");
            else if (key == "COUNT") count = atoi(val.c_str());
            else if (key == "INTERVAL") interval = std::max(1, atoi(val.c_str()));
            else if (key == "UNTIL") {
                int uy, um, ud;
                if (ParseYmd(val, uy, um, ud)) until = YmdToNoonEpoch(uy, um, ud) + 43200;
            }
        }
        if (semi == std::string::npos) break;
        pos = semi + 1;
    }
    if (!yearly) return false;
    if (count <= 0 && until == 0) count = 1;   // 无终止条件:只当单次,防无限展开
    ev.yearly_count = count;
    ev.yearly_interval = interval;
    ev.yearly_until = until;
    return true;
}

// 解析 DTSTART/DTEND 的属性值(值部分),填 RawEvent 对应半边。
// prop 形如 "DTSTART;VALUE=DATE" / "DTSTART;TZID=Asia/Shanghai" / "DTSTART"。
bool ParseDateProp(const std::string& prop, const std::string& value,
                   bool is_start, RawEvent& ev) {
    const bool value_date = prop.find("VALUE=DATE") != std::string::npos;
    const bool all_day = value_date || value.find('T') == std::string::npos;
    int y, m, d;
    if (!ParseYmd(value, y, m, d)) return false;
    int hh = 0, mm = 0;
    bool utc = false;
    if (!all_day) {
        const size_t t = value.find('T');
        if (t == std::string::npos || t + 5 >= value.size()) return false;
        const std::string hms = value.substr(t + 1);
        if (!isdigit((uint8_t)hms[0]) || !isdigit((uint8_t)hms[1]) ||
            !isdigit((uint8_t)hms[2]) || !isdigit((uint8_t)hms[3])) return false;
        hh = (hms[0]-'0')*10 + (hms[1]-'0');
        mm = (hms[2]-'0')*10 + (hms[3]-'0');
        if (hh > 23 || mm > 59) return false;
        utc = !hms.empty() && hms.back() == 'Z';
    }
    if (is_start) {
        ev.all_day = all_day; ev.utc = utc;
        ev.sy = y; ev.sm = m; ev.sd = d; ev.sh = hh; ev.smin = mm;
    } else {
        ev.has_end = true; ev.end_all_day = all_day;
        ev.ey = y; ev.em = m; ev.ed = d; ev.eh = hh; ev.emin = mm;
    }
    return true;
}

// 定时日程的历元:Z 后缀是 UTC 民用时,换算到设备本地历元要加时区偏移;
// 浮动时间/TZID=Asia/Shanghai 直接按本地民用时(设备 TZ=GMT+0,mktime 即历元)。
time_t ToLocalEpoch(int y, int m, int d, int hh, int mm, bool utc) {
    struct tm tmv = {};
    tmv.tm_year = y - 1900; tmv.tm_mon = m - 1; tmv.tm_mday = d;
    tmv.tm_hour = hh; tmv.tm_min = mm;
    time_t t = mktime(&tmv);
    if (utc) t += (time_t)TimeService_GetTzOffsetMin() * 60;
    return t;
}

// 把一条 VEVENT 按天展开追加到 out(全天 DTEND 为开区间;定时跨天也逐天显示)。
void ExpandEvent(const RawEvent& ev, std::vector<CalendarEvent>& out) {
    if (ev.summary.empty()) return;
    std::string title = ev.summary;
    TruncateTitle(title);

    char time_buf[8] = "";
    time_t start_t, end_exclusive_t;
    if (ev.all_day) {
        start_t = YmdToNoonEpoch(ev.sy, ev.sm, ev.sd);
        // 全天 DTEND 为开区间且粒度是"日":结束日 00:00 即不含。转成当天零点
        // (正午-12h)再比较,否则跨天事件会多展开一天(如春节假期尾日重复)。
        end_exclusive_t = ev.has_end && ev.end_all_day
            ? YmdToNoonEpoch(ev.ey, ev.em, ev.ed) - 43200
            : start_t + 43200;
    } else {
        snprintf(time_buf, sizeof(time_buf), "%02d:%02d", ev.sh, ev.smin);
        start_t = ToLocalEpoch(ev.sy, ev.sm, ev.sd, ev.sh, ev.smin, ev.utc);
        end_exclusive_t = ev.has_end
            ? (ev.end_all_day ? YmdToNoonEpoch(ev.ey, ev.em, ev.ed)
                              : ToLocalEpoch(ev.ey, ev.em, ev.ed, ev.eh, ev.emin, ev.utc))
            : start_t + 1;
        if (end_exclusive_t <= start_t) end_exclusive_t = start_t + 1;
    }

    int y, m, d;
    EpochToYmd(start_t, y, m, d);
    time_t day_noon = YmdToNoonEpoch(y, m, d);
    for (int i = 0; i < kMaxSpanDays; ++i) {
        const time_t cur_noon = day_noon + (time_t)i * 86400;
        if (cur_noon - 43200 >= end_exclusive_t) break;   // 本日起点已在结束(开区间)之后
        EpochToYmd(cur_noon, y, m, d);
        if ((int)out.size() >= kMaxEvents) return;
        CalendarEvent e;
        e.date = YmdToStr(y, m, d);
        e.time = ev.all_day ? "" : time_buf;
        e.title = title;
        e.from_ics = true;
        out.push_back(std::move(e));
        if (ev.all_day && !ev.has_end) break;   // 无 DTEND 的全天事件只此一天
    }
}

// RRULE=YEARLY 展开:首发生年 + interval 步长逐年平移(月日不变),至多 count 次、
// 不晚于 until;硬上限 10 次防恶意规则(苹果法定节假日 COUNT=6)。
void ExpandYearly(const RawEvent& ev, std::vector<CalendarEvent>& out) {
    for (int k = 0; k < 10; ++k) {
        if (ev.yearly_count > 0 && k >= ev.yearly_count) break;
        RawEvent occ = ev;
        occ.sy += k * ev.yearly_interval;
        if (occ.has_end) occ.ey += k * ev.yearly_interval;
        if (ev.yearly_until > 0 && YmdToNoonEpoch(occ.sy, occ.sm, occ.sd) > ev.yearly_until) break;
        ExpandEvent(occ, out);
    }
}

int ParseIcs(const std::string& ics, std::vector<CalendarEvent>& out) {
    int skipped = 0;
    bool in_event = false;
    bool has_start = false;
    bool rrule_ok = false;   // 遇到 RRULE 且是受支持的 YEARLY
    bool rrule_bad = false;  // 遇到不支持的 RRULE:整条跳过(展开会错,不如不显示)
    RawEvent ev;

    for (const std::string& line : UnfoldLines(ics)) {
        if (line == "BEGIN:VEVENT") {
            in_event = true; has_start = false; rrule_ok = false; rrule_bad = false;
            ev = RawEvent{};
            continue;
        }
        if (line == "END:VEVENT") {
            if (in_event && has_start) {
                if (rrule_ok) ExpandYearly(ev, out);
                else if (!rrule_bad) ExpandEvent(ev, out);
                else ++skipped;
            }
            in_event = false;
            continue;
        }
        if (!in_event) continue;

        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        const std::string prop = line.substr(0, colon);
        const std::string value = line.substr(colon + 1);

        if (prop.rfind("DTSTART", 0) == 0) {
            has_start = ParseDateProp(prop, value, true, ev);
        } else if (prop.rfind("DTEND", 0) == 0) {
            ParseDateProp(prop, value, false, ev);
        } else if (prop.rfind("SUMMARY", 0) == 0) {
            ev.summary = UnescapeText(value);
        } else if (prop.rfind("RRULE", 0) == 0) {
            if (ParseRrule(value, ev)) rrule_ok = true;
            else rrule_bad = true;
        }
    }

    std::sort(out.begin(), out.end(), [](const CalendarEvent& a, const CalendarEvent& b) {
        if (a.date != b.date) return a.date < b.date;
        if (a.time != b.time) return a.time < b.time;
        return a.title < b.title;
    });
    out.erase(std::unique(out.begin(), out.end(), [](const CalendarEvent& a, const CalendarEvent& b) {
        return a.date == b.date && a.time == b.time && a.title == b.title;
    }), out.end());
    return skipped;
}

// ---- gzip 解压 ----
// 实测苹果 calendars.icloud.com 无视 Accept-Encoding: identity 恒回 gzip。
// gzip 尾 4 字节 ISIZE = 原始大小 mod 2^32,据此分配输出(PSRAM);超上限直接拒。
bool Gunzip(const std::string& in, std::string& out, std::string& err) {
    if (in.size() < 18) { err = "gzip 数据过短"; return false; }
    const uint8_t* tail = reinterpret_cast<const uint8_t*>(in.data() + in.size() - 4);
    const uint32_t isize = (uint32_t)tail[0] | ((uint32_t)tail[1] << 8) |
                           ((uint32_t)tail[2] << 16) | ((uint32_t)tail[3] << 24);
    if (isize == 0 || isize > kMaxIcsBytes) {
        err = "解压后过大(>" + std::to_string(kMaxIcsBytes / 1024) + "KB)";
        return false;
    }
    uint8_t* buf = static_cast<uint8_t*>(
        heap_caps_malloc(isize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buf == nullptr) { err = "内存不足,无法解压"; return false; }

    z_stream zs = {};
    bool ok = false;
    if (inflateInit2(&zs, 16 + MAX_WBITS) == Z_OK) {   // 16+15:自动识别 gzip 头
        zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(in.data()));
        zs.avail_in = (uInt)in.size();
        zs.next_out = reinterpret_cast<Bytef*>(buf);
        zs.avail_out = isize;
        const int ret = inflate(&zs, Z_FINISH);
        if (ret == Z_STREAM_END) {
            out.assign(reinterpret_cast<char*>(buf), isize);
            ok = true;
        } else if (ret == Z_BUF_ERROR) {
            err = "文件过大(解压溢出)";
        } else {
            err = "gzip 解压失败(" + std::to_string(ret) + ")";
        }
        inflateEnd(&zs);
    } else {
        err = "gzip 初始化失败";
    }
    free(buf);
    return ok;
}

// ---- 下载 ----
// 一次性 GET(含 1 跳手动重定向);成功返回解压后的纯文本,失败置 err。
std::string HttpGet(const std::string& url, std::string& err) {
    auto network = Board::GetInstance().GetNetwork();
    if (network == nullptr) { err = "无网络接口"; return ""; }
    auto http = network->CreateHttp(0);
    if (http == nullptr) { err = "创建 HTTP 客户端失败"; return ""; }
    http->SetTimeout(kHttpTimeoutMs);

    std::string real_url = url;
    for (int hop = 0; hop < 2; ++hop) {
        // 注意:不要 ApplyCommonHeaders/ApplyJsonHeaders —— 订阅地址是第三方服务器,
        // 携带云端鉴权头会把 token 泄给外部。headers 在每次发送后被实现清空,须逐跳重设。
        http->SetHeader("User-Agent", "Metalio-EInk/1.0");
        http->SetHeader("Accept", "text/calendar, */*");
        if (!http->Open("GET", real_url)) {
            err = "连接失败(请检查网络/URL)";
            http->Close();
            return "";
        }
        const int status = http->GetStatusCode();
        if (status >= 200 && status < 300) break;
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308) {
            const std::string loc = http->GetResponseHeader("Location");
            http->Close();
            if (loc.empty()) { err = "重定向无 Location"; return ""; }
            if (hop == 1) { err = "重定向过多"; return ""; }
            real_url = loc;
            continue;
        }
        err = "HTTP " + std::to_string(status);
        http->Close();
        return "";
    }

    std::string body;
    body.reserve(64 * 1024);
    char chunk[2048];
    while (true) {
        const int r = http->Read(chunk, sizeof(chunk));
        if (r < 0) {
            err = "读取中断";
            http->Close();
            return "";
        }
        if (r == 0) break;
        if (body.size() + (size_t)r > kMaxIcsBytes) {
            err = "文件过大(>" + std::to_string(kMaxIcsBytes / 1024) + "KB)";
            http->Close();
            return "";
        }
        body.append(chunk, (size_t)r);
    }
    http->Close();

    // gzip 魔数 1F 8B:解压后再校验
    if (body.size() >= 2 && (uint8_t)body[0] == 0x1F && (uint8_t)body[1] == 0x8B) {
        std::string plain;
        if (!Gunzip(body, plain, err)) return "";
        body = std::move(plain);
    }
    if (body.size() < 32 || body.find("BEGIN:VCALENDAR") == std::string::npos) {
        err = "内容不是有效的 ICS 日历";
        return "";
    }
    return body;
}

void SetLastError(const std::string& err) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_last_error = err;
}

void PersistFetched(time_t t) {
    // NVS 写必须离开当前线程(可能是 PSRAM 栈工作线程),调度到 main 任务。
    Application::GetInstance().Schedule([t]() {
        Settings settings("calendar", true);
        settings.SetInt("ics_fetched", (int32_t)t);
    });
}

void FetchWorker(void* /*arg*/) {
    std::string url;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        url = s_url;
    }
    ESP_LOGI(TAG, "ics sync start: %s", url.c_str());

    // 短时 HTTP 同步:硬占网 + 期间不计无操作进待机(与 OTA 同档)。
    PowerNeedHold hold_net(PowerNeed::OtaDownload);

    if (!Board::GetInstance().IsNetworkReady() &&
        !Board::GetInstance().EnsureNetworkReady(15000)) {
        SetLastError("网络未就绪");
        goto done;
    }
    if (url.rfind("https://", 0) == 0 &&
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) <
            kTlsMinInternalBlock) {
        SetLastError("内部 RAM 不足,HTTPS 握手无法完成;请关闭其他应用后重试");
        goto done;
    }

    {
        std::string err;
        const std::string body = HttpGet(url, err);
        if (body.empty()) {
            SetLastError(err);
            goto done;
        }
        std::vector<CalendarEvent> events;
        const int skipped = ParseIcs(body, events);
        if (events.empty()) {
            SetLastError("解析不到任何日程");
            goto done;
        }
        if (skipped > 0) {
            ESP_LOGW(TAG, "跳过 %d 条 RRULE 重复日程(暂不展开)", skipped);
        }
        const int n = Calendar_ReplaceIcsEvents(std::move(events));
        const time_t now = time(nullptr);
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_last_fetched = now;
            s_last_error.clear();
        }
        PersistFetched(now);
        ESP_LOGI(TAG, "ics sync ok: %d events", n);
        void (*cb)() = nullptr;
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            cb = s_changed_cb;
        }
        if (cb != nullptr) cb();
    }

done:
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_fetching = false;
    }
    vTaskDeleteWithCaps(nullptr);   // WithCaps 创建必须配对 WithCaps 自删
}

}  // namespace

void CalendarIcs_Init() {
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_inited) return;
    s_inited = true;
    // boot main 任务(内部栈):读 NVS 安全。
    Settings settings("calendar", false);
    s_url = settings.GetString("ics_url", "");
    s_last_fetched = (time_t)settings.GetInt("ics_fetched", 0);
    ESP_LOGI(TAG, "init: url=%s last_fetched=%lld",
             s_url.empty() ? "(none)" : s_url.c_str(), (long long)s_last_fetched);
}

std::string CalendarIcs_Subscribe(const std::string& url) {
    std::string u = url;
    // 去首尾空白
    const size_t b = u.find_first_not_of(" \t\r\n");
    const size_t e = u.find_last_not_of(" \t\r\n");
    u = (b == std::string::npos) ? "" : u.substr(b, e - b + 1);

    if (u.size() < 12 || u.size() > 200 ||
        (u.rfind("https://", 0) != 0 && u.rfind("http://", 0) != 0)) {
        return "URL 非法:需以 https:// 或 http:// 开头(12~200 字符)";
    }
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_url = u;
        s_last_error.clear();
    }
    // 落盘走 main 任务(调用方可能是 MCP 协议任务,不做 NVS)。
    Application::GetInstance().Schedule([u]() {
        Settings settings("calendar", true);
        settings.SetString("ics_url", u);
    });

    std::string msg;
    CalendarIcs_Refresh(&msg);
    return "已订阅:" + u + "\n" + msg;
}

std::string CalendarIcs_Unsubscribe() {
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_url.empty()) return "当前未订阅任何日历";
        s_url.clear();
        s_last_fetched = 0;
        s_last_error.clear();
    }
    Application::GetInstance().Schedule([]() {
        Settings settings("calendar", true);
        settings.SetString("ics_url", "");
        settings.SetInt("ics_fetched", 0);
    });
    const int n = Calendar_ClearIcsEvents();
    void (*cb)() = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        cb = s_changed_cb;
    }
    if (cb != nullptr) cb();
    return "已退订,清除订阅日程 " + std::to_string(n) + " 条(手动日程保留)";
}

bool CalendarIcs_Refresh(std::string* out_msg) {
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_url.empty()) {
            if (out_msg) *out_msg = "未订阅日历,请先 subscribe_ics";
            return false;
        }
        if (s_fetching) {
            if (out_msg) *out_msg = "正在同步中,请稍候";
            return false;
        }
        s_fetching = true;
    }

    // PSRAM 栈:内部 RAM 精贵,https 在 PSRAM 栈任务上已有 GLM/radio 先例。
    // 工作线程只碰 SD/RAM,不碰 NVS/flash;自删用 vTaskDeleteWithCaps。
    const BaseType_t ok = xTaskCreatePinnedToCoreWithCaps(
        FetchWorker, "cal_ics", 16384, nullptr, tskIDLE_PRIORITY + 1, nullptr, 0,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (ok != pdPASS) {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_fetching = false;
        s_last_error = "同步任务启动失败(内存不足)";
        if (out_msg) *out_msg = s_last_error;
        return false;
    }
    if (out_msg) *out_msg = "后台同步中,完成后日历自动更新";
    return true;
}

void CalendarIcs_MaybeAutoRefresh() {
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        if (s_url.empty() || s_fetching) return;
        const time_t now = time(nullptr);
        if (s_last_fetched > 0 && now - s_last_fetched < kAutoRefreshIntervalSec) return;
    }
    if (!Board::GetInstance().IsNetworkReady()) return;   // 离线:用旧数据,不阻塞进屏
    CalendarIcs_Refresh();
}

CalendarIcsStatus CalendarIcs_GetStatus() {
    CalendarIcsStatus st;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        st.url = s_url;
        st.fetching = s_fetching;
        st.last_fetched = s_last_fetched;
        st.last_error = s_last_error;
    }
    st.event_count = Calendar_IcsCount();   // 走 store 自己的锁,别在 s_mutex 里调
    return st;
}

void CalendarIcs_SetChangedCallback(void (*cb)()) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_changed_cb = cb;
}
