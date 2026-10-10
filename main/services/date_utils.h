#pragma once
// main/services/date_utils.h —— 日期换算公共件(公共功能沉淀,
// 见 docs/code-review-optimization-2026-10.md §2.5)。
//
// 设备为"本地历元"模型:TZ=GMT+0,epoch 即本地民用时(见 time_service)。
// 本头文件的换算全部建立在该模型上;mktime/localtime_r 之间往返安全。
//
// 正确性已用宿主机对拍固化:2000~2200 年全部 2412 个月的首日星期/天数/
// 占用行数与 Python calendar 零偏差(tools/calendar_grid_check)。

#include <cstdio>
#include <ctime>
#include <string>

namespace date_utils {

inline void EpochToYmd(time_t t, int& y, int& m, int& d) {
    struct tm tmv = {};
    localtime_r(&t, &tmv);
    y = tmv.tm_year + 1900; m = tmv.tm_mon + 1; d = tmv.tm_mday;
}

// 正午历元:避 DST/时区边缘(设备 TZ=GMT+0 本无 DST,双保险)
inline time_t YmdToNoonEpoch(int y, int m, int d) {
    struct tm tmv = {};
    tmv.tm_year = y - 1900; tmv.tm_mon = m - 1; tmv.tm_mday = d; tmv.tm_hour = 12;
    return mktime(&tmv);
}

inline std::string YmdToStr(int y, int m, int d) {
    char b[40];   // 40:年份字段理论 11 字节,-Werror 防截断
    snprintf(b, sizeof(b), "%04d-%02d-%02d", y, m, d);
    return b;
}

// 格里高利历:2100 平年/2000 闰年都正确
inline int DaysInMonth(int y, int m) {
    static const int dm[] = {31,28,31,30,31,30,31,31,30,31,30,31};
    if (m == 2) { const bool leap = (y%4==0 && y%100!=0) || (y%400==0); return leap ? 29 : 28; }
    return dm[m-1];
}

// 0=周一 .. 6=周日
inline int WeekdayMonFirst(int y, int m, int d) {
    struct tm tmv = {};
    tmv.tm_year = y-1900; tmv.tm_mon = m-1; tmv.tm_mday = d; tmv.tm_hour = 12;
    mktime(&tmv);            // tm_wday 与时区无关(mktime 按民用日填充)
    return (tmv.tm_wday + 6) % 7;
}

}  // namespace date_utils
