// tools/pomo_app/main/pomo_main.c
// 番茄钟 动态应用：专注/休息循环计时，适配墨水屏慢刷新特性。
//
// 设计要点（墨水屏 480x800 / 1bpp 单色）：
//   - **不跳秒**：墨水屏刷一次要 0.3~2 秒，每秒跳数会把屏幕刷成浆糊、
//     疯狂耗电还加速屏老化。所以主显示只给"剩余分钟"（向上取整），
//     下面一行静态给出「开始 → 结束」的真实钟点，排水条每分钟长一格。
//   - 断电/退出不丢进度：状态一变就立刻落盘，不等空闲定时器。
//   - 阶段循环：专注 -> 短休 -> 专注 -> 短休 -> 专注 -> 短休 -> 专注 -> 长休，
//     每 4 个番茄进一次长休。
//   - 统计：今日番茄数 + 专注分钟 + 近 7 天柱状图（一次画完，一天不重刷）。
//   - 本机没有「确定」实体键（只有 BOOT/home、音量上下 + 屏幕三块触摸虚拟键），
//     所以屏幕上的按钮是主交互，物理键只做 时长/跳过 辅助。
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

#define POMO_DATA_FILE "pomo.txt"

// ---- 阶段 ----
#define PH_FOCUS 0
#define PH_SHORT 1
#define PH_LONG 2
#define PH_COUNT 3
static const char *kPhaseName[PH_COUNT] = {"专注", "短休", "长休"};

// ---- 专注时长档位（分钟）----
static const int kFocusDur[4] = {15, 25, 45, 60};
#define FOCUS_DUR_COUNT 4
#define SHORT_MIN 5
#define LONG_MIN 15
#define CYCLE_PERIOD 4 // 每 4 个番茄进一次长休

// ---- 计时状态 ----
#define ST_IDLE 0
#define ST_RUN 1
#define ST_PAUSE 2

// ---- 全局状态 ----
static int s_phase = PH_FOCUS;
static int s_dur_idx = 1;      // 默认 25 分钟
static int s_state = ST_IDLE;
static time_t s_end_epoch = 0;  // 结束时刻（wall clock，跨休眠/重启都准）
static int s_paused_sec = 0;   // 暂停时剩余秒数
static int s_cycle_pos = 0;    // 已完成几个专注（决定休息长短）
static int s_today_count = 0;  // 今日完成的专注数
static int s_today_min = 0;    // 今日专注总分钟
static int s_wins = 0;         // 累计完成专注数
static int s_day_cnt[7] = {0}; // 近 7 天（含今天）完成数，0=今天
static int s_day_min[7] = {0};
static char s_day_date[7][16] = {{0}}; // YYYY-MM-DD

static int s_view_stats = 0; // 0=计时页 1=统计页

// ---- 绘制对象 ----
#define SEG_DIGITS 2
static lv_obj_t *s_seg[SEG_DIGITS][7];      // 点亮的段
static lv_obj_t *s_seg_ghost[SEG_DIGITS][7]; // 未点亮的段（空心描边）
static lv_obj_t *s_min_lbl = NULL;   // "分钟"
static lv_obj_t *s_range_lbl = NULL; // "14:30 → 15:00 结束"
static lv_obj_t *s_bar_bg = NULL;
static lv_obj_t *s_bar_fill = NULL;
static lv_obj_t *s_phase_lbl = NULL;
static lv_obj_t *s_info_lbl = NULL;
static lv_obj_t *s_status_lbl = NULL;
static lv_obj_t *s_next_lbl = NULL;
static lv_obj_t *s_btn_label = NULL;
static lv_obj_t *s_stats_obj = NULL;
// 计时页的部件：切到统计页时整组隐藏，否则两个页面的文字会叠在一起
static lv_obj_t *s_timer_parts[16];
static int s_timer_parts_n = 0;

static void add_timer_part(lv_obj_t *o) {
    if (o != NULL && s_timer_parts_n < (int)(sizeof(s_timer_parts) / sizeof(s_timer_parts[0]))) {
        s_timer_parts[s_timer_parts_n++] = o;
    }
}

static void set_view(int stats) {
    for (int i = 0; i < s_timer_parts_n; ++i) {
        if (s_timer_parts[i] == NULL) continue;
        if (stats) lv_obj_add_flag(s_timer_parts[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(s_timer_parts[i], LV_OBJ_FLAG_HIDDEN);
    }
    if (s_stats_obj != NULL) {
        if (stats) lv_obj_clear_flag(s_stats_obj, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_stats_obj, LV_OBJ_FLAG_HIDDEN);
    }
}

static lv_obj_t *s_scr = NULL;
static lv_timer_t *s_tick = NULL;
static int s_last_min = -1;

// ---- 字体 ----
static const lv_font_t *ui_font(void) {
    const lv_font_t *f = fontpack_lv_font_ui();
    return f != NULL ? f : fontpack_lv_font_get(25, 2);
}
static const lv_font_t *title_font(void) {
    const lv_font_t *f = fontpack_lv_font_get(30, 2);
    return f != NULL ? f : ui_font();
}

// ================= 七段数码管 =================
// 段序: 0=a 1=b 2=c 3=d 4=e 5=f 6=g
//   --a--
//  f|   |b
//   --g--
//  e|   |c
//   --d--
static const uint8_t kSegMask[10] = {
    0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F,
};

static void seg_make_digit(int digit, lv_obj_t *parent, int x, int y, int w, int h, int s) {
    const int gap = 3;
    const int half = (h - s) / 2 - gap;
    struct {
        int x, y, w, h;
    } r[7];
    r[0] = (typeof(r[0])){x + s / 2, y, w - s, s};              // a 顶
    r[1] = (typeof(r[1])){x + w - s, y + s / 2, s, half};        // b 右上
    r[2] = (typeof(r[2])){x + w - s, y + (h + s) / 2, s, half};  // c 右下
    r[3] = (typeof(r[3])){x + s / 2, y + h - s, w - s, s};        // d 底
    r[4] = (typeof(r[4])){x, y + (h + s) / 2, s, half};          // e 左下
    r[5] = (typeof(r[5])){x, y + s / 2, s, half};                // f 左上
    r[6] = (typeof(r[6])){x + s / 2, y + (h - s) / 2, w - s, s};  // g 中

    for (int i = 0; i < 7; ++i) {
        lv_obj_t *on = lv_obj_create(parent);
        lv_obj_remove_style_all(on);
        lv_obj_set_pos(on, r[i].x, r[i].y);
        lv_obj_set_size(on, r[i].w, r[i].h);
        lv_obj_set_style_bg_color(on, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(on, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(on, 0, 0);
        lv_obj_clear_flag(on, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        s_seg[digit][i] = on;

        // 未点亮的段：只画段中央一条细线（整段空心矩形在 1bpp 上太重，像一堆空盒子）
        lv_obj_t *ghost = lv_obj_create(parent);
        lv_obj_remove_style_all(ghost);
        const int thin = 3;
        if (r[i].w > r[i].h) { // 横向段
            lv_obj_set_pos(ghost, r[i].x, r[i].y + (r[i].h - thin) / 2);
            lv_obj_set_size(ghost, r[i].w, thin);
        } else { // 纵向段
            lv_obj_set_pos(ghost, r[i].x + (r[i].w - thin) / 2, r[i].y);
            lv_obj_set_size(ghost, thin, r[i].h);
        }
        lv_obj_set_style_bg_color(ghost, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(ghost, LV_OPA_COVER, 0);
        lv_obj_clear_flag(ghost, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        s_seg_ghost[digit][i] = ghost;
    }
}

static void seg_set(int digit, int value) {
    const uint8_t mask = (value >= 0 && value <= 9) ? kSegMask[value] : 0;
    for (int i = 0; i < 7; ++i) {
        if (s_seg[digit][i] == NULL) continue;
        if (mask & (1 << i)) {
            lv_obj_clear_flag(s_seg[digit][i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_seg_ghost[digit][i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_seg[digit][i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(s_seg_ghost[digit][i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ================= 时间工具 =================
static int dur_minutes(void) {
    if (s_phase == PH_FOCUS) return kFocusDur[s_dur_idx];
    if (s_phase == PH_SHORT) return SHORT_MIN;
    return LONG_MIN;
}

static int remain_seconds(void) {
    if (s_state == ST_RUN) {
        const long r = (long)(s_end_epoch - time(NULL));
        return r > 0 ? (int)r : 0;
    }
    if (s_state == ST_PAUSE) return s_paused_sec;
    return dur_minutes() * 60;
}

static void fmt_hm(time_t epoch, char *out, size_t n) {
    struct tm tmv;
    localtime_r(&epoch, &tmv);
    snprintf(out, n, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
}

static void today_str(char *out, size_t n) {
    time_t now = time(NULL);
    struct tm tmv;
    localtime_r(&now, &tmv);
    snprintf(out, n, "%04d-%02d-%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
}

// 把近 7 天的统计滚动到"今天是最新的第 0 天"
static void roll_days(void) {
    char today[16];
    today_str(today, sizeof(today));
    if (strcmp(s_day_date[0], today) == 0) return;
    // 整体后移一天，最新的一天空出来
    for (int i = 6; i > 0; --i) {
        s_day_cnt[i] = s_day_cnt[i - 1];
        s_day_min[i] = s_day_min[i - 1];
        memcpy(s_day_date[i], s_day_date[i - 1], sizeof(s_day_date[i]));
    }
    s_day_cnt[0] = 0;
    s_day_min[0] = 0;
    s_day_date[0][0] = '\0';
    memcpy(s_day_date[0], today, sizeof(s_day_date[0]));
}

// ================= 绘制 =================
static void update_bar(int remain_min, int total_min) {
    if (s_bar_fill == NULL) return;
    const int w = 300;
    int pct = total_min > 0 ? (total_min - remain_min) * w / total_min : 0;
    if (pct < 0) pct = 0;
    if (pct > w) pct = w;
    lv_obj_set_width(s_bar_fill, pct > 0 ? pct : 1);
}

static void update_info_line(void) {
    if (s_info_lbl == NULL) return;
    char buf[64];
    snprintf(buf, sizeof(buf), "今日 %d 个 · %d 分", s_today_count, s_today_min);
    lv_label_set_text(s_info_lbl, buf);
}

static void update_status_line(void) {
    if (s_status_lbl == NULL) return;
    char buf[96];
    if (s_state == ST_RUN) {
        snprintf(buf, sizeof(buf), "%s中 · 退出也不会丢进度", kPhaseName[s_phase]);
    } else if (s_state == ST_PAUSE) {
        snprintf(buf, sizeof(buf), "已暂停 · 按确定继续");
    } else if (s_phase == PH_FOCUS) {
        snprintf(buf, sizeof(buf), "选时长后按确定开始 · 上键改时长");
    } else {
        snprintf(buf, sizeof(buf), "%s阶段 · 按确定开始", kPhaseName[s_phase]);
    }
    lv_label_set_text(s_status_lbl, buf);
}

static void update_button_label(void) {
    if (s_btn_label == NULL) return;
    const char *t = "开始";
    if (s_state == ST_RUN) t = "暂停";
    else if (s_state == ST_PAUSE) t = "继续";
    lv_label_set_text(s_btn_label, t);
}

static void render_timer_view(void) {
    const int total = dur_minutes();
    const int remain = remain_seconds();
    int remain_min = (remain + 59) / 60; // 向上取整
    if (remain_min > total) remain_min = total;
    if (remain_min < 0) remain_min = 0;

    seg_set(0, (remain_min / 10) % 10);
    seg_set(1, remain_min % 10);

    if (s_min_lbl != NULL) lv_label_set_text(s_min_lbl, "分钟");
    if (s_phase_lbl != NULL) lv_label_set_text(s_phase_lbl, kPhaseName[s_phase]);

    if (s_range_lbl != NULL) {
        char buf[64], a[16], b[16];
        if (s_state == ST_RUN) {
            fmt_hm(s_end_epoch - total * 60, a, sizeof(a));
            fmt_hm(s_end_epoch, b, sizeof(b));
            snprintf(buf, sizeof(buf), "%s → %s 结束", a, b);
        } else if (s_state == ST_PAUSE) {
            time_t now = time(NULL);
            fmt_hm(now, a, sizeof(a));
            fmt_hm(now + remain, b, sizeof(b));
            snprintf(buf, sizeof(buf), "已暂停 · 约 %s 结束", b);
        } else {
            time_t now = time(NULL);
            fmt_hm(now, a, sizeof(a));
            fmt_hm(now + total * 60, b, sizeof(b));
            snprintf(buf, sizeof(buf), "%s → %s 结束", a, b);
        }
        lv_label_set_text(s_range_lbl, buf);
    }
    if (s_next_lbl != NULL) {
        char nb[64];
        if (s_phase == PH_FOCUS) {
            const int is_long = (s_cycle_pos % CYCLE_PERIOD == CYCLE_PERIOD - 1);
            snprintf(nb, sizeof(nb), "下一个：%s %d 分钟 · 本轮第 %d 个番茄",
                     is_long ? "长休" : "短休", is_long ? LONG_MIN : SHORT_MIN,
                     s_cycle_pos % CYCLE_PERIOD + 1);
        } else {
            snprintf(nb, sizeof(nb), "下一个：专注 %d 分钟", kFocusDur[s_dur_idx]);
        }
        lv_label_set_text(s_next_lbl, nb);
    }
    update_bar(remain_min, total);
    update_info_line();
    update_status_line();
    update_button_label();
}

static void render_stats_view(void) {
    if (s_stats_obj == NULL) return;
    lv_obj_t *t = lv_obj_get_child(s_stats_obj, 0);
    if (t != NULL) {
        char buf[64];
        snprintf(buf, sizeof(buf), "今日 %d 个 · %d 分 · 累计 %d 个", s_today_count, s_today_min, s_wins);
        lv_label_set_text(t, buf);
    }
    int maxc = 1;
    for (int i = 0; i < 7; ++i)
        if (s_day_cnt[i] > maxc) maxc = s_day_cnt[i];
    for (int i = 0; i < 7; ++i) {
        lv_obj_t *bar = lv_obj_get_child(s_stats_obj, 1 + i);
        if (bar == NULL) continue;
        int h = s_day_cnt[i] * 180 / maxc;
        if (h < 3) h = 3;
        lv_obj_set_height(bar, h);
        lv_obj_set_pos(bar, 20 + i * 60, 230 - h);
        lv_obj_t *num = lv_obj_get_child(s_stats_obj, 15 + i);
        if (num != NULL) {
            if (s_day_cnt[i] == 0) {
                lv_obj_add_flag(num, LV_OBJ_FLAG_HIDDEN); // 没有番茄的日子不标 0，干净点
            } else {
                lv_obj_clear_flag(num, LV_OBJ_FLAG_HIDDEN);
                char b[8];
                snprintf(b, sizeof(b), "%d", s_day_cnt[i]);
                lv_label_set_text(num, b);
                lv_obj_set_pos(num, 20 + i * 60, 230 - h - 28);
            }
        }
        lv_obj_t *lab = lv_obj_get_child(s_stats_obj, 8 + i);
        if (lab != NULL) {
            const char *d = s_day_date[i];
            char b[8] = {0};
            if (d[0] != '\0') memcpy(b, d + 5, 5); // MM-DD
            lv_label_set_text(lab, b);
        }
    }
}

// ================= 持久化 =================
static void save_game(void) {
    FILE *fp = (FILE *)dynapp_fopen_data(POMO_DATA_FILE, "w");
    if (fp == NULL) return;
    fprintf(fp, "POMO1 %d %d %d %ld %d %d %d %d %d\n", s_phase, s_state, s_dur_idx,
            (long)s_end_epoch, s_paused_sec, s_cycle_pos, s_today_count, s_today_min, s_wins);
    for (int i = 0; i < 7; ++i) {
        fprintf(fp, "%s %d %d\n", s_day_date[i][0] ? s_day_date[i] : "-", s_day_cnt[i], s_day_min[i]);
    }
    fclose(fp);
}

static int load_game(void) {
    FILE *fp = (FILE *)dynapp_fopen_data(POMO_DATA_FILE, "r");
    if (fp == NULL) return 0;
    char magic[16] = {0};
    int phase = 0, state = 0, dur_idx = 0, paused = 0, cycle_pos = 0;
    int today_count = 0, today_min = 0, wins = 0;
    long end_epoch = 0;
    if (fscanf(fp, "%15s", magic) != 1 || strcmp(magic, "POMO1") != 0) {
        fclose(fp);
        return 0;
    }
    if (fscanf(fp, "%d %d %d %ld %d %d %d %d %d", &phase, &state, &dur_idx, &end_epoch, &paused,
               &cycle_pos, &today_count, &today_min, &wins) != 9) {
        fclose(fp);
        return 0;
    }
    if (phase < 0 || phase >= PH_COUNT || dur_idx < 0 || dur_idx >= FOCUS_DUR_COUNT ||
        state < ST_IDLE || state > ST_PAUSE) {
        fclose(fp);
        return 0;
    }
    for (int i = 0; i < 7; ++i) {
        char date[16] = {0};
        int cnt = 0, min = 0;
        if (fscanf(fp, "%15s %d %d", date, &cnt, &min) != 3) {
            fclose(fp);
            return 0;
        }
        if (strcmp(date, "-") != 0) snprintf(s_day_date[i], sizeof(s_day_date[i]), "%s", date);
        s_day_cnt[i] = cnt;
        s_day_min[i] = min;
    }
    fclose(fp);
    s_phase = phase;
    s_state = state;
    s_dur_idx = dur_idx;
    s_end_epoch = (time_t)end_epoch;
    s_paused_sec = paused;
    s_cycle_pos = cycle_pos;
    s_today_count = today_count;
    s_today_min = today_min;
    s_wins = wins;
    roll_days();
    return 1;
}

// ================= 阶段推进 =================
static void tick_cb(lv_timer_t *tmr);

static void next_phase(void) {
    if (s_phase == PH_FOCUS) {
        s_phase = (s_cycle_pos % CYCLE_PERIOD == CYCLE_PERIOD - 1) ? PH_LONG : PH_SHORT;
    } else {
        s_phase = PH_FOCUS;
    }
}

static void on_phase_complete(void) {
    const int finished_min = dur_minutes();
    const int was_focus = (s_phase == PH_FOCUS);
    s_state = ST_IDLE;
    s_end_epoch = 0;
    s_paused_sec = 0;
    if (was_focus) {
        s_today_count++;
        s_today_min += finished_min;
        s_day_cnt[0]++;
        s_day_min[0] += finished_min;
        s_wins++;
        s_cycle_pos++;
    }
    next_phase();
    save_game();
    render_timer_view();
    char msg[64];
    if (was_focus) {
        snprintf(msg, sizeof(msg), "专注完成！休息 %d 分钟", dur_minutes());
        dynapp_modal_alert("专注完成", msg, "确定");
    } else {
        snprintf(msg, sizeof(msg), "休息结束，开始新的专注");
        dynapp_modal_alert("休息结束", msg, "确定");
    }
}

static void start_phase(void) {
    const int total = dur_minutes();
    s_end_epoch = time(NULL) + total * 60;
    s_state = ST_RUN;
    s_last_min = -1;
    if (s_tick == NULL) s_tick = lv_timer_create(tick_cb, 1000, NULL);
    save_game(); // 状态一变就落盘：断电不丢进度
    render_timer_view();
}

static void pause_phase(void) {
    if (s_state != ST_RUN) return;
    s_paused_sec = remain_seconds();
    s_state = ST_PAUSE;
    if (s_tick != NULL) {
        lv_timer_delete(s_tick);
        s_tick = NULL;
    }
    save_game();
    render_timer_view();
}

static void resume_phase(void) {
    if (s_state != ST_PAUSE) return;
    s_end_epoch = time(NULL) + s_paused_sec;
    s_state = ST_RUN;
    s_last_min = -1;
    if (s_tick == NULL) s_tick = lv_timer_create(tick_cb, 1000, NULL);
    save_game();
    render_timer_view();
}

// ================= 交互 =================
static void on_primary_clicked(lv_event_t *e) {
    (void)e;
    if (s_state == ST_RUN) {
        pause_phase();
    } else if (s_state == ST_PAUSE) {
        resume_phase();
    } else {
        start_phase();
    }
}

static void on_dur_clicked(lv_event_t *e) {
    (void)e;
    if (s_state != ST_IDLE) {
        dynapp_toast_show("计时中不能改时长", 1200);
        return;
    }
    if (s_phase != PH_FOCUS) {
        dynapp_toast_show("休息时长固定", 1200);
        return;
    }
    s_dur_idx = (s_dur_idx + 1) % FOCUS_DUR_COUNT;
    save_game();
    render_timer_view();
    char b[32];
    snprintf(b, sizeof(b), "%d 分钟", kFocusDur[s_dur_idx]);
    dynapp_toast_show(b, 1000);
}

static void on_skip_clicked(lv_event_t *e) {
    (void)e;
    s_state = ST_IDLE;
    s_end_epoch = 0;
    s_paused_sec = 0;
    if (s_tick != NULL) {
        lv_timer_delete(s_tick);
        s_tick = NULL;
    }
    next_phase();
    save_game();
    render_timer_view();
    dynapp_toast_show(s_phase == PH_FOCUS ? "进入专注" : "进入休息", 1000);
}

static void on_stats_clicked(lv_event_t *e) {
    (void)e;
    s_view_stats = !s_view_stats;
    set_view(s_view_stats);
    if (s_view_stats) render_stats_view();
    else render_timer_view();
}

static void tick_cb(lv_timer_t *tmr) {
    (void)tmr;
    if (s_state != ST_RUN) {
        lv_timer_delete(tmr);
        s_tick = NULL;
        return;
    }
    if (s_end_epoch - time(NULL) <= 0) {
        on_phase_complete();
        return;
    }
    // 只在"分钟数变了"才刷：墨水屏刷新越少越好
    const int remain_min = (remain_seconds() + 59) / 60;
    if (remain_min != s_last_min) {
        s_last_min = remain_min;
        render_timer_view();
    }
}

static int pomo_on_vk_key(const char *key) {
    if (key == NULL) return 0;
    if (strcmp(key, "vk_home") == 0 || strcmp(key, "esc") == 0 || strcmp(key, "back") == 0) {
        if (s_tick != NULL) {
            lv_timer_delete(s_tick);
            s_tick = NULL;
        }
        // 不清状态：运行中的番茄钟要在后台继续跑（靠 wall clock 计时），
        // 回桌面只是停掉 UI 定时器；状态已落盘，重进 app 会重新拉起 tick。
        save_game();
        dynapp_screen_go_home();
        return 1;
    }
    if (strcmp(key, "vk_prev") == 0) { // 上键：改时长
        on_dur_clicked(NULL);
        return 1;
    }
    if (strcmp(key, "vk_next") == 0) { // 下键：跳过本阶段
        on_skip_clicked(NULL);
        return 1;
    }
    return 0;
}

// ================= 界面 =================
static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, lv_coord_t w, lv_coord_t h,
                          lv_event_cb_t cb, void *user_data, lv_obj_t **out_label) {
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    if (cb != NULL) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    const lv_font_t *f = ui_font();
    if (f != NULL) lv_obj_set_style_text_font(lbl, f, 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    if (out_label != NULL) *out_label = lbl;
    return btn;
}

static void on_screen_deleted(lv_event_t *e) {
    (void)e;
    if (s_tick != NULL) {
        lv_timer_delete(s_tick);
        s_tick = NULL;
    }
    save_game();
    s_scr = NULL;
    s_info_lbl = NULL;
    s_status_lbl = NULL;
    s_next_lbl = NULL;
    s_min_lbl = NULL;
    s_range_lbl = NULL;
    s_bar_bg = NULL;
    s_bar_fill = NULL;
    s_phase_lbl = NULL;
    s_btn_label = NULL;
    s_stats_obj = NULL;
    memset(s_seg, 0, sizeof(s_seg));
    memset(s_seg_ghost, 0, sizeof(s_seg_ghost));
    memset(s_timer_parts, 0, sizeof(s_timer_parts));
    s_timer_parts_n = 0;
}

static void *pomo_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    s_scr = scr;
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 800);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_screen_deleted, LV_EVENT_DELETE, NULL);

    dynapp_screen_status_bar(scr);
    dynapp_vk_attach(scr, "pomo", pomo_create, pomo_on_vk_key);

    if (!load_game()) {
        s_phase = PH_FOCUS;
        s_dur_idx = 1;
        s_state = ST_IDLE;
        s_cycle_pos = 0;
        s_today_count = 0;
        s_today_min = 0;
        s_wins = 0;
    }
    roll_days();

    const lv_font_t *f_ui = ui_font();
    const lv_font_t *f_title = title_font();

    // 1. 标题
    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "番茄钟");
    if (f_title != NULL) lv_obj_set_style_text_font(title, f_title, 0);
    lv_obj_set_style_text_color(title, lv_color_black(), 0);
    lv_obj_set_pos(title, 16, 48);

    s_info_lbl = lv_label_create(scr);
    if (f_ui != NULL) lv_obj_set_style_text_font(s_info_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_info_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_align(s_info_lbl, LV_ALIGN_TOP_RIGHT, -16, 56);

    // 2. 阶段标签
    s_phase_lbl = lv_label_create(scr);
    if (f_ui != NULL) lv_obj_set_style_text_font(s_phase_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_phase_lbl, lv_color_black(), 0);
    lv_obj_set_pos(s_phase_lbl, 16, 92);
    add_timer_part(s_phase_lbl);

    // 3. 数码管：两位大数字（剩余分钟）
    {
        const int dw = 92, dh = 140, ds = 15, gap = 18;
        for (int d = 0; d < SEG_DIGITS; ++d) {
            lv_obj_t *box = lv_obj_create(scr);
            lv_obj_remove_style_all(box);
            lv_obj_set_pos(box, 96 + d * (dw + gap), 140);
            lv_obj_set_size(box, dw, dh);
            lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
            seg_make_digit(d, box, 0, 0, dw, dh, ds);
            add_timer_part(box);
        }
    }

    s_min_lbl = lv_label_create(scr);
    lv_label_set_text(s_min_lbl, "分钟");
    if (f_ui != NULL) lv_obj_set_style_text_font(s_min_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_min_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_pos(s_min_lbl, 308, 196);
    add_timer_part(s_min_lbl);

    // 4. 起止钟点
    s_range_lbl = lv_label_create(scr);
    if (f_ui != NULL) lv_obj_set_style_text_font(s_range_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_range_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_pos(s_range_lbl, 16, 320);
    add_timer_part(s_range_lbl);

    // 5. 排水条：每分钟长一格
    s_bar_bg = lv_obj_create(scr);
    lv_obj_remove_style_all(s_bar_bg);
    lv_obj_set_pos(s_bar_bg, 90, 356);
    lv_obj_set_size(s_bar_bg, 300, 14);
    lv_obj_set_style_bg_color(s_bar_bg, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_bar_bg, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_bar_bg, lv_color_black(), 0);
    lv_obj_set_style_border_width(s_bar_bg, 1, 0);
    lv_obj_clear_flag(s_bar_bg, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_bar_fill = lv_obj_create(s_bar_bg);
    lv_obj_remove_style_all(s_bar_fill);
    lv_obj_set_pos(s_bar_fill, 1, 1);
    lv_obj_set_size(s_bar_fill, 1, 12);
    lv_obj_set_style_bg_color(s_bar_fill, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_bar_fill, LV_OPA_COVER, 0);
    lv_obj_clear_flag(s_bar_fill, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    add_timer_part(s_bar_bg);

    // 6. 统计页（默认隐藏）：[0]=标题 [1..7]=柱子 [8..14]=日期
    s_stats_obj = lv_obj_create(scr);
    lv_obj_remove_style_all(s_stats_obj);
    lv_obj_set_pos(s_stats_obj, 16, 92);
    lv_obj_set_size(s_stats_obj, 448, 300);
    lv_obj_clear_flag(s_stats_obj, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    {
        lv_obj_t *t = lv_label_create(s_stats_obj);
        lv_label_set_text(t, "");
        if (f_ui != NULL) lv_obj_set_style_text_font(t, f_ui, 0);
        lv_obj_set_style_text_color(t, lv_color_black(), 0);
        lv_obj_set_pos(t, 0, 0);
        for (int i = 0; i < 7; ++i) {
            lv_obj_t *bar = lv_obj_create(s_stats_obj);
            lv_obj_remove_style_all(bar);
            lv_obj_set_pos(bar, 20 + i * 60, 227);
            lv_obj_set_size(bar, 40, 3);
            lv_obj_set_style_bg_color(bar, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
            lv_obj_set_style_border_color(bar, lv_color_black(), 0);
            lv_obj_set_style_border_width(bar, 1, 0);
            lv_obj_clear_flag(bar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        }
        for (int i = 0; i < 7; ++i) {
            lv_obj_t *lab = lv_label_create(s_stats_obj);
            lv_label_set_text(lab, "");
            if (f_ui != NULL) lv_obj_set_style_text_font(lab, f_ui, 0);
            lv_obj_set_style_text_color(lab, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
            lv_obj_set_pos(lab, 20 + i * 60, 240);
        }
        // 柱子上的数字：必须是统计页的兄弟对象（做柱子的子对象会被父边界裁掉）
        for (int i = 0; i < 7; ++i) {
            lv_obj_t *num = lv_label_create(s_stats_obj);
            lv_label_set_text(num, "0");
            if (f_ui != NULL) lv_obj_set_style_text_font(num, f_ui, 0);
            lv_obj_set_style_text_color(num, lv_color_black(), 0);
            lv_obj_set_pos(num, 20 + i * 60, 207);
            lv_obj_clear_flag(num, LV_OBJ_FLAG_CLICKABLE);
        }
    }
    lv_obj_add_flag(s_stats_obj, LV_OBJ_FLAG_HIDDEN);

    // 7. 状态行
    s_status_lbl = lv_label_create(scr);
    if (f_ui != NULL) lv_obj_set_style_text_font(s_status_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_status_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_pos(s_status_lbl, 16, 392);
    add_timer_part(s_status_lbl);

    s_next_lbl = lv_label_create(scr);
    if (f_ui != NULL) lv_obj_set_style_text_font(s_next_lbl, f_ui, 0);
    lv_obj_set_style_text_color(s_next_lbl, lv_palette_darken(LV_PALETTE_GREY, 2), 0);
    lv_obj_set_pos(s_next_lbl, 16, 430);
    add_timer_part(s_next_lbl);

    // 8. 按钮
    const lv_coord_t bw = 107, bh = 56, bgap = 6;
    const lv_coord_t by = 700;
    lv_obj_t *b1 = make_btn(scr, "开始", bw, bh, on_primary_clicked, NULL, &s_btn_label);
    lv_obj_set_pos(b1, 16, by);
    lv_obj_t *b2 = make_btn(scr, "时长", bw, bh, on_dur_clicked, NULL, NULL);
    lv_obj_set_pos(b2, 16 + bw + bgap, by);
    lv_obj_t *b3 = make_btn(scr, "跳过", bw, bh, on_skip_clicked, NULL, NULL);
    lv_obj_set_pos(b3, 16 + 2 * (bw + bgap), by);
    lv_obj_t *b4 = make_btn(scr, "统计", bw, bh, on_stats_clicked, NULL, NULL);
    lv_obj_set_pos(b4, 16 + 3 * (bw + bgap), by);

    set_view(s_view_stats);
    render_timer_view();
    render_stats_view();
    save_game(); // 启动即落盘：把"今天"记进近 7 天统计
    // 存档里是"运行中"就把计时器拉起来：退出 app 或重启设备后，
    // 倒计时要靠 wall clock 继续走（tick 不拉起来 = 计时器直接死掉）。
    // 顺带：若结束时间已过，tick 第一次触发就会走完成流程。
    if (s_state == ST_RUN && s_tick == NULL) {
        s_last_min = -1;
        s_tick = lv_timer_create(tick_cb, 1000, NULL);
    }
    return scr;
}

void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "pomo";
    out->name = "番茄钟";
    out->icon = "icon.png";
    out->create = pomo_create;
}
