/**
 * @file    protocol.c
 * @brief   上位机串口协议实现（USART1）。
 *
 * 设计要点（契约 4.3）：
 *   1) USART1 中断只调用 Protocol_RxByteISR()，单字节写入 1024B 环形缓冲，
 *      中断内不做解析、不 printf、不延时、不刷 OLED；
 *   2) 主循环 Protocol_Task() 每次最多取 128B 组帧/解析并执行命令；
 *   3) 列表数据（记录/卡片/全量同步）由后台状态机分片发送，每轮最多 1 帧，
 *      全程不调用 delay_ms()，发送期间 OLED/RFID/按键仍可响应；
 *   4) 所有发送缓冲改为 static，消除原 data[600]/packet[300] 的 1KB 栈溢出风险。
 */

#include "protocol.h"
#include "parking_system.h"
#include <string.h>
#include <stdio.h>

/* ==================== 内部常量 ==================== */
#define PROTOCOL_RX_RING_MASK       (PROTOCOL_RX_RING_SIZE - 1u)  /* 1024 为 2 的幂 */
#define PROTOCOL_TASK_RX_BUDGET     128u    /* 单次 Protocol_Task 最多处理的接收字节数 */
#define PROTOCOL_HEARTBEAT_MS       30000u  /* 心跳周期（ms） */
#define PROTOCOL_SYNC_DELAY_MS      5000u   /* 待同步标志延迟（ms） */
#define PROTOCOL_GET_RECORDS_COUNT  50u     /* 0x05 单次请求扫描的记录槽位数（沿用原行为） */

/* 接收组帧状态 */
#define RX_STATE_HEAD1              0u
#define RX_STATE_HEAD2              1u
#define RX_STATE_CMD                2u
#define RX_STATE_LEN                3u
#define RX_STATE_DATA               4u

/* 列表后台发送阶段 */
#define SEND_PHASE_IDLE             0u
#define SEND_PHASE_STATUS           1u  /* 全量同步：状态帧 */
#define SEND_PHASE_CARDS            2u  /* 卡片列表分片 */
#define SEND_PHASE_CARD_END         3u  /* 卡片结束标记 [0x03,0x00] */
#define SEND_PHASE_RECORDS          4u  /* 记录列表分片 */
#define SEND_PHASE_RECORD_END       5u  /* 记录结束标记 [0x02,0x00] */
#define SEND_PHASE_SYNC_END         6u  /* 全量同步结束标记 [0x01,0x00] */

/* 列表后台发送任务类型（决定结束标记之后的去向） */
#define SEND_JOB_NONE               0u
#define SEND_JOB_RECORDS            1u
#define SEND_JOB_CARDS              2u
#define SEND_JOB_SYNC               3u

/* ==================== 接收：环形缓冲 ==================== */
static volatile uint8_t  s_rx_ring[PROTOCOL_RX_RING_SIZE];
static volatile uint16_t s_rx_head = 0;    /* 只由 ISR 写入 */
static volatile uint16_t s_rx_tail = 0;    /* 只由主循环写入 */

/* ==================== 接收：组帧缓冲与状态 ==================== */
static uint8_t  s_rx_frame[sizeof(ProtocolPacketType)];
static uint16_t s_rx_frame_len = 0;
static uint8_t  s_rx_state = RX_STATE_HEAD1;

/* ==================== 发送：static 帧缓冲（避免栈溢出） ==================== */
static uint8_t s_tx_packet[PROTOCOL_MAX_FRAME_DATA + 5];
static uint8_t s_list_buf[PROTOCOL_MAX_FRAME_DATA];

/* ==================== 后台分片发送状态 ==================== */
static uint8_t  s_send_phase = SEND_PHASE_IDLE;
static uint8_t  s_send_job = SEND_JOB_NONE;
static uint16_t s_send_index = 0;      /* 当前扫描位置 */
static uint16_t s_send_scan_end = 0;   /* 扫描结束位置（不含） */

/* ==================== 心跳 / 待同步标志 ==================== */
static uint8_t  s_sync_pending = 0;
static uint32_t s_sync_last_time = 0;
static uint32_t s_heartbeat_time = 0;

/* ==================== 内部函数声明 ==================== */
static uint8_t Protocol_Checksum(const uint8_t *data, uint8_t len);
static void Protocol_PutU32(uint16_t *pos, uint32_t value);
static void Protocol_Dispatch(uint8_t cmd, uint8_t *data, uint8_t len);
static void Protocol_SendRecordsFragment(void);
static void Protocol_SendCardsFragment(void);
static void Protocol_BackgroundCheck(void);
static void Protocol_BackgroundSendStep(void);

/* ==================== 基础工具 ==================== */
/**
 * @brief  计算校验和：cmd + len + sum(data)
 */
static uint8_t Protocol_Checksum(const uint8_t *data, uint8_t len)
{
    uint8_t sum = 0;
    uint8_t i;

    for (i = 0; i < len; i++) {
        sum = (uint8_t)(sum + data[i]);
    }
    return sum;
}

/**
 * @brief  向发送缓冲追加一个小端 32 位整数（static 缓冲，无栈开销）
 */
static void Protocol_PutU32(uint16_t *pos, uint32_t value)
{
    s_list_buf[*pos] = (uint8_t)(value & 0xFFu);
    (*pos)++;
    s_list_buf[*pos] = (uint8_t)((value >> 8) & 0xFFu);
    (*pos)++;
    s_list_buf[*pos] = (uint8_t)((value >> 16) & 0xFFu);
    (*pos)++;
    s_list_buf[*pos] = (uint8_t)((value >> 24) & 0xFFu);
    (*pos)++;
}

/* ==================== 初始化 / 中断入口 / 主循环任务 ==================== */
void Protocol_Init(void)
{
    s_rx_head = 0;
    s_rx_tail = 0;
    s_rx_frame_len = 0;
    s_rx_state = RX_STATE_HEAD1;

    s_send_phase = SEND_PHASE_IDLE;
    s_send_job = SEND_JOB_NONE;
    s_send_index = 0;
    s_send_scan_end = 0;

    s_sync_pending = 0;
    s_sync_last_time = timer_cnt;
    s_heartbeat_time = timer_cnt;
}

/**
 * @brief  USART1 接收中断唯一入口：只把字节放进环形缓冲，满则丢弃。
 * @note   单生产者（ISR）/单消费者（主循环）模型，head/tail 为 16 位对齐读写，天然原子。
 */
void Protocol_RxByteISR(uint8_t byte)
{
    uint16_t next;

    next = (uint16_t)((s_rx_head + 1u) & PROTOCOL_RX_RING_MASK);
    if (next != s_rx_tail) {
        s_rx_ring[s_rx_head] = byte;
        s_rx_head = next;
    }
}

/**
 * @brief  主循环协议任务：分片解析接收数据 + 心跳/待同步处理 + 后台分片发送。
 * @note   每次最多处理 128 字节接收数据、最多发送 1 个列表分片帧，主循环可保持响应。
 */
void Protocol_Task(void)
{
    uint8_t byte;
    uint8_t processed = 0;

    /* 1. 从环形缓冲取出至多 128 字节，逐字节走组帧状态机 */
    while ((processed < PROTOCOL_TASK_RX_BUDGET) && (s_rx_tail != s_rx_head)) {
        byte = s_rx_ring[s_rx_tail];
        s_rx_tail = (uint16_t)((s_rx_tail + 1u) & PROTOCOL_RX_RING_MASK);

        Protocol_ProcessByte(byte);
        processed++;
    }

    /* 2. 心跳（30s）与待同步标志（5s 后触发后台全量同步） */
    Protocol_BackgroundCheck();

    /* 3. 列表后台分片发送：每轮最多 1 帧 */
    Protocol_BackgroundSendStep();
}

/* ==================== 组帧状态机 ==================== */
/**
 * @brief  接收组帧状态机（主循环上下文调用，不再在中断里执行）。
 *         帧格式：AA 55 | cmd | len | data[len] | checksum
 */
void Protocol_ProcessByte(uint8_t byte)
{
    uint8_t len;

    switch (s_rx_state) {
    case RX_STATE_HEAD1:
        if (byte == PROTOCOL_HEAD_1) {
            s_rx_frame[0] = byte;
            s_rx_frame_len = 1;
            s_rx_state = RX_STATE_HEAD2;
        }
        break;

    case RX_STATE_HEAD2:
        if (byte == PROTOCOL_HEAD_2) {
            s_rx_frame[1] = byte;
            s_rx_frame_len = 2;
            s_rx_state = RX_STATE_CMD;
        } else if (byte == PROTOCOL_HEAD_1) {
            /* AA AA 55：保留当前 0xAA，继续等待 0x55 */
            s_rx_frame[0] = byte;
            s_rx_frame_len = 1;
        } else {
            s_rx_state = RX_STATE_HEAD1;
            s_rx_frame_len = 0;
        }
        break;

    case RX_STATE_CMD:
        s_rx_frame[2] = byte;
        s_rx_frame_len = 3;
        s_rx_state = RX_STATE_LEN;
        break;

    case RX_STATE_LEN:
        if (byte > PROTOCOL_MAX_FRAME_DATA) {
            /* 长度超出契约上限：判为脏数据，重新寻找帧头 */
            s_rx_state = RX_STATE_HEAD1;
            s_rx_frame_len = 0;
        } else {
            s_rx_frame[3] = byte;
            s_rx_frame_len = 4;
            s_rx_state = RX_STATE_DATA;
        }
        break;

    case RX_STATE_DATA:
        s_rx_frame[s_rx_frame_len] = byte;
        s_rx_frame_len++;
        len = s_rx_frame[3];
        if (s_rx_frame_len >= (uint16_t)((uint16_t)len + 5u)) {
            Protocol_HandlePacket((ProtocolPacketType *)s_rx_frame);
            s_rx_state = RX_STATE_HEAD1;
            s_rx_frame_len = 0;
        }
        break;

    default:
        s_rx_state = RX_STATE_HEAD1;
        s_rx_frame_len = 0;
        break;
    }
}

/* ==================== 接收命令处理 ==================== */
/**
 * @brief  校验一帧并分发命令（长度校验统一在 Protocol_Dispatch 中完成）。
 */
void Protocol_HandlePacket(ProtocolPacketType *packet)
{
    uint8_t i;
    uint8_t checksum;
    uint8_t received_checksum;

    received_checksum = packet->data[packet->len];

    checksum = (uint8_t)(packet->cmd + packet->len);
    for (i = 0; i < packet->len; i++) {
        checksum = (uint8_t)(checksum + packet->data[i]);
    }

    if (checksum != received_checksum) {
        return;   /* 校验失败：丢弃整帧 */
    }

    Protocol_Dispatch(packet->cmd, packet->data, packet->len);
}

/**
 * @brief  命令分发：先做最小 payload 长度校验，不足回 ERROR_PROTOCOL；再执行。
 */
static void Protocol_Dispatch(uint8_t cmd, uint8_t *data, uint8_t len)
{
    switch (cmd) {
    case CMD_HEARTBEAT:
        Protocol_SendReply(CMD_HEARTBEAT, 0x00);
        break;

    case CMD_GET_STATUS:
        Protocol_SendStatus();
        break;

    case CMD_GET_RECORDS:
        /* 列表发送期间忽略新的列表/同步请求（契约 4.3） */
        if (s_send_phase == SEND_PHASE_IDLE) {
            Protocol_SendRecords(0, PROTOCOL_GET_RECORDS_COUNT);
        }
        break;

    case CMD_GET_ALL_CARDS:
        if (s_send_phase == SEND_PHASE_IDLE) {
            Protocol_SendAllCards();
        }
        break;

    case CMD_SET_TIME:
        if (len < PROTOCOL_MIN_SET_TIME) {
            Protocol_SendReply(CMD_SET_TIME, ERROR_PROTOCOL);
        } else if (Protocol_UpdateTime(data)) {
            Protocol_SendReply(CMD_SET_TIME, 0x00);
        } else {
            Protocol_SendReply(CMD_SET_TIME, ERROR_PROTOCOL);
        }
        break;

    case CMD_GET_TIME:
        Protocol_SendTime();
        break;

    case CMD_ADD_CARD:
        if (len < PROTOCOL_MIN_ADD_CARD) {
            Protocol_SendReply(CMD_ADD_CARD, ERROR_PROTOCOL);
        } else {
            Protocol_AddCard(data);
        }
        break;

    case CMD_REMOVE_CARD:
        if (len < PROTOCOL_MIN_REMOVE_CARD) {
            Protocol_SendReply(CMD_REMOVE_CARD, ERROR_PROTOCOL);
        } else {
            Protocol_RemoveCard(data);
        }
        break;

    case CMD_UPDATE_CARD:
        if (len < PROTOCOL_MIN_UPDATE_CARD) {
            Protocol_SendReply(CMD_UPDATE_CARD, ERROR_PROTOCOL);
        } else {
            Protocol_UpdateCard(data);
        }
        break;

    case CMD_SET_FEE:
        if (len < PROTOCOL_MIN_SET_FEE) {
            Protocol_SendReply(CMD_SET_FEE, ERROR_PROTOCOL);
        } else {
            Protocol_SetFee(data);
            Protocol_SendReply(CMD_SET_FEE, 0x00);
        }
        break;

    case CMD_SYNC_CARD_DB:
        if (len < PROTOCOL_MIN_SYNC_CARD_DB) {
            Protocol_SendReply(CMD_SYNC_CARD_DB, ERROR_PROTOCOL);
        } else {
            Protocol_SyncCardDB(data);
        }
        break;

    case CMD_CLEAR_RECORDS:
        Protocol_ClearRecords();
        break;

    case CMD_SYNC_REQ:
        if (s_send_phase == SEND_PHASE_IDLE) {
            Protocol_SyncAllData();
        }
        break;

    /* ==================== 配置模式指令 ==================== */
    case CMD_ENTER_CONFIG:
        Protocol_EnterConfig();
        break;

    case CMD_EXIT_CONFIG:
        Protocol_ExitConfig();
        break;

    case CMD_READ_CARD:
        Protocol_ReadCard();
        break;

    case CMD_WRITE_CARD:
        if (len < PROTOCOL_MIN_WRITE_CARD) {
            Protocol_SendReply(CMD_WRITE_CARD, ERROR_PROTOCOL);
        } else {
            Protocol_WriteCard(data);
        }
        break;

    case CMD_SET_PLATE:
        if (len < PROTOCOL_MIN_SET_PLATE) {
            Protocol_SendReply(CMD_SET_PLATE, ERROR_PROTOCOL);
        } else {
            Protocol_SetPlate(data);
        }
        break;

    case CMD_SET_BALANCE:
        if (len < PROTOCOL_MIN_SET_BALANCE) {
            Protocol_SendReply(CMD_SET_BALANCE, ERROR_PROTOCOL);
        } else {
            Protocol_SetBalance(data);
        }
        break;

    case CMD_SET_BLACKLIST:
        if (len < PROTOCOL_MIN_SET_BLACKLIST) {
            Protocol_SendReply(CMD_SET_BLACKLIST, ERROR_PROTOCOL);
        } else {
            Protocol_SetBlacklist(data);
        }
        break;

    case CMD_PARKING_OUT_FEE:
        {
            uint32_t fee;

            if (len < PROTOCOL_MIN_PARKING_OUT_FEE) {
                Protocol_SendReply(CMD_PARKING_OUT_FEE, ERROR_PROTOCOL);
            } else {
                fee = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
                      ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
                ParkingSystem_HandleFeeResult(fee);
            }
        }
        break;

    default:
        break;
    }
}

/* ==================== 发送基础函数 ==================== */
/**
 * @brief  发送一帧：AA 55 | cmd | len | data[len] | checksum
 * @param  len 必须 <= PROTOCOL_MAX_FRAME_DATA(250)，缓冲为 static 全局，不使用栈。
 */
void Protocol_SendPacket(uint8_t cmd, uint8_t *data, uint8_t len)
{
    uint8_t i;
    uint8_t total;

    if (len > PROTOCOL_MAX_FRAME_DATA) {
        return;
    }

    s_tx_packet[0] = PROTOCOL_HEAD_1;
    s_tx_packet[1] = PROTOCOL_HEAD_2;
    s_tx_packet[2] = cmd;
    s_tx_packet[3] = len;

    for (i = 0; i < len; i++) {
        s_tx_packet[4 + i] = data[i];
    }

    s_tx_packet[4 + len] = Protocol_Checksum(&s_tx_packet[2], (uint8_t)(len + 2u));

    total = (uint8_t)(len + 5u);
    for (i = 0; i < total; i++) {
        USART_SendData(USART1, s_tx_packet[i]);
        while (USART_GetFlagStatus(USART1, USART_FLAG_TXE) == RESET) {
        }
    }
}

void Protocol_SendReply(uint8_t cmd, uint8_t result)
{
    uint8_t data[2];

    data[0] = cmd;
    data[1] = result;
    Protocol_SendPacket(CMD_REPLY_OK, data, 2);
}

void Protocol_SendParkingIn(uint8_t *card_uid, uint8_t *license_plate, uint32_t time, uint8_t space_id)
{
    uint8_t data[26];

    data[0] = space_id;
    data[1] = card_uid[0];
    data[2] = card_uid[1];
    data[3] = card_uid[2];
    data[4] = card_uid[3];
    memcpy(&data[5], license_plate, 16);
    data[21] = (uint8_t)(time & 0xFFu);
    data[22] = (uint8_t)((time >> 8) & 0xFFu);
    data[23] = (uint8_t)((time >> 16) & 0xFFu);
    data[24] = (uint8_t)((time >> 24) & 0xFFu);
    data[25] = 0;
    Protocol_SendPacket(CMD_PARKING_IN, data, 26);
}

void Protocol_SendParkingOut(uint8_t *card_uid, uint8_t *license_plate, uint32_t in_time, uint32_t out_time, uint32_t fee)
{
    uint8_t data[32];

    data[0] = card_uid[0];
    data[1] = card_uid[1];
    data[2] = card_uid[2];
    data[3] = card_uid[3];
    memcpy(&data[4], license_plate, 16);
    data[20] = (uint8_t)(in_time & 0xFFu);
    data[21] = (uint8_t)((in_time >> 8) & 0xFFu);
    data[22] = (uint8_t)((in_time >> 16) & 0xFFu);
    data[23] = (uint8_t)((in_time >> 24) & 0xFFu);
    data[24] = (uint8_t)(out_time & 0xFFu);
    data[25] = (uint8_t)((out_time >> 8) & 0xFFu);
    data[26] = (uint8_t)((out_time >> 16) & 0xFFu);
    data[27] = (uint8_t)((out_time >> 24) & 0xFFu);
    data[28] = (uint8_t)(fee & 0xFFu);
    data[29] = (uint8_t)((fee >> 8) & 0xFFu);
    data[30] = (uint8_t)((fee >> 16) & 0xFFu);
    data[31] = (uint8_t)((fee >> 24) & 0xFFu);
    Protocol_SendPacket(CMD_PARKING_OUT, data, 32);
}

void Protocol_SendStatus(void)
{
    uint8_t data[6];
    uint8_t i;
    uint8_t occupied = 0;

    /* 使用红外检测的状态来统计车位占用情况 */
    for (i = 0; i < MAX_PARKING_SPACES; i++) {
        if (ir_detected_parking[i]) {
            occupied++;
        }
    }

    data[0] = MAX_PARKING_SPACES;
    data[1] = occupied;
    data[2] = ir_detected_parking[0];
    data[3] = ir_detected_parking[1];
    data[4] = ir_detected_parking[2];
    data[5] = (uint8_t)(record_count & 0xFFu);

    Protocol_SendPacket(CMD_GET_STATUS, data, 6);
}

/* ==================== 列表后台分片发送 ==================== */
/**
 * @brief  启动记录列表后台分片发送（0x05）。
 * @param  start_index 起始记录槽位；count 扫描槽位数（沿用原语义）。
 * @note   非阻塞：实际发送在 Protocol_Task() 中每轮 1 帧完成。
 */
void Protocol_SendRecords(uint8_t start_index, uint8_t count)
{
    uint16_t end;

    if (s_send_phase != SEND_PHASE_IDLE) {
        return;   /* 列表发送中忽略新请求 */
    }

    if ((start_index >= MAX_VEHICLE_RECORDS) || (count == 0u)) {
        /* 空列表：直接发结束标记，让上位机结束累积 */
        Protocol_SendSyncAck(PROTOCOL_SYNC_ACK_RECORDS, 0x00);
        return;
    }

    end = (uint16_t)start_index + (uint16_t)count;
    if (end > (uint16_t)MAX_VEHICLE_RECORDS) {
        end = (uint16_t)MAX_VEHICLE_RECORDS;
    }

    s_send_job = SEND_JOB_RECORDS;
    s_send_index = (uint16_t)start_index;
    s_send_scan_end = end;
    s_send_phase = SEND_PHASE_RECORDS;
}

/**
 * @brief  启动卡片列表后台分片发送（0x0B），发送全部已注册卡片。
 */
void Protocol_SendAllCards(void)
{
    if (s_send_phase != SEND_PHASE_IDLE) {
        return;
    }

    s_send_job = SEND_JOB_CARDS;
    s_send_index = 0;
    s_send_scan_end = (uint16_t)MAX_REGISTERED_CARDS;
    s_send_phase = SEND_PHASE_CARDS;
}

/**
 * @brief  启动全量同步（0x20）：状态 -> 卡片列表([0x03]) -> 记录列表([0x02]) -> [0x01]。
 * @note   非阻塞，实际发送由 Protocol_Task() 后台状态机完成。
 */
void Protocol_SyncAllData(void)
{
    if (s_send_phase != SEND_PHASE_IDLE) {
        return;
    }

    s_send_job = SEND_JOB_SYNC;
    s_send_index = 0;
    s_send_scan_end = 0;
    s_send_phase = SEND_PHASE_STATUS;
    printf("[SYNC] background sync start\r\n");
}

/**
 * @brief  发送一个记录列表分片帧（最多 7 条 x 33B = 231B），扫描完置结束阶段。
 */
static void Protocol_SendRecordsFragment(void)
{
    uint16_t pos = 0;
    uint8_t sent = 0;
    VehicleRecordType *rec;

    while ((s_send_index < s_send_scan_end) && (sent < PROTOCOL_RECORDS_PER_FRAME)) {
        rec = &vehicle_records[s_send_index];
        s_send_index++;

        if (rec->status == RECORD_EMPTY) {
            continue;
        }

        s_list_buf[pos] = rec->card_uid[0];
        pos++;
        s_list_buf[pos] = rec->card_uid[1];
        pos++;
        s_list_buf[pos] = rec->card_uid[2];
        pos++;
        s_list_buf[pos] = rec->card_uid[3];
        pos++;
        memcpy(&s_list_buf[pos], rec->license_plate, 16);
        pos = (uint16_t)(pos + 16u);
        Protocol_PutU32(&pos, rec->in_time);
        Protocol_PutU32(&pos, rec->out_time);
        Protocol_PutU32(&pos, rec->fee);
        s_list_buf[pos] = rec->status;
        pos++;

        sent++;
    }

    if (pos > 0u) {
        Protocol_SendPacket(CMD_GET_RECORDS, s_list_buf, (uint8_t)pos);
    }

    if (s_send_index >= s_send_scan_end) {
        s_send_phase = SEND_PHASE_RECORD_END;
    }
}

/**
 * @brief  发送一个卡片列表分片帧（最多 8 张 x 29B = 232B），扫描完置结束阶段。
 */
static void Protocol_SendCardsFragment(void)
{
    uint16_t pos = 0;
    uint8_t sent = 0;
    RegisteredCardType *card;

    while ((s_send_index < s_send_scan_end) && (sent < PROTOCOL_CARDS_PER_FRAME)) {
        card = &card_database[s_send_index];
        s_send_index++;

        if (!card->is_registered) {
            continue;
        }

        s_list_buf[pos] = card->card_uid[0];
        pos++;
        s_list_buf[pos] = card->card_uid[1];
        pos++;
        s_list_buf[pos] = card->card_uid[2];
        pos++;
        s_list_buf[pos] = card->card_uid[3];
        pos++;
        memcpy(&s_list_buf[pos], card->license_plate, 16);
        pos = (uint16_t)(pos + 16u);
        Protocol_PutU32(&pos, card->balance);
        s_list_buf[pos] = card->is_blacklist;
        pos++;
        Protocol_PutU32(&pos, card->valid_date);

        sent++;
    }

    if (pos > 0u) {
        Protocol_SendPacket(CMD_GET_ALL_CARDS, s_list_buf, (uint8_t)pos);
    }

    if (s_send_index >= s_send_scan_end) {
        s_send_phase = SEND_PHASE_CARD_END;
    }
}

/**
 * @brief  心跳（30s）与待同步标志（5s）检查，并推进后台发送。
 */
static void Protocol_BackgroundCheck(void)
{
    uint32_t now = timer_cnt;

    if (s_sync_pending && ((now - s_sync_last_time) >= PROTOCOL_SYNC_DELAY_MS)) {
        if (s_send_phase == SEND_PHASE_IDLE) {
            s_sync_pending = 0;
            Protocol_SyncAllData();
        }
        /* 列表忙时保持 pending，下一轮再试 */
    }

    if ((now - s_heartbeat_time) >= PROTOCOL_HEARTBEAT_MS) {
        s_heartbeat_time = now;
        Protocol_SendReply(CMD_HEARTBEAT, 0x00);
    }
}

/**
 * @brief  列表后台发送状态机：每次调用最多发送 1 帧。
 */
static void Protocol_BackgroundSendStep(void)
{
    switch (s_send_phase) {
    case SEND_PHASE_STATUS:
        Protocol_SendStatus();
        s_send_index = 0;
        s_send_scan_end = (uint16_t)MAX_REGISTERED_CARDS;
        s_send_phase = SEND_PHASE_CARDS;
        break;

    case SEND_PHASE_CARDS:
        Protocol_SendCardsFragment();
        break;

    case SEND_PHASE_CARD_END:
        Protocol_SendSyncAck(PROTOCOL_SYNC_ACK_CARDS, 0x00);
        if (s_send_job == SEND_JOB_SYNC) {
            s_send_index = 0;
            s_send_scan_end = (uint16_t)MAX_VEHICLE_RECORDS;
            s_send_phase = SEND_PHASE_RECORDS;
        } else {
            s_send_phase = SEND_PHASE_IDLE;
            s_send_job = SEND_JOB_NONE;
        }
        break;

    case SEND_PHASE_RECORDS:
        Protocol_SendRecordsFragment();
        break;

    case SEND_PHASE_RECORD_END:
        Protocol_SendSyncAck(PROTOCOL_SYNC_ACK_RECORDS, 0x00);
        if (s_send_job == SEND_JOB_SYNC) {
            s_send_phase = SEND_PHASE_SYNC_END;
        } else {
            s_send_phase = SEND_PHASE_IDLE;
            s_send_job = SEND_JOB_NONE;
        }
        break;

    case SEND_PHASE_SYNC_END:
        Protocol_SendSyncAck(PROTOCOL_SYNC_ACK_ALL, 0x00);
        s_send_phase = SEND_PHASE_IDLE;
        s_send_job = SEND_JOB_NONE;
        break;

    case SEND_PHASE_IDLE:
    default:
        break;
    }
}

/* ==================== 同步/时间相关接口 ==================== */
void Protocol_MarkSyncPending(void)
{
    s_sync_pending = 1;
    s_sync_last_time = timer_cnt;
}

void Protocol_SendSyncAck(uint8_t sync_type, uint8_t result)
{
    uint8_t data[2];

    data[0] = sync_type;
    data[1] = result;
    Protocol_SendPacket(CMD_SYNC_ACK, data, 2);
}

/**
 * @brief  上位机 CMD_SET_TIME：调用统一时间服务，按真实日历校验。
 * @retval 1 成功；0 非法（越界/大小月/闰年不合法），时钟不变。
 */
uint8_t Protocol_UpdateTime(uint8_t *data)
{
    uint16_t y;

    y = (uint16_t)data[0] | ((uint16_t)data[1] << 8);
    return Time_SetDateTime(y, data[2], data[3], data[4], data[5], data[6]);
}

void Protocol_SendTime(void)
{
    uint8_t data[7];
    uint16_t y;
    uint8_t mo, d, h, mi, s;

    Time_GetDateTime(&y, &mo, &d, &h, &mi, &s);

    data[0] = (uint8_t)(y & 0xFFu);
    data[1] = (uint8_t)((y >> 8) & 0xFFu);
    data[2] = mo;
    data[3] = d;
    data[4] = h;
    data[5] = mi;
    data[6] = s;
    Protocol_SendPacket(CMD_GET_TIME, data, 7);
}

/* ==================== 卡片数据库命令 ==================== */
void Protocol_AddCard(uint8_t *data)
{
    uint8_t uid[4];
    uint8_t license_plate[16];

    uid[0] = data[0];
    uid[1] = data[1];
    uid[2] = data[2];
    uid[3] = data[3];
    memcpy(license_plate, &data[4], 16);

    if (AddCardToDatabase(uid, license_plate)) {
        Protocol_SendReply(CMD_ADD_CARD, 0x00);
    } else {
        Protocol_SendReply(CMD_ADD_CARD, ERROR_CARD_EXIST);
    }
}

void Protocol_RemoveCard(uint8_t *data)
{
    uint8_t uid[4];

    uid[0] = data[0];
    uid[1] = data[1];
    uid[2] = data[2];
    uid[3] = data[3];

    if (RemoveCardFromDatabase(uid)) {
        Protocol_SendReply(CMD_REMOVE_CARD, 0x00);
    } else {
        Protocol_SendReply(CMD_REMOVE_CARD, ERROR_CARD_NOT_EXIST);
    }
}

void Protocol_UpdateCard(uint8_t *data)
{
    uint8_t uid[4];
    uint8_t license_plate[16];
    uint32_t balance;
    uint8_t blacklist;
    uint8_t index;

    uid[0] = data[0];
    uid[1] = data[1];
    uid[2] = data[2];
    uid[3] = data[3];
    memcpy(license_plate, &data[4], 16);
    balance = (uint32_t)data[20] | ((uint32_t)data[21] << 8) |
              ((uint32_t)data[22] << 16) | ((uint32_t)data[23] << 24);
    blacklist = data[24];

    index = FindCardInDatabase(uid);
    if (index != 0xFF) {
        memcpy(card_database[index].license_plate, license_plate, 16);
        card_database[index].balance = balance;
        card_database[index].is_blacklist = blacklist;
        Protocol_SendReply(CMD_UPDATE_CARD, 0x00);
    } else {
        Protocol_SendReply(CMD_UPDATE_CARD, ERROR_CARD_NOT_EXIST);
    }
}

void Protocol_SyncCardDB(uint8_t *data)
{
    uint8_t uid[4];
    uint8_t license_plate[16];
    uint32_t balance;
    uint8_t blacklist;
    uint8_t index;

    uid[0] = data[0];
    uid[1] = data[1];
    uid[2] = data[2];
    uid[3] = data[3];
    memcpy(license_plate, &data[4], 15);
    license_plate[15] = 0;
    balance = (uint32_t)data[19] | ((uint32_t)data[20] << 8) |
              ((uint32_t)data[21] << 16) | ((uint32_t)data[22] << 24);
    blacklist = data[23];

    index = FindCardInDatabase(uid);
    if (index != 0xFF) {
        memcpy(card_database[index].license_plate, license_plate, 16);
        card_database[index].balance = balance;
        card_database[index].is_blacklist = blacklist;
        printf("[SyncDB] Updated: %02X%02X%02X%02X, plate=%s\r\n",
               uid[0], uid[1], uid[2], uid[3], license_plate);
    } else {
        AddCardToDatabase(uid, license_plate);
        index = FindCardInDatabase(uid);
        if (index != 0xFF) {
            card_database[index].balance = balance;
            card_database[index].is_blacklist = blacklist;
        }
        printf("[SyncDB] Added: %02X%02X%02X%02X, plate=%s\r\n",
               uid[0], uid[1], uid[2], uid[3], license_plate);
    }

    Protocol_SendReply(CMD_SYNC_CARD_DB, 0x00);
}

/**
 * @brief  上位机 CMD_SET_FEE：顺序读取，消除原同一表达式多次 i++ 的未定义行为。
 *         数据格式：unit(1) + duration(4) + amount(4) + min_fee(4) + free_duration(4)
 */
void Protocol_SetFee(uint8_t *data)
{
    uint8_t i = 0;

    g_fee_config.fee_unit = data[i];
    i++;

    g_fee_config.fee_duration = (uint32_t)data[i] | ((uint32_t)data[i + 1] << 8) |
                                ((uint32_t)data[i + 2] << 16) | ((uint32_t)data[i + 3] << 24);
    i = (uint8_t)(i + 4u);

    g_fee_config.fee_amount = (uint32_t)data[i] | ((uint32_t)data[i + 1] << 8) |
                              ((uint32_t)data[i + 2] << 16) | ((uint32_t)data[i + 3] << 24);
    i = (uint8_t)(i + 4u);

    g_fee_config.min_fee = (uint32_t)data[i] | ((uint32_t)data[i + 1] << 8) |
                           ((uint32_t)data[i + 2] << 16) | ((uint32_t)data[i + 3] << 24);
    i = (uint8_t)(i + 4u);

    g_fee_config.free_duration = (uint32_t)data[i] | ((uint32_t)data[i + 1] << 8) |
                                 ((uint32_t)data[i + 2] << 16) | ((uint32_t)data[i + 3] << 24);

    printf("[FEE] Config updated: unit=%d, duration=%d, amount=%d, min=%d, free=%d\r\n",
           g_fee_config.fee_unit,
           g_fee_config.fee_duration,
           g_fee_config.fee_amount,
           g_fee_config.min_fee,
           g_fee_config.free_duration);
}

void Protocol_ClearRecords(void)
{
    uint8_t i;

    for (i = 0; i < MAX_VEHICLE_RECORDS; i++) {
        vehicle_records[i].status = RECORD_EMPTY;
    }
    record_count = 0;
    Protocol_SendReply(CMD_CLEAR_RECORDS, 0x00);
}

/* ==================== 配置模式函数实现 ==================== */
void Protocol_EnterConfig(void)
{
    Mode_EnterConfig();
    Mode_UpdateActivityTime();
    Protocol_SendReply(CMD_ENTER_CONFIG, 0x00);
}

void Protocol_ExitConfig(void)
{
    Mode_ExitConfig();
    Protocol_SendReply(CMD_EXIT_CONFIG, 0x00);
}

void Protocol_ReadCard(void)
{
    uint8_t result;

    if (g_config_mode_state.mode != MODE_CONFIG) {
        Protocol_SendReply(CMD_READ_CARD, ERROR_PROTOCOL);
        return;
    }

    if (!g_config_mode_state.card_on_reader) {
        Protocol_SendReply(CMD_READ_CARD, ERROR_CARD_NOT_EXIST);
        return;
    }

    /* 读取卡片数据 */
    result = CardData_ReadFromCard(g_config_mode_state.current_card_uid,
                                   &g_config_mode_state.current_card_data);

    if (result) {
        Mode_UpdateActivityTime();
        /* 发送读取到的数据 */
        Protocol_SendCardData(g_config_mode_state.current_card_uid,
                              g_config_mode_state.current_card_data.is_valid,
                              g_config_mode_state.current_card_data.license_plate,
                              g_config_mode_state.current_card_data.balance,
                              g_config_mode_state.current_card_data.is_blacklist,
                              g_config_mode_state.current_card_data.valid_date);
    } else {
        Protocol_SendReply(CMD_READ_CARD, ERROR_CARD_NOT_EXIST);
    }
}

void Protocol_WriteCard(uint8_t *data)
{
    CardData_t card_data;
    uint8_t index;

    if (g_config_mode_state.mode != MODE_CONFIG) {
        Protocol_SendReply(CMD_WRITE_CARD, ERROR_PROTOCOL);
        return;
    }

    if (!g_config_mode_state.card_on_reader) {
        Protocol_SendReply(CMD_WRITE_CARD, ERROR_CARD_NOT_EXIST);
        return;
    }

    CardData_Init(&card_data);
    card_data.is_valid = data[4];
    memcpy(card_data.license_plate, &data[5], 15);
    card_data.license_plate[15] = 0;
    card_data.balance = (uint32_t)data[20] | ((uint32_t)data[21] << 8) |
                        ((uint32_t)data[22] << 16) | ((uint32_t)data[23] << 24);
    card_data.is_blacklist = data[24];
    card_data.valid_date = (uint32_t)data[25] | ((uint32_t)data[26] << 8) |
                           ((uint32_t)data[27] << 16) | ((uint32_t)data[28] << 24);

    printf("[WriteCard] is_valid=%d, balance=%d, plate=%s\r\n",
           card_data.is_valid, card_data.balance, card_data.license_plate);

    if (CardData_WriteToCard(g_config_mode_state.current_card_uid, &card_data)) {
        printf("[WriteCard] Success!\r\n");
        Mode_UpdateActivityTime();
        memcpy(&g_config_mode_state.current_card_data, &card_data, sizeof(CardData_t));

        index = FindCardInDatabase(g_config_mode_state.current_card_uid);
        if (index != 0xFF) {
            memcpy(card_database[index].license_plate, card_data.license_plate, 16);
            card_database[index].balance = card_data.balance;
            card_database[index].is_blacklist = card_data.is_blacklist;
            card_database[index].valid_date = card_data.valid_date;
            printf("[WriteCard] Updated database index=%d\r\n", index);
        } else {
            AddCardToDatabase(g_config_mode_state.current_card_uid, card_data.license_plate);
            printf("[WriteCard] Added to database\r\n");
        }

        Protocol_SendReply(CMD_WRITE_CARD, 0x00);
    } else {
        printf("[WriteCard] Failed!\r\n");
        Protocol_SendReply(CMD_WRITE_CARD, ERROR_CARD_NOT_EXIST);
    }
}

void Protocol_SetPlate(uint8_t *data)
{
    if (g_config_mode_state.mode != MODE_CONFIG) {
        Protocol_SendReply(CMD_SET_PLATE, ERROR_PROTOCOL);
        return;
    }

    if (!g_config_mode_state.card_on_reader) {
        Protocol_SendReply(CMD_SET_PLATE, ERROR_CARD_NOT_EXIST);
        return;
    }

    /* 设置车牌号 */
    memcpy(g_config_mode_state.current_card_data.license_plate, &data[4], 15);
    g_config_mode_state.current_card_data.license_plate[15] = 0;
    g_config_mode_state.current_card_data.is_valid = 1;
    Mode_UpdateActivityTime();

    /* 写入卡片 */
    if (CardData_WriteToCard(g_config_mode_state.current_card_uid,
                             &g_config_mode_state.current_card_data)) {
        Protocol_SendReply(CMD_SET_PLATE, 0x00);
    } else {
        Protocol_SendReply(CMD_SET_PLATE, ERROR_CARD_NOT_EXIST);
    }
}

void Protocol_SetBalance(uint8_t *data)
{
    if (g_config_mode_state.mode != MODE_CONFIG) {
        Protocol_SendReply(CMD_SET_BALANCE, ERROR_PROTOCOL);
        return;
    }

    if (!g_config_mode_state.card_on_reader) {
        Protocol_SendReply(CMD_SET_BALANCE, ERROR_CARD_NOT_EXIST);
        return;
    }

    /* 设置余额 */
    g_config_mode_state.current_card_data.balance =
        (uint32_t)data[4] | ((uint32_t)data[5] << 8) |
        ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24);
    g_config_mode_state.current_card_data.is_valid = 1;
    Mode_UpdateActivityTime();

    if (CardData_WriteToCard(g_config_mode_state.current_card_uid,
                             &g_config_mode_state.current_card_data)) {
        Protocol_SendReply(CMD_SET_BALANCE, 0x00);
    } else {
        Protocol_SendReply(CMD_SET_BALANCE, ERROR_CARD_NOT_EXIST);
    }
}

void Protocol_SetBlacklist(uint8_t *data)
{
    if (g_config_mode_state.mode != MODE_CONFIG) {
        Protocol_SendReply(CMD_SET_BLACKLIST, ERROR_PROTOCOL);
        return;
    }

    if (!g_config_mode_state.card_on_reader) {
        Protocol_SendReply(CMD_SET_BLACKLIST, ERROR_CARD_NOT_EXIST);
        return;
    }

    /* 设置黑名单状态 */
    g_config_mode_state.current_card_data.is_blacklist = data[4];
    g_config_mode_state.current_card_data.is_valid = 1;
    Mode_UpdateActivityTime();

    if (CardData_WriteToCard(g_config_mode_state.current_card_uid,
                             &g_config_mode_state.current_card_data)) {
        Protocol_SendReply(CMD_SET_BLACKLIST, 0x00);
    } else {
        Protocol_SendReply(CMD_SET_BLACKLIST, ERROR_CARD_NOT_EXIST);
    }
}

void Protocol_SendCardData(uint8_t *card_uid, uint8_t is_valid, uint8_t *license_plate,
                           uint32_t balance, uint8_t is_blacklist, uint32_t valid_date)
{
    uint8_t data[32];
    uint8_t i = 0;

    data[i] = card_uid[0];
    i++;
    data[i] = card_uid[1];
    i++;
    data[i] = card_uid[2];
    i++;
    data[i] = card_uid[3];
    i++;
    data[i] = is_valid;
    i++;
    memcpy(&data[i], license_plate, 16);
    i = (uint8_t)(i + 16u);
    data[i] = (uint8_t)(balance & 0xFFu);
    i++;
    data[i] = (uint8_t)((balance >> 8) & 0xFFu);
    i++;
    data[i] = (uint8_t)((balance >> 16) & 0xFFu);
    i++;
    data[i] = (uint8_t)((balance >> 24) & 0xFFu);
    i++;
    data[i] = is_blacklist;
    i++;
    data[i] = (uint8_t)(valid_date & 0xFFu);
    i++;
    data[i] = (uint8_t)((valid_date >> 8) & 0xFFu);
    i++;
    data[i] = (uint8_t)((valid_date >> 16) & 0xFFu);
    i++;
    data[i] = (uint8_t)((valid_date >> 24) & 0xFFu);
    i++;

    Protocol_SendPacket(CMD_READ_CARD, data, i);
}
