---
name: eink-screen-automation
description: >-
  Automate visual inspection and touch interaction with the Metalio-E-INK4 device.
  Use this skill whenever you need to take screenshots, inspect UI elements on the
  e-ink screen, simulate touch clicks or long-presses, navigate apps, or debug GUI issues.
---

# Metalio-E-INK4 Screen Automation & GUI Debugging Skill

This skill guides you through the full closed-loop visual inspection and touch interaction workflow for the Metalio-E-INK4 device.

---

## 1. Core Architecture & Coordinate System

- **Display Panel**: 3.97" 1bpp monochrome e-ink (SSD1677), logical portrait viewport $480 \times 800$.
- **Screen Coordinate Space**:
  - Horizontal ($X$): $0 \le x \le 479$ (left to right)
  - Vertical ($Y$): $0 \le y \le 799$ (top to bottom)
- **Cover Glass Virtual Touch Keys (Under Screen, $Y = 900$)**:
  - `vk_home` (返回/首页): $x = 80, y = 900$ (短按返回上一层/桌面，长按500ms强制回首页)
  - `vk_next` (下翻/右移): $x = 240, y = 900$
  - `vk_prev` (上翻/左移): $x = 400, y = 900$

---

## 2. Step 1: Capture & Inspect Screen (PNG)

Use the built-in script `tools/screenshot.sh` to grab the current screen state. It automatically handles network fetching, 90° clockwise rotation, and conversion to ultra-compact PNG (~10-20KB):

```bash
./tools/screenshot.sh
```

- Default output: `/tmp/device_screenshot_upright.png`
- View the screenshot with `view_file` to visually verify widgets, labels, dialogs, and their exact coordinates.

```bash
# Custom output path or target device
./tools/screenshot.sh /tmp/custom_screen.png --ip <设备IP> --pin <PIN>
```

---

## 3. Step 2: Simulate Touch Clicks

Use `tools/click.sh` to inject a physical touch tap into the device's LVGL input queue. This triggers genuine LVGL hit-testing, visual pressed styles, haptic vibration feedback, and `LV_EVENT_CLICKED` handlers:

```bash
# Usage: ./tools/click.sh <x> <y> [duration_ms] [--ip <ip>] [--pin <pin>]

# 1. Normal click (default 60ms)
./tools/click.sh 240 502

# 2. Long press (e.g. 600ms)
./tools/click.sh 80 900 600

# 3. Virtual Home key click (return to previous/desktop)
./tools/click.sh 80 900
```

### Alternative REST API & MCP Calls
- **REST HTTP**:
  ```bash
  curl -s -X POST -H "Authorization: Bearer <PIN>" \
    "http://<设备IP>:8080/input/click?x=240&y=502&duration_ms=60"
  ```
- **MCP Tool**:
  Call `self.input.click` with `{"x": 240, "y": 502, "duration_ms": 60}`.

---

## 4. UI Coordinate Reference Cheatsheet

### A. Desktop (Home Screen) 3x3 App Grid
Grid starts below the top status bar ($y > 42$):
- **Column Centers ($X$)**:
  - Left Column: $x \approx 96$
  - Middle Column: $x \approx 240$
  - Right Column: $x \approx 384$
- **Row Centers ($Y$)**:
  - Row 0 (Top): $y \approx 330$ (阅读, 应用商店, 系统升级)
  - Row 1 (Mid): $y \approx 502$ (电源管理, 设置, 备份恢复)
  - Row 2 (Bot): $y \approx 674$ (日历, 2048, GLM用量)

### B. Standard Bottom Button Bar (4 Buttons Layout)
Many dynamic apps (e.g., Tetris, Radio) use a 4-button bottom row ($y \approx 762$):
- Button 1 (Leftmost): $x \approx 73$
- Button 2 (Mid-Left): $x \approx 184$
- Button 3 (Mid-Right / Exit): $x \approx 295$
- Button 4 (Rightmost): $x \approx 406$

### C. Modal Dialogs / Message Boxes
- Center Confirmation / OK / Dismiss: $x \approx 240, y \approx 480$.

---

## 5. Full Closed-Loop Debugging Recipe

When debugging or testing any GUI feature:
1. **Capture**: Run `./tools/screenshot.sh`
2. **Inspect**: Call `view_file` on `/tmp/device_screenshot_upright.png` to analyze layout, text, or errors.
3. **Interact**: Call `./tools/click.sh <x> <y>` to click buttons or options.
4. **Wait**: Sleep 0.5s ~ 1s for LVGL render and partial e-ink refresh.
5. **Verify**: Run `./tools/screenshot.sh` again to inspect the new screen state.
6. **Exit / Reset**: Run `./tools/click.sh 80 900` to trigger `vk_home` and return to desktop.
