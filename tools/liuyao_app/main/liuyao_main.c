// tools/liuyao_app/main/liuyao_main.c
// 六爻占卜 动态应用：摇六爻成卦，看本卦变卦，断所问之事。
//
// 玩法：
//   1. 先选一件所问之事（事业/财运/姻缘/健康/出行/寻物/考试/官非）
//   2. 点「起卦」摇六爻。每爻三枚铜钱：
//        三背 = 老阳(动)  两背一字 = 少阳  一背两字 = 少阴  三字 = 老阴(动)
//      动爻是阳变阴、阴变阳，本卦变出「之卦」。
//   3. 看卦：本卦六爻图形（实线为阳、断线为阴）、卦名、象意。
//   4. 断事：卦象断语 + 结合所问之事的吉凶断语。
//
// 墨水屏设计要点：
//   - 卦象是几何图形（粗实线/断线），1bpp 上最清楚，比文字更有"卦"的样子
//   - 摇卦逐爻填，每摇一爻只改一小块 → 局刷，不整屏重刷
//   - 卦象页与断语页分开：图形和长文本不挤在一屏
//   - 一屏一段，不做滚动（墨水屏滚动 = 整屏重刷）
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "esp_random.h"
#include "dynamic_app_abi.h"
#include "dynamic_app_shims.h"
#include "fontpack_lvgl.h"

#define N_YAO 6
#define N_GUA 64
#define N_GUA_ALL 65 // 通行本 64 章 + 1 个只作变卦的卦象（坎上乾下）
#define N_CAT 8

// 爻值
#define YAO_OLD_YIN 0  // 老阴：阴爻动
#define YAO_YANG 1     // 少阳：阳爻静
#define YAO_YIN 2      // 少阴：阴爻静
#define YAO_OLD_YANG 3 // 老阳：阳爻动

// 页面
#define PH_ASK 0   // 选所问之事
#define PH_TOSS 1  // 摇卦中
#define PH_HEX 2   // 看卦
#define PH_WORD 3  // 断语

// 卦象几何
#define HEX_Y0 164   // 最上一爻的顶边
#define ROW_H 44     // 爻距
#define LINE_W 132   // 整爻宽度
#define SEG_W 56     // 断线单段宽
#define SEG_GAP 20   // 断线中间的缝
#define LINE_H 10    // 爻线粗
#define HEX_X1 58    // 本卦爻线左边
#define HEX_X2 302   // 变卦爻线左边

// 可见性标签
#define VIS_ALL (-1)  // 所有页面都显示
#define VIS_HEX (-2)  // 摇卦/看卦/断语都显示（所问标签等）
#define VIS_CAST (-3) // 只在摇卦 + 看卦显示（卦象图本体）

static inline int yao_is_yang(int v) { return v == YAO_YANG || v == YAO_OLD_YANG; }
static inline int yao_is_moving(int v) { return v == YAO_OLD_YIN || v == YAO_OLD_YANG; }

// ================= 六十四卦（通行本序） =================
// upper/lower 用先天八卦序：0乾 1兑 2离 3震 4巽 5坎 6艮 7坤
typedef struct {
    const char *name;  // 卦名
    uint8_t upper;     // 上卦
    uint8_t lower;     // 下卦
    const char *image; // 象意
    const char *word;  // 断语
    int8_t tone;       // -1 凶 / 0 平 / 1 吉
} Gua;

static const Gua kGua[N_GUA_ALL] = {
    {"乾为天", 0, 0, "刚健", "天道运行不息，大有可为。刚极易折，盛时须防过头。", 1},
    {"坤为地", 7, 7, "柔顺", "地势厚载，稳中守成。宜随不宜先，宜积不宜急。", 0},
    {"水雷屯", 5, 3, "初创艰难", "万事开头难，草创阶段多阻。宜蓄力守成，不宜求速成。", -1},
    {"山水蒙", 6, 5, "蒙昧待启", "事情糊涂不明，需人指点。宜请教求解，不宜自作主张。", -1},
    {"天水需", 0, 5, "待时而进", "好景尚需时日，时候未到。宜守候准备，忌强求。", 0},
    {"天水讼", 0, 5, "争讼相持", "两下相争各有道理，争下去两败俱伤。宜和解退让。", -1},
    {"地水师", 7, 5, "整军出征", "众心可用，须有统帅。宜谋定而后动，忌孤军冒进。", 0},
    {"水地比", 5, 7, "亲比相辅", "得众人相助，宜亲近同道，合作则成。", 1},
    {"风天小畜", 4, 0, "小有蓄积", "力量尚小，只能积一点。宜循序积累，忌贪大求快。", 0},
    {"天泽履", 0, 1, "履险有礼", "走在险路上，靠礼节与分寸可平安。宜谨慎守礼。", 0},
    {"地天泰", 7, 0, "通泰", "上下相通，天地交泰。诸事顺遂，宜合作往来。", 1},
    {"天地否", 0, 7, "闭塞不通", "时机未至，上下不通。宜静守待时，忌强求。", -1},
    {"天火同人", 0, 2, "同心协力", "志同道合者聚在一起。宜结伴同行，忌独断。", 1},
    {"火天大有", 2, 0, "大有所得", "眼下所得丰盛，格局开阔。宜把握机会，忌骄矜。", 1},
    {"地山谦", 7, 6, "谦抑", "有实而自谦，反得助力。宜低姿态、多请教。", 1},
    {"雷地豫", 3, 7, "顺时而动", "顺势而起，欢愉中宜备豫。宜趁势而入，忌沉溺安逸。", 0},
    {"泽雷随", 1, 3, "随时随势", "跟随时势则通，固执己见则阻。宜顺应变化。", 0},
    {"山风蛊", 6, 4, "积弊待治", "旧毛病到了非治不可的时候。宜整顿清理，忌拖延。", -1},
    {"地泽临", 7, 1, "临近", "好事将至，且能亲自到场。宜主动出面。", 1},
    {"风地观", 4, 7, "观望", "宜看不宜动，观察形势再定。宜静观，忌妄动。", 0},
    {"火雷噬嗑", 2, 3, "排除障碍", "硬骨头要咬断才能通。宜果断处置障碍。", 0},
    {"山火贲", 6, 2, "文饰其表", "表面光鲜，内里未必。宜重实轻虚，忌只做表面文章。", 0},
    {"山地剥", 6, 7, "剥落", "根基受损，东西一点点流失。宜止损固本，忌再投入。", -1},
    {"地雷复", 7, 3, "复归", "一阳来复，坏处转好。宜重新开始，忌半途而废。", 1},
    {"天雷无妄", 0, 3, "守真", "不存妄念，意外之灾可免。宜按规矩行事，忌投机取巧。", 0},
    {"山天大畜", 6, 0, "大有蓄养", "积蓄已厚，只待施展。宜厚积而后发。", 1},
    {"山雷颐", 6, 3, "养正", "宜养身、养德、养人。守正待时，忌贪食冒进。", 0},
    {"泽风大过", 1, 4, "栋梁过重", "负担过重，超出承载。宜减负求助，忌硬扛。", -1},
    {"坎为水", 5, 5, "险陷", "重重险难，重在不失其道。宜谨慎，忌涉险轻进。", -1},
    {"离为火", 2, 2, "附丽", "有所依傍则明，孤明不照。宜有所依托，忌孤立无援。", 0},
    {"泽山咸", 1, 6, "感应", "彼此感应，有感则通。宜以诚相感。", 1},
    {"雷风恒", 3, 4, "恒常", "长久之道在于守常。宜持之以恒，忌频繁改弦。", 0},
    {"天山遁", 0, 6, "退避", "该退的时候退，不是失败。宜退身保身，忌恋栈不去。", 0},
    {"雷天大壮", 3, 0, "壮大", "力量正盛，壮而不可恃强。宜用壮不用暴。", 1},
    {"火地晋", 2, 7, "晋升", "如日东升，步步高。宜进取，忌躁进。", 1},
    {"地火明夷", 7, 2, "韬光", "贤者遭晦，韬光养晦。宜藏拙守正，忌露锋芒。", -1},
    {"风火家人", 4, 2, "齐家", "内外有序则安。宜先理内务，忌外强中干。", 1},
    {"火泽睽", 2, 1, "乖离", "意见相左，各行其是。宜求同存异，忌强行统一。", -1},
    {"水山蹇", 5, 6, "行路难", "前路多阻，止则通。宜暂止待援，忌硬往里闯。", -1},
    {"雷水解", 3, 5, "解纷", "难题解开，绳结已解。宜趁早收拾残局。", 1},
    {"山泽损", 6, 1, "有损", "有所减损乃有益。宜舍得小利，忌锱铢必较。", 0},
    {"风雷益", 4, 3, "增益", "损上益下，愈损愈益。宜施予、宜扩张。", 1},
    {"泽天夬", 1, 0, "决断", "该决断的事拖不得。宜公开明断，忌私下犹疑。", 0},
    {"天风姤", 0, 4, "不期而遇", "意外相遇，防患未然。宜留意偶然之机，忌轻信。", 0},
    {"泽地萃", 1, 7, "汇聚", "人才财物聚集。宜聚合共事，忌人多口杂。", 1},
    {"地风升", 7, 4, "上升", "循序渐进，稳步向上。宜积小成大，忌求快。", 1},
    {"泽水困", 1, 5, "困顿", "资源枯竭，言辞无用。宜守志待时，忌多言强求。", -1},
    {"水风井", 5, 4, "井养不穷", "井水常在，改井费劲。宜守旧养人，忌频繁改换。", 0},
    {"泽火革", 1, 2, "变革", "非变不可，非变不成。宜改弦更张，忌因循守旧。", 0},
    {"火风鼎", 2, 4, "鼎新", "革故之后另立新局。宜建立新制，忌半途生变。", 1},
    {"震为雷", 3, 3, "惊动", "雷动而惊，惊而无害。宜果断应对，忌惊慌失措。", 0},
    {"艮为山", 6, 6, "时止", "该停就停，方能得时。宜静守，忌妄动。", 0},
    {"风山渐", 4, 6, "循序渐进", "慢慢来才是快。宜按次序，忌越级躁进。", 0},
    {"雷泽归妹", 3, 1, "归妹非正", "位置不对，本末倒置。宜正名分，忌将就。", -1},
    {"雷火丰", 3, 2, "丰大", "盛大之时，日中则昃。宜趁盛行事，忌不知止。", 0},
    {"火山旅", 2, 6, "行旅", "客居在外，行旅不定。宜谦下谨慎，忌张扬。", -1},
    {"巽为风", 4, 4, "顺入", "随风而入，柔而能入。宜以柔克刚，忌刚愎。", 0},
    {"兑为泽", 1, 1, "悦言", "言语和悦能成事，口舌亦能坏事。宜谨言，忌轻诺。", 0},
    {"风水涣", 4, 5, "涣散", "心气涣散，力量分散。宜聚拢核心，忌各自为政。", -1},
    {"水泽节", 5, 1, "节制", "有度则通，过则败。宜守规矩节流，忌放纵。", 1},
    {"风泽中孚", 4, 1, "诚信", "内诚外信，感而遂通。宜以诚待人，忌虚伪。", 1},
    {"雷山小过", 3, 6, "小有过越", "宜做小事，不宜做大事。宜谨小慎微，忌好高骛远。", 0},
    {"水火既济", 5, 2, "事已成", "事已成就，然需守成。宜维持，忌再起波澜。", 1},
    {"火水未济", 2, 5, "未成", "事未竟，还差一步。宜谨慎收尾，忌松劲。", 0},
    // 第 65 条：坎上乾下。通行本 64 章里「需」与「讼」是同一个卦象（乾上坎下），
    // 所以上下经并没有「坎上乾下」这一卦，它只作变卦出现。少了它，变卦查不到
    // 就会静默回退成乾。
    {"水天需", 5, 0, "上济", "上行而济，涉川可成。须有诚信，方能济事。", 1},
};

// ================= 所问之事 =================
typedef struct {
    const char *name; // 类别名
    const char *ask;  // 所问何事
    const char *good; // 吉时断语
    const char *flat; // 平
    const char *bad;  // 凶
} Cat;

static const Cat kCat[N_CAT] = {
    {"事业", "职位、去留、前程", "此事有大把力，宜主动去争。", "成不成全看自己肯不肯下功夫。", "阻力偏大，宜暂缓守成。"},
    {"财运", "财路、进项、还债", "财路正在转好，宜趁势多添一手。", "不增不减，守住旧的就够了。", "财上有耗损，宜先止损再图后计。"},
    {"姻缘", "缘分、对方心意", "两下有意，宜趁热把话说开。", "缘分尚浅，多见几面胜过急着挑明。", "眼下不合，宜缓一缓莫强求。"},
    {"健康", "病症、康复", "病势在退，宜静养并配合调治。", "无大碍但要养，忌熬夜与劳神。", "隐患在深处，宜尽早寻医莫拖。"},
    {"出行", "行程、归期、路上顺否", "一路顺风，宜择日即行。", "行程可成，路上留心天气与小事。", "路上多阻，宜缓行或改期。"},
    {"寻物", "失落之物、下落", "东西还在，且有找着的机会。", "还在附近，多留心旧地方。", "一时难寻，宜先放下再慢慢想。"},
    {"考试", "考运、成绩、录取", "运在身侧，宜放开去考。", "功到自然成，临场莫慌。", "临场多阻，宜再多下一分功夫。"},
    {"官非", "是非、纠纷、官司", "理在你这边，宜讲清楚。", "是非可解，须得有人从中说和。", "有理也难直说，宜寻个调停人。"},
};

// ================= 运行时状态 =================
static int s_cat = 0;
static int s_yao[N_YAO];
static int s_yao_done = 0;
static int s_phase = PH_ASK;
static int s_gua = 0;
static int s_gua2 = 0;
static int s_has_change = 0;

// ---- 绘制对象 ----
static lv_obj_t *s_cat_btn[N_CAT];
static lv_obj_t *s_cat_lbl[N_CAT];
static lv_obj_t *s_toss_btn;
static lv_obj_t *s_ask_lbl;      // 「所问：xxx」
static lv_obj_t *s_status_lbl;   // 状态行
static lv_obj_t *s_toss_lbl;     // 摇卦页：正在摇第 N 爻
static lv_obj_t *s_toss_desc;    // 摇卦页：本次摇出的爻
static lv_obj_t *s_hex_name;     // 卦象页：卦名
static lv_obj_t *s_hex_image;    // 卦象页：象意
static lv_obj_t *s_hex_note;     // 卦象页：动爻说明
static lv_obj_t *s_next_btn;     // 看断语
static lv_obj_t *s_word_name;    // 断语页：卦名 + 象意
static lv_obj_t *s_word_text;    // 断语页：卦象断语
static lv_obj_t *s_word_tail;    // 断语页：结语
static lv_obj_t *s_again_btn;    // 再问一卦

// 卦象图：本卦 / 变卦各 6 爻，每爻两段（阴爻断开）
static lv_obj_t *s_line1[N_YAO][2];
static lv_obj_t *s_mark1[N_YAO];
static lv_obj_t *s_line2[N_YAO][2];
static lv_obj_t *s_mark2[N_YAO];
static lv_obj_t *s_seq[N_YAO];
static lv_obj_t *s_zhi_lbl;      // 「之」
static lv_obj_t *s_hex_box;      // 卦象整组（起卦页整组收起）

static lv_obj_t *s_scr = NULL;
static lv_timer_t *s_toss_timer = NULL;

// ---- 字体 ----
static const lv_font_t *ui_font(void) {
    const lv_font_t *f = fontpack_lv_font_ui();
    return f != NULL ? f : fontpack_lv_font_get(25, 2);
}
static const lv_font_t *title_font(void) {
    const lv_font_t *f = fontpack_lv_font_get(30, 2);
    return f != NULL ? f : ui_font();
}

// ---- 可见性登记表 ----
#define MAX_VIS 48
static lv_obj_t *s_vis_obj[MAX_VIS];
static int s_vis_tag[MAX_VIS];
static int s_vis_n = 0;

static void reg_vis(lv_obj_t *o, int tag) {
    if (o == NULL || s_vis_n >= MAX_VIS) return;
    s_vis_obj[s_vis_n] = o;
    s_vis_tag[s_vis_n] = tag;
    s_vis_n++;
}

static bool vis_should_show(int tag) {
    if (tag == VIS_ALL) return true;
    if (tag == VIS_HEX) return s_phase >= PH_TOSS;
    if (tag == VIS_CAST) return s_phase == PH_TOSS || s_phase == PH_HEX;
    return s_phase == tag;
}

static void apply_visibility(void) {
    for (int i = 0; i < s_vis_n; ++i) {
        if (s_vis_obj[i] == NULL) continue;
        if (vis_should_show(s_vis_tag[i])) {
            lv_obj_clear_flag(s_vis_obj[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_vis_obj[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

// ---- UTF-8 边界处理 ----
static size_t utf8_char_len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

// 截到不超过 max 字节且落在完整字符边界上
static void utf8_clip(char *s, size_t max) {
    size_t len = strlen(s);
    if (len <= max) return;
    size_t end = max;
    size_t start = end;
    while (start > 0 && ((unsigned char)s[start] & 0xC0) == 0x80) start--;
    size_t need = utf8_char_len((unsigned char)s[start]);
    if (start + need > end) end = start;
    s[end] = '\0';
}

// ================= 卦象计算 =================
// 三爻（自下而上 b0=初爻）→ 先天八卦序码：0乾 1兑 2离 3震 4巽 5坎 6艮 7坤
static int trigram_of(const int *b) {
    int v = (b[0] ? 4 : 0) + (b[1] ? 2 : 0) + (b[2] ? 1 : 0);
    return 7 - v;
}

static int gua_lookup(int upper, int lower) {
    for (int i = 0; i < N_GUA_ALL; ++i) {
        if (kGua[i].upper == upper && kGua[i].lower == lower) return i;
    }
    return 0;
}

static void compute_gua(void) {
    int lo[N_YAO], lo2[N_YAO];
    int moving = 0;
    for (int i = 0; i < N_YAO; ++i) {
        lo[i] = yao_is_yang(s_yao[i]) ? 1 : 0;
        int v = s_yao[i];
        if (yao_is_moving(v)) moving++;
        // 动爻翻转
        if (v == YAO_OLD_YANG) v = YAO_YIN;
        else if (v == YAO_OLD_YIN) v = YAO_YANG;
        lo2[i] = yao_is_yang(v) ? 1 : 0;
    }
    s_gua = gua_lookup(trigram_of(&lo[3]), trigram_of(&lo[0]));
    s_gua2 = gua_lookup(trigram_of(&lo2[3]), trigram_of(&lo2[0]));
    s_has_change = moving > 0;
    if (s_zhi_lbl != NULL) {
        if (s_has_change) lv_obj_clear_flag(s_zhi_lbl, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_zhi_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

static const char *yao_name(int v) {
    switch (v) {
        case YAO_OLD_YANG: return "老阳（阳爻动）";
        case YAO_YANG: return "少阳（阳爻静）";
        case YAO_YIN: return "少阴（阴爻静）";
        default: return "老阴（阴爻动）";
    }
}

// ================= 持久化 =================
static void save_game(void) {
    FILE *fp = (FILE *)dynapp_fopen_data("liuyao.txt", "w");
    if (fp == NULL) return;
    fprintf(fp, "LYAO1 %d", s_cat);
    for (int i = 0; i < N_YAO; ++i) fprintf(fp, " %d", s_yao[i]);
    fprintf(fp, " %d %d %d %d %d\n", s_yao_done, s_phase, s_has_change, s_gua, s_gua2);
    fclose(fp);
}

static int load_game(void) {
    FILE *fp = (FILE *)dynapp_fopen_data("liuyao.txt", "r");
    if (fp == NULL) return 0;
    char magic[16] = {0};
    int cat = 0, yao[N_YAO] = {0}, done = 0, phase = 0, chg = 0, g1 = 0, g2 = 0;
    if (fscanf(fp, "%15s", magic) != 1 || strcmp(magic, "LYAO1") != 0) {
        fclose(fp);
        return 0;
    }
    if (fscanf(fp, "%d", &cat) != 1 || cat < 0 || cat >= N_CAT) {
        fclose(fp);
        return 0;
    }
    for (int i = 0; i < N_YAO; ++i) {
        if (fscanf(fp, "%d", &yao[i]) != 1 || yao[i] < 0 || yao[i] > 3) {
            fclose(fp);
            return 0;
        }
    }
    if (fscanf(fp, "%d %d %d", &done, &phase, &chg) != 3) {
        fclose(fp);
        return 0;
    }
    // 本卦/变卦序号是给外部脚本核对的，缺了也不影响继续玩
    if (fscanf(fp, "%d %d", &g1, &g2) != 2) {
        g1 = 0;
        g2 = 0;
    }
    fclose(fp);
    s_cat = cat;
    for (int i = 0; i < N_YAO; ++i) s_yao[i] = yao[i];
    s_yao_done = done < 0 ? 0 : (done > N_YAO ? N_YAO : done);
    s_phase = phase < 0 ? PH_ASK : (phase > PH_WORD ? PH_ASK : phase);
    s_has_change = chg ? 1 : 0;
    compute_gua();
    return 1;
}

// ================= 绘制 =================
static void set_label(lv_obj_t *lbl, const char *text) {
    if (lbl == NULL) return;
    lv_label_set_text(lbl, text);
}

// 卦象线段：阴爻是两段（中间留缝），阳爻是一整段。
// 可见性由本函数自己管（登记表会把阴爻的第二段也点亮），整组卦象另有一个容器开关。
static void make_bar(lv_obj_t *a, lv_obj_t *b, int x, int y, int value, bool col_visible) {
    const bool yang = yao_is_yang(value);
    if (a != NULL) {
        lv_obj_set_style_bg_color(a, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(a, LV_OPA_COVER, 0);
        lv_obj_set_size(a, yang ? LINE_W : SEG_W, LINE_H);
        lv_obj_set_pos(a, x, y);
        if (col_visible) lv_obj_clear_flag(a, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(a, LV_OBJ_FLAG_HIDDEN);
    }
    if (b != NULL) {
        if (!yang && col_visible) {
            lv_obj_set_size(b, SEG_W, LINE_H);
            lv_obj_set_pos(b, x + SEG_W + SEG_GAP, y);
            lv_obj_clear_flag(b, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void draw_yao(int idx, int value) {
    const int y = HEX_Y0 + (N_YAO - 1 - idx) * ROW_H;
    make_bar(s_line1[idx][0], s_line1[idx][1], HEX_X1, y, value, true);
    if (s_mark1[idx] != NULL) {
        if (yao_is_moving(value)) lv_obj_clear_flag(s_mark1[idx], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_mark1[idx], LV_OBJ_FLAG_HIDDEN);
    }
    // 变卦：动爻翻转后的样子；没有动爻就不显示变卦列
    int v2 = value;
    if (value == YAO_OLD_YANG) v2 = YAO_YIN;
    else if (value == YAO_OLD_YIN) v2 = YAO_YANG;
    make_bar(s_line2[idx][0], s_line2[idx][1], HEX_X2, y, v2, s_has_change);
    if (s_mark2[idx] != NULL) {
        if (s_has_change && yao_is_moving(value)) lv_obj_clear_flag(s_mark2[idx], LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(s_mark2[idx], LV_OBJ_FLAG_HIDDEN);
    }
}

static void draw_hexagram(void) {
    for (int i = 0; i < N_YAO; ++i) {
        draw_yao(i, s_yao[i]);
    }
}

static void render_cats(void) {
    for (int i = 0; i < N_CAT; ++i) {
        if (s_cat_btn[i] == NULL) continue;
        const bool sel = (i == s_cat);
        lv_obj_set_style_bg_color(s_cat_btn[i], sel ? lv_color_black() : lv_color_white(), 0);
        lv_obj_set_style_bg_opa(s_cat_btn[i], LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(s_cat_lbl[i], sel ? lv_color_white() : lv_color_black(), 0);
    }
}

static void update_ask_label(void) {
    char buf[64];
    snprintf(buf, sizeof(buf), "所问：%s", kCat[s_cat].name);
    utf8_clip(buf, sizeof(buf) - 1);
    set_label(s_ask_lbl, buf);
}

static void update_status(void) {
    if (s_status_lbl == NULL) return;
    switch (s_phase) {
        case PH_ASK:
            set_label(s_status_lbl, "选一件事 · 点「起卦」摇六爻");
            break;
        case PH_TOSS:
            set_label(s_status_lbl, "莫急，等它摇完");
            break;
        case PH_HEX:
            set_label(s_status_lbl, "点「看断语」或按侧键翻页");
            break;
        default:
            set_label(s_status_lbl, "点「再问一卦」重摇");
            break;
    }
}

static void render_hex_info(void) {
    char buf[96];
    set_label(s_hex_name, kGua[s_gua].name);
    set_label(s_hex_image, kGua[s_gua].image);
    if (s_has_change) {
        snprintf(buf, sizeof(buf), "动爻在变 · 之卦 %s", kGua[s_gua2].name);
    } else {
        snprintf(buf, sizeof(buf), "六爻不变 · 静卦");
    }
    utf8_clip(buf, sizeof(buf) - 1);
    set_label(s_hex_note, buf);
}

static void render_word(void) {
    char buf[160];
    snprintf(buf, sizeof(buf), "%s · %s", kGua[s_gua].name, kGua[s_gua].image);
    utf8_clip(buf, sizeof(buf) - 1);
    set_label(s_word_name, buf);
    snprintf(buf, sizeof(buf), "%s", kGua[s_gua].word);
    utf8_clip(buf, sizeof(buf) - 1);
    set_label(s_word_text, buf);
    const char *tail = kCat[s_cat].flat;
    if (kGua[s_gua].tone > 0) tail = kCat[s_cat].good;
    else if (kGua[s_gua].tone < 0) tail = kCat[s_cat].bad;
    snprintf(buf, sizeof(buf), "%s", tail);
    utf8_clip(buf, sizeof(buf) - 1);
    set_label(s_word_tail, buf);
}

// ================= 交互 =================
static void goto_phase(int p) {
    s_phase = p;
    apply_visibility();
    update_ask_label();
    update_status();
    if (p == PH_HEX) {
        compute_gua();
        draw_hexagram();
        render_hex_info();
    } else if (p == PH_WORD) {
        render_word();
    } else if (p == PH_ASK) {
        render_cats();
    }
    save_game();
}

static void toss_step(void) {
    if (s_yao_done >= N_YAO) return;
    int backs = 0;
    for (int i = 0; i < 3; ++i) {
        if (esp_random() % 2) backs++;
    }
    int v;
    const char *desc;
    if (backs == 3) {
        v = YAO_OLD_YANG;
        desc = "三背";
    } else if (backs == 2) {
        v = YAO_YANG;
        desc = "两背一字";
    } else if (backs == 1) {
        v = YAO_YIN;
        desc = "一背两字";
    } else {
        v = YAO_OLD_YIN;
        desc = "三字";
    }
    s_yao[s_yao_done] = v;
    s_yao_done++;
    draw_yao(s_yao_done - 1, v);

    char buf[64];
    snprintf(buf, sizeof(buf), "正在摇第 %d 爻", s_yao_done);
    utf8_clip(buf, sizeof(buf) - 1);
    set_label(s_toss_lbl, buf);
    snprintf(buf, sizeof(buf), "%s · %s", desc, yao_name(v));
    utf8_clip(buf, sizeof(buf) - 1);
    set_label(s_toss_desc, buf);

    if (s_yao_done >= N_YAO) {
        if (s_toss_timer != NULL) {
            lv_timer_delete(s_toss_timer);
            s_toss_timer = NULL;
        }
        compute_gua();
        goto_phase(PH_HEX);
        dynapp_epd_full_refresh();
    }
}

static void toss_cb(lv_timer_t *tmr) {
    (void)tmr;
    toss_step();
}

// 重新起卦前把上一卦的爻线收干净：否则摇到第 1 爻时，
// 上一卦画好的另外 5 爻还留在屏上
static void clear_hex_draw(void) {
    for (int i = 0; i < N_YAO; ++i) {
        for (int k = 0; k < 2; ++k) {
            if (s_line1[i][k] != NULL) lv_obj_add_flag(s_line1[i][k], LV_OBJ_FLAG_HIDDEN);
            if (s_line2[i][k] != NULL) lv_obj_add_flag(s_line2[i][k], LV_OBJ_FLAG_HIDDEN);
        }
        if (s_mark1[i] != NULL) lv_obj_add_flag(s_mark1[i], LV_OBJ_FLAG_HIDDEN);
        if (s_mark2[i] != NULL) lv_obj_add_flag(s_mark2[i], LV_OBJ_FLAG_HIDDEN);
    }
}

static void start_toss(void) {
    s_yao_done = 0;
    memset(s_yao, 0, sizeof(s_yao));
    s_has_change = 0;
    clear_hex_draw();
    goto_phase(PH_TOSS);
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "正在摇第 1 爻");
        utf8_clip(buf, sizeof(buf) - 1);
        set_label(s_toss_lbl, buf);
        set_label(s_toss_desc, "铜钱在手里，晃三晃……");
    }
    if (s_toss_timer != NULL) lv_timer_delete(s_toss_timer);
    s_toss_timer = lv_timer_create(toss_cb, 1100, NULL);
    save_game();
}

static void on_toss_clicked(lv_event_t *e) {
    (void)e;
    if (s_phase != PH_ASK) return;
    start_toss();
}

static void on_cat_clicked(lv_event_t *e) {
    const int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= N_CAT) return;
    s_cat = i;
    render_cats();
    update_ask_label();
    save_game();
}

static void on_next_clicked(lv_event_t *e) {
    (void)e;
    if (s_phase != PH_HEX) return;
    goto_phase(PH_WORD);
    dynapp_epd_full_refresh();
}

static void clear_hex(void) {
    s_yao_done = 0;
    memset(s_yao, 0, sizeof(s_yao));
    s_has_change = 0;
}

static void on_again_clicked(lv_event_t *e) {
    (void)e;
    clear_hex();
    goto_phase(PH_ASK);
    dynapp_epd_full_refresh();
}

static int liuyao_on_vk_key(const char *key) {
    if (key == NULL) return 0;
    if (strcmp(key, "vk_home") == 0 || strcmp(key, "esc") == 0 || strcmp(key, "back") == 0) {
        dynapp_screen_go_home();
        return 1;
    }
    // 这台设备只有 HOME / 上一页(vk_prev) / 下一页(vk_next)，没有确定键：
    // 起卦页用侧键在八件事之间选，「起卦」靠点按；
    // 看卦页 vk_next = 看断语，断语页 vk_next = 再问一卦，vk_prev = 退回上一步。
    if (strcmp(key, "vk_prev") == 0) {
        if (s_phase == PH_ASK) {
            s_cat = (s_cat + N_CAT - 1) % N_CAT;
            render_cats();
            update_ask_label();
            save_game();
            return 1;
        }
        if (s_phase == PH_HEX) {
            goto_phase(PH_ASK);
            save_game();
            return 1;
        }
        if (s_phase == PH_WORD) {
            goto_phase(PH_HEX);
            return 1;
        }
        return 0;
    }
    if (strcmp(key, "vk_next") == 0) {
        if (s_phase == PH_ASK) {
            s_cat = (s_cat + 1) % N_CAT;
            render_cats();
            update_ask_label();
            save_game();
            return 1;
        }
        if (s_phase == PH_HEX) {
            goto_phase(PH_WORD);
            dynapp_epd_full_refresh();
            return 1;
        }
        if (s_phase == PH_WORD) {
            clear_hex();
            goto_phase(PH_ASK);
            return 1;
        }
        return 0;
    }
    return 0;
}

// ================= 界面 =================
static lv_obj_t *make_btn(lv_obj_t *parent, const char *text, int w, int h, lv_event_cb_t cb, void *ud,
                          lv_obj_t **out_lbl) {
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_remove_style_all(btn);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(btn, lv_color_black(), 0);
    lv_obj_set_style_border_width(btn, 2, 0);
    lv_obj_set_style_radius(btn, 8, 0);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    if (cb != NULL) lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, ud);
    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    const lv_font_t *f = ui_font();
    if (f != NULL) lv_obj_set_style_text_font(lbl, f, 0);
    lv_obj_set_style_text_color(lbl, lv_color_black(), 0);
    lv_obj_center(lbl);
    lv_obj_clear_flag(lbl, LV_OBJ_FLAG_CLICKABLE);
    if (out_lbl != NULL) *out_lbl = lbl;
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
    if (s_toss_timer != NULL) {
        lv_timer_delete(s_toss_timer);
        s_toss_timer = NULL;
    }
    save_game();
    s_scr = NULL;
}

static void *liuyao_create(void) {
    lv_obj_t *scr = lv_obj_create(NULL);
    s_scr = scr;
    lv_obj_remove_style_all(scr);
    lv_obj_set_size(scr, 480, 800);
    lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_screen_deleted, LV_EVENT_DELETE, NULL);

    dynapp_screen_status_bar(scr);
    dynapp_vk_attach(scr, "liuyao", liuyao_create, liuyao_on_vk_key);

    if (!load_game()) {
        s_cat = 0;
        memset(s_yao, 0, sizeof(s_yao));
        s_yao_done = 0;
        s_phase = PH_ASK;
        s_has_change = 0;
    }

    const lv_font_t *f_ui = ui_font();
    const lv_font_t *f_title = title_font();

    // 标题
    lv_obj_t *title = make_label(scr, "六爻占卜", 16, 46, 240, f_title, false);
    reg_vis(title, VIS_ALL);
    // 标题右侧：所问
    s_ask_lbl = make_label(scr, "", 250, 56, 214, f_ui, false);
    lv_obj_align(s_ask_lbl, LV_ALIGN_TOP_RIGHT, -16, 56);
    reg_vis(s_ask_lbl, VIS_HEX);

    // ---- 起卦页：八件所问之事 ----
    lv_obj_t *ask_ttl = make_label(scr, "所问之事", 16, 100, 200, f_ui, false);
    reg_vis(ask_ttl, PH_ASK);
    for (int i = 0; i < N_CAT; ++i) {
        const int col = i % 2, row = i / 2;
        const int x = 16 + col * 236;
        const int y = 134 + row * 64;
        lv_obj_t *lbl = NULL;
        lv_obj_t *btn = make_btn(scr, kCat[i].name, 220, 54, on_cat_clicked, (void *)(intptr_t)i, &lbl);
        lv_obj_set_pos(btn, x, y);
        s_cat_btn[i] = btn;
        s_cat_lbl[i] = lbl;
        reg_vis(btn, PH_ASK);
    }
    s_toss_btn = make_btn(scr, "起卦", 220, 54, on_toss_clicked, NULL, NULL);
    lv_obj_set_pos(s_toss_btn, 130, 410);
    reg_vis(s_toss_btn, PH_ASK);

    // ---- 摇卦页 ----
    s_toss_lbl = make_label(scr, "正在摇第 1 爻", 16, 104, 448, f_ui, false);
    reg_vis(s_toss_lbl, PH_TOSS);
    s_toss_desc = make_label(scr, "", 16, 420, 448, f_ui, true);
    reg_vis(s_toss_desc, PH_TOSS);

    // ---- 卦象页 ----
    s_hex_name = make_label(scr, "", 16, 424, 240, f_title, false);
    reg_vis(s_hex_name, PH_HEX);
    s_hex_image = make_label(scr, "", 16, 470, 240, f_ui, false);
    reg_vis(s_hex_image, PH_HEX);
    s_hex_note = make_label(scr, "", 16, 512, 448, f_ui, false);
    reg_vis(s_hex_note, PH_HEX);
    s_next_btn = make_btn(scr, "看断语", 220, 54, on_next_clicked, NULL, NULL);
    lv_obj_set_pos(s_next_btn, 130, 700);
    reg_vis(s_next_btn, PH_HEX);

    // ---- 断语页 ----
    s_word_name = make_label(scr, "", 16, 104, 448, f_ui, false);
    reg_vis(s_word_name, PH_WORD);
    s_word_text = make_label(scr, "", 16, 150, 448, f_ui, true);
    reg_vis(s_word_text, PH_WORD);
    s_word_tail = make_label(scr, "", 16, 250, 448, f_ui, true);
    reg_vis(s_word_tail, PH_WORD);
    s_again_btn = make_btn(scr, "再问一卦", 220, 54, on_again_clicked, NULL, NULL);
    lv_obj_set_pos(s_again_btn, 130, 700);
    reg_vis(s_again_btn, PH_WORD);

    // ---- 卦象图（六爻图形，整组放进一个容器统一开关） ----
    s_hex_box = lv_obj_create(scr);
    lv_obj_remove_style_all(s_hex_box);
    lv_obj_set_pos(s_hex_box, 0, 0);
    lv_obj_set_size(s_hex_box, 480, 800);
    lv_obj_set_style_bg_opa(s_hex_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_layout(s_hex_box, LV_LAYOUT_NONE, 0);
    lv_obj_clear_flag(s_hex_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(s_hex_box, LV_OBJ_FLAG_CLICKABLE);
    reg_vis(s_hex_box, VIS_CAST);

    static const char *kSeq[N_YAO] = {"初", "二", "三", "四", "五", "上"};
    for (int i = 0; i < N_YAO; ++i) {
        const int y = HEX_Y0 + (N_YAO - 1 - i) * ROW_H;
        s_seq[i] = make_label(s_hex_box, kSeq[i], 18, y - 6, 34, f_ui, false);
        for (int k = 0; k < 2; ++k) {
            lv_obj_t *seg = lv_obj_create(s_hex_box);
            lv_obj_remove_style_all(seg);
            lv_obj_set_style_bg_color(seg, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
            lv_obj_clear_flag(seg, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_size(seg, 0, 0);
            lv_obj_add_flag(seg, LV_OBJ_FLAG_HIDDEN);
            s_line1[i][k] = seg;
        }
        s_mark1[i] = make_label(s_hex_box, "动", HEX_X1 + LINE_W + 6, y - 7, 30, f_ui, false);
        lv_obj_add_flag(s_mark1[i], LV_OBJ_FLAG_HIDDEN);
        for (int k = 0; k < 2; ++k) {
            lv_obj_t *seg = lv_obj_create(s_hex_box);
            lv_obj_remove_style_all(seg);
            lv_obj_set_style_bg_color(seg, lv_color_black(), 0);
            lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
            lv_obj_clear_flag(seg, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_size(seg, 0, 0);
            lv_obj_add_flag(seg, LV_OBJ_FLAG_HIDDEN);
            s_line2[i][k] = seg;
        }
        s_mark2[i] = make_label(s_hex_box, "动", HEX_X2 + LINE_W + 6, y - 7, 30, f_ui, false);
        lv_obj_add_flag(s_mark2[i], LV_OBJ_FLAG_HIDDEN);
    }
    s_zhi_lbl = make_label(s_hex_box, "之", 232, 258, 40, f_title, false);
    lv_obj_add_flag(s_zhi_lbl, LV_OBJ_FLAG_HIDDEN);

    // 状态行
    s_status_lbl = make_label(scr, "", 16, 660, 448, f_ui, false);
    reg_vis(s_status_lbl, VIS_ALL);

    render_cats();
    update_ask_label();
    update_status();
    if (s_phase == PH_HEX || s_phase == PH_WORD) {
        compute_gua();
        draw_hexagram();
        render_hex_info();
        if (s_phase == PH_WORD) render_word();
    } else if (s_phase == PH_TOSS) {
        // 上次退出时正在摇卦：把已摇的爻画回来，接着摇剩下的
        for (int i = 0; i < s_yao_done; ++i) draw_yao(i, s_yao[i]);
        goto_phase(PH_TOSS);
        if (s_toss_timer != NULL) lv_timer_delete(s_toss_timer);
        s_toss_timer = lv_timer_create(toss_cb, 1100, NULL);
    }
    apply_visibility();
    save_game();
    return scr;
}

void metalio_app_entry(metalio_app_desc_t *out) {
    out->abi_version = METALIO_DYNAPP_ABI_VERSION;
    out->id = "liuyao";
    out->name = "六爻占卜";
    out->icon = "icon.png";
    out->create = liuyao_create;
}
