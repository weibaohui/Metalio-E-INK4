#!/usr/bin/env python3
# scripts/gen_symbols.sh —— 固件符号导出表自动生成器
#
# 用法: 固件构建完成后运行  ./scripts/gen_symbols.sh
# 产物(确定性输出,按符号名排序):
#   1. main/services/dynamic_apps/generated_symbol_table.inc  —— 编译进固件的导出表
#   2. sdk/symbols.txt —— 第三方 .so 应用可用的 API 符号清单(= SDK 文档)
#
# 策略: 候选 = 固件 ELF 中的 lv_* / dynapp_* 符号 ∪ 人工保留清单 ∪ 基础库清单,
# 然后让编译器当裁判: 以与 dynamic_app_loader.cc 完全相同的编译参数生成探测文件
# (static const void* kProbe[] = {&每个候选符号}),编译器报"未声明"的符号即剔除,
# 循环到干净为止——无论是私有符号、被 #if 关掉的声明还是宏形式差异,
# 最终产物与加载器取址上下文严格一致。
import json
import os
import re
import shlex
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ELF = os.path.join(ROOT, "build", "xiaozhi.elf")
CC_JSON = os.path.join(ROOT, "build", "compile_commands.json")
INC_OUT = os.path.join(ROOT, "main", "services", "dynamic_apps", "generated_symbol_table.inc")
SDK_OUT = os.path.join(ROOT, "sdk", "symbols.txt")

# 基础库清单(libgcc 内建函数在加载器里手写,不在此处;它们需要 extern 声明)
BASE_SYMBOLS = """
atoi calloc cJSON_AddBoolToObject cJSON_AddItemToArray cJSON_AddItemToObject
cJSON_AddNumberToObject cJSON_AddStringToObject cJSON_CreateArray cJSON_CreateBool
cJSON_CreateNumber cJSON_CreateObject cJSON_CreateString cJSON_Delete
cJSON_GetArrayItem cJSON_GetArraySize cJSON_GetObjectItem cJSON_GetObjectItemCaseSensitive
cJSON_GetStringValue cJSON_IsArray cJSON_IsBool cJSON_IsNull cJSON_IsNumber
cJSON_IsString cJSON_IsTrue cJSON_Parse cJSON_Print cJSON_PrintUnformatted
esp_random fclose fflush fgets fontpack_lv_font_get fontpack_lv_font_math fontpack_lv_font_ui fopen
fprintf fputs fread free fscanf fseek ftell fwrite gmtime_r localtime_r
malloc memcmp memcpy memmove memset mktime
radio_catalog_count radio_catalog_get radio_play radio_play_indexed
radio_player_shutdown radio_player_snapshot radio_player_start radio_set_volume radio_stop
rand realloc snprintf sprintf srand sscanf strcat strchr strcmp strcpy strdup
strlen strncmp strncpy strrchr strstr strtol time
""".split()

# 人工保留清单:历史导出表里需要但可能不在 ELF 出现的 lv_* 符号
# (static inline 未被编译出独立副本/宏别名等,地址经公开头解析)
LEGACY_LV_SYMBOLS = """
lv_obj_set_style_margin_ver lv_obj_set_style_pad_ver lv_obj_set_style_pad_all
lv_obj_clear_flag lv_obj_set_style_pad_hor
""".split()

# libgcc 内建函数(64 位整数/双精度浮点全族):不生成进 .inc(加载器文件头以 extern
# 声明手写导出),但属于 ABI 的一部分,必须列进 symbols.txt
LIBGCC_SYMBOLS = """
__adddf3 __divdf3 __divdi3 __eqdf2 __extendsfdf2 __floatsidf __floatundidf
__floatunsidf __gedf2 __gtdf2 __ledf2 __ltdf2 __moddi3 __muldf3 __muldi3
__nedf2 __negdf2 __powidf2 __subdf3 __udivdi3 __umoddi3 __unorddf2
""".split()

# 探测文件头:与 dynamic_app_loader.cc 取址上下文相同的包含集合
PROBE_INCLUDES = """
#include <lvgl.h>
#include <esp_random.h>
#include <cJSON.h>
#include "dynamic_app_shims.h"
#include "display/font/fontpack_lvgl.h"
#include "services/radio/radio_player.h"
#include "services/radio/radio_catalog.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
"""


def find_toolchain():
    """从 compile_commands.json 提取工具链 nm、g++ 与 main 组件的 -I/-D/-include 参数。"""
    if not os.path.isfile(CC_JSON):
        sys.exit(f"错误: {CC_JSON} 不存在,请先 idf.py build 固件")
    db = json.load(open(CC_JSON))
    cxx_entry = None
    for e in db:
        if "/main/" in e["file"] and e["file"].endswith(".cc"):
            cxx_entry = shlex.split(e["command"])
            break
    if cxx_entry is None:
        sys.exit("错误: compile_commands.json 中找不到 main 组件 C++ 编译命令")
    nm = os.path.join(os.path.dirname(cxx_entry[0]), "xtensa-esp32s3-elf-nm")
    if not os.path.isfile(nm):
        sys.exit(f"错误: 找不到 nm: {nm}")
    # 提取 -I / -D / -include / @文件 参数(与 tools/hello_app/build_so.sh 同款逻辑:
    # -o/-c 跳过其自身,其值因不匹配过滤自然丢弃;-include 保留其后跟参数)
    flags = []
    skip_next = False
    for p in cxx_entry[1:]:
        if skip_next:
            flags.append(p)
            skip_next = False
            continue
        if p in ("-o", "-c"):
            continue
        if p == "-include":
            flags.append(p)
            skip_next = True
            continue
        if p.startswith(("-I", "-D", "@")):
            flags.append(p)
    return nm, cxx_entry[0], flags


def elf_symbols(nm_path):
    out = subprocess.run([nm_path, "--defined-only", "-g", ELF],
                         capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit(f"错误: nm 解析失败: {out.stderr.strip()}")
    syms = set()
    for line in out.stdout.splitlines():
        parts = line.split()
        # nm --defined-only 输出: [地址] [类型] [符号名]
        # A/a=绝对符号(newlib libc 优化实现的别名), W/w=弱符号(newlib 默认实现)
        if len(parts) >= 3 and parts[-2] in ("T", "t", "D", "d", "B", "b", "R", "r", "A", "a", "W", "w"):
            syms.add(parts[-1])
    return syms


def sdk_declared_dynapp_symbols():
    """从 SDK 公共头(services.h)的 extern "C" 块里提取 dynapp_* 函数名。

    为什么需要这一层:新增的垫片函数在固件内部无人引用,链接器 --gc-sections
    会把它从 ELF 里裁掉;而导出表又是从 ELF 反推的,于是新符号永远进不了
    导出表(鸡生蛋)。SDK 头是 ABI 的唯一事实源,这里以头文件为准补齐候选,
    再交给编译器取址探测裁决——头里声明了却没实现的符号会在固件链接时报错,
    这正是我们想要的强制约束。
    """
    header = os.path.join(ROOT, "sdk", "include", "metalio", "services.h")
    if not os.path.isfile(header):
        return set()
    lines = open(header, encoding="utf-8").read().splitlines()
    try:
        start = next(i for i, l in enumerate(lines) if l.strip() == 'extern "C" {')
    except StopIteration:
        return set()
    # extern "C" 块以「整行只有一个 }」结束;块内的结构体收尾都是 `} xxx_t;`
    end = next((i for i in range(start + 1, len(lines))
                if lines[i].strip() == "}"), len(lines))
    block = "\n".join(lines[start:end])
    # 只认「标识符紧跟左括号」= 函数声明;typedef 名后面跟的是 `)` 或 `;`
    return {m.group(1) for m in re.finditer(r"\b(dynapp_[A-Za-z0-9_]+)\s*\(", block)}


def probe_compile(cxx, flags, candidates, probe_dir):
    """用与加载器相同的参数编译探测文件,返回编译器判定"未声明"的符号集。"""
    src = os.path.join(probe_dir, "probe.cc")
    body = PROBE_INCLUDES + "\nstatic const void* kProbe[] = {\n"
    for s in candidates:
        body += f"    (const void*)&{s},\n"
    body += "};\n"
    open(src, "w").write(body)
    cmd = [cxx, "-std=gnu++2b", "-fsyntax-only",
           f"-I{os.path.join(ROOT, 'main', 'services', 'dynamic_apps')}"] + flags + [src]
    out = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    bad = set(re.findall(r"error: '([A-Za-z0-9_]+)' was not declared in this scope", out.stderr))
    if not bad and out.returncode != 0:
        sys.exit("错误: 探测编译出现非符号错误:\n" + out.stderr[-2000:])
    return bad


def main():
    if not os.path.isfile(ELF):
        sys.exit(f"错误: {ELF} 不存在,请先 idf.py build 固件")
    nm, cxx, flags = find_toolchain()
    defined = elf_symbols(nm)

    lv_candidates = sorted({s for s in defined if s.startswith("lv_")} |
                           {s for s in LEGACY_LV_SYMBOLS})
    # SDK 头声明的 dynapp_* 全量并入候选(ELF 里被 gc-sections 裁掉的也在内)
    sdk_dynapp = sdk_declared_dynapp_symbols()
    elf_dynapp = {s for s in defined if s.startswith("dynapp_")}
    dynapp_candidates = sorted(elf_dynapp | sdk_dynapp)
    only_in_sdk = sorted(sdk_dynapp - elf_dynapp)
    if only_in_sdk:
        print(f"提示: {len(only_in_sdk)} 个 SDK 头声明的符号本次未出现在固件 ELF "
              f"(链接器 gc-sections 裁掉或尚未实现),仍按头文件纳入导出候选: "
              f"{only_in_sdk}")
    all_candidates = lv_candidates + dynapp_candidates + list(BASE_SYMBOLS)

    rejected = []
    bad = set()
    with tempfile.TemporaryDirectory() as probe_dir:
        for _ in range(6):
            bad = probe_compile(cxx, flags, all_candidates, probe_dir)
            if not bad:
                break
            rejected.extend(bad)
            all_candidates = [s for s in all_candidates if s not in bad]
    if bad:
        sys.exit(f"错误: 探测编译超过 6 轮仍未干净,剩余问题符号: {sorted(bad)}")

    survivors = set(all_candidates)
    lv_syms = [s for s in lv_candidates if s in survivors]
    dynapp_syms = [s for s in dynapp_candidates if s in survivors]
    base_syms = [s for s in BASE_SYMBOLS if s in survivors]
    elf_missing_base = [s for s in BASE_SYMBOLS if s not in defined]
    if elf_missing_base:
        print(f"提示: 基础清单中 {len(elf_missing_base)} 个符号固件 ELF 不存在"
              f"(编译器可能内联提供,保留与否以编译器判定为准): {elf_missing_base}")
    print(f"编译器判定通过 {len(survivors)} 个符号(lv_={len(lv_syms)} "
          f"dynapp_={len(dynapp_syms)} 基础={len(base_syms)});"
          f"剔除不可取址符号 {len(set(rejected))} 个")
    if rejected:
        uniq = sorted(set(rejected))
        print(f"  被剔除: {uniq[:20]}{' ...' if len(uniq) > 20 else ''}")

    sections = [
        ("LVGL 图形库(lv_* 前缀全量,动态应用的全部工具箱)", lv_syms),
        ("动态应用系统服务垫片(dynapp_*)", dynapp_syms),
        ("基础库(libc/cJSON/esp/fontpack/radio/时间函数)", base_syms),
    ]

    with open(INC_OUT, "w") as f:
        f.write("// 本文件由 scripts/gen_symbols.sh 自动生成,禁止手改\n")
        f.write("// 源: build/xiaozhi.elf 符号表 + 与加载器同参数的编译器取址探测\n")
        for title, syms in sections:
            f.write(f"\n    // ---- {title} ----\n")
            for s in syms:
                f.write(f"    ESP_ELFSYM_EXPORT({s}),\n")
    total = sum(len(s) for _, s in sections)

    with open(SDK_OUT, "w") as f:
        f.write("# Metalio 动态应用 SDK —— 固件导出符号清单\n")
        f.write("# 由 scripts/gen_symbols.sh 从固件 ELF 自动生成并经编译器取址验证;\n")
        f.write("# 第三方 .so 应用只能调用下列符号\n")
        f.write("# (ABI 稳定政策: 签名只增不改;破坏签名须递增 METALIO_DYNAPP_ABI_VERSION)\n")
        f.write("\n# ==== libgcc 内建函数(64 位整数/浮点运算) ====\n")
        for s in LIBGCC_SYMBOLS:
            f.write(f"{s}\n")
        for title, syms in sections:
            f.write(f"\n# ==== {title} ====\n")
            for s in syms:
                f.write(f"{s}\n")

    print(f"已生成: {os.path.relpath(INC_OUT, ROOT)} ({total} 个符号)")
    print(f"已生成: {os.path.relpath(SDK_OUT, ROOT)}")


if __name__ == "__main__":
    main()
