#!/usr/bin/env python3
"""日历月视图格子算法回归对拍。

固件的日期换算(services/date_utils.h)驱动日历格子摆放:首日星期 +
当月天数决定每个日期落在 7 列网格的哪一格。本脚本用 Python calendar
(公历标准实现)对拍 2000..2200 全部 2412 个月,并验证月视图 6 行封顶够用。

用法(在 tools/calendar_grid_check/ 下):
    clang++ -std=c++17 -O1 -I../../main grid_dump.cc -o grid_dump
    ./grid_dump | python3 check.py
退出码 0 = 全部一致。
"""
import calendar
import sys


def main() -> int:
    bad = 0
    rows_seen = set()
    months = 0
    for line in sys.stdin:
        parts = line.split()
        if len(parts) != 5:
            continue
        y, m, fw, nd, rows = map(int, parts)
        py_fw, py_nd = calendar.monthrange(y, m)  # Monday=0
        months += 1
        rows_seen.add(rows)
        if fw != py_fw or nd != py_nd or rows > 6:
            bad += 1
            print(f"MISMATCH {y}-{m:02d}: fw={fw} nd={nd} rows={rows} | python fw={py_fw} nd={py_nd}")
    if months != 2412:
        print(f"ERROR: 期望 2412 个月,实收 {months}")
        return 1
    if bad:
        print(f"FAIL: {bad} 个月不一致")
        return 1
    print(f"OK: 2412 个月(2000-2200)全部一致;行数分布 {sorted(rows_seen)}(6 行封顶)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
