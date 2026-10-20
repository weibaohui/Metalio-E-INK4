#pragma once

namespace wifi_file {

// WiFi 传输 Web 端:传书(直传设备书库) + 全盘文件管理,双页签合一。
const char kFileManagerHtml[] = R"rawliteral(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
<title>Metalio E-INK4 · WiFi 传输</title>
<style>
  :root {
    --bg: #f8f9fa;
    --card: #ffffff;
    --border: #212529;
    --text: #212529;
    --muted: #6c757d;
    --primary: #000000;
    --danger: #dc3545;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "PingFang SC", "Microsoft YaHei", sans-serif; }
  body { background: var(--bg); color: var(--text); padding: 16px; max-width: 820px; margin: 0 auto; line-height: 1.5; }
  header { text-align: center; padding: 20px 0 16px; border-bottom: 2px solid var(--border); margin-bottom: 16px; }
  h1 { font-size: 22px; font-weight: 700; letter-spacing: 0.5px; }
  .badge { display: inline-block; padding: 3px 10px; background: #000; color: #fff; border-radius: 16px; font-size: 12px; margin-top: 6px; }
  .tabs { display: flex; gap: 0; border: 2px solid var(--border); border-radius: 10px; overflow: hidden; margin-bottom: 16px; background: var(--card); }
  .tab-btn { flex: 1; padding: 11px 8px; font-size: 15px; font-weight: 700; background: var(--card); color: var(--text); border: none; cursor: pointer; font-family: inherit; }
  .tab-btn + .tab-btn { border-left: 2px solid var(--border); }
  .tab-btn.active { background: #000; color: #fff; }
  .card { background: var(--card); border: 2px solid var(--border); border-radius: 12px; padding: 16px; margin-bottom: 16px; box-shadow: 2px 2px 0px rgba(0,0,0,0.08); }
  .toolbar { display: flex; flex-wrap: wrap; gap: 8px; justify-content: space-between; align-items: center; margin-bottom: 12px; }
  .btn { display: inline-flex; align-items: center; justify-content: center; background: #000; color: #fff; border: 2px solid #000; padding: 7px 14px; border-radius: 8px; font-size: 13px; font-weight: 600; cursor: pointer; text-decoration: none; transition: background 0.15s; }
  .btn:hover { background: #333; }
  .btn-outline { background: #fff; color: #000; }
  .btn-outline:hover { background: #eee; }
  .btn-danger { background: #fff; color: var(--danger); border-color: var(--danger); }
  .btn-danger:hover { background: var(--danger); color: #fff; }
  .btn-sm { padding: 4px 10px; font-size: 12px; }
  .breadcrumbs { display: flex; flex-wrap: wrap; align-items: center; gap: 6px; font-size: 14px; padding: 8px 12px; background: #f1f3f5; border-radius: 8px; border: 1px solid #ced4da; margin-bottom: 12px; }
  .breadcrumb-item { cursor: pointer; color: #000; font-weight: 600; text-decoration: underline; }
  .breadcrumb-item:hover { color: #555; }
  .breadcrumb-sep { color: var(--muted); font-size: 12px; }
  .breadcrumb-cur { color: var(--muted); font-weight: 500; text-decoration: none; cursor: default; }
  .dropzone { border: 2px dashed var(--border); border-radius: 10px; padding: 24px 16px; text-align: center; cursor: pointer; background: #fafafa; margin-bottom: 12px; transition: all 0.2s; }
  .dropzone.hover { background: #e9ecef; border-color: #000; }
  .dropzone-icon { font-size: 30px; margin-bottom: 4px; }
  .dropzone-tip { font-size: 13px; color: var(--muted); }
  .progress-wrap { margin: 12px 0; display: none; }
  .progress-bar { height: 10px; background: #e9ecef; border-radius: 5px; overflow: hidden; border: 1px solid var(--border); }
  .progress-fill { width: 0%; height: 100%; background: #000; transition: width 0.15s; }
  .progress-text { font-size: 12px; font-weight: 600; text-align: center; margin-top: 4px; }
  .file-table { width: 100%; border-collapse: collapse; }
  .file-table th { text-align: left; padding: 10px 8px; border-bottom: 2px solid var(--border); font-size: 13px; font-weight: 700; background: #f8f9fa; }
  .file-table td { padding: 10px 8px; border-bottom: 1px solid #e9ecef; font-size: 13px; vertical-align: middle; }
  .file-table tr:hover { background: #f8f9fa; }
  .file-name { display: flex; align-items: center; gap: 8px; font-weight: 600; cursor: pointer; color: #000; word-break: break-all; }
  .file-name:hover { text-decoration: underline; }
  .file-icon { font-size: 18px; flex-shrink: 0; }
  .file-size { color: var(--muted); font-size: 12px; white-space: nowrap; }
  .file-date { color: var(--muted); font-size: 12px; white-space: nowrap; }
  .file-actions { display: flex; gap: 6px; justify-content: flex-end; }
  .empty-tip { text-align: center; padding: 36px 12px; color: var(--muted); font-size: 14px; }
  .storage-bar { font-size: 13px; color: var(--muted); display: flex; justify-content: space-between; align-items: center; padding-top: 8px; border-top: 1px solid #e9ecef; }
</style>
</head>
<body>
  <header>
    <h1>📡 Metalio E-INK4 · WiFi 传输</h1>
    <div class="badge">传书入库 · 全盘文件管理</div>
  </header>

  <div class="tabs">
    <button class="tab-btn active" data-mode="books" onclick="switchMode('books')">📖 传书</button>
    <button class="tab-btn" data-mode="wallpaper" onclick="switchMode('wallpaper')">🖼️ 壁纸</button>
    <button class="tab-btn" data-mode="fs" onclick="switchMode('fs')">💾 文件管理</button>
  </div>

  <div class="card">
    <div class="breadcrumbs" id="breadcrumbs">
      <span class="breadcrumb-cur">书库</span>
    </div>

    <div class="toolbar">
      <div style="display: flex; gap: 8px;">
        <button class="btn btn-outline btn-sm" id="btnUp" onclick="goUp()">⬆ 返回上级</button>
        <button class="btn btn-outline btn-sm" id="btnMkdir" onclick="promptMkdir()">📁 新建文件夹</button>
      </div>
      <div>
        <button class="btn btn-sm" onclick="document.getElementById('fileInput').click()">📤 上传文件</button>
        <input type="file" id="fileInput" multiple style="display:none;" onchange="handleFileSelect(this.files)">
      </div>
    </div>

    <div class="dropzone" id="dropzone" onclick="document.getElementById('fileInput').click()">
      <div class="dropzone-icon" id="dzIcon">📂</div>
      <div style="font-weight:600; font-size:14px;" id="dzTitle">点击选择或拖放文件到此处</div>
      <div class="dropzone-tip" id="dzTip">传书到设备书库,传完立即可用阅读器打开</div>
    </div>

    <div class="progress-wrap" id="progressWrap">
      <div class="progress-bar"><div class="progress-fill" id="progressFill"></div></div>
      <div class="progress-text" id="progressText">准备上传...</div>
    </div>

    <div style="overflow-x: auto;">
      <table class="file-table">
        <thead>
          <tr>
            <th>名称</th>
            <th style="width: 90px;">大小</th>
            <th style="width: 140px;">修改日期</th>
            <th style="width: 110px; text-align: right;">操作</th>
          </tr>
        </thead>
        <tbody id="fileList">
          <tr><td colspan="4" class="empty-tip">加载中...</td></tr>
        </tbody>
      </table>
    </div>

    <div class="storage-bar" id="storageBar">
      <span id="storageInfo">正在获取存储空间...</span>
      <button class="btn btn-outline btn-sm" onclick="refreshCurrentDir()">刷新</button>
    </div>
  </div>

<script>
const BOOK_EXTS = ['txt', 'epub', 'ebook', 'pdf'];
const IMAGE_EXTS = ['png', 'jpg', 'jpeg', 'a2i1', 'bin'];

// 三个页签共用一套列表/上传逻辑,差异只在目标目录、允许的扩展名与文案
const MODE_CFG = {
  books: {
    dir: 'books', exts: BOOK_EXTS, icon: '📖',
    title: '点击选择或拖放书籍到此处(可多选)',
    tip: '传书到设备书库,传完立即可用阅读器打开 (.txt/.epub/.ebook/.pdf)',
    crumb: '📖 设备书库', empty: '书库暂无书籍,拖一本书进来试试'
  },
  wallpaper: {
    dir: 'wallpaper', exts: IMAGE_EXTS, icon: '🖼️',
    title: '点击选择或拖放图片到此处(可多选)',
    tip: '传壁纸到设备壁纸库,再打开设备「待机壁纸」应用挑选 (.png/.jpg/.a2i1)',
    crumb: '🖼️ 设备壁纸库', empty: '壁纸库暂无图片,拖一张进来试试'
  },
  fs: {
    dir: null, exts: null, icon: '📂',
    title: '点击选择或拖放文件到此处',
    tip: '支持任意文件,直传存入当前目录',
    crumb: null, empty: '此目录为空'
  }
};

let mode = 'books';               // 'books' 传书 | 'wallpaper' 壁纸 | 'fs' 全盘文件管理
let currentPath = '/';
let roots = { books: '/metalio/e-ink/books', wallpaper: '/metalio/e-ink/wallpaper' };

function cfg() { return MODE_CFG[mode]; }

function formatSize(bytes) {
  if (bytes === 0) return '0 B';
  const k = 1024;
  const sizes = ['B', 'KB', 'MB', 'GB'];
  const i = Math.floor(Math.log(bytes) / Math.log(k));
  return parseFloat((bytes / Math.pow(k, i)).toFixed(1)) + ' ' + sizes[i];
}

function getFileIcon(name, isDir) {
  if (isDir) return '📁';
  const ext = name.split('.').pop().toLowerCase();
  if (BOOK_EXTS.includes(ext)) return '📖';
  if (IMAGE_EXTS.includes(ext)) return '🖼️';
  if (['mp3', 'wav', 'ogg', 'aac', 'flac'].includes(ext)) return '🎵';
  if (['ttf', 'otf', 'woff'].includes(ext)) return '🔤';
  return '📄';
}

function escapeHtml(str) {
  return str.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;").replace(/'/g, "&#039;");
}

function isAllowedExt(name) {
  const exts = cfg().exts;
  if (!exts) return true;
  const ext = name.split('.').pop().toLowerCase();
  return exts.includes(ext);
}

// 读取设备上的图书库/壁纸库路径(服务端权威,页面不硬编码)
async function loadRoots() {
  try {
    const res = await fetch('/api/fs/roots');
    const data = await res.json();
    if (data && data.books) roots.books = data.books;
    if (data && data.wallpaper) roots.wallpaper = data.wallpaper;
  } catch (err) {
    /* 保持默认路径 */
  }
}

function switchMode(m) {
  mode = m;
  const c = cfg();
  document.querySelectorAll('.tab-btn').forEach(b => {
    b.classList.toggle('active', b.getAttribute('data-mode') === m);
  });

  // 传书/壁纸页签固定落在各自的库目录,目录类操作隐藏
  const fixed = !!c.dir;
  document.getElementById('btnUp').style.display = fixed ? 'none' : '';
  document.getElementById('btnMkdir').style.display = fixed ? 'none' : '';
  document.getElementById('fileInput').accept = c.exts ? c.exts.map(e => '.' + e).join(',') : '';

  document.getElementById('dzIcon').innerText = c.icon;
  document.getElementById('dzTitle').innerText = c.title;
  document.getElementById('dzTip').innerText = c.tip;

  loadDir(fixed ? roots[c.dir] : '/');
}

function renderBreadcrumbs(path) {
  const container = document.getElementById('breadcrumbs');
  container.innerHTML = '';

  const c = cfg();
  if (c.crumb) {
    const cur = document.createElement('span');
    cur.className = 'breadcrumb-cur';
    cur.innerText = c.crumb + ' (' + path + ')';
    container.appendChild(cur);
    return;
  }

  const rootSpan = document.createElement('span');
  rootSpan.className = path === '/' ? 'breadcrumb-cur' : 'breadcrumb-item';
  rootSpan.innerText = '根目录 (/)';
  rootSpan.onclick = () => loadDir('/');
  container.appendChild(rootSpan);

  if (path === '/') return;

  const parts = path.split('/').filter(p => p.length > 0);
  let accumulated = '';
  parts.forEach((part, index) => {
    const sep = document.createElement('span');
    sep.className = 'breadcrumb-sep';
    sep.innerText = '/';
    container.appendChild(sep);

    accumulated += '/' + part;
    const pPath = accumulated;
    const isLast = (index === parts.length - 1);

    const span = document.createElement('span');
    span.className = isLast ? 'breadcrumb-cur' : 'breadcrumb-item';
    span.innerText = part;
    if (!isLast) {
      span.onclick = () => loadDir(pPath);
    }
    container.appendChild(span);
  });
}

async function loadDir(path) {
  currentPath = path;
  renderBreadcrumbs(path);
  const tbody = document.getElementById('fileList');
  tbody.innerHTML = '<tr><td colspan="4" class="empty-tip">正在读取目录...</td></tr>';

  try {
    const res = await fetch('/api/fs/list?path=' + encodeURIComponent(path));
    if (!res.ok) throw new Error('读取目录失败');
    const data = await res.json();

    if (!data.items || data.items.length === 0) {
      tbody.innerHTML = '<tr><td colspan="4" class="empty-tip">' + cfg().empty + '</td></tr>';
    } else {
      tbody.innerHTML = '';
      data.items.forEach(item => {
        const tr = document.createElement('tr');

        // 名称
        const tdName = document.createElement('td');
        const nameDiv = document.createElement('div');
        nameDiv.className = 'file-name';
        nameDiv.innerHTML = `<span class="file-icon">${getFileIcon(item.name, item.is_dir)}</span><span>${escapeHtml(item.name)}</span>`;
        if (item.is_dir) {
          nameDiv.onclick = () => {
            const next = currentPath === '/' ? '/' + item.name : currentPath + '/' + item.name;
            loadDir(next);
          };
        } else {
          nameDiv.onclick = () => downloadFile(item.name);
        }
        tdName.appendChild(nameDiv);
        tr.appendChild(tdName);

        // 大小
        const tdSize = document.createElement('td');
        tdSize.className = 'file-size';
        tdSize.innerText = item.is_dir ? '--' : formatSize(item.size);
        tr.appendChild(tdSize);

        // 日期
        const tdDate = document.createElement('td');
        tdDate.className = 'file-date';
        tdDate.innerText = item.date || '--';
        tr.appendChild(tdDate);

        // 操作
        const tdActions = document.createElement('td');
        tdActions.className = 'file-actions';

        if (!item.is_dir) {
          const dlBtn = document.createElement('button');
          dlBtn.className = 'btn btn-outline btn-sm';
          dlBtn.innerText = '下载';
          dlBtn.onclick = () => downloadFile(item.name);
          tdActions.appendChild(dlBtn);
        }

        const delBtn = document.createElement('button');
        delBtn.className = 'btn btn-danger btn-sm';
        delBtn.innerText = '删除';
        delBtn.onclick = () => deleteItem(item.name, item.is_dir);
        tdActions.appendChild(delBtn);

        tr.appendChild(tdActions);
        tbody.appendChild(tr);
      });
    }

    if (data.total_mb !== undefined && data.free_mb !== undefined) {
      const usedMb = data.total_mb - data.free_mb;
      document.getElementById('storageInfo').innerText =
        `SD 空间: 已用 ${(usedMb/1024).toFixed(1)} GB / 共 ${(data.total_mb/1024).toFixed(1)} GB (可用 ${(data.free_mb/1024).toFixed(1)} GB)`;
    }
  } catch (err) {
    tbody.innerHTML = `<tr><td colspan="4" class="empty-tip" style="color:var(--danger)">加载失败: ${err.message}</td></tr>`;
  }
}

function goUp() {
  if (cfg().dir || currentPath === '/') return;
  const parts = currentPath.split('/').filter(p => p.length > 0);
  parts.pop();
  loadDir(parts.length === 0 ? '/' : '/' + parts.join('/'));
}

function refreshCurrentDir() {
  loadDir(currentPath);
}

function downloadFile(name) {
  const filePath = currentPath === '/' ? '/' + name : currentPath + '/' + name;
  window.open('/api/fs/download?path=' + encodeURIComponent(filePath), '_blank');
}

async function deleteItem(name, isDir) {
  const itemType = isDir ? '文件夹' : '文件';
  const target = currentPath === '/' ? '/' + name : currentPath + '/' + name;
  if (isDir) {
    if (!confirm(`确定要彻底删除文件夹【${target}】吗？\n\n该文件夹及其全部内容都会被递归删除,且无法恢复!`)) return;
  } else {
    if (!confirm(`确定要彻底删除此${itemType}【${name}】吗？`)) return;
  }

  try {
    const res = await fetch('/api/fs/delete?path=' + encodeURIComponent(target), { method: 'POST' });
    const data = await res.json();
    if (!data.success) throw new Error(data.message || '删除失败');
    refreshCurrentDir();
  } catch (err) {
    alert('删除失败: ' + err.message);
  }
}

async function promptMkdir() {
  if (cfg().dir) return;
  const name = prompt('请输入新文件夹名称：');
  if (!name || name.trim() === '') return;
  const cleanName = name.trim();
  const target = currentPath === '/' ? '/' + cleanName : currentPath + '/' + cleanName;

  try {
    const res = await fetch('/api/fs/mkdir?path=' + encodeURIComponent(target), { method: 'POST' });
    const data = await res.json();
    if (!data.success) throw new Error(data.message || '创建文件夹失败');
    refreshCurrentDir();
  } catch (err) {
    alert('创建文件夹失败: ' + err.message);
  }
}

// 批量上传管理
async function handleFileSelect(files) {
  if (!files || files.length === 0) return;

  let list = Array.from(files);
  const exts = cfg().exts;
  if (exts) {
    const skipped = list.filter(f => !isAllowedExt(f.name)).map(f => f.name);
    list = list.filter(f => isAllowedExt(f.name));
    if (skipped.length > 0) {
      alert('本页只接受 ' + exts.map(e => '.' + e).join('/') + ' 格式,已跳过:\n' + skipped.join('\n'));
    }
    if (list.length === 0) return;
  }

  const pWrap = document.getElementById('progressWrap');
  const pFill = document.getElementById('progressFill');
  const pText = document.getElementById('progressText');
  pWrap.style.display = 'block';

  for (let i = 0; i < list.length; ++i) {
    const f = list[i];
    pText.innerText = `正在上传 (${i+1}/${list.length}): ${f.name}`;
    pFill.style.width = '0%';

    try {
      await uploadSingleFile(f, (pct) => {
        pFill.style.width = pct + '%';
        pText.innerText = `正在上传 (${i+1}/${list.length}) ${pct}%: ${f.name}`;
      });
    } catch (err) {
      alert(`文件【${f.name}】上传失败: ${err.message}`);
    }
  }

  pText.innerText = '全部上传完成！';
  pFill.style.width = '100%';
  setTimeout(() => { pWrap.style.display = 'none'; }, 2000);
  refreshCurrentDir();
}

function uploadSingleFile(file, onProgress) {
  return new Promise((resolve, reject) => {
    const xhr = new XMLHttpRequest();
    const url = '/api/fs/upload?path=' + encodeURIComponent(currentPath);
    xhr.open('POST', url, true);
    xhr.setRequestHeader('X-Filename', encodeURIComponent(file.name));

    xhr.upload.onprogress = (e) => {
      if (e.lengthComputable) {
        const pct = Math.round((e.loaded / e.total) * 100);
        onProgress(pct);
      }
    };

    xhr.onload = () => {
      if (xhr.status >= 200 && xhr.status < 300) {
        resolve();
      } else {
        reject(new Error(xhr.responseText || 'HTTP ' + xhr.status));
      }
    };

    xhr.onerror = () => reject(new Error('网络中断'));
    xhr.send(file);
  });
}

// 拖拽支持
const dz = document.getElementById('dropzone');
['dragenter', 'dragover'].forEach(name => {
  dz.addEventListener(name, (e) => { e.preventDefault(); e.stopPropagation(); dz.classList.add('hover'); }, false);
});
['dragleave', 'drop'].forEach(name => {
  dz.addEventListener(name, (e) => { e.preventDefault(); e.stopPropagation(); dz.classList.remove('hover'); }, false);
});
dz.addEventListener('drop', (e) => {
  const dt = e.dataTransfer;
  if (dt && dt.files && dt.files.length > 0) {
    handleFileSelect(dt.files);
  }
});

// 初始化:先取图书库/壁纸库路径,再切到传书页签
window.onload = async () => {
  await loadRoots();
  switchMode('books');
};
</script>
</body>
</html>
)rawliteral";

}  // namespace wifi_file
