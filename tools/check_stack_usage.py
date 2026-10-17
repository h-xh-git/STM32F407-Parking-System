#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""扫描固件源码中的大栈数组，提示 1KB 栈溢出风险。"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIRS = ["app", "board", "bsp", "module"]
SKIP_FILES = {"oledfont.h", "bmp.h"}
LIMIT = 128

# 匹配形如: uint8_t buf[256]; char tmp[64];
ARRAY_RE = re.compile(r'\b(?:u8|u16|u32|uint8_t|uint16_t|uint32_t|char|int|short|long|float|double)\s+\w+\s*\[\s*(\d+)\s*\]')


def scan():
    hits = []
    for d in DIRS:
        for dirpath, _dirnames, filenames in os.walk(os.path.join(ROOT, d)):
            for name in filenames:
                if not name.endswith(".c") or name in SKIP_FILES:
                    continue
                path = os.path.join(dirpath, name)
                try:
                    lines = open(path, encoding="utf-8", errors="replace").read().splitlines()
                except OSError:
                    continue
                for i, line in enumerate(lines, 1):
                    stripped = line.lstrip()
                    if stripped.startswith(("//", "/*", "*")):
                        continue
                    if not line[:1].isspace():
                        continue  # 全局/文件作用域声明，不计入栈
                    if "static" in line or "extern" in line:
                        continue
                    m = ARRAY_RE.search(line)
                    if not m:
                        continue
                    size = int(m.group(1))
                    if size > LIMIT:
                        hits.append((os.path.relpath(path, ROOT), i, size, stripped[:80]))
    return hits


def main():
    hits = scan()
    if not hits:
        print("[stack] 通过: 未发现 >{} 字节的栈数组".format(LIMIT))
        return 0
    for path, line, size, text in hits:
        print(f"[stack] 警告: {path}:{line} 局部数组 {size} 字节  {text}")
    print("[stack] 至少 {} 处大数组，请确认已改为 static 或缩小".format(len(hits)))
    return 1


if __name__ == "__main__":
    sys.exit(main())
