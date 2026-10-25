# hello_app —— 动态应用（.app Bundle）标准参考示范

本工程是 Metalio E-INK 动态应用（.app Bundle）的标准开发模板，展示了现代动态应用的核心规范：
1. **.app Bundle 打包结构**与 `manifest.json` 元数据规范
2. **系统状态栏**（时钟/电量/WiFi）与**虚拟按键**挂接
3. **沙箱数据持久化**（`dynapp_fopen_data`），演示启动计数器在 `data/launch_count.txt` 中的存取
4. 退出生命周期与资源清理

---

## 1. 目录结构

```text
tools/hello_app/
├── main/
│   └── hello_main.c         # 纯 C + LVGL 源码
├── pkg/
│   └── hello.app/           # 生成的 Bundle 包
│       ├── manifest.json    # 元数据清单
│       ├── app.so           # 编译出的动态库
│       ├── icon.png         # 70x70 图标
│       └── data/            # 运行时私有持久化目录（重装不覆盖）
│           └── launch_count.txt
├── build_so.sh              # 底层编译脚本
└── README.md
```

---

## 2. 快速开发与一键部署

在工程根目录下执行统一打包工具 [`tools/package_app.sh`](file:///Users/weibh/Desktop/Metalio-E-INK4/tools/package_app.sh)：

### 本地打包
```bash
./tools/package_app.sh tools/hello_app
```
产物将生成在 `tools/hello_app/pkg/hello.app/` 目录下。

### 通过 Wi-Fi 直接热部署到真机
设备开机连上 Wi-Fi 后，执行：
```bash
./tools/package_app.sh tools/hello_app --upload <设备IP> --pin <PIN>
```
- 无需插拔 SD 卡，无需连接 USB 线，无需编译整机固件。
- 秒级上传并热重载，桌面即时出现应用图标。

---

## 3. 开发规范核心点

1. **入口函数**：导出 `void metalio_app_entry(metalio_app_desc_t *out)`，纯填充元数据，不得产生副作用。
2. **生命周期清理**：在屏幕的 `LV_EVENT_DELETE` 回调函数中释放所有自建定时器（`lv_timer` / `esp_timer`）与任务。
3. **数据读写规范**：一律使用 `dynapp_fopen_data("xxx", "r"/"w")`，严禁硬编码绝对路径。
4. **返回桌面**：调用 `dynapp_screen_go_home()`。

详细架构设计与 API 参考手册请阅读：[docs/dynamic-apps.md](file:///Users/weibh/Desktop/Metalio-E-INK4/docs/dynamic-apps.md)。
