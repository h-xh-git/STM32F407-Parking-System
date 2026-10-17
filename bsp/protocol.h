#ifndef __PROTOCOL_H__
#define __PROTOCOL_H__

/**
 * @file    protocol.h
 * @brief   上位机串口协议（USART1）接口定义。
 *
 * 线上帧格式（与 上位机/parking_client.py 成对兼容，未改变）：
 *   AA 55 | cmd | len | data[len] | checksum
 *   checksum = (cmd + len + sum(data)) & 0xFF
 *
 * 本次重构（契约 4.3）：
 *   1) USART1 中断只调用 Protocol_RxByteISR() 将字节写入 1024B 环形缓冲，
 *      禁止 printf/延时/OLED；解析与命令执行全部搬到主循环 Protocol_Task()。
 *   2) Protocol_Task() 每次最多处理 128 字节接收数据；每轮最多发送 1 个
 *      列表分片帧；心跳 30s；待同步标志 5s 后触发后台分片同步。
 *   3) 列表分帧：记录每帧最多 7 条（33B/条），卡片每帧最多 8 张（29B/张）。
 *      记录列表发完追加 CMD_SYNC_ACK(0x22) payload [0x02,0x00] 表示记录结束；
 *      卡片列表发完追加 [0x03,0x00] 表示卡片结束；
 *      全量同步在这之后最后再发 [0x01,0x00]。
 *      列表发送期间收到 0x05/0x0B/0x20 请求直接忽略。
 *
 * 时间语义：epoch = 2000-01-01 00:00:00（本地时间）的 32 位秒数，
 * 显示状态与时间服务统一由 app_state.h 提供（本文件不再重复声明）。
 */

#include "stm32f4xx.h"
#include "app_state.h"

/* ==================== 帧头与缓冲上限 ==================== */
#define PROTOCOL_HEAD_1             0xAA
#define PROTOCOL_HEAD_2             0x55

#define PROTOCOL_RX_RING_SIZE       1024    /* 接收环形缓冲字节数（必须为 2 的幂） */
#define PROTOCOL_MAX_FRAME_DATA     250     /* 单帧 payload 上限（len 为 1 字节） */

#define PROTOCOL_RECORD_SIZE        33      /* 记录列表单条字节数 */
#define PROTOCOL_CARD_SIZE          29      /* 卡片列表单条字节数 */
#define PROTOCOL_RECORDS_PER_FRAME  7       /* 7*33 = 231 <= 250 */
#define PROTOCOL_CARDS_PER_FRAME    8       /* 8*29 = 232 <= 250 */

/* ==================== 命令码 ==================== */
#define CMD_HEARTBEAT            0x01
#define CMD_PARKING_IN           0x02
#define CMD_PARKING_OUT          0x03
#define CMD_GET_STATUS           0x04
#define CMD_GET_RECORDS          0x05
#define CMD_SET_TIME             0x06
#define CMD_GET_TIME             0x21
#define CMD_ADD_CARD             0x07
#define CMD_REMOVE_CARD          0x08
#define CMD_UPDATE_CARD          0x09
#define CMD_SET_FEE              0x0A
#define CMD_GET_ALL_CARDS        0x0B
#define CMD_CLEAR_RECORDS        0x0C
/* ==================== 配置模式相关指令 ==================== */
#define CMD_ENTER_CONFIG         0x10  /* 进入配置模式 */
#define CMD_EXIT_CONFIG          0x11  /* 退出配置模式 */
#define CMD_READ_CARD            0x12  /* 读取卡片数据 */
#define CMD_WRITE_CARD           0x13  /* 写入卡片数据 */
#define CMD_SET_PLATE            0x14  /* 设置车牌号 */
#define CMD_SET_BALANCE          0x15  /* 设置余额 */
#define CMD_SET_BLACKLIST        0x16  /* 设置黑名单状态 */
#define CMD_SYNC_REQ             0x20  /* 请求同步所有数据 */
#define CMD_SYNC_ACK             0x22  /* 同步确认/列表结束标记 */
#define CMD_SYNC_CARD_DB         0x23  /* 同步卡片数据库 */
#define CMD_PARKING_OUT_FEE      0x24  /* 上位机回传车辆出场费用 */
#define CMD_REPLY_OK             0x80
#define CMD_REPLY_ERROR          0x81

/* ==================== 列表结束标记（CMD_SYNC_ACK 的 payload[0]） ==================== */
#define PROTOCOL_SYNC_ACK_ALL       0x01   /* 全量同步结束 */
#define PROTOCOL_SYNC_ACK_RECORDS   0x02   /* 记录列表结束 */
#define PROTOCOL_SYNC_ACK_CARDS     0x03   /* 卡片列表结束 */

/* ==================== 错误码 ==================== */
#define ERROR_NONE               0x00
#define ERROR_CARD_NOT_EXIST     0x01
#define ERROR_CARD_EXIST         0x02
#define ERROR_CARD_BLACKLIST     0x03
#define ERROR_NO_SPACE           0x04
#define ERROR_CAR_NOT_IN_PARKING 0x05
#define ERROR_PROTOCOL           0x06
#define ERROR_NO_RECORD          0x07
#define ERROR_FULL               0x08

/* ==================== 各命令最小 payload（不足回 ERROR_PROTOCOL） ==================== */
#define PROTOCOL_MIN_ADD_CARD        20   /* uid(4) + plate(16) */
#define PROTOCOL_MIN_REMOVE_CARD     4    /* uid(4) */
#define PROTOCOL_MIN_UPDATE_CARD     25   /* uid(4) + plate(16) + balance(4) + blacklist(1) */
#define PROTOCOL_MIN_SET_FEE         17   /* unit(1)+duration(4)+amount(4)+min(4)+free(4) */
#define PROTOCOL_MIN_SET_TIME        7    /* year(2)+mon+day+hour+min+sec */
#define PROTOCOL_MIN_SYNC_CARD_DB    24   /* uid(4)+plate(15)+balance(4)+blacklist(1) */
#define PROTOCOL_MIN_WRITE_CARD      29   /* uid(4)+valid(1)+plate(15)+balance(4)+bl(1)+date(4) */
#define PROTOCOL_MIN_SET_PLATE       19   /* uid(4)+plate(15) */
#define PROTOCOL_MIN_SET_BALANCE     8    /* uid(4)+balance(4) */
#define PROTOCOL_MIN_SET_BLACKLIST   5    /* uid(4)+blacklist(1) */
#define PROTOCOL_MIN_PARKING_OUT_FEE 4   /* fee(4) */

#pragma pack(1)

typedef struct {
    uint8_t head1;
    uint8_t head2;
    uint8_t cmd;
    uint8_t len;
    uint8_t data[256];
    uint8_t checksum;
} ProtocolPacketType;

typedef struct {
    uint8_t card_uid[4];
    uint8_t license_plate[16];
    uint32_t time;
} ParkingInDataType;

typedef struct {
    uint8_t card_uid[4];
    uint32_t time;
} ParkingOutDataType;

typedef struct {
    uint8_t space_count;
    uint8_t occupied_count;
    uint8_t space_status[3];
    uint8_t total_records;
} ParkingStatusType;

typedef struct {
    uint8_t card_uid[4];
    uint8_t license_plate[16];
    uint32_t balance;
    uint8_t is_blacklist;
    uint32_t valid_date;
} CardInfoType;

typedef struct {
    uint32_t in_time;
    uint32_t out_time;
    uint32_t fee;
    uint8_t license_plate[16];
    uint8_t card_uid[4];
} RecordDataType;

// ==================== 配置模式数据结构 ====================
typedef struct {
    uint8_t card_uid[4];
    uint8_t is_valid;
    uint8_t license_plate[16];
    uint32_t balance;
    uint8_t is_blacklist;
    uint32_t valid_date;
} CardDataType;

#pragma pack()

/* ==================== 中断 / 主循环接口（契约 4.3） ==================== */
void Protocol_Init(void);
void Protocol_RxByteISR(uint8_t byte);   /* 仅 USART1_IRQHandler 调用：只入环形缓冲 */
void Protocol_Task(void);                /* 主循环调用：解析、后台分片发送、心跳、待同步 */
void Protocol_ProcessByte(uint8_t byte); /* 内部组帧状态机（主循环上下文调用） */

/* ==================== 发送函数（签名保持不变） ==================== */
void Protocol_SendPacket(uint8_t cmd, uint8_t *data, uint8_t len);
void Protocol_SendReply(uint8_t cmd, uint8_t result);
void Protocol_SendParkingIn(uint8_t *card_uid, uint8_t *license_plate, uint32_t time, uint8_t space_id);
void Protocol_SendParkingOut(uint8_t *card_uid, uint8_t *license_plate, uint32_t in_time, uint32_t out_time, uint32_t fee);
void Protocol_SendStatus(void);
void Protocol_SendRecords(uint8_t start_index, uint8_t count);  /* 启动后台分片发送（非阻塞） */
void Protocol_SendAllCards(void);                               /* 启动后台分片发送（非阻塞） */
void Protocol_SyncAllData(void);                                /* 启动全量后台同步（非阻塞） */
void Protocol_MarkSyncPending(void);
void Protocol_SendTime(void);
void Protocol_SendSyncAck(uint8_t sync_type, uint8_t result);
void Protocol_SendCardData(uint8_t *card_uid, uint8_t is_valid, uint8_t *license_plate, uint32_t balance, uint8_t is_blacklist, uint32_t valid_date);

/* ==================== 接收命令处理函数 ==================== */
void Protocol_HandlePacket(ProtocolPacketType *packet);
uint8_t Protocol_UpdateTime(uint8_t *data);   /* 返回 1=成功，0=非法日期时间 */
void Protocol_SyncCardDB(uint8_t *data);
void Protocol_AddCard(uint8_t *data);
void Protocol_RemoveCard(uint8_t *data);
void Protocol_UpdateCard(uint8_t *data);
void Protocol_SetFee(uint8_t *data);
void Protocol_ClearRecords(void);

/* ==================== 配置模式函数 ==================== */
void Protocol_EnterConfig(void);
void Protocol_ExitConfig(void);
void Protocol_ReadCard(void);
void Protocol_WriteCard(uint8_t *data);
void Protocol_SetPlate(uint8_t *data);
void Protocol_SetBalance(uint8_t *data);
void Protocol_SetBlacklist(uint8_t *data);

#endif /* __PROTOCOL_H__ */
