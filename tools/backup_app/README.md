# 备份与恢复 (Backup & Restore) 架构与使用说明

本应用基于**全新三层备份恢复模型**构建，彻底摒弃旧版单文件 JSON 混杂设计的历史包袱，实现**系统底层配置、应用持久化状态与应用本体**的解耦与多层级快照管理。

---

## 1. 核心架构：三层快照模型

| 层级 | 覆盖范围 | 存储形态 | 典型用途 |
| :--- | :--- | :--- | :--- |
| **Layer 1: 系统配置 (System Config)** | Wi-Fi 凭证、系统参数、显示休眠、日志等级、电源与升级策略、MCP 服务配置等 | NVS 核心系统命名空间 (`wifi`, `settings`, `syslog`, `dbg`, `power`, `mcp` 等) -> `system_config.json` | 换设备配置无缝迁移、固件重刷前保存网络与偏好设置 |
| **Layer 2: 应用数据 (App Data & State)** | 各动态应用的 NVS 配置参数 + SD 卡私有数据沙箱文件 | NVS 应用命名空间 (`dynapp`, `apps`, `calendar`, `radio` 等) -> `app_config.json`<br>应用沙箱 (`apps/<app>.app/data/`) -> `app_data/` 目录 | 备份便签、记账、收音机自定义电台与收藏、日历日程等私有数据 |
| **Layer 3: 应用包本体 (App Bundles)** | 动态应用安装目录中的应用清单、图标、说明文档及共享库 | `apps/<app>.app/` (`manifest.json`, `icon.png`, `README.md`, `lib/app.so`) -> `app_bundles/` | 完整系统离线克隆、换卡一键装回全部应用 |

> **安全机制**：备份引擎在抓取 NVS 时会自动排除 `phy`（射频校准数据）与 `nvs.net80211`（Wi-Fi 底层驱动内部状态），避免设备间迁移导致射频硬件校准失效或底层驱动冲突。

---

## 2. SD 卡存储结构规范

所有备份快照存放在 SD 卡固定目录中：
`/sdcard/metalio/e-ink/backup/<时间戳>_<类型>/`

快照目录内部采用标准分层结构：

```text
/sdcard/metalio/e-ink/backup/
└── 20261018_123000_full/              # 快照根目录（以时间戳及类型命名）
    ├── backup_meta.json                # 快照元数据（时间、版本、设备信息、各层项数）
    ├── system_config.json              # Layer 1: 系统 NVS 结构化键值镜像
    ├── app_config.json                 # Layer 2: 应用 NVS 结构化键值镜像
    ├── app_data/                       # Layer 2: 应用私有数据沙箱镜像
    │   ├── radio/
    │   │   └── stations.json
    │   ├── calendar/
    │   │   └── events.db
    │   └── notes/
    │       └── notes.json
    └── app_bundles/                    # Layer 3: 动态应用包本体克隆
        ├── radio.app/
        │   ├── manifest.json
        │   ├── icon.png
        │   ├── README.md
        │   └── lib/app.so
        └── calendar.app/
            ├── manifest.json
            └── lib/app.so
```

### 元数据定义 (`backup_meta.json`)
```json
{
  "version": 2,
  "type": 0,
  "type_str": "full",
  "created_at": 1792305000,
  "created_str": "2026-10-18 12:30:00",
  "device_model": "Metalio-E-INK4",
  "firmware_version": "1.0.0",
  "system_entries": 18,
  "app_entries": 42,
  "app_count": 8,
  "total_size_bytes": 1048576
}
```

---

## 3. 三种备份模式

在应用主界面顶部提供了三种快捷备份触发操作：

1. **[★ 完整快照] (Full Snapshot)**
   - 抓取全部系统配置、全部应用 NVS、全部应用私有数据沙箱、全部动态应用包。
   - 适合：整机克隆、刷机重置前完整保留、跨 TF 卡完整迁移。
2. **[系统配置] (System Only)**
   - 仅备份系统核心参数与 Wi-Fi 凭证，生成轻量级配置包。
   - 适合：仅希望备份 Wi-Fi 与全局系统偏好，不需要备份应用内部数据。
3. **[应用数据] (App Data Only)**
   - 仅备份应用相关 NVS 偏好与所有应用在 `data/` 下的文件。
   - 适合：日常业务数据周期性归档，且不污染或覆盖目标设备的硬件系统配置。

---

## 4. 细粒度恢复与安全检查

点击任意已存在的快照项，将弹出精细化恢复弹窗：

- **恢复全部 (Full Restore)**：按快照所包含的所有内容完整复原。
- **仅系统配置 (System Only)**：只恢复 Wi-Fi 及系统设置（恢复后将自动提示或触发设备安全重启，确保 NVS 配置生效）。
- **仅应用数据 (App Data Only)**：只复原应用的 NVS 配置与私有沙箱文件，不触碰系统 Wi-Fi 等全局硬件设置。
- **删除快照 (Delete)**：安全递归删除 SD 卡中的该快照目录及内部全部文件。

---

## 5. 常见问题与注意事项

1. **TF 卡可用空间检测**：
   - 备份前界面仪表板会实时显示当前 TF 卡的剩余可用容量。执行完整快照前请确保剩余空间大于 10MB。
2. **系统重启要求**：
   - 若恢复了包含系统 Wi-Fi 或底层参数的配置，建议按屏幕提示重启设备以让 ESP-IDF 系统网络协议栈重新加载。
3. **旧版单一 JSON 备份文件迁移**：
   - 本版本已全面转向规范的目录式分层快照。旧版备份请勿直接导入；建议在升级到当前固件后直接执行一次 **[★ 完整快照]** 建立全新基础基线。
