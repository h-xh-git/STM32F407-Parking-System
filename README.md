# STM32F407 智能停车场管理系统

基于 STM32F407VETx 的停车场管理 demo：RFID 刷卡 + MaixCAM 车牌识别双通道进出场，
OLED 本地显示、道闸舵机控制、红外车位检测，并配套 Python 上位机（Tkinter GUI + SQLite）。

项目在原有“可运行 demo”的基础上做过一轮系统性优化：修复了协议栈溢出/记录错位/时间进位等确定性 bug，
把 OLED 刷新、舵机、红外、协议同步等长耗时操作改为非阻塞实现，并补齐了文档与静态自检工具。
详见 [优化报告](docs/optimization-report.md)。

> 新手请先阅读 **[操作使用说明](docs/usage.md)**：硬件接线、Keil 编译烧录、上位机连接、卡片注册、
> 刷卡/车牌进出场、配置模式读写卡、MaixCAM 部署、可调参数与常见问题排查。

> GitHub 仓库：https://github.com/h-xh-git/STM32F407-Parking-System

---

## 1. 功能特性

- **双通道进出场**：
  - RFID（RC522）刷卡：未注册卡拒绝、黑名单拦截、在库卡自动判定出场；
  - MaixCAM 车牌识别：通过 USART6 接收 `IN:车牌` / `OUT:车牌`，与刷卡共用一套进出场核心逻辑。
- **车位管理**：3 个车位，三路红外检测车位占用，OLED 实时显示车位/红外状态。
- **道闸控制**：两路 360° 舵机（入口/出口），开闸→保持 2s→关闸，非阻塞状态机。
- **配置模式**：按键 KEY3 或上位机命令进入，支持 Mifare Classic / Ultralight 卡的数据读写
  （车牌、余额、黑名单、有效期）。
- **计费**：上位机按分钟/小时周期计费并回传，STM32 更新记录并显示费用。
- **上位机 GUI**：串口连接、实时车位、车辆记录、卡片管理、计费参数、配置模式读写卡、SQLite 持久化。
- **OLED 界面**：主界面（时间/车位/红外）、入场/出场提示、配置模式、满位/未注册/黑名单提示。

---

## 2. 系统架构

```
+-------------------+        USART1 115200        +-------------------------+
|  上位机 (Python)  | <--------------------------> |                         |
|  Tkinter + SQLite |        自定义二进制协议       |                         |
+-------------------+                              |                         |
                                                   |     STM32F407VETx       |
+-------------------+        USART6 115200        |                         |
| MaixCAM 车牌识别  | ---------------------------> |  app/main.c             |
| MaixPy + PP-OCR   |     IN:车牌 / OUT:车牌       |  bsp/parking_system.c   |
+-------------------+                              |  bsp/protocol.c         |
                                                   |  bsp/OLED  bsp/RC522    |
+-------------------+        GPIO                  |  bsp/sg90  bsp/hongwai   |
| RC522 / OLED /    | <--------------------------> |  bsp/uart6              |
| 舵机 x2 / 红外 x3 |                              +-------------------------+
+-------------------+
```

固件采用**前后台架构**：TIM2 提供 1ms 节拍，主循环轮询各个非阻塞任务，
USART 中断只做字节收发，不做耗时处理。

---

## 3. 目录结构

```
STM32F407_ProjectTemplate/
├─ app/
│  ├─ main.c                 # 初始化、主循环、时间服务、RFID 刷卡流程
│  └─ app_state.h            # 应用共享状态与时间服务声明
├─ board/                    # 板级初始化与 SysTick 延时
├─ bsp/
│  ├─ parking_system.c/h     # 停车场业务：车位/卡库/记录/统一进出场
│  ├─ protocol.c/h           # 与上位机的通信协议（环形缓冲 + 分片发送）
│  ├─ OLED/                  # SSD1306 软件 I2C 驱动（局部刷新）
│  ├─ RC522/                 # RC522 软件 SPI 驱动
│  ├─ sg90/                  # 两路舵机 PWM + 非阻塞状态机 + 按键
│  ├─ hongwai/               # 三路红外采样与去抖
│  ├─ uart/                  # USART1（上位机）中断与 printf 重定向
│  └─ uart6/                 # USART6（MaixCAM）行协议 + 省份转拼音
├─ module/                   # stm32f4xx_conf.h / 中断向量实现
├─ libraries/                # CMSIS + STM32F4xx StdPeriph 驱动（第三方，勿改）
├─ project/
│  └─ MDK(V5)/               # Keil MDK5 工程（Project.uvprojx）
├─ 上位机/
│  ├─ parking_client.py      # Tkinter 上位机
│  └─ requirements.txt
├─ LPR/                      # MaixCAM 车牌识别应用（MaixPy）
├─ docs/                     # 操作说明、协议、硬件、优化报告
├─ tools/                    # 静态检查/自检脚本
└─ README.md
```

---

## 4. 硬件

主控 STM32F407VET6（168MHz，512KB Flash / 128KB RAM），详细接线见 [硬件与引脚说明](docs/hardware.md)。

| 模块 | 接口/引脚 |
|---|---|
| 上位机 | USART1：PA9(TX) / PA10(RX)，115200 |
| MaixCAM | USART6：PC6(TX) / PC7(RX)，115200 |
| RC522 | 软件 SPI：PA5(CS) / PA7(SCK) / PC4(MOSI) / PA4(MISO) / PA6(RST) |
| OLED | 软件 I2C：PB10(SCL) / PB11(SDA) |
| 舵机 1/2 | PB5(TIM3_CH2) / PB6(TIM4_CH1)，50Hz PWM |
| 红外 1/2/3 | PB0 / PB1 / PB4，上拉输入，低有效 |
| 按键 1-4 | PA0 / PA1 / PA2 / PA3 |

---

## 5. 编译与烧录

### 5.1 环境

- Keil MDK 5（uVision 5）
- ARM Compiler 5.06 update 5（AC5）
- STM32F4xx_DFP 2.17.1（器件包）
- 目标器件：STM32F407VETx

### 5.2 步骤

1. 用 Keil 打开 `project/MDK(V5)/Project.uvprojx`；
2. 确认 Options → Device 为 `STM32F407VETx`，C/C++ → C99 已勾选，宏定义 `USE_STDPERIPH_DRIVER, STM32F40_41xxx`；
3. `Project → Rebuild all target files`；
4. 连接 ST-Link，`Download` 烧录到 0x08000000；
5. 复位运行。

> 本仓库不包含 Keil 编译产物（`Objects/`、`Listings/`、`*.hex`），首次打开工程后请先 Rebuild。
> 代码经过静态检查，但尚未在真机回归，请按[优化报告第 8 节](docs/optimization-report.md)的上板清单验证。

---

## 6. 上位机使用

```bash
cd 上位机
pip install -r requirements.txt
python parking_client.py
```

1. 用 USB-TTL 连接 STM32 USART1（PA9/PA10），注意共地；
2. 点击“连接”，选择串口、115200；
3. 连接后会自动同步时间、车位状态、卡片数据库；
4. 在“车辆监控 / 卡片管理 / 系统配置”页完成对应操作。

数据库文件 `parking.db` 与脚本同目录，首次运行自动创建。

完整的界面说明、卡片注册流程、配置模式操作与排障见 [操作使用说明](docs/usage.md)。

---

## 7. MaixCAM 车牌识别

1. 将 `LPR/` 目录部署到 MaixCAM（MaixPy 应用）；
2. 依赖 `/root/models/pp_ocr.mud` 模型；
3. 串口默认 `/dev/ttyS0`，115200，与 STM32 USART6 交叉连接（TX→RX，RX→TX）；
4. 识别到有效车牌后发送 `IN:车牌` 或 `OUT:车牌`（UTF-8，换行结尾）；
5. 演示逻辑：车牌首次出现触发入场，入场至少 30s 后再次出现触发出场（`MIN_PARKING_SECONDS` 可调）。

---

## 8. 操作说明

除了本 README 的快速上手内容，完整的分步操作（硬件接线核对、首次上电、卡片注册、进出场流程、
配置模式读写卡、MaixCAM 部署、参数调整、常见问题）见 [docs/usage.md](docs/usage.md)。

---

## 9. 通信协议

自定义二进制协议：`AA 55 CMD LEN DATA... CHECKSUM`，命令/字段/时间语义见
[通信协议规范](docs/protocol.md)。

要点：

- 时间戳 epoch = 2000-01-01 00:00:00（本地时间）的 32 位秒数；
- 记录/卡片列表采用多帧传输，`0x22` 帧为列表结束标记；
- 固件对所有命令做最小长度校验，短包返回 `0x80 [cmd, 0x06]`。

---

## 10. 优化说明

本轮优化覆盖：

- **稳定性**：协议发送栈溢出/长度回绕、短包越界、MaixCAM 出场记录错位、月进位时间停止、
  RC522 SAK 读取错误、省份映射表错误、未初始化 GPIO 结构体；
- **实时性**：OLED 局部刷新（约 250ms → 典型 25ms）、舵机非阻塞（2.6s → 0）、
  红外非阻塞（30ms → 0）、协议后台分片发送、显示按秒/事件刷新；
- **可维护性**：卡片/车牌两条路径统一到 `ParkingSystem_HandleEntry/Exit`、共享状态收口到 `app_state.h`、
  协议/显示解耦、删除重复代码；
- **文档与工具**：本 README、协议/硬件/优化报告、libclang 静态检查与自检脚本。

详细的前后对比与文件清单见 [优化报告](docs/optimization-report.md)。

---

## 11. 自检工具（可选）

没有 Keil 时，可在 PC 上做静态检查：

```bash
pip install libclang
python tools/verify.py
```

`tools/verify.py` 会依次执行：

- `tools/check_c.py`：以 Cortex-M4/C99 为目标，用 libclang 解析全部固件 .c（无需 ARM 工具链）；
- Python 文件语法检查（`py_compile`）；
- `tools/check_plate_table.py`：校验省份 Unicode 表；
- `tools/check_stack_usage.py`：扫描 >128 字节的栈数组。

> 静态检查不能替代真机测试；涉及硬件时序、舵机角度、RC522 天线、串口电平的改动仍需上板验证。

---

## 12. 已知限制

- STM32 无硬件 RTC，时间由软件模拟，需要上位机连接后校时；断电后时间复位；
- OLED 为 ASCII 字库，中文车牌在本地仅显示省份拼音缩写；
- 注册卡数据库、车辆记录保存在 RAM 中，掉电丢失（上位机 SQLite 持久化）；
- MaixCAM 车牌进出场为演示逻辑（按时间阈值判定），不是真实地感/道闸联动的工业方案；
- 配置模式读写卡依赖卡片数据格式（Mifare Classic 6/7 块 / Ultralight 0~5 页），换卡类型需同步调整。

---

## 13. 许可

`libraries/` 下 CMSIS 与 STM32F4xx StdPeriph Driver 版权归 STMicroelectronics，遵循其 LICENSE。
项目其它代码仅用于学习与演示。
