#!/usr/bin/env python3
"""生成城市库头文件 tools/weather_app/main/city_db.h。

数据来源：
  - 城市清单：内置（全国地级市 + 港澳台），带省份用于校验
  - 经纬度：open-meteo geocoding API（免 key）
  - 拼音首字母：pypinyin

为什么用拼音查地���：
  open-meteo 的 geocoding **中文字段索引不全**（常州/温州/厦门/朔州… 全查不到），
  但拼音名全能查到。查回来后用中文的 admin2 字段校验是不是同一个市。

产物是一张**纯字符数组**（不是指针表）——动态应用里全局 const char* 指针表
会触发 LoadProhibited 崩机，必须用纯字符数组。

每行格式：名称|首字母|纬度(度×10000)|经度(度×10000)

用法：
  /tmp/pinyinenv/bin/python gen_cities.py            # 生成 city_db.h
  /tmp/pinyinenv/bin/python gen_cities.py --fetch    # 忽略缓存重新拉
"""
import argparse
import json
import os
import re
import sys
import time
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
CACHE = os.path.join(HERE, "city_geo_cache.json")
OUT = os.path.join(HERE, "main", "city_db.h")

# ---- 城市清单：省份 -> 城市 ----
PROVINCES = {
    "北京": "北京",
    "天津": "天津",
    "上海": "上海",
    "重庆": "重庆",
    "河北": "石家庄 唐山 秦皇岛 邯郸 邢台 保定 张家口 承德 沧州 廊坊 衡水",
    "山西": "太原 大同 阳泉 长治 晋城 朔州 晋中 运城 忻州 临汾 吕梁",
    "内蒙古": "呼和浩特 包头 乌海 赤峰 通辽 鄂尔多斯 呼伦贝尔 乌兰察布 巴彦淖尔 兴安 锡林郭勒 阿拉善",
    "辽宁": "沈阳 大连 鞍山 抚顺 本溪 丹东 锦州 营口 阜新 辽阳 盘锦 铁岭 朝阳 葫芦岛",
    "吉林": "长春 吉林 四平 辽源 通化 白山 松原 白城 延边",
    "黑龙江": "哈尔滨 齐齐哈尔 鸡西 鹤岗 双鸭山 大庆 伊春 佳木斯 七台河 牡丹江 黑河 绥化 大兴安岭",
    "江苏": "南京 无锡 徐州 常州 苏州 南通 连云港 淮安 盐城 扬州 镇江 泰州 宿迁",
    "浙江": "杭州 宁波 温州 嘉兴 湖州 绍兴 金华 衢州 舟山 台州 丽水",
    "安徽": "合肥 芜湖 蚌埠 淮南 马鞍山 淮北 铜陵 安庆 黄山 滁州 阜阳 宿州 六安 亳州 池州 宣城",
    "福建": "福州 厦门 莆田 三明 泉州 漳州 南平 龙岩 宁德",
    "江西": "南昌 景德镇 萍乡 九江 新余 鹰潭 赣州 吉安 宜春 抚州 上饶",
    "山东": "济南 青岛 淄博 枣庄 东营 烟台 潍坊 济宁 泰安 威海 日照 临沂 德州 聊城 滨州 菏泽",
    "河南": "郑州 开封 洛阳 平顶山 安阳 鹤壁 新乡 焦作 濮阳 许昌 漯河 三门峡 南阳 商丘 信阳 周口 驻马店",
    "湖北": "武汉 黄石 十堰 宜昌 襄阳 鄂州 荆门 孝感 荆州 黄冈 咸宁 随州 恩施",
    "湖南": "长沙 株洲 湘潭 衡阳 邵阳 岳阳 常德 张家界 益阳 郴州 永州 怀化 娄底 湘西",
    "广东": "广州 深圳 珠海 汕头 佛山 韶关 河源 梅州 惠州 汕尾 东莞 中山 潮州 揭阳 云浮 清远 肇庆 阳江 茂名 湛江",
    "广西": "南宁 柳州 桂林 梧州 北海 防城港 钦州 贵港 玉林 百色 贺州 河池 来宾 崇左",
    "海南": "海口 三亚 儋州",
    "四川": "成都 自贡 攀枝花 泸州 德阳 绵阳 广元 遂宁 内江 乐山 南充 眉山 宜宾 广安 达州 雅安 巴中 资阳 阿坝 甘孜 凉山",
    "贵州": "贵阳 六盘水 遵义 安顺 毕节 铜仁 黔西南 黔东南 黔南",
    "云南": "昆明 曲靖 玉溪 保山 昭通 丽江 普洱 临沧 楚雄 红河 文山 西双版纳 大理 德宏 怒江 迪庆",
    "西藏": "拉萨 日喀则 昌都 林芝 山南 那曲 阿里",
    "陕西": "西安 铜川 宝鸡 咸阳 渭南 延安 榆林 安康 商洛",
    "甘肃": "兰州 嘉峪关 金昌 白银 天水 武威 张掖 平凉 酒泉 庆阳 定西 陇南 临夏 甘南",
    "青海": "西宁 海东 海北 黄南 果洛 玉树 海西",
    "宁夏": "银川 石嘴山 吴忠 固原 中卫",
    "新疆": "乌鲁木齐 克拉玛依 吐鲁番 哈密 昌吉 博尔塔拉 巴音郭楞 阿克苏 克孜勒苏 喀什 和田 伊犁 塔城 阿勒泰",
    "台湾": "台北 高雄 台中 台南 新北 基隆 新竹 嘉义 桃园 苗栗 彰化 南投 云林 屏东 宜兰 花莲 台东 澎湖",
    "香港": "香港",
    "澳门": "澳门",
}

# geocoding 挑不出 / 挑错的，手工指定（名称 -> (纬度, 经度)）
OVERRIDE = {
    "香港": (22.3193, 114.1694),
    "澳门": (22.1987, 113.5439),
    "台北": (25.0330, 121.5654),
    "高雄": (22.6273, 120.3014),
    "台中": (24.1477, 120.6736),
    "台南": (22.9999, 120.2270),
    "新北": (25.0120, 121.4657),
    "基隆": (25.1276, 121.7392),
    "新竹": (24.8138, 120.9675),
    "嘉义": (23.4801, 120.4491),
    "桃园": (24.9934, 121.3010),
    "苗栗": (24.5602, 120.8214),
    "彰化": (24.0685, 120.5424),
    "南投": (23.9609, 120.9719),
    "云林": (23.7092, 120.4313),
    "屏东": (22.6813, 120.4879),
    "宜兰": (24.7021, 121.7378),
    "花莲": (23.9871, 121.6015),
    "台东": (22.7583, 121.1444),
    "澎湖": (23.5712, 119.5793),
    # 地名带"盟/州/地区"后缀的，admin2 里对不上，只能手工给
    "西双版纳": (22.0017, 100.7979),
    "德宏": (24.4337, 98.5859),
    "怒江": (25.8500, 98.8500),
    "迪庆": (27.8260, 99.7060),
    "伊犁": (43.9166, 83.0000),
    "吐鲁番": (42.9476, 89.1841),
    "克拉玛依": (45.5809, 84.8892),
    "阿里": (32.5000, 80.0000),
    "那曲": (31.4760, 92.0600),
    "昌都": (31.1369, 97.1785),
    "山南": (29.2370, 91.7660),
    "日喀则": (29.2670, 88.8800),
    "甘南": (34.9860, 102.9110),
    "临夏": (35.5990, 103.2100),
    "陇南": (33.3880, 104.9210),
    "定西": (35.5800, 104.6260),
    "庆阳": (35.7090, 107.6380),
    "酒泉": (39.7320, 98.4940),
    "海西": (37.3700, 97.3700),
    "玉树": (33.0040, 97.0080),
    "果洛": (34.4700, 100.2400),
    "黄南": (35.5200, 102.0200),
    "海北": (36.9600, 100.9800),
    "海东": (36.5000, 102.1100),
    "固原": (36.0157, 106.2853),
    "吴忠": (37.9976, 106.1988),
    "石嘴山": (38.9841, 106.3844),
    "中卫": (37.5150, 105.1968),
    "阿勒泰": (47.8480, 88.1380),
    "塔城": (46.7460, 82.9850),
    "克孜勒苏": (39.7120, 76.1720),
    "喀什": (39.4704, 75.9898),
    "和田": (37.1107, 79.9278),
    "阿克苏": (41.1680, 80.2650),
    "巴音郭楞": (41.7640, 86.1500),
    "博尔塔拉": (44.8940, 82.0740),
    "昌吉": (44.0140, 87.3040),
    "哈密": (42.8188, 93.5152),
    "巴中": (31.8670, 106.7530),
    "资阳": (30.1220, 104.6280),
    "雅安": (29.9800, 103.0010),
    "达州": (31.2100, 107.4680),
    "广安": (30.4600, 106.6400),
    "凉山": (27.8940, 102.2640),
    "甘孜": (30.0500, 101.9630),
    "阿坝": (31.9000, 102.2200),
    "眉山": (30.0750, 103.8480),
    "宜宾": (28.7510, 104.6430),
    "南充": (30.8370, 106.1100),
    "乐山": (29.5520, 103.7660),
    "内江": (29.5800, 105.0580),
    "遂宁": (30.5330, 105.5930),
    "广元": (32.4350, 105.8440),
    "毕节": (27.2970, 105.2860),
    "黔南": (26.2580, 107.5180),
    "黔东南": (26.5830, 107.9770),
    "黔西南": (25.0890, 104.8980),
    "铜仁": (27.7180, 109.1920),
    "安顺": (26.2450, 105.9470),
    "六盘水": (26.5850, 104.8300),
    "临沧": (23.8860, 100.0870),
    "普洱": (22.7770, 100.9720),
    "楚雄": (25.0320, 101.5460),
    "文山": (23.3690, 104.2440),
    "红河": (23.3800, 103.3840),
    # 拼音查会撞到同音别处（辽源→揭阳、延边→平凉、益阳→宜阳、儋州→丹洲）
    "辽源": (42.8876, 125.1454),
    "延边": (42.9048, 129.5091),
    "益阳": (28.5539, 112.3550),
    "儋州": (19.5212, 109.5764),
    "七台河": (45.7710, 131.0030),
    "锡林郭勒": (43.9333, 116.0861),
    "许昌": (34.0357, 113.8478),
    "百色": (23.9022, 106.6186),
    # 拼音会撞到同音别处（马鞍山→汉中、泰安→鞍山机场、湘西→香溪镇）
    "马鞍山": (31.6704, 118.5065),
    "泰安": (36.1994, 117.0879),
    "湘西": (28.2640, 109.7380),
    "淮安": (33.5975, 119.0212),
    "阿拉善": (38.8446, 105.7345),
    # pypinyin 的多音字读音和地名实际读法对不上（长治→zhangzhi、吕梁→lvliang）
    "长治": (36.1839, 113.1053),
    "吕梁": (37.5192, 111.1444),
    "朝阳": (41.5703, 120.4586),
}

# 拼音首字母手工订正（pypinyin 对多音字给的不是地名实际读音）
INITIAL_FIX = {
    "长治": "C",
    "乐清": "Y",
    "铅山": "Y",
    "单县": "S",
    "台州": "T",
    "华山": "H",
    "华阴": "H",
    "六安": "L",
    "亳州": "B",
    "乐山": "L",
    "蚌埠": "B",
    "番禺": "P",
    "东莞": "D",
    "枞阳": "Z",
    "歙县": "S",
    "黟县": "Y",
    "砀山": "D",
    "潜山": "Q",
    "涡阳": "G",
    "涡县": "G",
    "郫都": "P",
    "筠连": "J",
    "珙县": "G",
    "犍为": "Q",
    "郫县": "P",
    "荥经": "Y",
    "荥阳": "X",
    "洪雅": "H",
    "洪江": "H",
    "屏山": "P",
    "鄢陵": "Y",
    "睢县": "S",
    "秭归": "Z",
    "耒阳": "L",
    "儋州": "D",
    "亳州": "B",
}

# 行政中心优先级：省/州首府 > 人口普查首府 > 一般聚落
FEATURE_RANK = {"PPLA": 0, "PPLC": 1, "PPLA2": 2, "ADM1": 3, "PPL": 4, "PPLA3": 5, "PPLA4": 6, "PPLL": 7}
PROV_SUFFIX = ("特别行政区", "自治区", "自治州", "地区", "盟", "省", "市", "县", "区")


def strip_suffix(s):
    for suf in PROV_SUFFIX:
        if s.endswith(suf) and len(s) > len(suf):
            return s[: -len(suf)]
    return s


def norm_prov(s):
    return strip_suffix((s or "").strip())


def query(q):
    url = ("https://geocoding-api.open-meteo.com/v1/search?name="
           + urllib.parse.quote(q) + "&count=10&language=zh&format=json")
    for attempt in range(4):
        try:
            with urllib.request.urlopen(url, timeout=20) as r:
                data = json.loads(r.read().decode())
            results = data.get("results") or []
            if results:
                return results
            # 空结果多半是被限流了，退避一下再试
            time.sleep(0.8 * (attempt + 1))
        except Exception as e:  # noqa: BLE001
            print(f"  ! {q}: {e}", file=sys.stderr)
            time.sleep(1.5)
    return []


def fetch_geo(city, prov, cache):
    key = f"{prov}/{city}"
    if key in cache:
        return cache[key]
    from pypinyin import lazy_pinyin
    py = re.sub(r"[^a-z]", "", "".join(lazy_pinyin(city)).lower())

    best = None
    for q in ([py] if py else []) + [city]:
        results = query(q)
        if not results:
            continue
        for item in results:
            if item.get("country_code") not in ("CN", "TW", "HK", "MO"):
                continue
            a1 = norm_prov(item.get("admin1"))
            a2 = item.get("admin2") or ""
            nm = item.get("name") or ""
            # 省份要对上（直辖市/港澳的 admin1 就是自己）
            if prov not in ("北京", "天津", "上海", "重庆", "香港", "澳门") and a1 != norm_prov(prov):
                continue
            # 城市名要出现在 admin2（中文）或 name（中文查询时）里
            if city not in a2 and city not in nm:
                continue
            rank = FEATURE_RANK.get(item.get("feature_code"), 9)
            if best is None or rank < best[0]:
                best = (rank, item)
        if best is not None:
            break
    if best is None:
        return None
    item = best[1]
    cache[key] = (round(item["latitude"], 4), round(item["longitude"], 4))
    return cache[key]


def pinyin_initial(name):
    from pypinyin import pinyin, Style
    res = pinyin(name, style=Style.FIRST_LETTER, errors="default")
    letters = "".join(r[0] if isinstance(r, (list, tuple)) else str(r) for r in res)
    letters = letters.strip().upper()
    if not letters:
        return "#"
    c = letters[0]
    return c if "A" <= c <= "Z" else "#"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fetch", action="store_true", help="忽略缓存重新拉")
    args = ap.parse_args()

    cache = {}
    if os.path.exists(CACHE) and not args.fetch:
        with open(CACHE, encoding="utf-8") as f:
            cache = {k: tuple(v) for k, v in json.load(f).items()}

    rows = []
    missing = []
    seen = set()
    for prov, cities in PROVINCES.items():
        for city in cities.split():
            if (prov, city) in seen:
                continue
            seen.add((prov, city))
            if city in OVERRIDE:
                geo = OVERRIDE[city]
            else:
                geo = fetch_geo(city, prov, cache)
                if args.fetch:
                    time.sleep(0.12)
            if geo is None:
                missing.append(f"{prov}/{city}")
                continue
            lat, lon = geo
            init = INITIAL_FIX.get(city) or pinyin_initial(city)
            rows.append((city, init, int(round(lat * 10000)), int(round(lon * 10000))))

    with open(CACHE, "w", encoding="utf-8") as f:
        json.dump(cache, f, ensure_ascii=False, indent=1)

    rows.sort(key=lambda r: (r[1], r[0]))
    lines = [
        "// 自动生成，勿手改 —— 由 tools/weather_app/gen_cities.py 生成",
        "// 城市库：" + str(len(rows)) + " 个（全国地级市 + 港澳台）",
        "// 经纬度来自 open-meteo geocoding（用拼音查、中文 admin2 校验），拼音首字母来自 pypinyin",
        "// 每行：名称|首字母|纬度(度×10000)|经度(度×10000)",
        "// 注意：这是纯字符数组，不是指针表——动态应用里全局指针表会触发 LoadProhibited",
        "static const char kCityDb[] =",
    ]
    for name, init, lat, lon in rows:
        lines.append('    "%s|%s|%d|%d\\n"' % (name, init, lat, lon))
    lines.append("    ;")
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

    print(f"生成 {OUT}：{len(rows)} 个城市")
    if missing:
        print(f"缺经纬度 {len(missing)} 个：{missing}", file=sys.stderr)
    from collections import Counter
    c = Counter(r[1] for r in rows)
    print("首字母分布:", " ".join(f"{k}{v}" for k, v in sorted(c.items())))


if __name__ == "__main__":
    main()
