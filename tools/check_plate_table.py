#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""校验 bsp/uart6/bsp_uart6.c 中省份 Unicode 表是否正确。"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "bsp", "uart6", "bsp_uart6.c")

# 正确映射：汉字 -> (Unicode 码点, 期望拼音/简写)
EXPECTED = {
    "京": (0x4EAC, "BJ"), "津": (0x6D25, "TJ"), "沪": (0x6CAA, "SH"), "渝": (0x6E1D, "CQ"),
    "冀": (0x5180, "HE"), "晋": (0x664B, "SX"), "蒙": (0x8499, "NM"), "辽": (0x8FBD, "LN"),
    "吉": (0x5409, "JL"), "黑": (0x9ED1, "HL"), "苏": (0x82CF, "JS"), "浙": (0x6D59, "ZJ"),
    "皖": (0x7696, "AH"), "闽": (0x95FD, "FJ"), "赣": (0x8D63, "JX"), "鲁": (0x9C81, "SD"),
    "豫": (0x8C6B, "HA"), "鄂": (0x9102, "HB"), "湘": (0x6E58, "HN"), "粤": (0x7CA4, "GD"),
    "桂": (0x6842, "GX"), "琼": (0x743C, "HI"), "川": (0x5DDD, "SC"), "蜀": (0x8700, "SC"),
    "贵": (0x8D35, "GZ"), "黔": (0x9ED4, "GZ"), "云": (0x4E91, "YN"), "滇": (0x6EC7, "YN"),
    "藏": (0x85CF, "XZ"), "陕": (0x9655, "SN"), "秦": (0x79E6, "SN"), "甘": (0x7518, "GS"),
    "陇": (0x9647, "GS"), "青": (0x9752, "QH"), "宁": (0x5B81, "NX"), "新": (0x65B0, "XJ"),
    "港": (0x6E2F, "HK"), "澳": (0x6FB3, "MO"), "台": (0x53F0, "TW"),
}

PAIR_RE = re.compile(r'\{\s*0x([0-9A-Fa-f]{4})\s*,\s*"([A-Z]{2})"\s*\}')


def main():
    if not os.path.isfile(SRC):
        print("[plate] 找不到源文件:", SRC)
        return 1
    text = open(SRC, encoding="utf-8", errors="replace").read()
    pairs = PAIR_RE.findall(text)
    if not pairs:
        print("[plate] 未解析到省份表条目（代码结构可能已变化）")
        return 1
    code_to_hanzi = {v[0]: k for k, v in EXPECTED.items()}
    errors = 0
    seen = {}
    for hexstr, pinyin in pairs:
        code = int(hexstr, 16)
        hanzi = code_to_hanzi.get(code)
        if hanzi is None:
            print(f"[plate] 错误: 0x{code:04X} -> {pinyin} 不是合法的省份码点")
            errors += 1
            continue
        want_code, want_pinyin = EXPECTED[hanzi]
        if pinyin != want_pinyin:
            print(f"[plate] 错误: {hanzi} 0x{code:04X} 期望拼音 {want_pinyin}，实际 {pinyin}")
            errors += 1
        key = code
        if key in seen:
            print(f"[plate] 警告: 0x{code:04X} ({hanzi}) 重复出现")
        seen[key] = hanzi
    missing = [h for h, v in EXPECTED.items() if v[0] not in seen]
    if missing:
        print("[plate] 警告: 缺少省份条目:", " ".join(missing))
    if errors:
        print(f"[plate] 失败: {errors} 处错误")
        return 1
    print(f"[plate] 通过: {len(pairs)} 条省份映射正确")
    return 0


if __name__ == "__main__":
    sys.exit(main())
