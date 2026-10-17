# -*- coding: utf-8 -*-
"""
从 LVGL v8 字体头 (font_chinese_14.h) 中提取项目需要的汉字，
生成 STM32 OLED 使用的 14x14 1bpp 字库和 UTF-8 文本宏。

用法:
    python tools/gen_cn_font.py [源字体头路径]

默认源文件: tools/font/font_chinese_14.h
输出:
    bsp/OLED/oled_cn14.h       字库 + GB2312 省份映射
    bsp/OLED/oled_cn_text.h    UI 文本（UTF-8 字节转义）
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, "tools", "font", "font_chinese_14.h")
OUT_DIR = os.path.join(ROOT, "bsp", "OLED")

txt = open(SRC, encoding="utf-8", errors="replace").read()


def get_array(name):
    m = re.search(name + r'\[\]\s*LV_ATTRIBUTE_LARGE_CONST\s*=\s*\{(.*?)\n\};', txt, re.S)
    if not m:
        raise SystemExit("array not found: " + name)
    return m.group(1)


bitmap = bytes(int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{2})', get_array("font_chinese_14_glyph_bitmap")))
gds = [tuple(map(int, x)) for x in re.findall(
    r'\.bitmap_index=(\d+),\.adv_w=(\d+),\.box_w=(\d+),\.box_h=(\d+),\.ofs_x=(-?\d+),\.ofs_y=(-?\d+)',
    get_array("font_chinese_14_glyph_dsc"))]
ulist = [int(x, 16) for x in re.findall(r'0x([0-9A-Fa-f]{4})', get_array("font_chinese_14_cmap_unicode_list"))]

uni2gid = {cp: cp - 32 for cp in range(32, 127)}
for i, cp in enumerate(ulist):
    uni2gid.setdefault(cp, 95 + i)

# ==================== 需要显示的字符 ====================
UI_STRINGS = [
    "时间", "车位", "有", "无", "空闲车位", "记录", "在场",
    "欢迎光临", "车牌", "卡号", "余额", "元",
    "请稍候", "正在计算费用", "请勿离开", "一路平安", "本次费用", "卡余额",
    "未注册卡", "禁止入场", "请联系管理员", "黑名单卡", "车位已满", "请稍后再来",
    "配置模式", "请放置卡片", "读卡或写卡", "超时", "秒", "卡片信息", "状态",
    "未写入", "无效", "有效", "黑名单",
]
PROVINCES = "京津冀沪渝冀晋蒙辽吉黑苏浙皖闽赣鲁豫鄂湘粤桂琼川贵云藏陕甘青宁新台港澳"
SPECIAL = "挂学警使领"

chars = []
for s in UI_STRINGS:
    chars.extend(s)
chars.extend(PROVINCES)
chars.extend(SPECIAL)

seen = set()
charset = []
for c in chars:
    cp = ord(c)
    if cp not in seen:
        seen.add(cp)
        charset.append(cp)
charset.sort()

missing = [chr(cp) for cp in charset if cp not in uni2gid]
if missing:
    print("[WARN] 字体中缺少字符:", "".join(missing))

# ==================== 提取 14x14 1bpp 位图 ====================
CELL_W = CELL_H = 14
THRESHOLD = 8


def extract(cp):
    gid = uni2gid.get(cp)
    if gid is None:
        return None
    bi, adv, bw, bh, ofsx, ofsy = gds[gid]
    rowbytes = (bw * 4 + 7) // 8
    cell = [[0] * CELL_W for _ in range(CELL_H)]
    top = 13 - ofsy
    for r in range(bh):
        y = top + r
        if not (0 <= y < CELL_H):
            continue
        for c in range(bw):
            bit = c * 4
            byte = bitmap[bi + r * rowbytes + bit // 8]
            nib = (byte >> (4 if bit % 8 == 0 else 0)) & 0x0F
            x = ofsx + c
            if nib >= THRESHOLD and 0 <= x < CELL_W:
                cell[y][x] = 1
    out = []
    for row in cell:
        v = 0
        for x, px in enumerate(row):
            if px:
                v |= (0x8000 >> x)
        out.append(v)
    return out


def c_bytes(vals):
    return ",".join("0x%02X,0x%02X" % ((v >> 8) & 0xFF, v & 0xFF) for v in vals)


# ==================== GB2312 -> Unicode（仅省份/特殊字，用于上位机车牌） ====================
gb_entries = []
for cp in charset:
    ch = chr(cp)
    try:
        b = ch.encode("gb2312")
    except UnicodeEncodeError:
        continue
    if len(b) == 2:
        gb_entries.append((b[0], b[1], cp))
gb_entries.sort()

# ==================== 生成字库头 ====================
font_lines = []
font_lines.append("#ifndef __OLED_CN14_H__")
font_lines.append("#define __OLED_CN14_H__")
font_lines.append("")
font_lines.append("/**")
font_lines.append(" * @file    oled_cn14.h")
font_lines.append(" * @brief   14x14 1bpp 中文字库（OLED 专用）")
font_lines.append(" *")
font_lines.append(" * 由 tools/gen_cn_font.py 自动生成，请勿手工修改。")
font_lines.append(" * 来源: LVGL v8 fmt-txt 4bpp 字体 font_chinese_14.h（simsun.ttf, size 14）")
font_lines.append(" * 共 %d 个汉字，每个字 14x14 像素、28 字节，按 Unicode 升序二分查找。" % len(charset))
font_lines.append(" * 仅由 oled.c 包含，避免多份 static 数据占用 Flash。")
font_lines.append(" */")
font_lines.append("")
font_lines.append('#include "stm32f4xx.h"')
font_lines.append("")
font_lines.append("#define OLED_CN14_WIDTH       14")
font_lines.append("#define OLED_CN14_HEIGHT      14")
font_lines.append("#define OLED_CN14_BYTES       28")
font_lines.append("")
font_lines.append("typedef struct {")
font_lines.append("    uint16_t unicode;   /* Unicode 码点 */")
font_lines.append("    uint16_t index;     /* 位图数组下标 */")
font_lines.append("} OLED_CN14_MapTypeDef;")
font_lines.append("")
font_lines.append("/* 14 行 x 2 字节；每行 bit15..bit2 对应 x=0..13，1 表示点亮 */")
font_lines.append("static const uint8_t oled_cn14_bitmap[%d][OLED_CN14_BYTES] = {" % len(charset))
for idx, cp in enumerate(charset):
    cell = extract(cp)
    if cell is None:
        cell = [0] * CELL_H
    font_lines.append("    { %s }, /* U+%04X %s */" % (c_bytes(cell), cp, chr(cp)))
font_lines.append("};")
font_lines.append("")
font_lines.append("static const OLED_CN14_MapTypeDef oled_cn14_map[%d] = {" % len(charset))
for idx, cp in enumerate(charset):
    font_lines.append("    { 0x%04X, %d }, /* %s */" % (cp, idx, chr(cp)))
font_lines.append("};")
font_lines.append("")
font_lines.append("#define OLED_CN14_COUNT  ((uint16_t)(sizeof(oled_cn14_map) / sizeof(oled_cn14_map[0])))")
font_lines.append("")
font_lines.append("/* GB2312 双字节 -> Unicode（上位机通过 GB2312 编码下发车牌，用于 OLED 汉字显示） */")
font_lines.append("typedef struct {")
font_lines.append("    uint8_t  hi;")
font_lines.append("    uint8_t  lo;")
font_lines.append("    uint16_t unicode;")
font_lines.append("} OLED_GB2312_MapTypeDef;")
font_lines.append("")
font_lines.append("static const OLED_GB2312_MapTypeDef oled_gb2312_map[%d] = {" % len(gb_entries))
for hi, lo, cp in gb_entries:
    font_lines.append("    { 0x%02X, 0x%02X, 0x%04X }, /* %s */" % (hi, lo, cp, chr(cp)))
font_lines.append("};")
font_lines.append("")
font_lines.append("#define OLED_GB2312_COUNT ((uint16_t)(sizeof(oled_gb2312_map) / sizeof(oled_gb2312_map[0])))")
font_lines.append("")
font_lines.append("#endif /* __OLED_CN14_H__ */")
font_lines.append("")

open(os.path.join(OUT_DIR, "oled_cn14.h"), "w", encoding="utf-8", newline="\n").write("\n".join(font_lines))

# ==================== 生成 UI 文本宏 ====================
TEXT_MAP = [
    ("CN_TXT_TIME", "时间"),
    ("CN_TXT_SLOT", "车位"),
    ("CN_TXT_HAS", "有"),
    ("CN_TXT_NO", "无"),
    ("CN_TXT_FREE_SLOT", "空闲车位"),
    ("CN_TXT_RECORD", "记录"),
    ("CN_TXT_IN_PARK", "在场"),
    ("CN_TXT_WELCOME", "欢迎光临"),
    ("CN_TXT_PLATE", "车牌"),
    ("CN_TXT_CARD", "卡号"),
    ("CN_TXT_BALANCE", "余额"),
    ("CN_TXT_YUAN", "元"),
    ("CN_TXT_PLEASE_WAIT", "请稍候"),
    ("CN_TXT_CALC_FEE", "正在计算费用"),
    ("CN_TXT_DO_NOT_LEAVE", "请勿离开"),
    ("CN_TXT_SAFE_TRAVEL", "一路平安"),
    ("CN_TXT_FEE", "本次费用"),
    ("CN_TXT_CARD_BALANCE", "卡余额"),
    ("CN_TXT_UNREGISTERED", "未注册卡"),
    ("CN_TXT_FORBIDDEN", "禁止入场"),
    ("CN_TXT_CONTACT_ADMIN", "请联系管理员"),
    ("CN_TXT_BLACKLIST", "黑名单卡"),
    ("CN_TXT_FULL", "车位已满"),
    ("CN_TXT_TRY_LATER", "请稍后再来"),
    ("CN_TXT_CONFIG_MODE", "配置模式"),
    ("CN_TXT_PLACE_CARD", "请放置卡片"),
    ("CN_TXT_READ_WRITE", "读卡或写卡"),
    ("CN_TXT_TIMEOUT", "超时"),
    ("CN_TXT_SECOND", "秒"),
    ("CN_TXT_CARD_INFO", "卡片信息"),
    ("CN_TXT_STATUS", "状态"),
    ("CN_TXT_NOT_WRITTEN", "未写入"),
    ("CN_TXT_INVALID", "无效"),
    ("CN_TXT_VALID", "有效"),
    ("CN_TXT_BLACKLIST_STATUS", "黑名单"),
]


def utf8_escape(s):
    return "".join("\\x%02X" % b for b in s.encode("utf-8"))


text_lines = []
text_lines.append("#ifndef __OLED_CN_TEXT_H__")
text_lines.append("#define __OLED_CN_TEXT_H__")
text_lines.append("")
text_lines.append("/**")
text_lines.append(" * @file    oled_cn_text.h")
text_lines.append(" * @brief   OLED 中文 UI 文本（UTF-8 字节显式转义）")
text_lines.append(" *")
text_lines.append(" * 由 tools/gen_cn_font.py 自动生成。")
text_lines.append(" * 使用字节转义而不是源码里的中文，避免 Keil AC5 对源码编码的依赖。")
text_lines.append(" */")
text_lines.append("")
for name, s in TEXT_MAP:
    text_lines.append('#define %-26s "%s"  /* %s */' % (name, utf8_escape(s), s))
text_lines.append("")
text_lines.append("#endif /* __OLED_CN_TEXT_H__ */")
text_lines.append("")
open(os.path.join(OUT_DIR, "oled_cn_text.h"), "w", encoding="utf-8", newline="\n").write("\n".join(text_lines))

print("charset:", len(charset), "bitmap flash bytes:", len(charset) * 28)
print("gb2312 entries:", len(gb_entries), "(", "".join(chr(cp) for _, _, cp in gb_entries), ")")
print("written: bsp/OLED/oled_cn14.h, bsp/OLED/oled_cn_text.h")
if missing:
    print("missing:", "".join(missing))
