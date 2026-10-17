#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
无 Keil 环境下的固件 C 源码静态检查工具。

用法（在项目根目录执行）:
    python tools/check_c.py                 # 检查全部固件 .c 文件
    python tools/check_c.py app/main.c      # 只检查指定文件
    python tools/check_c.py --warnings      # 同时输出告警

原理:
    使用 Python 的 libclang 绑定，以 arm-none-eabi / Cortex-M4 / C99 为目标，
    用 tools/check/stubs 下的最小标准库桩头文件替代 newlib，
    从而在 PC 上完成语法 + 类型检查，无需安装 ARM/Keil 工具链。

依赖:
    pip install libclang

退出码:
    0  通过（我们的源码无 error 级诊断）
    1  存在 error
    2  环境不可用（未安装 libclang 等）
"""
import argparse
import os
import sys

try:
    from clang.cindex import Index
except ImportError:
    print("[check_c] 未安装 libclang 的 Python 绑定，请执行: pip install libclang")
    sys.exit(2)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STUB_DIR = os.path.join(ROOT, "tools", "check", "stubs")

INCLUDE_DIRS = [
    os.path.join(ROOT, "libraries", "CMSIS", "Include"),
    os.path.join(ROOT, "libraries", "CMSIS", "Device", "ST", "STM32F4xx", "Include"),
    os.path.join(ROOT, "libraries", "STM32F4xx_StdPeriph_Driver", "inc"),
    os.path.join(ROOT, "app"),
    os.path.join(ROOT, "board"),
    os.path.join(ROOT, "bsp", "OLED"),
    os.path.join(ROOT, "bsp", "uart"),
    os.path.join(ROOT, "bsp", "uart6"),
    os.path.join(ROOT, "bsp", "RC522"),
    os.path.join(ROOT, "bsp", "sg90"),
    os.path.join(ROOT, "bsp", "hongwai"),
    os.path.join(ROOT, "bsp"),
    os.path.join(ROOT, "module"),
]

DEFAULT_FILES = [
    "app/main.c",
    "board/board.c",
    "module/stm32f4xx_it.c",
    "bsp/parking_system.c",
    "bsp/protocol.c",
    "bsp/uart/bsp_uart.c",
    "bsp/uart6/bsp_uart6.c",
    "bsp/OLED/oled.c",
    "bsp/RC522/bsp_rc522.c",
    "bsp/sg90/bsp_sg90.c",
    "bsp/hongwai/bsp_hongwai.c",
]

THIRD_PARTY_PREFIXES = (
    os.path.join(ROOT, "libraries"),
    os.path.join(ROOT, "tools", "check", "stubs"),
)


def is_ours(path):
    if not path:
        return False
    if hasattr(path, "name"):
        path = path.name
    path = os.path.normcase(os.path.abspath(path))
    return not any(path.startswith(os.path.normcase(p)) for p in THIRD_PARTY_PREFIXES)


def build_args():
    args = [
        "-DUSE_STDPERIPH_DRIVER=1",
        "-DSTM32F40_41xxx=1",
        "-I" + STUB_DIR,
    ]
    args += ["-I" + d for d in INCLUDE_DIRS]
    args += [
        "-target", "arm-none-eabi",
        "-mcpu=cortex-m4",
        "-mthumb",
        "-std=c99",
        "-Wall",
        "-Wextra",
        "-Wno-unused-parameter",
        "-Wno-unused-variable",
        "-Wno-unused-function",
    ]
    return args


def check_file(index, rel_path, show_warnings):
    """解析单个文件。注意：诊断对象在 TranslationUnit 销毁后不可再访问，
    因此这里立即把需要的字段抽取为字符串保存。"""
    abs_path = os.path.join(ROOT, rel_path)
    if not os.path.isfile(abs_path):
        print("[check_c] 文件不存在: " + rel_path)
        return 1, [], []
    tu = index.parse(abs_path, args=build_args())
    errors, warnings = [], []
    for diag in tu.diagnostics:
        sev = int(diag.severity)
        loc = diag.location
        file_obj = loc.file
        ours = is_ours(file_obj)
        if not ours:
            continue
        try:
            file_part = os.path.relpath(file_obj.name, ROOT)
        except Exception:
            file_part = "<unknown>"
        text = "{}:{}:{}: {}".format(file_part, loc.line, loc.column, diag.spelling)
        if sev >= 3:
            errors.append(text)
        elif sev == 2 and show_warnings:
            warnings.append(text)
    return len(errors), errors, warnings


def main():
    parser = argparse.ArgumentParser(description="固件 C 源码静态检查 (libclang)")
    parser.add_argument("files", nargs="*", help="待检查的 .c 文件（相对项目根目录）")
    parser.add_argument("--warnings", action="store_true", help="输出告警")
    opts = parser.parse_args()

    files = opts.files or DEFAULT_FILES
    index = Index.create()
    failed = False
    total_err = 0
    for rel in files:
        count, errors, warnings = check_file(index, rel, opts.warnings)
        status = "OK" if count == 0 else "FAIL({})".format(count)
        print("[{:>10}] {}".format(status, rel))
        for text in errors:
            print("           " + text)
        if opts.warnings:
            for text in warnings:
                print("           [warn] " + text)
        if count:
            failed = True
            total_err += count

    print("-" * 60)
    if failed:
        print("[check_c] 失败: {} 个错误".format(total_err))
        return 1
    print("[check_c] 通过: 所有文件无 error 级诊断")
    return 0


if __name__ == "__main__":
    sys.exit(main())
