# 硬件与引脚说明

## 1. 主控

| 项目 | 说明 |
|---|---|
| MCU | STM32F407VET6（Keil 器件名 STM32F407VETx，Cortex-M4F，LQFP100） |
| Flash | 512 KB |
| SRAM | 128 KB（另有 64 KB CCM，当前工程未使用） |
| 外部晶振 | HSE 8 MHz |
| 系统时钟 | SYSCLK 168 MHz / AHB 168 MHz / APB1 42 MHz（定时器时钟 84 MHz）/ APB2 84 MHz |
| 调试 | SWD（PA13/PA14） |
| 开发环境 | Keil MDK 5 + ARM Compiler 5.06 update 5（AC5）+ STM32F4xx_DFP 2.17.1 |

工程在 `project/MDK(V5)/Project.uvprojx` 中固定使用上述器件与启动文件 `startup_stm32f40xx.s`。

---

## 2. 外设与引脚分配

### 2.1 串口

| 外设 | 引脚 | 功能 | 说明 |
|---|---|---|---|
| USART1 | PA9 = TX，PA10 = RX | AF7，115200-8N1 | 与上位机（PC）通信，跑停车场协议 |
| USART6 | PC6 = TX，PC7 = RX | AF8，115200-8N1 | 与 MaixCAM 通信，接收 `IN:车牌` / `OUT:车牌` |

USART1/6 均开启 RXNE 中断。USART1 中断只把字节放入环形缓冲，协议解析在主循环完成。

### 2.2 RFID（RC522，13.56 MHz Mifare）

| 信号 | 引脚 | 说明 |
|---|---|---|
| CS（SDA） | PA5 | 片选，低有效 |
| SCK | PA7 | 软件 SPI 时钟 |
| MOSI | PC4 | 主出从入 |
| MISO | PA4 | 主入从出 |
| RST | PA6 | 复位，低有效 |
| VCC / GND | 3.3V / GND | 不要接 5V |

RC522 使用**软件模拟 SPI**（`bsp/RC522/bsp_rc522.c`），时序由 `delay_us` 控制。

### 2.3 OLED（SSD1306，128x64，I2C）

| 信号 | 引脚 | 说明 |
|---|---|---|
| SCL | PB10 | 软件 I2C 时钟 |
| SDA | PB11 | 软件 I2C 数据 |
| VCC / GND | 3.3V / GND | — |

I2C 地址 0x78（写），使用软件 I2C 位翻转；驱动内部按页维护 `OLED_GRAM`，只刷新脏页。

显示采用 4 行 x 16px 布局：汉字 14x14（1bpp，SimSun 风格），ASCII 6x12（`asc2_1206` 字库），
两者混排于同一行。中文字库由 `tools/gen_cn_font.py` 生成（`bsp/OLED/oled_cn14.h`，
112 个常用字 + 31 个省份简称，约 3.5KB Flash；`oled_cn_text.h` 存 UTF-8 文本宏）。

### 2.4 道闸舵机（SG90 360 度连续旋转）

| 舵机 | 信号 | 定时器 | 说明 |
|---|---|---|---|
| 入口道闸 | PB5 | TIM3_CH2 | 50 Hz PWM，15 为停止，约 20/10 为两个方向 |
| 出口道闸 | PB6 | TIM4_CH1 | 50 Hz PWM，同上 |

PWM 参数：TIM_Period=200、TIM_Prescaler=8400，基于 84 MHz 定时器时钟得到 50 Hz。
舵机开闸/关闸由非阻塞状态机 `Servo_Task()` 驱动（开闸 320ms、保持 2s、关闸 310ms）。

### 2.5 红外车位检测

| 车位 | 引脚 | 说明 |
|---|---|---|
| P1 | PB0 | 上拉输入，低电平=检测到车 |
| P2 | PB1 | 上拉输入，低电平=检测到车 |
| P3 | PB4 | 上拉输入，低电平=检测到车 |

采样与 20ms 去抖在 `IR_Task()` 中周期执行，主循环通过 `IR1_Check()/IR2_Check()/IR3_Check()` 读取缓存值。

> 注意：PB4 复位后默认是 JTAG 的 NJTRST，本工程把它当 GPIO 使用。若使用 JTAG（而非 SWD）调试，需要重新规划。

### 2.6 按键

| 按键 | 引脚 | 功能 |
|---|---|---|
| KEY1 | PA0 | 手动触发入口道闸 |
| KEY2 | PA1 | 手动触发出口道闸 |
| KEY3 | PA2 | 进入/退出配置模式 |
| KEY4 | PA3 | 预留 |

按键为上拉输入，按下为低电平；软件 20ms 消抖 + 500ms 重复触发间隔。

### 2.7 定时器

| 定时器 | 用途 | 配置 |
|---|---|---|
| TIM2 | 系统 1ms 节拍 | Prescaler=83、Period=999，中断优先级 2 |
| TIM3 | 入口舵机 PWM | 50 Hz |
| TIM4 | 出口舵机 PWM | 50 Hz |

`timer_cnt` 每 1ms 加 1，所有超时/防抖/调度都以它或 `Time_GetSeconds()` 为基准。

---

## 3. MaixCAM（车牌识别）

- MaixCAM 运行 `LPR/` 下的 MaixPy 应用（`app.yaml` + `main.py` + `pp_ocr.mud` 模型）；
- 摄像头识别车牌后通过 UART0（对应 STM32 的 USART6）发送 ASCII 行协议：
  - `IN:京A12345\n`：请求入场
  - `OUT:京A12345\n`：请求出场
- STM32 收到后把车牌首字（省份汉字，UTF-8 编码）转换为拼音缩写再显示到 OLED；
- 具体接线：MaixCAM TX -> STM32 PC7（USART6_RX），MaixCAM RX -> STM32 PC6（USART6_TX），GND 共地。

---

## 4. 供电与接线注意事项

1. 舵机峰值电流较大，建议独立 5V 供电并与 STM32 共地，不要直接从核心板 3.3V 取电；
2. RC522、OLED 使用 3.3V；
3. USART1 与上位机之间建议共地并使用 USB-TTL（3.3V 电平）；
4. OLED 与 RC522 都是软件时序，调试时不要随意改动 `delay_us` 的 SysTick 基准；
5. 上电顺序：先给 STM32/外设 3.3V，再给舵机 5V，避免上电瞬间舵机抖动。

---

## 5. 资源占用（优化前 Keil Map 参考）

| 项目 | 大小 |
|---|---|
| ROM（Code + RO Data + RW Data） | 约 30.3 KB / 512 KB |
| RAM（RW + ZI） | 约 13.4 KB / 128 KB |
| 栈 Stack_Size | 1 KB（startup 默认） |
| 堆 Heap_Size | 0.4 KB（当前未使用动态内存） |

优化后 ROM/RAM 会略有变化，以 Keil Rebuild 后的 Map 文件为准。
