#pragma once

namespace wifi_transfer {

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

  ['dragenter', 'dragover', 'dragleave', 'drop'].forEach(ev => {
    dropzone.addEventListener(ev, e => { e.preventDefault(); e.stopPropagation(); }, false);
  });
  ['dragenter', 'dragover'].forEach(ev => {
    dropzone.addEventListener(ev, () => dropzone.classList.add('hover'), false);
  });
  ['dragleave', 'drop'].forEach(ev => {
    dropzone.addEventListener(ev, () => dropzone.classList.remove('hover'), false);
  });
  dropzone.addEventListener('drop', e => {
    const files = e.dataTransfer.files;
    if (files.length > 0) handleFiles(files);
  });

  async function handleFiles(files) {
    if (!files || files.length === 0) return;
    progressWrap.style.display = 'block';
    for (let i = 0; i < files.length; i++) {
      await uploadFile(files[i], i + 1, files.length);
    }
    setTimeout(() => {
      progressWrap.style.display = 'none';
      progressFill.style.width = '0%';
      loadBooks();
    }, 1200);
  }

  function uploadFile(file, cur, total) {
    return new Promise((resolve) => {
      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/upload', true);
      xhr.setRequestHeader('X-Filename', encodeURIComponent(file.name));
      xhr.upload.onprogress = (e) => {
        if (e.lengthComputable) {
          const pct = Math.round((e.loaded / e.total) * 100);
          progressFill.style.width = pct + '%';
          statusMsg.innerText = `正在上传 (${cur}/${total}) ${file.name} - ${pct}%`;
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

}  // namespace wifi_transfer
