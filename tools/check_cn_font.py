#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
校验中文字库与 UI 文本宏是否匹配：
1) oled_cn14.h 中 bitmap 数量与 map 数量一致、map 按 Unicode 升序；
2) oled_cn_text.h 中所有中文宏字符都能在字库中找到。
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT = os.path.join(ROOT, "bsp", "OLED", "oled_cn14.h")
TEXT = os.path.join(ROOT, "bsp", "OLED", "oled_cn_text.h")


def main():
    if not os.path.isfile(FONT) or not os.path.isfile(TEXT):
        print("[cn_font] 找不到 oled_cn14.h / oled_cn_text.h")
        return 1
    font = open(FONT, encoding="utf-8").read()
    text = open(TEXT, encoding="utf-8").read()

    bm = font[font.index("oled_cn14_bitmap["):]
    bm = bm[bm.index("{"):bm.index("};") + 2]
    bitmap_count = len(re.findall(r'\{\s*0x[0-9A-Fa-f]{2},', bm))
    mp = font[font.index("oled_cn14_map["):]
    mp = mp[mp.index("{"):mp.index("};") + 2]
    codepoints = [int(x, 16) for x in re.findall(r'\{\s*0x([0-9A-Fa-f]{4}),\s*\d+\s*\}', mp)]
    if bitmap_count != len(codepoints):
        print("[cn_font] FAIL: bitmap %d 个，map %d 个" % (bitmap_count, len(codepoints)))
        return 1
    if codepoints != sorted(codepoints):
        print("[cn_font] FAIL: map 未按 Unicode 升序排列")
        return 1

    missing = []
    for name, value in re.findall(r'#define\s+(CN_TXT_[A-Z_]+)\s+"([^"]*)"', text):
        # 解析 \xE4 这类 UTF-8 字节转义
        raw = bytes(int(x, 16) for x in re.findall(r'\\x([0-9A-Fa-f]{2})', value))
        try:
            s = raw.decode("utf-8")
        except UnicodeDecodeError:
            s = ""
        for ch in s:
            if ord(ch) > 0x7F and ord(ch) not in codepoints:
                missing.append((name, ch))

    if missing:
        for name, ch in missing:
            print("[cn_font] FAIL: %s 需要字符 %s 但字库中没有" % (name, ch))
        return 1
    print("[cn_font] 通过: %d 个字形, %d 个 UI 文本宏" % (len(codepoints), len(re.findall(r'#define\s+CN_TXT_', text))))
    return 0


if __name__ == "__main__":
    sys.exit(main())
