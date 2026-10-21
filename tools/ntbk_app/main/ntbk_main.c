// tools/ntbk_app/main/ntbk_main.c
// 夜市卜卦 动态应用：抉择式文字冒险，一步一刷，适配墨水屏低刷新率特性。
//
// 玩法：
//   你是夜市东头的卦摊先生，师父留下一本册子，每晚替一位来客断一件事。
//   客人说的话里，藏着"要断的是哪一头"——
//     身体上的事（气色/症状）→ 观相准
//     心里的事（别人的事/自己的隐情）→ 灵签准
//   铜钱卦三种都准，但费心力(2)；灵签花钱(2)省心力(1)；观相不花钱不费心。
//   断对：信望+1 银钱+1；断错：信望-1。
//   信望/银钱/心力任一归零 = 散场；撑过第 20 晚 = 出师（按最终数值分档）。
//
// 墨水屏设计要点：
//   - 一屏只刷一次：整段叙事 + 三个选项 + 三个数值，都是静态文本
//   - 正文限长(≤7 行)，不用滚动（墨水屏滚动 = 整屏重刷）
//   - 文字占满屏的刷新成本远低于图形：一次局刷刷几百个像素 vs 一整段字
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "esp_random.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

#define NIGHT_TOTAL 20 // 撑过这么多晚算出师
#define GUEST_COUNT 20
#define MAX_STAT 99
// 往事一行 = "第N晚 客人名：结果句"，最长约 120 字节，留够余量
#define HIST_LINE_MAX 160

// 三种断法
#define M_COIN 0 // 铜钱卦：必准，耗心力
#define M_LOOK 1 // 观相：不花钱不费心，只对"身体事"准
#define M_STICK 2 // 灵签：花银钱省心力，只对"心结"准

// 客人类型
#define K_BODY 0 // 说身体上的事
#define K_MIND 1 // 藏着心里的事

typedef struct {
    const char *name;
    const char *story;
    int kind;
} Guest;

static const Guest kGuests[GUEST_COUNT] = {
    {"王二娘", "我这几日面上发烫，夜里躺下就觉着有股虚火往上冒，街坊都说我气色不对。", K_BODY},
    {"李秀才", "自打她走后，我读书读不进去，人在书房，心思不知飘去了哪边。", K_MIND},
    {"陈木匠", "我这两日手抖得厉害，握凿子都握不稳，夜里躺下更睡不着。", K_BODY},
    {"周小娘子", "我那娃儿三月初七出门，到今儿不见人影，梦里全是他的脸。", K_MIND},
    {"药铺李掌柜", "我嘴里生疮，喝水都疼，嗓子干得直冒烟。", K_BODY},
    {"绸缎商赵四", "合伙的兄弟这半年说话越来越虚，生意上的事一桩桩瞒着我。", K_MIND},
    {"更夫老吴", "我这几夜眼前发花，走夜路撞了两回墙，耳边还嗡嗡作响。", K_BODY},
    {"绣娘阿桃", "我娘病重，我嘴上说不怕，手里的针却抖得缝不下去。", K_MIND},
    {"走方郎中", "我夜里总做乱梦，醒一身冷汗，胸口闷得像压着块石头。", K_BODY},
    {"米行掌柜", "账上短了一笔银子，我翻了三遍账本，怎么也算不回平。", K_MIND},
    {"老船工郑三", "我头发一把一把地掉，枕上的发圈得收一收才盖得住。", K_BODY},
    {"县衙书办", "上头有人递了话，说要提我上去，可这文书还没落笔。", K_MIND},
    {"卖烧饼的秦汉", "我这胃一到半夜就绞痛，贴着热水袋才能缓过来。", K_BODY},
    {"秦娘子", "夫君的休书已经写好，只等娘家那头点头。", K_MIND},
    {"弹词先生", "我这两日耳朵里嗡嗡直响，琵琶的音都听不全了。", K_BODY},
    {"孙寡妇", "公公的牌位我一直供着，可我怕他真在那边看着我。", K_MIND},
    {"赶车的马六", "我这两日出了一身虚汗，鞭子攥在手里都是滑的。", K_BODY},
    {"私塾的孟先生", "考官的卷子我压了三天没敢拆，我怕自己一拆就名落孙山。", K_MIND},
    {"卖茶的钟九", "我这舌头麻得尝不出味，喝什么都是一个淡字。", K_BODY},
    {"收租的赵保长", "佃户欠的租我催了三回，可我心里明白，这钱催不回来。", K_MIND},
};

// ---- 运行时状态 ----
static int s_night = 1;             // 第几晚
static int s_favor = 30;            // 信望
static int s_coin = 30;             // 银钱
static int s_vigor = 30;            // 心力
static int s_order[GUEST_COUNT];    // 客人出场顺序
static int s_order_pos = 0;         // 当前是第几位客人
static int s_sel = 0;               // 当前选中的断法
static int s_phase = 0;             // 0=选断法 1=看结果
static char s_result[192];           // 结果文本
static int s_ended = 0;             // 已结束
static int s_end_kind = 0;           // 1=信望尽 2=银钱尽 3=心力尽 4=出师

// 往事记录：每晚一行
static char s_hist[NIGHT_TOTAL][HIST_LINE_MAX];
static int s_hist_n = 0;
static int s_hist_page = 0;          // 往事翻页
static int s_hist_open = 0;          // 往事视图是否展开

// ---- UTF-8 边界处理 ----
// 汉字占 3 字节，按字节硬截会把一个字劈成两半：存档变成非法 UTF-8，
// 读档时显示成豆腐块，外部脚本解码直接报错。所以截断一律落在字符边界上。

// 该字符需要几个字节（按首字节判断）
static size_t utf8_char_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

// 把 s 截到不超过 max 字节，且一定落在完整字符边界上
static void utf8_clip(char *s, size_t max) {
    size_t len = strlen(s);
    if (len <= max) return;
    size_t end = max;
    // end 若落在某个字符中间，先退回该字符的首字节
    size_t start = end;
    while (start > 0 && ((unsigned char)s[start] & 0xC0) == 0x80) start--;
    size_t need = utf8_char_len((unsigned char)s[start]);
    if (start + need > end) end = start; // 这个字符被劈开了 → 整字丢掉
    s[end] = '\0';
}

// 去掉行尾残缺的 UTF-8 字符（旧存档里被硬截出来的半个字）
static void utf8_fix_tail(char *s) {
    size_t len = strlen(s);
    if (len == 0) return;
    size_t i = len;
    while (i > 0 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) i--;
    if (i == 0) { s[0] = '\0'; return; }
    size_t need = utf8_char_len((unsigned char)s[i - 1]);
    if (i - 1 + need > len) s[i - 1] = '\0';
}

// ---- 绘制对象 ----
static lv_obj_t *s_stat_lbl[3] = {NULL, NULL, NULL};
static lv_obj_t *s_night_lbl = NULL;
static lv_obj_t *s_guest_lbl = NULL;
static lv_obj_t *s_story_lbl = NULL;
static lv_obj_t *s_opt_lbl[3] = {NULL, NULL, NULL};
static lv_obj_t *s_cursor_lbl = NULL;
static lv_obj_t *s_result_lbl = NULL;
static lv_obj_t *s_status_lbl = NULL;
static lv_obj_t *s_hist_lbl = NULL;
static lv_obj_t *s_next_btn = NULL;   // 结果阶段的「下一晚」

static lv_obj_t *s_scr = NULL;
static int s_move_count = 0;
static int s_last_full_refresh = 0;
#define FULL_REFRESH_EVERY 8

// ---- 字体 ----
static const lv_font_t *ui_font(void) {
    const lv_font_t *f = fontpack_lv_font_ui();
    return f != NULL ? f : fontpack_lv_font_get(25, 2);
}
static const lv_font_t *title_font(void) {
    const lv_font_t *f = fontpack_lv_font_get(30, 2);
    return f != NULL ? f : ui_font();
}

// ================= 局面 =================
static void shuffle_order(void) {
    for (int i = 0; i < GUEST_COUNT; ++i) s_order[i] = i;
    for (int i = GUEST_COUNT - 1; i > 0; --i) {
        const int j = (int)(esp_random() % (uint32_t)(i + 1));
        const int t = s_order[i];
        s_order[i] = s_order[j];
        s_order[j] = t;
    }
    s_order_pos = 0;
}

static int current_guest(void) {
    if (s_order_pos >= GUEST_COUNT) return -1;
    return s_order[s_order_pos];
}

static void new_game(void) {
    s_night = 1;
    s_favor = 30;
    s_coin = 30;
    s_vigor = 30;
    s_hist_n = 0;
    s_hist_page = 0;
    s_ended = 0;
    s_end_kind = 0;
    s_sel = 0;
    s_phase = 0;
    s_result[0] = '\0';
    shuffle_order();
}

// 三种断法说明
static const char *kMethodName[3] = {"铜钱卦", "观相", "灵签"};

static void apply_result(int method, int kind) {
    int ok = 0;
    if (method == M_COIN) {
        ok = 1;
    } else if (method == M_LOOK) {
        ok = (kind == K_BODY);
    } else {
        ok = (kind == K_MIND);
    }
    if (method == M_COIN) s_vigor -= 2;
    if (method == M_STICK) s_coin -= 2;

    if (ok) {
        s_favor += 1;
        s_coin += 1;
        if (method == M_COIN) {
            snprintf(s_result, sizeof(s_result), "铜钱落定，卦象昭然。这桩事你断得分毫不差。");
        } else if (method == M_LOOK) {
            snprintf(s_result, sizeof(s_result), "你盯着他的气色看了半晌——症结就在脸上，一眼便知。");
        } else {
            snprintf(s_result, sizeof(s_result), "签诗一摇，句句都是他心里那桩事。他听了便红了眼。");
        }
    } else {
        s_favor -= 1;
        if (method == M_LOOK) {
            snprintf(s_result, sizeof(s_result), "你只顾看他的脸。他说的哪里是身上的事，分明是心里压着的那头。");
        } else if (method == M_STICK) {
            snprintf(s_result, sizeof(s_result), "签上的话四平八稳。他心里的结，根本不在这里。");
        } else {
            snprintf(s_result, sizeof(s_result), "卦象倒是清楚了，只是你断的不是他真正愁的那件事。");
        }
    }
    if (s_favor > MAX_STAT) s_favor = MAX_STAT;
    if (s_coin > MAX_STAT) s_coin = MAX_STAT;
    if (s_vigor > MAX_STAT) s_vigor = MAX_STAT;
    if (s_favor <= 0) {
        s_ended = 1;
        s_end_kind = 1;
    } else if (s_coin <= 0) {
        s_ended = 1;
        s_end_kind = 2;
    } else if (s_vigor <= 0) {
        s_ended = 1;
        s_end_kind = 3;
    }
}

// ================= 持久化 =================
static void save_game(void) {
    FILE *fp = (FILE *)dynapp_fopen_data("ntbk.txt", "w");
    if (fp == NULL) return;
    fprintf(fp, "NTBK1 %d %d %d %d %d %d %d\n", s_night, s_favor, s_coin, s_vigor, s_order_pos,
            s_hist_n, s_ended);
    for (int i = 0; i < GUEST_COUNT; ++i) fprintf(fp, "%d,", s_order[i]);
    fputc('\n', fp);
    for (int i = 0; i < s_hist_n && i < NIGHT_TOTAL; ++i) fprintf(fp, "%s\n", s_hist[i]);
    fclose(fp);
}

static int load_game(void) {
    FILE *fp = (FILE *)dynapp_fopen_data("ntbk.txt", "r");
    if (fp == NULL) return 0;
    char magic[16] = {0};
    int night = 0, favor = 0, coin = 0, vigor = 0, opos = 0, hn = 0, ended = 0;
    if (fscanf(fp, "%15s", magic) != 1 || strcmp(magic, "NTBK1") != 0) {
        fclose(fp);
        return 0;
    }
    if (fscanf(fp, "%d %d %d %d %d %d %d", &night, &favor, &coin, &vigor, &opos, &hn, &ended) != 7) {
        fclose(fp);
        return 0;
    }
    for (int i = 0; i < GUEST_COUNT; ++i) {
        int v = 0;
        if (fscanf(fp, "%d,", &v) != 1 || v < 0 || v >= GUEST_COUNT) {
            fclose(fp);
            return 0;
        }
        s_order[i] = v;
    }
    s_night = night;
    s_favor = favor;
    s_coin = coin;
    s_vigor = vigor;
    s_order_pos = opos;
    s_hist_n = hn;
    s_ended = ended;
    s_sel = 0;
    s_phase = 0;
    s_result[0] = '\0';
    // 读历史行：格式串开头的 \n 顺带吃掉上一行剩下的换行
    //（注意 fgetc 不在固件导出符号表里，只能用 fscanf）
    s_hist_n = 0;
    for (int i = 0; i < NIGHT_TOTAL; ++i) {
        if (fscanf(fp, "\n%159[^\n]", s_hist[i]) != 1) break;
        utf8_fix_tail(s_hist[i]);
        s_hist_n = i + 1;
    }
    fclose(fp);
    return 1;
}

// ================= 绘制 =================
static void set_label(lv_obj_t *lbl, const char *text) {
    if (lbl == NULL) return;
    lv_label_set_text(lbl, text);
}

static void update_stats(void) {
    char buf[32];
    snprintf(buf, sizeof(buf), "信望 %d", s_favor);
    set_label(s_stat_lbl[0], buf);
    snprintf(buf, sizeof(buf), "银钱 %d", s_coin);
    set_label(s_stat_lbl[1], buf);
    snprintf(buf, sizeof(buf), "心力 %d", s_vigor);
    set_label(s_stat_lbl[2], buf);
}

static void update_night_label(void) {
    char buf[32];
    if (s_ended) {
        snprintf(buf, sizeof(buf), "第 %d 晚", s_night);
    } else {
        snprintf(buf, sizeof(buf), "第 %d / %d 晚", s_night, NIGHT_TOTAL);
    }
    set_label(s_night_lbl, buf);
}

static void update_status_line(void) {
    if (s_status_lbl == NULL) return;
    if (s_ended) {
        set_label(s_status_lbl, "按「重开」再摆一次摊");
    } else if (s_phase == 1) {
        set_label(s_status_lbl, "点「下一晚」继续");
    } else {
        set_label(s_status_lbl, "▲▼ 选断法 · 点选项即断卦");
    }
}

static void show_options(int visible) {
    for (int i = 0; i < 3; ++i) {
        if (s_opt_lbl[i] == NULL) continue;
        if (visible) {
            lv_obj_clear_flag(s_opt_lbl[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_opt_lbl[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_cursor_lbl != NULL) {
        if (visible) {
            lv_obj_clear_flag(s_cursor_lbl, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_cursor_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void render_scene(void) {
    if (s_ended) {
        const char *title;
        const char *body;
        switch (s_end_kind) {
            case 1:
                title = "散场了";
                body = "信望散尽。夜市的人绕开你的摊子，卦筒还留在原地，没有人再来看一眼。";
                break;
            case 2:
                title = "揭不开摊";
                body = "银钱见底。香烛、纸钱、请签的花费，样样都要钱。师父的册子还摊在你面前。";
                break;
            case 3:
                title = "你病倒了";
                body = "心力耗尽。铜钱捏在手里发烫，卦摊前的灯却亮了一夜，天亮时你伏在桌上没有起来。";
                break;
            default:
                title = "出师";
                if (s_favor >= 45 && s_vigor >= 20) {
                    body = "二十晚过去，册子写满了。夜市东头都传开了，说这位卦摊先生看得准、说得稳。";
                } else if (s_favor >= 30) {
                    body = "二十晚过去，册子写满了。有人开始专门绕远路来找你断一卦。";
                } else {
                    body = "二十晚过去，册子写满了。你还站在摊子后头，只是眼神比从前沉了些。";
                }
                break;
        }
        set_label(s_guest_lbl, title);
        set_label(s_story_lbl, body);
        show_options(0);
        set_label(s_result_lbl, "");
        update_night_label();
        update_stats();
        update_status_line();
        return;
    }

    const int g = current_guest();
    if (g < 0) {
        set_label(s_guest_lbl, "收摊");
        set_label(s_story_lbl, "这一晚没有人来。师父的册子上，最后一页还空着。");
        show_options(0);
        set_label(s_result_lbl, "");
        update_night_label();
        update_stats();
        update_status_line();
        return;
    }

    char buf[128];
    snprintf(buf, sizeof(buf), "来客 · %s", kGuests[g].name);
    set_label(s_guest_lbl, buf);
    set_label(s_story_lbl, kGuests[g].story);

    if (s_phase == 0) {
        set_label(s_result_lbl, "");
        if (s_next_btn != NULL) lv_obj_add_flag(s_next_btn, LV_OBJ_FLAG_HIDDEN);
        show_options(1);
        for (int i = 0; i < 3; ++i) {
            snprintf(buf, sizeof(buf), "%s", kMethodName[i]);
            set_label(s_opt_lbl[i], buf);
        }
        if (s_cursor_lbl != NULL) {
            lv_obj_set_pos(s_cursor_lbl, 16, 430 + s_sel * 46 + 2);
        }
    } else {
        show_options(0);
        set_label(s_result_lbl, s_result);
        if (s_next_btn != NULL) lv_obj_clear_flag(s_next_btn, LV_OBJ_FLAG_HIDDEN);
    }
    update_night_label();
    update_stats();
    update_status_line();
}

static void render_history(void) {
    if (s_hist_lbl == NULL) return;
    if (s_hist_n == 0) {
        set_label(s_hist_lbl, "还没有往事。第一位客人还没上门。");
        return;
    }
    const int per = 5;
    const int pages = (s_hist_n + per - 1) / per;
    if (s_hist_page >= pages) s_hist_page = pages - 1;
    if (s_hist_page < 0) s_hist_page = 0;
    char buf[1024];
    size_t used = 0;
    // 注意：snprintf 返回的是"本该写入的长度"，可能超过缓冲区。
    // 累加后 used 可能 > sizeof(buf)，再拿 sizeof(buf) - used 去算剩余空间会下溢，
    // 所以每步都单独判长度并及时收手。
    int n = snprintf(buf, sizeof(buf), "第 %d / %d 页\n\n", s_hist_page + 1, pages);
    if (n < 0) n = 0;
    used = ((size_t)n < sizeof(buf)) ? (size_t)n : sizeof(buf) - 1;
    for (int i = s_hist_page * per; i < s_hist_page * per + per && i < s_hist_n; ++i) {
        if (used + 1 >= sizeof(buf)) break;
        int m = snprintf(buf + used, sizeof(buf) - used, "%s\n", s_hist[i]);
        if (m < 0) break;
        used += (size_t)m;
        if (used >= sizeof(buf) - 1) { used = sizeof(buf) - 1; break; }
    }
    buf[used] = '\0';
    utf8_clip(buf, sizeof(buf) - 1);
    set_label(s_hist_lbl, buf);
}

// ================= 交互 =================
static lv_timer_t *s_save_timer = NULL;
static lv_timer_t *s_refresh_timer = NULL;
static int s_save_dirty = 0;

static void idle_save_cb(lv_timer_t *tmr) {
    if (s_save_dirty) {
        s_save_dirty = 0;
        save_game();
    }
    lv_timer_delete(tmr);
    s_save_timer = NULL;
}

static void idle_full_refresh_cb(lv_timer_t *tmr) {
    if (s_move_count - s_last_full_refresh >= FULL_REFRESH_EVERY) {
        s_last_full_refresh = s_move_count;
        dynapp_epd_full_refresh();
    }
    lv_timer_delete(tmr);
    s_refresh_timer = NULL;
}

static void schedule_idle_maintenance(void) {
    s_save_dirty = 1;
    if (s_save_timer != NULL) lv_timer_delete(s_save_timer);
    s_save_timer = lv_timer_create(idle_save_cb, 3500, NULL);
    if (s_refresh_timer != NULL) lv_timer_delete(s_refresh_timer);
    s_refresh_timer = lv_timer_create(idle_full_refresh_cb, 6000, NULL);
}

static void next_night(void) {
    s_night++;
    s_order_pos++;
    s_phase = 0;
    s_sel = 0;
    s_result[0] = '\0';
    if (s_night > NIGHT_TOTAL) {
        s_ended = 1;
        s_end_kind = 4;
    }
    render_scene();
    schedule_idle_maintenance();
}

static void choose_method(int m) {
    if (s_ended || s_phase == 1) return;
    const int g = current_guest();
    if (g < 0) return;
    s_sel = m;
    apply_result(m, kGuests[g].kind);
    s_phase = 1;
    // 记一笔往事
    if (s_hist_n < NIGHT_TOTAL) {
        snprintf(s_hist[s_hist_n], HIST_LINE_MAX, "第%d晚 %s：%s", s_night, kGuests[g].name, s_result);
        utf8_clip(s_hist[s_hist_n], HIST_LINE_MAX - 1);
        s_hist_n++;
    }
    render_scene();
    schedule_idle_maintenance();
}

static void on_option_clicked(lv_event_t *e) {
    const int m = (int)(intptr_t)lv_event_get_user_data(e);
    if (m < 0 || m > 2) return;
    choose_method(m);
}

static void on_ok_key(void) {
    if (s_ended) return;
    if (s_phase == 1) {
        next_night();
        return;
    }
    choose_method(s_sel);
}

static void on_up(void) {
    if (s_ended || s_phase == 1) return;
    s_sel = (s_sel + 2) % 3;
    render_scene();
}

static void on_down(void) {
    if (s_ended || s_phase == 1) return;
    s_sel = (s_sel + 1) % 3;
    render_scene();
}

// 主视图 / 往事视图 是同一块屏幕上的两套内容，来回切
static lv_obj_t *s_main_objs[16];
static int s_main_obj_n = 0;
static lv_obj_t *s_hist_objs[4];
static int s_hist_obj_n = 0;

static void reg_main(lv_obj_t *o) {
    if (o != NULL && s_main_obj_n < (int)(sizeof(s_main_objs) / sizeof(s_main_objs[0]))) {
        s_main_objs[s_main_obj_n++] = o;
    }
}
static void reg_hist(lv_obj_t *o) {
    if (o != NULL && s_hist_obj_n < (int)(sizeof(s_hist_objs) / sizeof(s_hist_objs[0]))) {
        s_hist_objs[s_hist_obj_n++] = o;
    }
}
static void show_history_view(bool show) {
    for (int i = 0; i < s_main_obj_n; ++i) {
        if (s_main_objs[i] == NULL) continue;
        if (show) lv_obj_add_flag(s_main_objs[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(s_main_objs[i], LV_OBJ_FLAG_HIDDEN);
    }
    for (int i = 0; i < s_hist_obj_n; ++i) {
        if (s_hist_objs[i] == NULL) continue;
        if (show) lv_obj_clear_flag(s_hist_objs[i], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_hist_objs[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void on_hist_clicked(lv_event_t *e) {
    (void)e;
    if (s_hist_lbl == NULL) return;
    s_hist_page = 0;
    render_history();
    show_history_view(true);
    s_hist_open = 1;
    dynapp_toast_show("往事", 1000);
}

static void on_hist_page_up(lv_event_t *e) {
    (void)e;
    s_hist_page--;
    if (s_hist_page < 0) s_hist_page = 0;
    render_history();
}

static void on_hist_page_down(lv_event_t *e) {
    (void)e;
    s_hist_page++;
    render_history();
}

static void on_hist_close(lv_event_t *e) {
    (void)e;
    show_history_view(false);
    s_hist_open = 0;
}

static void on_next_clicked(lv_event_t *e) {
    (void)e;
    if (!s_ended && s_phase == 1) next_night();
}

static void on_new_game_clicked(lv_event_t *e) {
    (void)e;
    new_game();
    render_scene();
    save_game();
    dynapp_toast_show("重新开摊", 1200);
    dynapp_epd_full_refresh();
}

static int ntbk_on_vk_key(const char *key) {
    if (key == NULL) return 0;
    if (strcmp(key, "vk_home") == 0 || strcmp(key, "esc") == 0 || strcmp(key, "back") == 0) {
        dynapp_screen_go_home();
        return 1;
    }
    // 这台设备只有 HOME / 上一页(vk_prev) / 下一页(vk_next) 三个键，没有确定键：
    // 「确定断卦」靠点按选项；侧键负责上下选择（结果阶段则直接进入下一晚）
    if (strcmp(key, "vk_prev") == 0) {
        if (s_hist_open) {
            s_hist_page--;
            if (s_hist_page < 0) s_hist_page = 0;
            render_history();
            return 1;
        }
        on_up();
        return 1;
    }
    if (strcmp(key, "vk_next") == 0) {
        if (s_hist_open) {
            s_hist_page++;
            render_history();
            return 1;
        }
        on_down();
        return 1;
    }
    return 0;
}

// ================= 界面 =================
static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, lv_coord_t w, lv_coord_t h,
                          lv_event_cb_t cb, void *user_data) {
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
    return btn;
}

static lv_obj_t *make_label(lv_obj_t *parent, const char *text, int x, int y, int w, const lv_font_t *f,
                            bool wrap) {
    lv_obj_t *lbl = lv_label_create(parent);
    lv_label_set_text(lbl, text);
    if (f != NULL) lv_obj_set_style_text_font(lbl, f, 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    if (wrap) lv_label_set_long_mode(lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(lbl, w);
    lv_obj_set_pos(lbl, x, y);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    return lbl;
}

static void on_screen_deleted(lv_event_t *e) {
    (void)e;
    save_game();
    s_scr = NULL;
    s_night_lbl = NULL;
    s_guest_lbl = NULL;
    s_story_lbl = NULL;
    s_result_lbl = NULL;
    s_status_lbl = NULL;
    s_hist_lbl = NULL;
    s_cursor_lbl = NULL;
    for (int i = 0; i < 3; ++i) {
        s_stat_lbl[i] = NULL;
        s_opt_lbl[i] = NULL;
    }
}

static void *ntbk_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    s_scr = scr;
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 800);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_screen_deleted, LV_EVENT_DELETE, NULL);

    dynapp_screen_status_bar(scr);
    dynapp_vk_attach(scr, "ntbk", ntbk_create, ntbk_on_vk_key);

    if (!load_game()) new_game();

    const lv_font_t *f_ui = ui_font();
    const lv_font_t *f_title = title_font();

    // 标题
    lv_obj_t *title = make_label(scr, "夜市卜卦", 16, 48, 200, f_title, false);
    (void)title;
    s_night_lbl = make_label(scr, "", 280, 56, 184, f_ui, false);
    lv_obj_align(s_night_lbl, LV_ALIGN_TOP_RIGHT, -16, 56);

    // 三个数值
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *box = lv_obj_create(scr);
        lv_obj_remove_style_all(box);
        lv_obj_set_size(box, 140, 50);
        lv_obj_set_pos(box, 16 + i * 156, 92);
        lv_obj_set_style_bg_color(box, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(box, lv_color_black(), 0);
        lv_obj_set_style_border_width(box, 1, 0);
        lv_obj_clear_flag(box, LV_OBJ_FLAG_CLICKABLE);
        s_stat_lbl[i] = make_label(box, "", 8, 10, 124, f_ui, false);
    }

    // 来客 + 叙述
    s_guest_lbl = make_label(scr, "", 16, 160, 448, f_ui, false);
    s_story_lbl = make_label(scr, "", 16, 196, 448, f_ui, true);

    // 三个断法（可点）
    s_cursor_lbl = make_label(scr, "▲", 16, 432, 30, f_ui, false);
    for (int i = 0; i < 3; ++i) {
        lv_obj_t *opt = make_label(scr, kMethodName[i], 48, 430 + i * 46, 400, f_ui, false);
        lv_obj_add_flag(opt, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(opt, on_option_clicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        s_opt_lbl[i] = opt;
    }

    // 结果
    s_result_lbl = make_label(scr, "", 16, 580, 448, f_ui, true);

    // 「下一晚」按钮（只在结果阶段出现）
    s_next_btn = make_btn(scr, "下一晚", 200, 46, on_next_clicked, NULL);
    lv_obj_set_pos(s_next_btn, 16, 430);
    lv_obj_add_flag(s_next_btn, LV_OBJ_FLAG_HIDDEN);

    // 状态行
    s_status_lbl = make_label(scr, "", 16, 660, 448, f_ui, false);

    // 按钮
    const lv_coord_t bw = 144, bh = 52, bgap = 8;
    const lv_coord_t by = 700;
    lv_obj_t *b1 = make_btn(scr, "往事", bw, bh, on_hist_clicked, NULL);
    lv_obj_set_pos(b1, 16, by);
    lv_obj_t *b2 = make_btn(scr, "重开", bw, bh, on_new_game_clicked, NULL);
    lv_obj_set_pos(b2, 16 + bw + bgap, by);

    // 往事视图（默认收起）
    s_hist_lbl = make_label(scr, "", 16, 120, 448, f_ui, true);
    reg_hist(s_hist_lbl);
    {
        lv_obj_t *h1 = make_btn(scr, "上一页", 140, 48, on_hist_page_up, NULL);
        lv_obj_set_pos(h1, 16, 700);
        reg_hist(h1);
        lv_obj_t *h2 = make_btn(scr, "下一页", 140, 48, on_hist_page_down, NULL);
        lv_obj_set_pos(h2, 164, 700);
        reg_hist(h2);
        lv_obj_t *h3 = make_btn(scr, "关上", 140, 48, on_hist_close, NULL);
        lv_obj_set_pos(h3, 312, 700);
        reg_hist(h3);
    }

    // 主视图对象（往事展开时整块收起）
    reg_main(s_guest_lbl);
    reg_main(s_story_lbl);
    reg_main(s_result_lbl);
    reg_main(s_status_lbl);
    reg_main(s_cursor_lbl);
    reg_main(s_next_btn);
    for (int i = 0; i < 3; ++i) reg_main(s_opt_lbl[i]);
    reg_main(b1);
    reg_main(b2);

    render_scene();
    render_history();
    show_history_view(false);
    // 一开局就落盘：玩家还没动手就退出，棋局也不该丢
    save_game();
    return scr;
}

void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "ntbk";
    out->name = "夜市卜卦";
    out->icon = "icon.png";
    out->create = ntbk_create;
}
