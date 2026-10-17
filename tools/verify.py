#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""项目一键静态自检：C 语法检查 + Python 语法检查 + 省份表 + 栈数组扫描。"""
import os
import py_compile
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PY_DIRS = ["上位机", "LPR", "tools"]


def run(name, func):
    print("=" * 60)
    print("[verify]", name)
    ok = func()
    print("[verify]", name, "->", "PASS" if ok else "FAIL")
    return ok


def check_c():
    return subprocess.call([sys.executable, os.path.join(ROOT, "tools", "check_c.py")]) == 0


def check_python():
    ok = True
    for d in PY_DIRS:
        for dirpath, _dirnames, filenames in os.walk(os.path.join(ROOT, d)):
            for name in filenames:
                if not name.endswith(".py"):
                    continue
                path = os.path.join(dirpath, name)
                try:
                    py_compile.compile(path, doraise=True)
                    print("[py] OK", os.path.relpath(path, ROOT))
                except py_compile.PyCompileError as exc:
                    print("[py] FAIL", os.path.relpath(path, ROOT))
                    print("     ", exc)
                    ok = False
    return ok


def check_plate():
    return subprocess.call([sys.executable, os.path.join(ROOT, "tools", "check_plate_table.py")]) == 0


def check_stack():
    return subprocess.call([sys.executable, os.path.join(ROOT, "tools", "check_stack_usage.py")]) == 0


def check_cn_font():
    return subprocess.call([sys.executable, os.path.join(ROOT, "tools", "check_cn_font.py")]) == 0


def main():
    results = [
        run("C 固件静态检查", check_c),
        run("Python 语法检查", check_python),
        run("省份映射表", check_plate),
        run("栈数组扫描", check_stack),
        run("中文字库与文本宏", check_cn_font),
    ]
    print("=" * 60)
    if all(results):
        print("[verify] 全部通过")
        return 0
    print("[verify] 存在失败项")
    return 1


if __name__ == "__main__":
    sys.exit(main())
