# tools 说明

本项目在没有 Keil/ARM 工具链的机器上也能做基础静态自检。

| 脚本 | 作用 |
|---|---|
| `check_c.py` | 用 libclang 按 Cortex-M4/C99 解析全部固件 .c（含 tools/check/stubs 最小标准库桩），检查语法与类型错误 |
| `check_plate_table.py` | 校验 `bsp/uart6/bsp_uart6.c` 中省份 Unicode 映射表 |
| `check_stack_usage.py` | 扫描 >128 字节的函数内局部数组，提示 1KB 栈溢出风险 |
| `verify.py` | 一键运行以上检查 + Python 语法检查 |

## 依赖

```bash
pip install libclang
```

## 用法

```bash
python tools/verify.py
# 或只检查 C
python tools/check_c.py
python tools/check_c.py app/main.c bsp/protocol.c
```

## 说明

- `check_c.py` 的 `-I` 路径与 Keil 工程保持一致，并用 `tools/check/stubs` 替代 newlib 头文件；
- 第三方 CMSIS 的 `vfpcc` 内联汇编是 libclang 的已知误报，脚本会自动忽略 `libraries/` 下的诊断；
- 静态检查不能替代 Keil Rebuild 和真机验证。
