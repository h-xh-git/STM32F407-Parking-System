# 通信协议规范（STM32 <-> 上位机）

本文档描述 STM32F407 停车场控制器与上位机 `上位机/parking_client.py` 之间的串口协议。
波特率 115200-8N1，多字节整数一律 **小端序**。

> 本版为优化后的 v2 协议。相对旧版的差异：
> 1. 时间戳统一为 **epoch = 2000-01-01 00:00:00 本地时间** 的 32 位秒数；
> 2. 记录列表 / 卡片列表改为**多帧传输**，并以 `0x22` 结束标记收尾；
> 3. 固件侧新增了各命令的最小长度校验。其余命令与旧版兼容。

---

## 1. 帧格式

```
+--------+--------+--------+--------+------------------+----------+
| 0xAA   | 0x55   | CMD    | LEN    | DATA[0..LEN-1]   | CHECKSUM |
+--------+--------+--------+--------+------------------+----------+
  1 byte   1 byte   1 byte   1 byte   LEN bytes          1 byte
```

- `CMD`：命令字，见第 2 节。
- `LEN`：DATA 长度，0~250。
- `CHECKSUM = (CMD + LEN + sum(DATA)) & 0xFF`。

---

## 2. 命令字总表

| CMD | 方向 | 名称 | 说明 |
|---|---|---|---|
| 0x01 | 上位机 -> STM32 | HEARTBEAT | 心跳，回 0x80 |
| 0x02 | STM32 -> 上位机 | PARKING_IN | 车辆入场事件 |
| 0x03 | STM32 -> 上位机 | PARKING_OUT | 车辆出场事件（费用待回传） |
| 0x04 | 双向 | GET_STATUS | 查询/上报车位状态 |
| 0x05 | 上行请求 / 下行多帧 | GET_RECORDS | 查询车辆记录 |
| 0x06 | 上位机 -> STM32 | SET_TIME | 设置 STM32 软件时钟 |
| 0x07 | 上位机 -> STM32 | ADD_CARD | 添加注册卡 |
| 0x08 | 上位机 -> STM32 | REMOVE_CARD | 删除注册卡 |
| 0x09 | 上位机 -> STM32 | UPDATE_CARD | 更新注册卡 |
| 0x0A | 上位机 -> STM32 | SET_FEE | 设置计费参数 |
| 0x0B | 上行请求 / 下行多帧 | GET_ALL_CARDS | 查询全部注册卡 |
| 0x0C | 上位机 -> STM32 | CLEAR_RECORDS | 清空记录 |
| 0x10 | 上位机 -> STM32 | ENTER_CONFIG | 进入配置模式 |
| 0x11 | 上位机 -> STM32 | EXIT_CONFIG | 退出配置模式 |
| 0x12 | 上位机请求 / STM32 上行 | READ_CARD | 读取当前卡片数据 |
| 0x13 | 上位机 -> STM32 | WRITE_CARD | 写当前卡片 |
| 0x14 | 上位机 -> STM32 | SET_PLATE | 设置卡片车牌 |
| 0x15 | 上位机 -> STM32 | SET_BALANCE | 设置卡片余额 |
| 0x16 | 上位机 -> STM32 | SET_BLACKLIST | 设置卡片黑名单 |
| 0x20 | 上位机 -> STM32 | SYNC_REQ | 请求全量同步 |
| 0x21 | 上位机请求 / STM32 上行 | GET_TIME | 查询 STM32 时钟 |
| 0x22 | STM32 -> 上位机 | SYNC_ACK | 列表/整轮同步结束标记 |
| 0x23 | 上位机 -> STM32 | SYNC_CARD_DB | 同步一张卡到 STM32 数据库 |
| 0x24 | 上位机 -> STM32 | PARKING_OUT_FEE | 回传本次出场费用 |
| 0x80 | STM32 -> 上位机 | REPLY_OK | 通用应答 |

### 0x80 通用应答

```
DATA = [ 原命令字, 结果码 ]
```

结果码：

| 值 | 含义 |
|---|---|
| 0x00 | 成功 |
| 0x01 | 卡不存在 |
| 0x02 | 卡已存在 |
| 0x03 | 黑名单卡 |
| 0x04 | 无空车位 |
| 0x05 | 车辆不在场 |
| 0x06 | 协议/参数错误 |
| 0x07 | 无记录 |
| 0x08 | 已满 |

---

## 3. 时间语义

- 固件维护一个软件时钟（无硬件 RTC），默认从 `2024-04-30 00:00:00` 开始计时；
- 上位机连接后可发送 `0x06 SET_TIME` 校准；
- **时间戳 = 自 2000-01-01 00:00:00（本地时间）起的秒数**，32 位；
- 上位机 `timestamp_to_str()` 使用 `datetime(2000,1,1) + timedelta(seconds=ts)` 解码；
- 记录中的 `out_time = 0` 表示尚未出场，上位机显示为 `-`；
- 计费使用 `out_time - in_time` 的秒数，与日期显示无关。

---

## 4. 命令详情

### 0x02 车辆入场（STM32 -> 上位机）
```
DATA[26] = space_id(1) | uid(4) | license_plate(16) | in_time(4, LE) | 保留(1)
```

### 0x03 车辆出场（STM32 -> 上位机）
```
DATA[32] = uid(4) | license_plate(16) | in_time(4) | out_time(4) | fee(4，暂填 0)
```
上层收到后计算费用，并以 `0x24` 回传。

### 0x04 车位状态
查询：DATA 为空。
上报：
```
DATA[6] = total_spaces(1) | occupied(1) | status[3] | record_count低8位(1)
```
`status[i]`：0=空，1=占用（来自三路红外）。

### 0x05 车辆记录（多帧）
请求：DATA 为空。
上报：每帧 payload 为若干条记录，**每条 33 字节**：
```
uid(4) | license_plate(16) | in_time(4) | out_time(4) | fee(4) | status(1)
```
`status`：0=空记录（不发送），1=在场，2=已出场。
每帧最多 7 条（231 字节）。全部帧发完后发送：
```
0x22  DATA = [0x02, 0x00]   // 记录列表结束
```
上位机需累积所有 0x05 帧，收到结束标记后一次性刷新界面。

### 0x0B 全部注册卡（多帧）
请求：DATA 为空。
上报：每帧 payload 为若干张卡，**每张 29 字节**：
```
uid(4) | license_plate(16) | balance(4) | is_blacklist(1) | valid_date(4)
```
每帧最多 8 张（232 字节）。全部帧发完后发送：
```
0x22  DATA = [0x03, 0x00]   // 卡片列表结束
```

### 0x06 设置时间
```
DATA[7] = year(2, LE) | month(1) | day(1) | hour(1) | minute(1) | second(1)
```
越界参数会被拒绝（回 `0x80 [0x06, 0x06]`）。

### 0x21 查询时间
上报：
```
DATA[7] = year(2, LE) | month(1) | day(1) | hour(1) | minute(1) | second(1)
```

### 0x07 / 0x08 / 0x09 / 0x0C 卡片管理
```
0x07 ADD_CARD     DATA[20] = uid(4) | license_plate(16)
0x08 REMOVE_CARD  DATA[4]  = uid(4)
0x09 UPDATE_CARD  DATA[25] = uid(4) | license_plate(16) | balance(4) | blacklist(1)
0x0C CLEAR_RECORDS DATA 为空
```

### 0x0A 计费参数
```
DATA[17] = fee_unit(1) | fee_duration(4) | fee_amount(4) | min_fee(4) | free_duration(4)
```
- `fee_unit`：0=按小时，1=按分钟；
- `fee_duration`：计费周期（分钟）；
- `fee_amount`：每个周期的金额（元）；
- `min_fee`：最低收费；
- `free_duration`：免费时长（分钟）。

### 0x10 / 0x11 配置模式
进入/退出配置模式，DATA 为空，回 `0x80`。
配置模式下读卡/写卡命令才有效。

### 0x12 读取当前卡片
请求 DATA 为空（需已进入配置模式且读卡器上有卡）。
上报：
```
DATA[30] = uid(4) | is_valid(1) | license_plate(16) | balance(4) | is_blacklist(1) | valid_date(4)
```

### 0x13 写卡片
```
DATA[29] = uid(4) | is_valid(1) | license_plate(15) | balance(4) | is_blacklist(1) | valid_date(4)
```

### 0x14 / 0x15 / 0x16 设置车牌/余额/黑名单
```
0x14 SET_PLATE     DATA[19] = uid(4) | license_plate(15)
0x15 SET_BALANCE   DATA[8]  = uid(4) | balance(4)
0x16 SET_BLACKLIST DATA[5]  = uid(4) | is_blacklist(1)
```

### 0x20 全量同步请求
DATA 为空。STM32 后台依次发送：
```
0x04 状态帧
0x0B 卡片列表多帧 -> 0x22 [0x03, 0x00]
0x05 记录列表多帧 -> 0x22 [0x02, 0x00]
0x22 [0x01, 0x00]   // 整轮同步完成
```

### 0x23 同步单张卡到 STM32
```
DATA[24] = uid(4) | license_plate(15) | balance(4) | is_blacklist(1)
```

### 0x24 回传出场费用
```
DATA[4] = fee(4, LE)
```
STM32 更新等待中的出场记录并刷新 OLED。

---

## 5. 兼容性与容错

- 上位机收到未知命令直接忽略；
- 校验和错误时丢弃当前帧，并重新搜索 `AA 55` 帧头；
- 若记录/卡片列表在 1500ms 内没有收到结束标记（连接的是旧固件），上位机用超时兜底刷新一次；
- 固件对短包（长度不足）回 `0x80 [cmd, 0x06]`，不会越界访问；
- 全量同步会分帧后台发送，期间主循环仍可处理其他事件。
