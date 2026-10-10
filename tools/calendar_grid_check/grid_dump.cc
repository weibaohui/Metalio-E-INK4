// tools/calendar_grid_check/grid_dump.cc —— 用固件里的真实 date_utils.h
//  dump 2000..2200 每个月的 首日星期/天数/占用行数,供 check.py 与 Python
//  calendar 对拍。编译:clang++ -std=c++17 -O1 -I../../main grid_dump.cc -o grid_dump
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "services/date_utils.h"

int main() {
    setenv("TZ", "GMT0", 1); tzset();   // 设备 TZ=GMT+0 本地历元模型
    for (int y = 2000; y <= 2200; ++y) {
        for (int m = 1; m <= 12; ++m) {
            const int fw = date_utils::WeekdayMonFirst(y, m, 1);
            const int nd = date_utils::DaysInMonth(y, m);
            const int rows = (fw + nd + 6) / 7;   // 月视图 7 列所需的行数
            printf("%d %d %d %d %d\n", y, m, fw, nd, rows);
        }
    }
    return 0;
}
