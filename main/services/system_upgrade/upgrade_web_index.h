#pragma once

namespace system_upgrade {

const char kUpgradeWebHtml[] = R"rawliteral(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=no">
<title>Metalio E-INK4 · 系统固件无线升级</title>
<style>
  :root {
    --bg: #f5f6f8;
    --card-bg: #ffffff;
    --border: #111111;
    --text: #111111;
    --muted: #666666;
    --primary: #000000;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "PingFang SC", "Microsoft YaHei", sans-serif; }
  body { background: var(--bg); color: var(--text); padding: 16px; max-width: 620px; margin: 0 auto; line-height: 1.5; }
  header { text-align: center; padding: 24px 0 16px; border-bottom: 2px solid var(--border); margin-bottom: 20px; }
  h1 { font-size: 22px; font-weight: 700; letter-spacing: 0.5px; }
  .badge { display: inline-block; padding: 4px 12px; background: #000; color: #fff; border-radius: 20px; font-size: 13px; margin-top: 8px; font-weight: 500; }
  .card { background: var(--card-bg); border: 2px solid var(--border); border-radius: 12px; padding: 18px; margin-bottom: 20px; box-shadow: 3px 3px 0px rgba(0,0,0,0.08); }
  .card-title { font-size: 16px; font-weight: 700; margin-bottom: 12px; display: flex; justify-content: space-between; align-items: center; }
  .info-row { display: flex; justify-content: space-between; padding: 7px 0; border-bottom: 1px dashed #e2e4e8; font-size: 14px; }
  .info-row:last-child { border-bottom: none; }
  .info-label { color: var(--muted); }
  .info-val { font-weight: 600; }
  .dropzone { border: 2px dashed var(--border); border-radius: 10px; padding: 36px 16px; text-align: center; cursor: pointer; transition: all 0.2s; background: #fafafa; }
  .dropzone.hover { background: #e9ecef; border-color: #000; }
  .dropzone-icon { font-size: 38px; margin-bottom: 8px; }
  .dropzone-tip { font-size: 14px; color: var(--muted); margin-top: 6px; }
  .btn { display: inline-block; background: #000; color: #fff; border: 2px solid #000; padding: 10px 22px; border-radius: 8px; font-size: 15px; font-weight: 600; cursor: pointer; text-decoration: none; width: 100%; text-align: center; margin-top: 14px; }
  .btn:hover { background: #222; }
  .btn:disabled { background: #888; border-color: #888; cursor: not-allowed; }
  .progress-wrap { margin-top: 16px; display: none; }
  .progress-bar { height: 14px; background: #e9ecef; border-radius: 7px; overflow: hidden; border: 1px solid var(--border); }
  .progress-fill { width: 0%; height: 100%; background: #000; transition: width 0.15s; }
  .status-msg { margin-top: 12px; font-size: 14px; text-align: center; font-weight: 600; line-height: 1.6; }
  .warning-box { background: #fff8e6; border: 1px solid #ffd591; padding: 12px; border-radius: 8px; font-size: 13px; color: #ad6800; margin-top: 14px; }
</style>
</head>
<body>
  <header>
    <h1>⚡ Metalio E-INK4 · 固件无线升级</h1>
    <div class="badge">局域网 OTA 直刷 · 自动校验与引导切换</div>
  </header>

  <div class="card">
    <div class="card-title">
      <span>当前设备状态</span>
      <span style="font-size: 12px; color: var(--muted);">Running Info</span>
    </div>
    <div class="info-row">
      <span class="info-label">硬件型号</span>
      <span class="info-val">Metalio E-INK 4</span>
    </div>
    <div class="info-row">
      <span class="info-label">当前运行固件</span>
      <span class="info-val" id="curVer">加载中...</span>
    </div>
    <div class="info-row">
      <span class="info-label">当前活动分区</span>
      <span class="info-val" id="curPart">加载中...</span>
    </div>
    <div class="info-row">
      <span class="info-label">待写入升级分区</span>
      <span class="info-val" id="nextPart">加载中...</span>
    </div>
  </div>

  <div class="card">
    <div class="card-title">
      <span>选择固件文件 (.bin)</span>
      <span style="font-size: 12px; color: var(--muted);">ESP32 Firmware</span>
    </div>
    <div class="dropzone" id="dropzone" onclick="document.getElementById('fileInput').click()">
      <div class="dropzone-icon">📥</div>
      <div style="font-weight: 600; font-size: 16px;" id="fileSelectTip">点击或将固件拖拽至此处</div>
      <div class="dropzone-tip">支持 ESP-IDF 编译生成的 xiaozhi.bin 固件</div>
      <input type="file" id="fileInput" accept=".bin" style="display: none;" onchange="handleFileSelected(this.files)">
    </div>

    <button id="uploadBtn" class="btn" style="display:none;" onclick="startUpload()">开始烧录升级</button>

    <div class="progress-wrap" id="progWrap">
      <div class="progress-bar">
        <div class="progress-fill" id="progFill"></div>
      </div>
      <div class="status-msg" id="statusMsg">准备上传...</div>
    </div>

    <div class="warning-box">
      ⚠️ <b>升级须知：</b><br>
      1. 请保持设备电量充足（建议接驳电源），烧录过程中切勿断电或重启设备；<br>
      2. 固件上传后设备将边接收边校验 Magic 镜像头并写入 Flash，写入完毕将自动重启；<br>
      3. 系统配备双分区 A/B 容灾机制，若新固件无法启动将自动回退。
    </div>
  </div>

<script>
let selectedFile = null;

function fetchInfo() {
  fetch('/api/upgrade_info')
    .then(r => r.json())
    .then(d => {
      document.getElementById('curVer').innerText = (d.version || '未知') + ' (' + (d.build_date || '') + ' ' + (d.build_time || '') + ')';
      document.getElementById('curPart').innerText = d.running_partition || 'ota_0';
      document.getElementById('nextPart').innerText = d.next_partition || 'ota_1';
    })
    .catch(() => {
      document.getElementById('curVer').innerText = '获取失败';
    });
}
fetchInfo();

const dz = document.getElementById('dropzone');
['dragenter', 'dragover'].forEach(e => {
  dz.addEventListener(e, (ev) => { ev.preventDefault(); dz.classList.add('hover'); });
});
['dragleave', 'drop'].forEach(e => {
  dz.addEventListener(e, (ev) => { ev.preventDefault(); dz.classList.remove('hover'); });
});
dz.addEventListener('drop', (ev) => {
  if (ev.dataTransfer.files && ev.dataTransfer.files.length > 0) {
    handleFileSelected(ev.dataTransfer.files);
  }
});

function handleFileSelected(files) {
  if (!files || files.length === 0) return;
  const f = files[0];
  if (!f.name.toLowerCase().endsWith('.bin')) {
    alert('请选择以 .bin 结尾的固件文件！');
    return;
  }
  selectedFile = f;
  const sizeMb = (f.size / (1024 * 1024)).toFixed(2);
  document.getElementById('fileSelectTip').innerHTML = '已选择: <b>' + f.name + '</b> (' + sizeMb + ' MB)';
  document.getElementById('uploadBtn').style.display = 'block';
  document.getElementById('uploadBtn').innerText = '确认写入固件 (' + sizeMb + ' MB) 并重启';
}

function startUpload() {
  if (!selectedFile) return;
  const btn = document.getElementById('uploadBtn');
  btn.disabled = true;
  document.getElementById('progWrap').style.display = 'block';
  const fill = document.getElementById('progFill');
  const msg = document.getElementById('statusMsg');

  const xhr = new XMLHttpRequest();
  xhr.open('POST', '/upload_ota', true);
  xhr.setRequestHeader('X-Filename', encodeURIComponent(selectedFile.name));
  xhr.setRequestHeader('Content-Type', 'application/octet-stream');

  let startTime = Date.now();
  xhr.upload.onprogress = function(e) {
    if (e.lengthComputable) {
      const pct = Math.floor((e.loaded / e.total) * 100);
      fill.style.width = pct + '%';
      const elapsed = (Date.now() - startTime) / 1000;
      const speed = elapsed > 0 ? (e.loaded / 1024 / elapsed).toFixed(0) : 0;
      msg.innerHTML = '正在写入固件 Flash... <b>' + pct + '%</b> (' + speed + ' KB/s)';
    }
  };

  xhr.onload = function() {
    if (xhr.status === 200) {
      fill.style.width = '100%';
      msg.innerHTML = '🎉 <b>固件写入与校验成功！</b><br>设备正在切换引导分区并自动重启，请稍候...';
      btn.style.display = 'none';
    } else {
      btn.disabled = false;
      let errText = '升级失败 (' + xhr.status + ')';
      try {
        const res = JSON.parse(xhr.responseText);
        if (res.error) errText += ': ' + res.error;
      } catch(e) {}
      msg.innerHTML = '<span style="color:#cf1322;">❌ ' + errText + '</span>';
    }
  };

  xhr.onerror = function() {
    btn.disabled = false;
    msg.innerHTML = '<span style="color:#cf1322;">❌ 网络传输中断，升级未完成</span>';
  };

  xhr.send(selectedFile);
}
</script>
</body>
</html>
)rawliteral";

}  // namespace system_upgrade
