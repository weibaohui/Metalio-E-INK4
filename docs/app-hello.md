# Hello（hello）

动态应用**标准开发模板**：展示 Bundle 结构、状态栏挂接、虚拟键、沙箱持久化计数器的最小可运行样例。新应用都从拷它开始。

![图标](../tools/hello_app/icon.png)

## 它演示了什么

- **Bundle 结构**：`manifest.json` + `icon.png` + `lib/app.so` 的标准打包形态
- **系统桥接**：`dynapp_screen_status_bar` 挂状态栏、`dynapp_vk_attach` 挂虚拟键、`dynapp_screen_go_home` 安全回桌面
- **沙箱持久化**：每次启动读 `data/launch_count.txt` 计数 +1 写回——重装应用计数不丢（`data/` 绝不覆盖机制的最直观证明）
- **生命周期**：`LV_EVENT_DELETE` 回调清理自建资源的标准写法

## 界面

标题「Hello Metalio .app」+ 三行说明：Bundle 路径、Data 目录、Launch Count（启动次数）。

## 用它起步

```bash
# 1. 拷模板
cp -r tools/hello_app tools/my_app

# 2. 改 manifest.json 的 id / name，改源码

# 3. 编译打包 + WiFi 热部署到真机
./tools/package_app.sh tools/my_app --upload <设备IP> --pin <PIN>
```

完整开发规范、沙箱 API、云能力（TTS / 异步 HTTP）与避坑清单见 [`docs/dynamic-apps.md`](dynamic-apps.md) 与 [`docs/app-store.md`](app-store.md)。
