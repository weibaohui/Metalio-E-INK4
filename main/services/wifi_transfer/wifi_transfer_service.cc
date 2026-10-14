#include "wifi_transfer_service.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <esp_log.h>
#include <esp_http_server.h>
#include <esp_netif.h>
#include <cJSON.h>

#include "board.h"
#include "reader/book_library.h"
#include "reader/file_size.h"
#include "SdCardManager.hpp"
#include "sd_paths.h"
#include "ff.h"

namespace wifi_transfer {

namespace {

constexpr const char* TAG = "WifiTransfer";

std::string UrlDecode(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        if (in[i] == '%' && i + 2 < in.size()) {
            char hex[3] = {in[i + 1], in[i + 2], '\0'};
            char* end = nullptr;
            long val = std::strtol(hex, &end, 16);
            if (end == hex + 2) {
                out.push_back(static_cast<char>(val));
                i += 2;
                continue;
            }
        } else if (in[i] == '+') {
            out.push_back(' ');
            continue;
        }
        out.push_back(in[i]);
    }
    return out;
}

std::string SanitizeFilename(const std::string& name) {
    std::string s;
    for (char c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            continue;
        }
        s.push_back(c);
    }
    while (!s.empty() && (s.front() == '.' || s.front() == ' ')) s.erase(0, 1);
    while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();
    return s;
}

const char kIndexHtml[] = R"rawliteral(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
<title>Metalio E-INK4 · WiFi 传书</title>
<style>
  :root {
    --bg: #f8f9fa;
    --card-bg: #ffffff;
    --border: #212529;
    --text: #212529;
    --muted: #6c757d;
    --primary: #000000;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "PingFang SC", "Hiragino Sans GB", "Microsoft YaHei", sans-serif; }
  body { background: var(--bg); color: var(--text); padding: 16px; max-width: 680px; margin: 0 auto; line-height: 1.5; }
  header { text-align: center; padding: 24px 0 16px; border-bottom: 2px solid var(--border); margin-bottom: 20px; }
  h1 { font-size: 24px; font-weight: 700; letter-spacing: 0.5px; }
  .badge { display: inline-block; padding: 4px 10px; background: #000; color: #fff; border-radius: 20px; font-size: 12px; margin-top: 8px; }
  .card { background: var(--card-bg); border: 2px solid var(--border); border-radius: 12px; padding: 18px; margin-bottom: 20px; box-shadow: 3px 3px 0px rgba(0,0,0,0.1); }
  .card-title { font-size: 17px; font-weight: 700; margin-bottom: 12px; display: flex; justify-content: space-between; align-items: center; }
  .dropzone { border: 2px dashed var(--border); border-radius: 10px; padding: 36px 16px; text-align: center; cursor: pointer; transition: all 0.2s; background: #fafafa; }
  .dropzone.hover { background: #e9ecef; border-color: #000; }
  .dropzone-icon { font-size: 38px; margin-bottom: 8px; }
  .dropzone-tip { font-size: 14px; color: var(--muted); margin-top: 6px; }
  .btn { display: inline-block; background: #000; color: #fff; border: 2px solid #000; padding: 8px 18px; border-radius: 8px; font-size: 14px; font-weight: 600; cursor: pointer; text-decoration: none; }
  .btn:hover { background: #333; }
  .btn-sm { padding: 4px 10px; font-size: 12px; }
  .btn-outline { background: #fff; color: #000; }
  .btn-outline:hover { background: #eee; }
  .progress-wrap { margin-top: 14px; display: none; }
  .progress-bar { height: 10px; background: #e9ecef; border-radius: 5px; overflow: hidden; border: 1px solid var(--border); }
  .progress-fill { width: 0%; height: 100%; background: #000; transition: width 0.15s; }
  .status-msg { margin-top: 10px; font-size: 13px; text-align: center; font-weight: 600; }
  .book-item { display: flex; align-items: center; justify-content: space-between; padding: 10px 0; border-bottom: 1px solid #e9ecef; }
  .book-item:last-child { border-bottom: none; }
  .book-info { overflow: hidden; text-overflow: ellipsis; white-space: nowrap; max-width: 65%; }
  .book-name { font-weight: 600; font-size: 15px; }
  .book-size { font-size: 12px; color: var(--muted); }
  .book-ops { display: flex; gap: 8px; }
  .storage-bar { font-size: 13px; color: var(--muted); margin-top: 8px; }
</style>
</head>
<body>
  <header>
    <h1>📖 Metalio E-INK4 · WiFi 传书</h1>
    <div class="badge">局域网极速直传 · 零内存占用</div>
  </header>

  <div class="card">
    <div class="card-title">
      <span>文件上传</span>
      <span style="font-size: 12px; color: var(--muted);">存入 SD 图书目录</span>
    </div>
    <div class="dropzone" id="dropzone" onclick="document.getElementById('fileInput').click()">
      <div class="dropzone-icon">📥</div>
      <div style="font-weight: 700; font-size: 16px;">点击选择 或 拖拽电子书到此处</div>
      <div class="dropzone-tip">支持 .txt / .epub / .ebook / .pdf 等格式</div>
    </div>
    <input type="file" id="fileInput" multiple accept=".txt,.epub,.ebook,.pdf" style="display:none;" onchange="handleFiles(this.files)">
    <div class="progress-wrap" id="progressWrap">
      <div class="progress-bar"><div class="progress-fill" id="progressFill"></div></div>
      <div class="status-msg" id="statusMsg">准备上传...</div>
    </div>
  </div>

  <div class="card">
    <div class="card-title">
      <span>📚 已存图书 (<span id="bookCount">0</span>)</span>
      <button class="btn btn-outline btn-sm" onclick="loadBooks()">刷新列表</button>
    </div>
    <div id="bookList">正在加载图书列表...</div>
    <div class="storage-bar" id="storageInfo"></div>
  </div>

<script>
  const dropzone = document.getElementById('dropzone');
  const progressWrap = document.getElementById('progressWrap');
  const progressFill = document.getElementById('progressFill');
  const statusMsg = document.getElementById('statusMsg');

  ['dragenter', 'dragover'].forEach(n => {
    dropzone.addEventListener(n, e => { e.preventDefault(); dropzone.classList.add('hover'); });
  });
  ['dragleave', 'drop'].forEach(n => {
    dropzone.addEventListener(n, e => { e.preventDefault(); dropzone.classList.remove('hover'); });
  });
  dropzone.addEventListener('drop', e => {
    if (e.dataTransfer && e.dataTransfer.files) handleFiles(e.dataTransfer.files);
  });

  async function handleFiles(files) {
    if (!files || files.length === 0) return;
    progressWrap.style.display = 'block';
    for (let i = 0; i < files.length; i++) {
      const file = files[i];
      await uploadSingle(file, i + 1, files.length);
    }
    setTimeout(() => { progressWrap.style.display = 'none'; }, 2500);
    loadBooks();
  }

  function uploadSingle(file, idx, total) {
    return new Promise((resolve) => {
      statusMsg.innerText = `正在传输 (${idx}/${total}): ${file.name}`;
      progressFill.style.width = '0%';
      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/upload');
      xhr.setRequestHeader('X-Filename', encodeURIComponent(file.name));
      xhr.upload.onprogress = (e) => {
        if (e.lengthComputable) {
          const p = Math.round((e.loaded / e.total) * 100);
          progressFill.style.width = p + '%';
        }
      };
      xhr.onload = () => {
        if (xhr.status === 200) {
          statusMsg.innerText = `✓ 传输成功: ${file.name}`;
        } else {
          statusMsg.innerText = `✗ 上传失败: ${xhr.statusText || '错误'}`;
        }
        resolve();
      };
      xhr.onerror = () => {
        statusMsg.innerText = `✗ 网络错误: ${file.name}`;
        resolve();
      };
      xhr.send(file);
    });
  }

  function formatSize(b) {
    if (b >= 1024 * 1024) return (b / (1024 * 1024)).toFixed(1) + ' MB';
    if (b >= 1024) return (b / 1024).toFixed(1) + ' KB';
    return b + ' B';
  }

  async function loadBooks() {
    try {
      const res = await fetch('/api/books');
      const data = await res.json();
      document.getElementById('bookCount').innerText = data.books ? data.books.length : 0;
      const listEl = document.getElementById('bookList');
      if (!data.books || data.books.length === 0) {
        listEl.innerHTML = '<div style="color:var(--muted);text-align:center;padding:16px;">暂无图书，请在上方上传</div>';
      } else {
        listEl.innerHTML = data.books.map(b => `
          <div class="book-item">
            <div class="book-info">
              <div class="book-name" title="${b.name}">${b.name}</div>
              <div class="book-size">${formatSize(b.size)} · ${b.date || ''}</div>
            </div>
            <div class="book-ops">
              <a class="btn btn-outline btn-sm" href="/api/download?file=${encodeURIComponent(b.name)}" download>下载</a>
              <button class="btn btn-outline btn-sm" style="color:#d9534f;border-color:#d9534f;" onclick="deleteBook('${encodeURIComponent(b.name)}')">删除</button>
            </div>
          </div>
        `).join('');
      }
      if (data.free_mb !== undefined && data.total_mb !== undefined) {
        document.getElementById('storageInfo').innerText = `SD 卡剩余容量: ${(data.free_mb / 1024).toFixed(1)} GB / ${(data.total_mb / 1024).toFixed(1)} GB`;
      }
    } catch (e) {
      document.getElementById('bookList').innerText = '加载图书失败: ' + e;
    }
  }

  async function deleteBook(name) {
    const raw = decodeURIComponent(name);
    if (!confirm('确定要删除图书「' + raw + '」吗？')) return;
    try {
      const res = await fetch('/api/delete?file=' + name, { method: 'POST' });
      if (res.ok) {
        loadBooks();
      } else {
        alert('删除失败');
      }
    } catch(e) {
      alert('网络错误: ' + e);
    }
  }

  loadBooks();
</script>
</body>
</html>)rawliteral";

// HTTP Handler: GET /
esp_err_t HandleGetIndex(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, kIndexHtml, HTTPD_RESP_USE_STRLEN);
}

// HTTP Handler: POST /upload
esp_err_t HandlePostUpload(httpd_req_t* req) {
    char fn_hdr[256] = {};
    if (httpd_req_get_hdr_value_str(req, "X-Filename", fn_hdr, sizeof(fn_hdr)) != ESP_OK) {
        ESP_LOGW(TAG, "upload header X-Filename missing");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing X-Filename header");
        return ESP_FAIL;
    }

    std::string decoded = UrlDecode(fn_hdr);
    std::string clean_name = SanitizeFilename(decoded);
    if (clean_name.empty()) {
        ESP_LOGW(TAG, "invalid clean filename");
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid filename");
        return ESP_FAIL;
    }

    const char* books_dir = WifiTransferService::GetBooksDir();
    mkdir(SD_APP_ROOT, 0755);
    mkdir(books_dir, 0755);

    std::string target_path = std::string(books_dir) + "/" + clean_name;
    ESP_LOGI(TAG, "saving upload to %s, total size %d", target_path.c_str(), req->content_len);

    FILE* fp = fopen(target_path.c_str(), "wb");
    if (fp == nullptr) {
        ESP_LOGE(TAG, "failed to open %s for write", target_path.c_str());
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to open destination file on SD");
        return ESP_FAIL;
    }

    char chunk[4096];
    size_t total_recv = 0;
    int remaining = req->content_len;
    while (remaining > 0) {
        int to_read = (remaining < static_cast<int>(sizeof(chunk))) ? remaining : static_cast<int>(sizeof(chunk));
        int ret = httpd_req_recv(req, chunk, to_read);
        if (ret <= 0) {
            if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;
            }
            ESP_LOGE(TAG, "httpd_req_recv failed: %d", ret);
            fclose(fp);
            unlink(target_path.c_str());
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Upload stream interrupted");
            return ESP_FAIL;
        }
        fwrite(chunk, 1, ret, fp);
        total_recv += ret;
        remaining -= ret;
    }
    fclose(fp);

    reader::InvalidateBookLibraryCache();

    auto& svc = WifiTransferService::GetInstance();
    if (svc.IsRunning()) {
        UploadEvent ev{clean_name, total_recv, true, "上传成功"};
        svc.NotifyUpload(ev);
    }

    httpd_resp_set_type(req, "application/json");
    cJSON* root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "success", true);
    cJSON_AddStringToObject(root, "filename", clean_name.c_str());
    cJSON_AddNumberToObject(root, "size", total_recv);
    char* json_str = cJSON_PrintUnformatted(root);
    esp_err_t res = httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return res;
}

// HTTP Handler: GET /api/books
esp_err_t HandleGetBooks(httpd_req_t* req) {
    cJSON* root = cJSON_CreateObject();
    cJSON* arr = cJSON_AddArrayToObject(root, "books");

    const char* dir_path = WifiTransferService::GetBooksDir();
    DIR* d = opendir(dir_path);
    if (d != nullptr) {
        dirent* ent = nullptr;
        while ((ent = readdir(d)) != nullptr) {
            if (ent->d_name[0] == '.') continue;
            std::string path = std::string(dir_path) + "/" + ent->d_name;
            struct stat st = {};
            if (stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                std::string name = ent->d_name;
                // 忽略旁边缓存索引文件
                if (name.size() > 4 && (name.rfind(".pos") == name.size() - 4 ||
                                        name.rfind(".idx") == name.size() - 4 ||
                                        name.rfind(".tmp") == name.size() - 4)) {
                    continue;
                }
                cJSON* item = cJSON_CreateObject();
                cJSON_AddStringToObject(item, "name", ent->d_name);
                cJSON_AddNumberToObject(item, "size", st.st_size);
                struct tm tm_info = {};
                localtime_r(&st.st_mtime, &tm_info);
                char date_buf[64] = {};
                snprintf(date_buf, sizeof(date_buf), "%04d-%02d-%02d %02d:%02d",
                         tm_info.tm_year + 1900, tm_info.tm_mon + 1, tm_info.tm_mday,
                         tm_info.tm_hour, tm_info.tm_min);
                cJSON_AddStringToObject(item, "date", date_buf);
                cJSON_AddItemToArray(arr, item);
            }
        }
        closedir(d);
    }

    // SD 卡容量
    FATFS* fs = nullptr;
    DWORD free_clusters = 0;
    if (f_getfree("0:", &free_clusters, &fs) == FR_OK && fs != nullptr) {
        constexpr DWORD kSectorSize = 512;
        uint64_t total_bytes = static_cast<uint64_t>(fs->n_fatent - 2) * fs->csize * kSectorSize;
        uint64_t free_bytes = static_cast<uint64_t>(free_clusters) * fs->csize * kSectorSize;
        cJSON_AddNumberToObject(root, "total_mb", total_bytes / (1024 * 1024));
        cJSON_AddNumberToObject(root, "free_mb", free_bytes / (1024 * 1024));
    }

    char* json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    esp_err_t res = httpd_resp_send(req, json_str, HTTPD_RESP_USE_STRLEN);
    cJSON_free(json_str);
    cJSON_Delete(root);
    return res;
}

// HTTP Handler: POST /api/delete
esp_err_t HandlePostDelete(httpd_req_t* req) {
    char query[256] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query");
        return ESP_FAIL;
    }

    char fn[128] = {};
    if (httpd_query_key_value(query, "file", fn, sizeof(fn)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file parameter");
        return ESP_FAIL;
    }

    std::string clean = SanitizeFilename(UrlDecode(fn));
    if (clean.empty()) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid file name");
        return ESP_FAIL;
    }

    std::string full_path = std::string(WifiTransferService::GetBooksDir()) + "/" + clean;
    unlink(full_path.c_str());
    unlink((full_path + ".pos").c_str());
    unlink((full_path + ".idx").c_str());
    reader::InvalidateBookLibraryCache();

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, "{\"success\":true}", HTTPD_RESP_USE_STRLEN);
}

// HTTP Handler: GET /api/download
esp_err_t HandleGetDownload(httpd_req_t* req) {
    char query[256] = {};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing query");
        return ESP_FAIL;
    }

    char fn[128] = {};
    if (httpd_query_key_value(query, "file", fn, sizeof(fn)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing file parameter");
        return ESP_FAIL;
    }

    std::string clean = SanitizeFilename(UrlDecode(fn));
    std::string full_path = std::string(WifiTransferService::GetBooksDir()) + "/" + clean;
    FILE* fp = fopen(full_path.c_str(), "rb");
    if (fp == nullptr) {
        httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "File not found");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/octet-stream");
    char disp[256];
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", clean.c_str());
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    char chunk[4096];
    size_t n = 0;
    while ((n = fread(chunk, 1, sizeof(chunk), fp)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, n) != ESP_OK) {
            fclose(fp);
            return ESP_FAIL;
        }
    }
    fclose(fp);
    httpd_resp_send_chunk(req, nullptr, 0);
    return ESP_OK;
}

}  // namespace

WifiTransferService& WifiTransferService::GetInstance() {
    static WifiTransferService instance;
    return instance;
}

const char* WifiTransferService::GetBooksDir() {
    return SD_PATH_BOOKS;
}

std::string WifiTransferService::GetLocalIp() const {
    esp_netif_t* netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    if (netif != nullptr && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
        char ip_str[32] = {};
        esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
        return std::string(ip_str);
    }
    return {};
}

std::string WifiTransferService::GetServerUrl() const {
    std::string ip = GetLocalIp();
    if (ip.empty()) return {};
    char buf[64];
    snprintf(buf, sizeof(buf), "http://%s:%u/", ip.c_str(), port_);
    return std::string(buf);
}

bool WifiTransferService::Start(uint16_t port) {
    if (server_ != nullptr) {
        ESP_LOGI(TAG, "server already running");
        return true;
    }

    port_ = port;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = port_;
    config.ctrl_port = port_ + 1;
    config.max_uri_handlers = 8;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    httpd_handle_t handle = nullptr;
    esp_err_t err = httpd_start(&handle, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return false;
    }
    server_ = handle;

    // 注册 URI
    httpd_uri_t uri_index = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = HandleGetIndex,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(handle, &uri_index);

    httpd_uri_t uri_upload = {
        .uri = "/upload",
        .method = HTTP_POST,
        .handler = HandlePostUpload,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(handle, &uri_upload);

    httpd_uri_t uri_books = {
        .uri = "/api/books",
        .method = HTTP_GET,
        .handler = HandleGetBooks,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(handle, &uri_books);

    httpd_uri_t uri_delete = {
        .uri = "/api/delete",
        .method = HTTP_POST,
        .handler = HandlePostDelete,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(handle, &uri_delete);

    httpd_uri_t uri_download = {
        .uri = "/api/download",
        .method = HTTP_GET,
        .handler = HandleGetDownload,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(handle, &uri_download);

    ESP_LOGI(TAG, "WiFi Transfer server started at port %u", port_);
    return true;
}

void WifiTransferService::Stop() {
    if (server_ != nullptr) {
        httpd_stop(static_cast<httpd_handle_t>(server_));
        server_ = nullptr;
        ESP_LOGI(TAG, "WiFi Transfer server stopped");
    }
}

}  // namespace wifi_transfer
