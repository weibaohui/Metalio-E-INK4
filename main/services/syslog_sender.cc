// main/services/syslog_sender.cc —— 见 syslog_sender.h(需求一.2)。
#include "syslog_sender.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <mutex>
#include <unistd.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "utils/psr_worker.h"
#include <freertos/semphr.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_event.h>
#include <esp_netif.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>

#include "mcp_server.h"
#include "settings.h"

namespace {

constexpr const char* TAG = "Syslog";
constexpr const char* kNvsNs = "syslog";

constexpr size_t kFormatCap = 256;   // 钩子内格式化上限(只上栈,超长截断)
constexpr size_t kRingCap = 4096;    // 字节环(静态,内部 RAM 预算紧张,4K 约二十行日志)
constexpr size_t kLineCap = 384;     // 单条 syslog 消息上限
constexpr int kIdlePollMs = 100;
constexpr int kReopenRetryMs = 5000;
constexpr int kDefaultPort = 514;
constexpr int kSyslogFacilityUser = 1;  // user-level messages

// ---- 配置(RAM 缓存;NVS 读写只在 main 任务) ----
std::mutex s_cfg_mutex;
bool s_enabled = false;
char s_host[64] = {};
int s_port = kDefaultPort;
std::atomic<uint32_t> s_cfg_gen{0};   // 配置代际:变更即自增,发送任务据之重连

// ---- vprintf 钩子 / 字节环 / 发送任务 ----
vprintf_like_t s_prev_vprintf = nullptr;
TaskHandle_t s_task = nullptr;
SemaphoreHandle_t s_data_sem = nullptr;
SemaphoreHandle_t s_net_sem = nullptr;

portMUX_TYPE s_ring_mux = portMUX_INITIALIZER_UNLOCKED;
char s_ring[kRingCap];
size_t s_ring_head = 0, s_ring_tail = 0, s_ring_used = 0;

char s_hostname[20] = "metalio";
std::atomic<bool> s_running{false};   // 发送任务运行标志(Stop 置 false 让其自退)
std::atomic<bool> s_hooked{false};
volatile bool s_ip_ready = false;
volatile uint32_t s_hook_depth = 0;

// esp_log 级别字母 → syslog severity
int SeverityOf(const char* line) {
    // 行首可能是颜色转义 "\x1b[3Xm" 后跟级别字母 "X (ts) TAG:"
    const char* p = line;
    if (p[0] == 0x1b) {  // 跳过 \x1b[...m
        while (*p && *p != 'm') p++;
        if (*p == 'm') p++;
    }
    switch (p[0]) {
        case 'E': return 3;  // error
        case 'W': return 4;  // warning
        case 'I': return 6;  // info
        case 'D': return 7;  // debug
        case 'V': return 7;  // verbose
        default:  return 6;
    }
}

size_t RingFree() { return kRingCap - s_ring_used; }

void RingWriteBytes(const char* data, size_t len) {
    if (data == nullptr || len == 0) return;
    if (len > kRingCap) { data += (len - kRingCap); len = kRingCap; }
    portENTER_CRITICAL(&s_ring_mux);
    if (len > RingFree()) {
        const size_t drop = len - RingFree();
        s_ring_tail = (s_ring_tail + drop) % kRingCap;
        s_ring_used -= drop;
    }
    const size_t first = (s_ring_head + len <= kRingCap) ? len : (kRingCap - s_ring_head);
    memcpy(s_ring + s_ring_head, data, first);
    if (first < len) memcpy(s_ring, data + first, len - first);
    s_ring_head = (s_ring_head + len) % kRingCap;
    s_ring_used += len;
    portEXIT_CRITICAL(&s_ring_mux);
    if (s_data_sem != nullptr) xSemaphoreGive(s_data_sem);
}

size_t RingReadBytes(char* out, size_t max_len) {
    if (out == nullptr || max_len == 0) return 0;
    portENTER_CRITICAL(&s_ring_mux);
    const size_t n = s_ring_used < max_len ? s_ring_used : max_len;
    if (n == 0) { portEXIT_CRITICAL(&s_ring_mux); return 0; }
    const size_t first = (s_ring_tail + n <= kRingCap) ? n : (kRingCap - s_ring_tail);
    memcpy(out, s_ring + s_ring_tail, first);
    if (first < n) memcpy(out + first, s_ring, n - first);
    s_ring_tail = (s_ring_tail + n) % kRingCap;
    s_ring_used -= n;
    portEXIT_CRITICAL(&s_ring_mux);
    return n;
}

void RingDiscardAll() {
    portENTER_CRITICAL(&s_ring_mux);
    s_ring_head = s_ring_tail = s_ring_used = 0;
    portEXIT_CRITICAL(&s_ring_mux);
}

int HookedVprintf(const char* fmt, va_list args) {
    // ISR/重入:绝不碰环与信号量,直接走原路径
    if (xPortInIsrContext() || s_hook_depth > 0) {
        return s_prev_vprintf ? s_prev_vprintf(fmt, args) : vprintf(fmt, args);
    }
    s_hook_depth++;
    char stack_buf[kFormatCap];
    va_list copy;
    va_copy(copy, args);
    const int needed = vsnprintf(stack_buf, sizeof(stack_buf), fmt, copy);
    va_end(copy);
    if (needed > 0) {
        const size_t n = (size_t)needed < sizeof(stack_buf) ? (size_t)needed : sizeof(stack_buf) - 1;
        RingWriteBytes(stack_buf, n);
    }
    const int ret = s_prev_vprintf ? s_prev_vprintf(fmt, args) : vprintf(fmt, args);
    s_hook_depth--;
    return ret;
}

void OnIpEvent(void*, esp_event_base_t base, int32_t id, void*) {
    if (base != IP_EVENT) return;
    if (id == IP_EVENT_STA_GOT_IP) {
        s_ip_ready = true;
        if (s_net_sem != nullptr) xSemaphoreGive(s_net_sem);
    } else if (id == IP_EVENT_STA_LOST_IP) {
        s_ip_ready = false;
    }
}

void FormatSyslogHeader(char* out, size_t cap, int severity) {
    // RFC3164: <PRI>Mmm dd HH:MM:SS host
    const int pri = kSyslogFacilityUser * 8 + severity;
    time_t now = time(nullptr);
    struct tm local = {};
    localtime_r(&now, &local);
    char ts[24];
    if (local.tm_year + 1900 >= 2025) {
        static const char* kMon[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
        snprintf(ts, sizeof(ts), "%s %2d %02d:%02d:%02d", kMon[local.tm_mon],
                 local.tm_mday, local.tm_hour, local.tm_min, local.tm_sec);
    } else {
        // 未对时:用开机以来的秒数,避免误导性的 1970
        snprintf(ts, sizeof(ts), "uptime %us", (unsigned)(esp_timer_get_time() / 1000000));
    }
    snprintf(out, cap, "<%d>%s %s ", pri, ts, s_hostname);
}

bool ResolveDest(sockaddr_in* out, uint32_t* gen_out) {
    std::lock_guard<std::mutex> lock(s_cfg_mutex);
    if (!s_enabled || s_host[0] == '\0') return false;
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET;
    out->sin_port = htons((uint16_t)s_port);
    if (inet_pton(AF_INET, s_host, &out->sin_addr) != 1) return false;
    if (gen_out != nullptr) *gen_out = s_cfg_gen.load();
    return true;
}

void CloseSock(int* sock) {
    if (sock != nullptr && *sock >= 0) { close(*sock); *sock = -1; }
}

void SenderTask(void*) {
    int sock = -1;
    uint32_t cur_gen = 0;
    sockaddr_in dest{};
    bool dest_ok = false;
    TickType_t last_open = 0;
    char chunk[512];
    char line[kLineCap];
    size_t line_len = 0;

    while (s_running.load()) {
        if (!s_ip_ready) {
            if (s_net_sem != nullptr) xSemaphoreTake(s_net_sem, pdMS_TO_TICKS(1000));
            else vTaskDelay(pdMS_TO_TICKS(kIdlePollMs));
            RingDiscardAll();
            line_len = 0;
            CloseSock(&sock);
            continue;
        }
        // 配置变了(host/port/enabled)则重连
        if (s_cfg_gen.load() != cur_gen) {
            CloseSock(&sock);
            dest_ok = ResolveDest(&dest, &cur_gen);
            if (!dest_ok) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
        }
        if (!dest_ok) {  // 未启用/无 IP:清空并睡
            RingDiscardAll();
            line_len = 0;
            vTaskDelay(pdMS_TO_TICKS(kIdlePollMs));
            continue;
        }

        if (s_data_sem != nullptr) xSemaphoreTake(s_data_sem, pdMS_TO_TICKS(kIdlePollMs));
        else vTaskDelay(pdMS_TO_TICKS(kIdlePollMs));

        size_t n = RingReadBytes(chunk, sizeof(chunk));
        if (n == 0) continue;

        // 按行拆分,逐条封 RFC3164 发送
        for (size_t i = 0; i < n; ++i) {
            char c = chunk[i];
            if (c == '\r') continue;
            if (c != '\n') {
                if (line_len < kLineCap - 1) line[line_len++] = c;
                continue;
            }
            line[line_len] = '\0';
            if (line_len == 0) continue;
            if (sock < 0) {
                const TickType_t now = xTaskGetTickCount();
                if (last_open != 0 && (now - last_open) < pdMS_TO_TICKS(kReopenRetryMs)) break;
                last_open = now;
                sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                if (sock >= 0) {
                    const int flags = fcntl(sock, F_GETFL, 0);
                    if (flags >= 0) fcntl(sock, F_SETFL, flags | O_NONBLOCK);
                }
                if (sock < 0) break;
            }
            char packet[kLineCap + 96];
            FormatSyslogHeader(packet, sizeof(packet), SeverityOf(line));
            size_t off = strlen(packet);
            size_t copy = line_len < sizeof(packet) - off - 1 ? line_len : sizeof(packet) - off - 1;
            memcpy(packet + off, line, copy);
            off += copy;
            packet[off] = '\0';
            const ssize_t sent = sendto(sock, packet, off, 0, (sockaddr*)&dest, sizeof(dest));
            if (sent < 0) { CloseSock(&sock); break; }
            line_len = 0;
        }
        // 行未遇换行则留待下一块(append)
    }
    CloseSock(&sock);
    s_task = nullptr;
    // return 即自动自删(SpawnPsrWorker 包装,勿再 vTaskDeleteWithCaps)
}

void LoadHostname() {
    uint8_t mac[6] = {};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        snprintf(s_hostname, sizeof(s_hostname), "metalio-%02x%02x%02x", mac[3], mac[4], mac[5]);
    }
}

void InstallHook() {
    if (!s_hooked.load()) {
        s_prev_vprintf = esp_log_set_vprintf(HookedVprintf);
        s_hooked.store(true);
    }
}

void UninstallHook() {
    if (s_hooked.exchange(false) && s_prev_vprintf != nullptr) {
        esp_log_set_vprintf(s_prev_vprintf);
        s_prev_vprintf = nullptr;
    }
}

bool s_ip_handlers_registered = false;  // IP 事件处理只注册一次(防 enable/disable 重注册)

void StartLocked() {
    // 已在 main 任务持锁之外调用;此处只负责起任务+装钩子
    if (s_data_sem == nullptr) s_data_sem = xSemaphoreCreateBinary();
    if (s_net_sem == nullptr) s_net_sem = xSemaphoreCreateBinary();
    if (s_data_sem == nullptr || s_net_sem == nullptr) return;

    if (!s_ip_handlers_registered) {
        s_ip_handlers_registered = true;
        (void)esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &OnIpEvent, nullptr);
        (void)esp_event_handler_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &OnIpEvent, nullptr);
    }

    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != nullptr) {
        esp_netif_ip_info_t info{};
        if (esp_netif_get_ip_info(netif, &info) == ESP_OK && info.ip.addr != 0) {
            s_ip_ready = true;
            xSemaphoreGive(s_net_sem);
        }
    }

    if (s_task == nullptr) {
        s_running.store(true);
        // 栈放 PSRAM(统一入口 SpawnPsrWorker):内部 RAM 预算紧张(承载
        // httpd/lwip/WiFi),本任务只做 socket 收发,与 radio 任务同先例。
        if (!SpawnPsrWorker("syslog", 4096, 1, SenderTask, nullptr, &s_task)) {
            s_running.store(false);
            ESP_LOGE(TAG, "syslog sender task create failed");
            return;
        }
    }
    InstallHook();
    ESP_LOGI(TAG, "syslog -> %s:%d (IPv4 UDP RFC3164)", s_host, s_port);
}

void StopLocked() {
    UninstallHook();
    s_running.store(false);   // 发送任务下个循环自退
    RingDiscardAll();
}

void LoadConfig() {
    // main 任务(内部栈)读 NVS
    Settings settings(kNvsNs, false);
    std::lock_guard<std::mutex> lock(s_cfg_mutex);
    s_enabled = settings.GetBool("enabled", false);
    const std::string host = settings.GetString("host", "");
    strncpy(s_host, host.c_str(), sizeof(s_host) - 1);
    s_host[sizeof(s_host) - 1] = '\0';
    s_port = settings.GetInt("port", kDefaultPort);
    if (s_port <= 0 || s_port > 65535) s_port = kDefaultPort;
}

void PersistConfig() {
    // 必须在 main 任务(内部栈);调用方(MCP 工具回调)经 Application::Schedule 在 main 执行
    std::string host;
    bool enabled;
    int port;
    {
        std::lock_guard<std::mutex> lock(s_cfg_mutex);
        host = s_host;
        enabled = s_enabled;
        port = s_port;
    }
    Settings settings(kNvsNs, true);
    settings.SetBool("enabled", enabled);
    settings.SetString("host", host);
    settings.SetInt("port", port);
}

void ApplyRunningState() {
    // 依配置决定起/停。Called on main task.
    std::lock_guard<std::mutex> lock(s_cfg_mutex);
    const bool want = s_enabled && s_host[0] != '\0';
    if (want) {
        if (s_task == nullptr || !s_hooked.load()) StartLocked();
    } else {
        if (s_task != nullptr || s_hooked.load()) StopLocked();
    }
}

}  // namespace

void Syslog_Init() {
    LoadHostname();
    LoadConfig();
    ApplyRunningState();
}

void Syslog_Configure(const std::string& host, int port) {
    {
        std::lock_guard<std::mutex> lock(s_cfg_mutex);
        strncpy(s_host, host.c_str(), sizeof(s_host) - 1);
        s_host[sizeof(s_host) - 1] = '\0';
        if (port > 0 && port <= 65535) s_port = port;
        s_cfg_gen.fetch_add(1);
    }
    PersistConfig();
    ApplyRunningState();
    ESP_LOGI(TAG, "syslog configured: enabled=%d host=%s port=%d",
             Syslog_IsEnabled(), Syslog_GetHost().c_str(), Syslog_GetPort());
}

void Syslog_SetEnabled(bool enable) {
    {
        std::lock_guard<std::mutex> lock(s_cfg_mutex);
        s_enabled = enable;
        s_cfg_gen.fetch_add(1);
    }
    PersistConfig();
    ApplyRunningState();
}

bool Syslog_IsEnabled() {
    std::lock_guard<std::mutex> lock(s_cfg_mutex);
    return s_enabled;
}
bool Syslog_IsRunning() { return s_running.load() && s_hooked.load(); }
std::string Syslog_GetHost() {
    std::lock_guard<std::mutex> lock(s_cfg_mutex);
    return s_host;
}
int Syslog_GetPort() {
    std::lock_guard<std::mutex> lock(s_cfg_mutex);
    return s_port;
}

// ---- MCP 工具(暂不做界面,需求一.2)。回调经 Application::Schedule 在 main 任务执行,
// 故 PersistConfig 的 NVS 写安全。 ----
void SyslogRegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();

    mcp.AddTool("self.syslog.configure",
        "Configure the syslog (RFC3164 over UDP) target host and port. host must be an IPv4 literal (x.x.x.x). port defaults to 514.",
        PropertyList({Property("host", kPropertyTypeString),
                      Property("port", kPropertyTypeInteger, 514)}),
        [](const PropertyList& properties) -> ReturnValue {
            const std::string host = properties["host"].value<std::string>();
            const int port = properties["port"].value<int>();
            Syslog_Configure(host, port);
            return std::string("syslog 目标已设为 ") + host + ":" + std::to_string(port) +
                   "(enabled 状态不变,用 self.syslog.enable 开)";
        });

    mcp.AddTool("self.syslog.enable",
        "Enable or disable syslog log forwarding over UDP.",
        PropertyList({Property("on", kPropertyTypeBoolean)}),
        [](const PropertyList& properties) -> ReturnValue {
            const bool on = properties["on"].value<bool>();
            Syslog_SetEnabled(on);
            return std::string("syslog 已") + (on ? "开启" : "关闭") +
                   (on ? "(需先 configure 设好 IP)" : "");
        });

    mcp.AddTool("self.syslog.status",
        "Get syslog forwarder status: enabled, running, host, port.",
        PropertyList(),
        [](const PropertyList&) -> ReturnValue {
            return std::string("syslog: enabled=") + (Syslog_IsEnabled() ? "是" : "否") +
                   " running=" + (Syslog_IsRunning() ? "是" : "否") +
                   " host=" + Syslog_GetHost() + ":" + std::to_string(Syslog_GetPort());
        });

    ESP_LOGI(TAG, "syslog MCP tools registered");
}
