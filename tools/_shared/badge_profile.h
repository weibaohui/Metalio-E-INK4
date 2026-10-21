// tools/_shared/badge_profile.h —— 电子工牌 / 二维码名片 共用的配置解析
//
// 配置文件放在 app 自己的 data 目录（profile.txt），格式：
//
//   # 注释
//   [card]
//   name=张三
//   title=产品设计师
//   company=示例科技
//   dept=产品部
//   id=EMP-1024
//   phone=13800000000
//   email=zhangsan@example.com
//   url=https://example.com
//   status=今天也在搬砖
//   avatar=avatar.png
//   qr=1
//   [card]
//   name=李四
//   ...
//
// 说明：
//   - 动态应用的写权限只在自身 data/ 目录，读权限也只有自身 data/ 与 bundle，
//     所以配置文件和头像都必须放进 app 自己的 data 目录。
//   - 设备提供了 /fs/upload（相对 metalio/e-ink 的路径），可以用它把
//     profile.txt / avatar.png 推进对应 app 的 data 目录。
#ifndef BADGE_PROFILE_H
#define BADGE_PROFILE_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PROFILE_MAX_CARDS 4
#define PROFILE_LINE_MAX 256

typedef struct {
    char name[32];
    char title[32];
    char company[40];
    char dept[24];
    char empid[24];
    char phone[24];
    char email[48];
    char url[96];
    char status[48];
    char avatar[32];
    int show_qr;
} ProfileCard;

typedef struct {
    ProfileCard cards[PROFILE_MAX_CARDS];
    int n;
} ProfileSet;

static void profile_copy(char *dst, size_t cap, const char *src) {
    if (dst == NULL || cap == 0) return;
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    size_t n = strlen(src);
    if (n >= cap) n = cap - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void profile_set_field(ProfileCard *c, const char *key, const char *val) {
    if (strcmp(key, "name") == 0) profile_copy(c->name, sizeof(c->name), val);
    else if (strcmp(key, "title") == 0) profile_copy(c->title, sizeof(c->title), val);
    else if (strcmp(key, "company") == 0) profile_copy(c->company, sizeof(c->company), val);
    else if (strcmp(key, "dept") == 0) profile_copy(c->dept, sizeof(c->dept), val);
    else if (strcmp(key, "id") == 0) profile_copy(c->empid, sizeof(c->empid), val);
    else if (strcmp(key, "phone") == 0) profile_copy(c->phone, sizeof(c->phone), val);
    else if (strcmp(key, "email") == 0) profile_copy(c->email, sizeof(c->email), val);
    else if (strcmp(key, "url") == 0) profile_copy(c->url, sizeof(c->url), val);
    else if (strcmp(key, "status") == 0) profile_copy(c->status, sizeof(c->status), val);
    else if (strcmp(key, "avatar") == 0) profile_copy(c->avatar, sizeof(c->avatar), val);
    else if (strcmp(key, "qr") == 0) c->show_qr = (atoi(val) != 0);
}

/* 解析 profile 文本；返回卡片数 */
static int profile_parse(const char *text, ProfileSet *set) {
    memset(set, 0, sizeof(*set));
    if (text == NULL) return 0;
    ProfileCard *cur = NULL;
    char line[PROFILE_LINE_MAX];
    const char *p = text;
    while (*p != '\0') {
        size_t i = 0;
        while (*p != '\0' && *p != '\n' && i + 1 < sizeof(line)) {
            line[i++] = *p++;
        }
        line[i] = '\0';
        while (*p == '\n' || *p == '\r') p++;

        char *s = line;
        while (*s == ' ' || *s == '\t') s++;
        if (*s == '\0' || *s == '#') continue;
        if (*s == '[') {
            if (set->n >= PROFILE_MAX_CARDS) break;
            cur = &set->cards[set->n++];
            memset(cur, 0, sizeof(*cur));
            cur->show_qr = 1;
            continue;
        }
        char *eq = strchr(s, '=');
        if (eq == NULL || cur == NULL) continue;
        *eq = '\0';
        char *val = eq + 1;
        /* 去掉行尾空白 */
        size_t vl = strlen(val);
        while (vl > 0 && (val[vl - 1] == ' ' || val[vl - 1] == '\t')) val[--vl] = '\0';
        profile_set_field(cur, s, val);
    }
    return set->n;
}

/* 拼一份 vCard：手机扫码后可直接存成联系人 */
static int profile_build_vcard(const ProfileCard *c, char *out, size_t cap) {
    int n = snprintf(out, cap,
                     "BEGIN:VCARD\r\n"
                     "VERSION:3.0\r\n"
                     "N:%s;;;;\r\n"
                     "FN:%s\r\n"
                     "ORG:%s\r\n"
                     "TITLE:%s\r\n"
                     "NOTE:%s\r\n",
                     c->name, c->name, c->company, c->title, c->status);
    if (c->dept[0] != '\0') n += snprintf(out + n, cap > (size_t)n ? cap - n : 0, "DEPARTMENT:%s\r\n", c->dept);
    if (c->phone[0] != '\0') n += snprintf(out + n, cap > (size_t)n ? cap - n : 0, "TEL;TYPE=CELL:%s\r\n", c->phone);
    if (c->email[0] != '\0') n += snprintf(out + n, cap > (size_t)n ? cap - n : 0, "EMAIL:%s\r\n", c->email);
    if (c->url[0] != '\0') n += snprintf(out + n, cap > (size_t)n ? cap - n : 0, "URL:%s\r\n", c->url);
    n += snprintf(out + n, cap > (size_t)n ? cap - n : 0, "END:VCARD\r\n");
    return n;
}

/* 兜底默认资料：没配 profile.txt 也能看到效果 */
static void profile_default(ProfileSet *set) {
    memset(set, 0, sizeof(*set));
    set->n = 1;
    ProfileCard *c = &set->cards[0];
    profile_copy(c->name, sizeof(c->name), "你的名字");
    profile_copy(c->title, sizeof(c->title), "职位");
    profile_copy(c->company, sizeof(c->company), "公司");
    profile_copy(c->phone, sizeof(c->phone), "13800000000");
    profile_copy(c->email, sizeof(c->email), "you@example.com");
    profile_copy(c->status, sizeof(c->status), "点「怎么改」看配置方法");
    c->show_qr = 1;
}

#endif /* BADGE_PROFILE_H */
