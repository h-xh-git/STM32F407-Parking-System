# 优化报告（2026-10）

本文记录本轮对停车场项目的系统性优化：问题定位、修改方案、前后对比、验证结果与遗留风险。
接口与约束见 `app/app_state.h` 与 `bsp/*.h` 的头文件注释，协议细节见 [协议规范](protocol.md)，
硬件与引脚见 [硬件说明](hardware.md)。

---

## 1. 优化目标

1. **稳定性**：消除栈溢出、越界读写、未定义行为、记录张冠李戴等确定性 bug；
2. **实时性**：去掉主循环中的长阻塞（OLED 全刷约 250ms、舵机 2.6s、红外 30ms、协议同步数 KB），
   保证刷卡/车牌/按键/显示都能及时响应；
3. **可维护性**：卡片与车牌两条进出场路径合并、共享状态收口、协议与显示解耦、删除重复/死代码；
4. **可交付性**：补齐 README、协议/硬件/优化文档，并提供无 Keil 环境下的静态自检工具。

约束：不改变硬件接线、不修改 Keil 工程文件、不新增 .c 文件、保持 Keil AC5 + C99 可编译。

---

## 2. 修复的确定性 bug

| # | 位置（优化前） | 问题 | 处理 |
|---|---|---|---|
| 1 | bsp/protocol.c `Protocol_SendRecords` / `SendAllCards` | `uint8_t data[600]` 装 50/200 条记录（1650B/6600B）→ 1KB 栈溢出；`packet[300]` 叠加；`len` 为 uint8_t 会回绕 | 发送缓冲全部改为 static；列表按 7 条记录/8 张卡片分帧（≤250B），结束标记 0x22 |
| 2 | bsp/protocol.c USART1 中断 | ISR 内解析命令、printf、delay_ms、刷 OLED → 长关中断/重入/栈叠加 | ISR 只把字节写入 1024B 环形缓冲；解析、执行、分片发送全部搬到主循环 `Protocol_Task()` |
| 3 | bsp/protocol.c `Protocol_SetFee` | 同一表达式多次 `i++`，C 未定义行为，计费参数可能解析错乱 | 改为逐字段顺序读取 |
| 4 | bsp/protocol.c 命令分发 | 短包直接按固定偏移取数 → 越界读 | 11 条命令统一做最小 payload 校验，不足回 `ERROR_PROTOCOL` |
| 5 | bsp/parking_system.c `MaixCAM_HandleExit` | 把“车位索引”当“记录索引”写 `vehicle_records[space_id]` → 记录错乱 | `ParkingSpaceType` 增加 `record_index`，入场绑定、出场/费用回传优先使用；统一进出场后两条路径共用 |
| 6 | bsp/parking_system.c MaixCAM 防抖 | `last_maixcam_event_time` 是 uint8_t，与 32 位秒数比较，约 256s 后防抖失效 | 改为 `uint32_t`，用无符号差值比较 |
| 7 | app/main.c 软件时钟 | 月末 31→1 时 `mon*2592000` 与 `day*86400` 互相抵消 → 时间戳停止前进；所有月份按 31 天，与上位机 30 天/月解码不一致 | 改为 epoch=2000-01-01 的 uint32 秒为唯一基准，按真实日历（大小月/闰年）反算年月日；上位机同步改为 `datetime` 解码 |
| 8 | bsp/uart6/bsp_uart6.c 省份表 | 13 处错误/重复码点（渝、冀、皖、鲁、湘、粤、琼、蜀、黔、滇、甘等） | 用 Python `ord()/unicodedata` 重新核对 39 条映射，补齐并去重；表改 `static const` 放 Flash |
| 9 | bsp/RC522/bsp_rc522.c `PcdSelect` | 读取响应后 FIFO 已空，调用者再读 `FIFODataReg` 取 SAK 恒为 0 → Classic 卡被当 Ultralight | 新增 `PcdSelectEx(pSnr, pSak)` 直接输出接收缓冲第 0 字节 SAK |
| 10 | bsp/RC522/bsp_rc522.c `PcdAuthState` | 认证命令拷贝 6 字节 UID，而 UID 只有 4 字节 → 越界读 | 只拷贝 4 字节；`RC522_Rese` 等待循环加超时 |
| 11 | 配置模式读卡 | 主循环先 PcdSelect 激活卡，再让 `CardData_ReadFromCard` 发 REQA（激活态不响应寻卡）→ 读卡失败 | 读卡函数改为直接 `PcdSelectEx` 取 SAK 后读块；配置模式自动读卡与上位机“读取卡片”均走同一路径 |
| 12 | bsp/sg90/bsp_sg90.c | `GPIO_InitTypeDef` 未 `GPIO_StructInit` 就使用，OType/PuPd 为栈垃圾 | 初始化前补 `GPIO_StructInit`/`TIM_OCStructInit`，显式设置各字段 |
| 13 | app/main.c / bsp/parking_system.c | 卡片进出场与车牌进出场各写一遍（约 150 行重复），记录索引逻辑不同 | 合并为 `ParkingSystem_HandleEntry/HandleExit/HandleFeeResult/ShowDenied` |
| 14 | app/main.c | `UpdateParkingSpacesByIR()` 每轮调用两次；`Protocol_PrintState` 空函数仍每轮调用 | 合并为每轮一次；删除空函数调用 |

---

## 3. 实时性优化（前后对比）

| 项目 | 优化前 | 优化后 |
|---|---|---|
| OLED 刷新 | 软件 I2C 13us 半周期 + 每轮 8 页全刷，约 **250ms/次**，主循环每轮都刷 | I2C 5us（≈100kHz）；`OLED_Clear` 只标脏不刷屏；`OLED_Refresh` 只刷脏页、单次最多 2 页（≈25ms），无脏页立即返回。主界面每秒只在时间变化时重绘时间行（2 页） |
| 道闸舵机 | `Servo_360_KeyAction` 阻塞 320+2000+310 ≈ **2.6s** | 两路独立非阻塞状态机 `Servo_Task()`；触发函数立即返回，忙时忽略 |
| 红外检测 | 车在位时 `IRx_Check` 内 `delay_ms(10)`×3 ≈ **30ms/轮** | `IR_Task()` 每 10ms 采样、20ms 去抖；`IRx_Check` 只读缓存，无 delay |
| 协议列表同步 | 一次同步连发数 KB，阻塞且可能栈溢出 | 后台状态机每轮最多发 1 帧（≤250B，约 20ms），同步期间 OLED/刷卡/按键仍响应 |
| 显示重绘 | 主界面每轮全量重绘 | 按“秒变化 / 车位变化 / 红外变化 / 界面切换”触发；事件界面只绘制一次 |
| 主循环结构 | 顺序执行 + 多处长延时 | 固定调度：`Time_Update → Servo_Task → IR_Task → UpdateParkingSpacesByIR → Protocol_Task → Process_RC522_Card → MaixCAM_ProcessEvents → Key_Task → Process_KeyActions → Display_Task`，全部非阻塞 |

RAM 小幅变化：协议环形缓冲 + static 帧缓冲约 +1.5KB，省份表移 Flash 约 -320B，`OLED_GRAM` 144→128 列约 -128B；
F407 有 128KB SRAM，余量充足。ROM 预计小幅增加（新增时间/去抖/分片逻辑），以 Keil Rebuild 的 Map 为准。

---

## 4. 可维护性改造

- **共享状态收口**：新增 `app/app_state.h`，统一 `timer_cnt`、年月日时分秒、显示模式/显示缓存、时间服务接口；
  `protocol.h` 不再重复声明这些内容。
- **业务统一**：`ParkingSystem_HandleEntry/HandleExit` 同时服务 RFID 与车牌；记录与车位通过 `record_index` 绑定；
  入场车牌优先级：暂存车牌 → 卡数据库车牌 → `CAR+UID`。
- **协议解耦**：线上帧格式仅新增“列表分帧结束标记”，其余不变；ISR 与主循环职责清晰。
- **显示解耦**：所有 `OLED_Display*` 只绘制不刷新，刷新统一由 `Display_Task()` 控制。
- **驱动职责**：舵机/红外/按键各自提供 `Servo_Task/IR_Task/Key_Task`，主循环只做调度。
- **删除死代码**：`Is_Card_In_Parking`、`Check_Card_Blacklist`、`Check_Card_Registered`、`Handle_Car_In/Out`、
  `Get_Current_Time_Seconds`、`Protocol_PrintState`、重复的全局变量与 typedef 等。
- **编码**：本轮重写的文件统一 UTF-8（无 BOM）、LF；未改动 `libraries/`、`project/`、`module/`。

---

## 5. 上位机与 MaixCAM 程序

### 5.1 上位机 `上位机/parking_client.py`

- 时间戳按 epoch 2000-01-01 用 `datetime` 解码，与固件一致；
- 记录/卡片列表支持**多帧累积**：0x05/0x0B 帧累积，0x22 结束标记（0x02=记录、0x03=卡片）后统一刷新；
  兼容旧固件：1500ms 未收到结束标记自动兜底刷新；
- 串口分帧状态机重写：帧头不对重新搜索 `AA 55`、校验失败丢弃并重同步、512B 缓冲上限、handler 异常不杀线程；
- 所有 handler 增加最小长度校验（入场≥25、出场≥28、状态≥6、记录%33、卡片%29、卡数据≥30），杜绝 IndexError；
- 数据库操作加 `threading.Lock`，卡片落库统一切到 Tk 主线程；串口一次读取 `in_waiting` 字节；
- 合并 `calculate_fee` 重复分支，明确计费参数语义；新增 `上位机/requirements.txt`；
- 未重构 UI 布局（降低风险）。

### 5.2 MaixCAM `LPR/main.py`

- 时间改用 `time.monotonic()`，避免系统时间跳变影响防抖/出场判定；
- `plate_records` 改有界（128 条），屏幕最多显示最近 5 辆在场车辆；
- `normalize_plate` 重写：过滤非法字符、字母统一大写、长度 7/8、首字省份汉字、第 2 位 A-Z；
- 异常日志限频，UART 发送失败重试一次；保持 `IN:车牌\n`/`OUT:车牌\n`（UTF-8）协议与 maix API。

### 5.3 OLED 中文字库与中文界面

- **字库来源**：用户提供的 LVGL v8 fmt-txt 4bpp 字体 `font_chinese_14.h`（simsun.ttf, 14px）；
- **裁剪提取**：`tools/gen_cn_font.py` 解析 LVGL 字库，按项目需要的 112 个汉字（UI 常用字 + 31 个省份简称）
  转为 1bpp、固定 14x14x28 字节，生成 `bsp/OLED/oled_cn14.h`（约 3.5KB Flash，含 Unicode 映射与 GB2312 省份映射）；
- **UTF-8 文本宏**：`bsp/OLED/oled_cn_text.h` 用 `\xE6...` 字节转义保存 UI 文本，避免 Keil AC5 对源码编码的依赖；
- **混合排版**：`OLED_ShowCNString()` 同时渲染 14x14 汉字与 6x12 ASCII，`OLED_ShowCNStringCenter()` 居中，
  `OLED_ShowPlate()` 自动识别 UTF-8 / GB2312 / ASCII 车牌；
- **界面中文化**：主界面、欢迎/出场等待/出场结算、未注册/黑名单/满位、配置模式与卡信息全部改为中文
  （主界面 4 行：时间、车位1有/无、空闲车位、记录/在场）；
- **性能**：仍是脏页局部刷新，主界面每秒只刷时间行（2 页）；中文标题只在界面切换时整屏重绘；
- **校验**：`tools/check_cn_font.py` 检查字库数量/排序、以及每个 UI 文本宏的汉字是否都在字库中。

预览图（按实际字模与坐标模拟生成）：`docs/images/oled_ui_preview.png`。

---

## 6. 验证结果

### 6.1 静态检查（本机无 Keil/ARM 工具链）

新增 `tools/` 自检工具：

```bash
python tools/verify.py
```

本轮结果：

- `tools/check_c.py`：11 个固件 `.c` 全部通过（libclang，目标 Cortex-M4/C99，0 error）；
- Python `py_compile`：`上位机/`、`LPR/`、`tools/` 全部通过；
- `tools/check_plate_table.py`：39 条省份映射全部正确；
- `tools/check_stack_usage.py`：无 >128B 的局部数组（原 600B/300B 栈数组已消除）；
- `tools/check_cn_font.py`：112 个字形与 35 个 UI 文本宏匹配，字库按 Unicode 升序。

### 6.2 逻辑自测（开发过程中）

- 时间服务：与 Python `datetime` 交叉验证 20000+ 随机日期、月末/年末进位、闰年、跨月单调性；
- 省份转换：39 条映射 + 单字/ASCII/2 字节/4 字节/非法 UTF-8 逐条模拟；
- 上位机协议：无串口/无 Tk 冒烟测试多帧累积、0x22 结束、旧固件兜底、坏校验重同步、短包、时间戳解码；
- 跨文件符号审计：无未解析函数引用、无重复全局定义。

### 6.3 尚未完成的验证

⚠️ 本机没有 Keil MDK/ARM 编译器，无法产出新 `.hex`，也无法上板；
以上结果均为静态检查 + 逻辑自测。**最终必须在 Keil 中 Rebuild 并上板回归**（清单见第 8 节）。

---

## 7. 协议兼容性说明

本轮有两个**成对修改**的线上语义变化（固件与上位机已同步）：

1. 时间戳由“月×2592000 + 日×86400 + …”的旧算法改为 **epoch 2000-01-01 秒数**；
   新固件 + 新上位机显示正确；旧上位机显示会错位（不崩溃）。
2. 记录/卡片列表由单帧改为**多帧 + 0x22 结束标记**；新固件 + 新上位机正常；
   新上位机连接旧固件时靠 1500ms 超时兜底；旧上位机连接新固件时只能看到最后一帧。

其余命令（0x02/0x03/0x04/0x06/0x07/0x08/0x09/0x0A/0x0B/0x0C/0x10~0x16/0x20/0x21/0x23/0x24/0x80）
的帧格式与旧版兼容。详见 [协议规范](protocol.md)。

---

## 8. 上板回归清单

1. Keil 打开 `project/MDK(V5)/Project.uvprojx` → Rebuild all（确认 0 error、关注新增告警）；
2. 基础：上电显示主界面、时间每秒进位、按键 KEY3 进出配置模式、OLED 无残影/撕裂；
3. 卡片：注册卡入场/出场、未注册拒绝、黑名单拒绝、满位提示、重复刷卡不重复触发；
4. 车牌：MaixCAM 发 `IN:`/`OUT:`，中文车牌 OLED 显示拼音缩写、上位机记录中文车牌、出场费用回传后显示；
5. 配置模式：进入后放卡自动显示卡信息、上位机读取卡片、写卡/改车牌/改余额/黑名单、60s 超时自动退出；
6. 道闸：入口/出口舵机开→保持 2s→关，两路可先后触发、动作中重复触发被忽略；
7. 红外：三路车位状态变化能刷新 OLED 并上报上位机；按键消抖正常；
8. 上位机：连接、时间同步、状态/记录/卡片列表（多帧结束标记）、计费参数、断线重连；
9. 性能：长时间运行观察是否丢帧/卡顿；协议同步时刷卡、按键、OLED 仍可响应。

---

## 9. 遗留问题与风险

- **未真机编译/验证**：本机无 Keil；AC5 对 C99 的兼容性按现有代码风格保证，但需 Rebuild 确认；
- **OLED 时序**：I2C 半周期由 13us 改为 5us，软件 I2C 在部分模块上可能需要微调（如出现花屏可改回 8~13us）；
- **OLED 分片刷新**：整屏重绘需约 4 个主循环完成（`OLED_REFRESH_PAGES_PER_CALL=2`），
  如需一次刷完可将该宏改为 8（代价是单次约 100ms）；
- **配置模式读卡路径**已改为 SELECT 直读，需上板验证 Mifare Classic / Ultralight 两种卡；
- **车牌路径新增黑名单拒绝**（原车牌路径不查黑名单），如演示不需要可删 `HandleEntry` 中 PLATE 分支的黑名单判断；
- **`g_pending_license_plate / g_license_plate_received`** 仍无写入方（预留给“先识别车牌、后刷卡”的场景），
  当前不影响功能；
- **旧构建产物**：`project/MDK(V5)/Objects|Listings/*` 与 `Project.hex` 是优化前的产物，
  不要直接烧录；Rebuild 后会自动覆盖；
- **历史编码**：`libraries/` 与部分老驱动注释存在 GBK/乱码（历史遗留），不影响编译，未做全量转码；
- **上位机 GUI** 无法在本机交互验证，仅通过语法检查 + 协议冒烟测试，需真机连接回归；
- **LPR** 需在 MaixCAM 真机验证摄像头/OCR/UART。

---

## 10. 变更文件清单

| 文件 | 说明 |
|---|---|
| `app/main.c` | 重写：时间服务、主循环调度、RFID 流程、显示调度；删除重复函数 |
| `app/app_state.h` | 新增：共享状态与时间服务接口 |
| `bsp/parking_system.c` | 重写：统一进出场、记录索引、显示/模式/红外、卡数据读写 |
| `bsp/parking_system.h` | 新增统一接口、`record_index`、事件来源/错误码 |
| `bsp/protocol.c` | 重写：环形缓冲、主循环协议任务、分片发送、长度校验、静态缓冲 |
| `bsp/protocol.h` | 新增缓冲区/分帧/最小长度常量；删除重复的时间/显示声明 |
| `bsp/uart/bsp_uart.c` | USART1 中断只入环形缓冲 |
| `bsp/uart6/bsp_uart6.c` | 省份表修正、UTF-8/长度/噪声防护、ISR 去 printf |
| `bsp/OLED/oled.c` | 脏页局部刷新、I2C 提速、GRAM 缩小、越界保护；新增 14x14 汉字渲染与中英混排 |
| `bsp/OLED/oled_cn14.h` | 新增：112 个 14x14 汉字 1bpp 字库 + Unicode/GB2312 映射（生成文件） |
| `bsp/OLED/oled_cn_text.h` | 新增：中文 UI 文本（UTF-8 字节转义宏，生成文件） |
| `bsp/RC522/bsp_rc522.c` | `PcdSelectEx`、认证 UID 越界修复、复位超时、缩进告警 |
| `bsp/sg90/bsp_sg90.c` | 非阻塞舵机状态机、非阻塞按键去抖、GPIO 结构体初始化 |
| `bsp/hongwai/bsp_hongwai.c` | 非阻塞采样 + 去抖缓存 |
| `上位机/parking_client.py` | 时间/多帧累积/解析健壮性/线程安全/费用计算 |
| `上位机/requirements.txt` | 新增 pyserial 依赖 |
| `LPR/main.py` | monotonic 时间、有界记录、车牌校验、异常限频、UART 重试 |
| `tools/*` | libclang 静态检查、省份表/栈检查、中文库校验、一键 verify |
| `tools/gen_cn_font.py` | 新增：从 LVGL 字体提取/生成项目中文字库 |
| `tools/font/font_chinese_14.h` | 字库源文件（LVGL v8 fmt-txt 4bpp） |
| `docs/images/oled_ui_preview.png` | 新增：OLED 中文界面模拟预览 |
| `README.md`、`docs/*` | 新增项目/协议/硬件/优化文档 |
