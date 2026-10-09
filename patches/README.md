# managed_components 本地补丁

`idf.py update-components` / 重新解析依赖会覆盖 managed_components,覆盖后**必须重放**本目录记录的补丁。

## 1. esp_lvgl_adapter 0.6.3 — mono/diff 桥区域钳制(防死机)

文件:`managed_components/espressif__esp_lvgl_adapter/src/display/bridge/v9/lvgl_bridge_v9.c`

**背景**:2026-10-09 两次整机死机的根因。该文件对 LVGL flush/repair 区域**无任何边界校验**:

- `display_bridge_v9_prepare_mono`(~840 行):双重像素循环直接跑 `area->y1..y2`,异常几何
  (NULL 控件/负尺寸产生的越界 area)时循环数十亿次 → 整机假死 90 秒(OpenOCD PC 采样定位)。
- `copy_diff_repair_from_front_to_back`(~3010 行):`copy_w_px = (size_t)(rect.x2 - rect.x1 + 1)`
  对翻转/越界 rect 强转成天文数字 → memcpy 在 PSRAM 帧缓冲上越界 → Cache/存储异常
  (EXCCAUSE=7),且 panic 处理器自身随后卡死(无打印、无 coredump)。

**补丁内容**(两处,均已带 `PATCH(metalio)` 注释):
1. `prepare_mono`:memset 后新增 `lv_area_t safe` 钳制到 [0,log_w)×[0,log_h),空区域直接 return true。
2. `copy_diff_repair_from_front_to_back`:per-rect 循环开头钳制到 [0,hor_res)×[0,ver_res),翻转矩形 continue。

**验证补丁存在**:

```bash
grep -c "PATCH(metalio)" managed_components/espressif__esp_lvgl_adapter/src/display/bridge/v9/lvgl_bridge_v9.c
# 期望输出:2
```

被覆盖时:按上面的位置说明重放(代码块见 git 历史 patches/README.md 首次提交)。
