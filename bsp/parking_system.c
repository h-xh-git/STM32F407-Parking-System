#include "parking_system.h"   // 停车场业务逻辑头文件（数据结构定义、宏定义）
#include "app_state.h"        // 应用共享状态（timer_cnt / 时间 / 显示模式，统一声明）
#include "oled.h"             // OLED 显示屏驱动头文件
#include "bsp_hongwai.h"      // 红外传感器驱动头文件
#include "bsp_rc522.h"        // RC522 驱动头文件（PcdSelectEx / PcdAuthState 等）
#include "bsp_sg90.h"         // SG90 舵机驱动（Servo_360_KeyAction/Servo2_360_KeyAction，非阻塞触发）
#include "bsp_uart6.h"        // USART6/MaixCAM（g_maixcam_plate_display）
#include "protocol.h"         // 协议头文件（对外发送函数）
#include <string.h>           // 内存/字符串操作库（memcpy、memset等）
#include <stdio.h>            // 标准输入输出库（sprintf等）
#include "board.h"

// ==================== 全局变量定义 ====================
ConfigModeState_t g_config_mode_state;  // 配置模式状态
FeeConfig_t g_fee_config;               // 收费配置

// ==================== 全局数据结构定义 ====================
ParkingSpaceType parking_spaces[MAX_PARKING_SPACES];    // 车位状态数组
RegisteredCardType card_database[MAX_REGISTERED_CARDS];  // 注册卡数据库数组
VehicleRecordType vehicle_records[MAX_VEHICLE_RECORDS];  // 车辆进出记录数组
uint32_t record_count = 0;                                // 记录总数（当前未完全使用）
uint8_t ir_detected_parking[MAX_PARKING_SPACES] = {0};    // 红外检测到的车位状态

// ==================== 显示状态（契约 4.0：定义在本文件，声明在 app_state.h） ====================
volatile DisplayMode_t g_disp_mode = DISP_MODE_MAIN;   // 当前显示模式
volatile uint32_t g_disp_start_time = 0;               // 进入当前显示界面的 timer_cnt
DispTempData_t g_disp_temp_data;                       // 入场/出场界面的临时显示数据

// ==================== 等待上位机计费的临时数据（契约 4.2：定义在本文件） ====================
volatile WaitingFeeState_t g_waiting_fee_state = WAITING_FEE_NONE;
uint8_t g_waiting_uid[4];
uint8_t g_waiting_license_plate[MAX_LICENSE_PLATE_LEN];
uint8_t g_waiting_record_index = 0xFF;

// 卡片入场时暂存的车牌（MaixCAM / 上位机下发），优先级高于卡数据库车牌
uint8_t g_pending_license_plate[MAX_LICENSE_PLATE_LEN];
uint8_t g_license_plate_received = 0;

// 红外状态变化时上次上报的时间（ms）
static uint32_t last_status_send_time = 0;

// MaixCAM 事件防抖时间（秒，epoch=2000-01-01）。
// 原实现为 uint8_t，与 32 位时间戳比较，约 256s 后会回绕导致防抖失效，这里修正为 uint32_t。
static uint32_t last_maixcam_event_time = 0;

// ==================== 内部小工具函数 ====================
/**
 * @brief  比较两个 4 字节 UID 是否相同
 */
static uint8_t Uid_IsEqual(const uint8_t *a, const uint8_t *b)
{
    if (a == NULL || b == NULL) {
        return 0;
    }
    return (uint8_t)(a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3]);
}

/**
 * @brief  判断 4 字节 UID 是否全 0（车牌路径可能没有绑定卡片）
 */
static uint8_t Uid_IsZero(const uint8_t *uid)
{
    if (uid == NULL) {
        return 1;
    }
    return (uint8_t)(uid[0] == 0 && uid[1] == 0 && uid[2] == 0 && uid[3] == 0);
}

// ==================== 系统初始化函数 ====================
/**
 * @brief  停车场系统初始化函数
 * @note   清空所有车位、注册卡数据库和车辆记录，为系统运行做准备
 */
void ParkingSystem_Init(void)
{
    uint8_t i;
    record_count = 0;  // 初始化记录计数

    // 1. 初始化所有车位状态
    for (i = 0; i < MAX_PARKING_SPACES; i++) {
        parking_spaces[i].is_occupied = 0;                          // 标记车位为空闲
        memset(parking_spaces[i].card_uid, 0, 4);                   // 清空车位绑定的 RFID 卡 UID
        memset(parking_spaces[i].license_plate, 0, MAX_LICENSE_PLATE_LEN);  // 清空车位绑定的车牌号
        parking_spaces[i].record_index = 0xFF;                      // 该车位尚无对应车辆记录
    }

    // 2. 初始化注册卡数据库
    for (i = 0; i < MAX_REGISTERED_CARDS; i++) {
        card_database[i].is_registered = 0;    // 标记卡为未注册
        card_database[i].is_blacklist = 0;     // 标记卡为非黑名单
        card_database[i].balance = 0;          // 初始化余额为 0
        card_database[i].valid_date = 0;       // 初始化有效期为 0
        memset(card_database[i].card_uid, 0, 4);
        memset(card_database[i].license_plate, 0, MAX_LICENSE_PLATE_LEN);
    }

    // 3. 初始化收费配置（默认值）
    g_fee_config.fee_unit = 0;         // 默认按小时计费
    g_fee_config.fee_duration = 60;    // 默认60分钟
    g_fee_config.fee_amount = 5;       // 默认每小时5元
    g_fee_config.min_fee = 5;          // 默认最低收费5元
    g_fee_config.free_duration = 0;    // 默认无免费时长

    // 4. 初始化车辆进出记录
    for (i = 0; i < MAX_VEHICLE_RECORDS; i++) {
        vehicle_records[i].status = RECORD_EMPTY;  // 标记记录为空
        vehicle_records[i].card_uid[0] = 0;
        vehicle_records[i].card_uid[1] = 0;
        vehicle_records[i].card_uid[2] = 0;
        vehicle_records[i].card_uid[3] = 0;
        memset(vehicle_records[i].license_plate, 0, MAX_LICENSE_PLATE_LEN);
        vehicle_records[i].in_time = 0;
        vehicle_records[i].out_time = 0;
        vehicle_records[i].fee = 0;
        vehicle_records[i].in_parking_lot = 0;
    }

    // 5. 初始化配置模式状态
    g_config_mode_state.mode = MODE_NORMAL;       // 默认为正常模式
    g_config_mode_state.last_activity_time = 0;
    g_config_mode_state.card_on_reader = 0;
    memset(g_config_mode_state.current_card_uid, 0, 4);
    CardData_Init(&g_config_mode_state.current_card_data);

    // 6. 初始化显示与等待状态
    g_disp_mode = DISP_MODE_MAIN;
    g_disp_start_time = 0;
    memset(&g_disp_temp_data, 0, sizeof(DispTempData_t));
    g_waiting_fee_state = WAITING_FEE_NONE;
    memset(g_waiting_uid, 0, 4);
    memset(g_waiting_license_plate, 0, MAX_LICENSE_PLATE_LEN);
    g_waiting_record_index = 0xFF;
    last_maixcam_event_time = 0;
    last_status_send_time = 0;
}

// ==================== 注册卡管理函数 ====================
/**
 * @brief  在注册卡数据库中查找指定 RFID 卡
 * @param  uid: 待查找的 RFID 卡 UID
 * @retval 卡的索引（0~MAX_REGISTERED_CARDS-1），未找到返回 0xFF
 */
uint8_t FindCardInDatabase(uint8_t *uid)
{
    uint8_t i;
    // 遍历所有注册卡
    for (i = 0; i < MAX_REGISTERED_CARDS; i++) {
        // 检查卡是否已注册，且 UID 匹配
        if (card_database[i].is_registered &&
            card_database[i].card_uid[0] == uid[0] &&
            card_database[i].card_uid[1] == uid[1] &&
            card_database[i].card_uid[2] == uid[2] &&
            card_database[i].card_uid[3] == uid[3]) {
            return i;  // 找到匹配的卡，返回索引
        }
    }
    return 0xFF;  // 未找到，返回 0xFF
}

/**
 * @brief  添加一张 RFID 卡到注册卡数据库
 * @param  uid: 待添加的 RFID 卡 UID
 * @param  license_plate: 绑定的车牌号（可为 NULL）
 * @retval 1: 添加成功；0: 添加失败（卡已存在或数据库已满）
 */
uint8_t AddCardToDatabase(uint8_t *uid, uint8_t *license_plate)
{
    uint8_t index;
    uint8_t i;

    // 1. 先检查卡是否已注册
    index = FindCardInDatabase(uid);
    if (index != 0xFF) {
        return 0;  // 卡已存在，添加失败
    }

    // 2. 查找数据库中的空位置
    for (i = 0; i < MAX_REGISTERED_CARDS; i++) {
        if (!card_database[i].is_registered) {  // 找到未注册的位置
            // 填写卡信息
            card_database[i].is_registered = 1;                          // 标记为已注册
            card_database[i].card_uid[0] = uid[0];                       // 保存 UID
            card_database[i].card_uid[1] = uid[1];
            card_database[i].card_uid[2] = uid[2];
            card_database[i].card_uid[3] = uid[3];
            if (license_plate != NULL) {  // 如果提供了车牌号
                memcpy(card_database[i].license_plate, license_plate, MAX_LICENSE_PLATE_LEN);  // 保存车牌号
            } else {
                memset(card_database[i].license_plate, 0, MAX_LICENSE_PLATE_LEN);
            }
            card_database[i].balance = 0;              // 初始化余额为 0
            card_database[i].is_blacklist = 0;          // 初始化为非黑名单
            card_database[i].valid_date = 0xFFFFFFFF;   // 初始化有效期为永久
            return 1;  // 添加成功
        }
    }
    return 0;  // 数据库已满，添加失败
}

/**
 * @brief  从注册卡数据库中删除指定 RFID 卡
 * @param  uid: 待删除的 RFID 卡 UID
 * @retval 1: 删除成功；0: 删除失败（卡不存在）
 */
uint8_t RemoveCardFromDatabase(uint8_t *uid)
{
    uint8_t index = FindCardInDatabase(uid);  // 查找卡
    if (index == 0xFF) {
        return 0;  // 卡不存在，删除失败
    }
    card_database[index].is_registered = 0;  // 标记为未注册（逻辑删除）
    return 1;  // 删除成功
}

/**
 * @brief  设置指定 RFID 卡的黑名单状态
 * @param  uid: 待设置的 RFID 卡 UID
 * @param  blacklist: 1=加入黑名单；0=移出黑名单
 * @retval 1: 设置成功；0: 设置失败（卡不存在）
 */
uint8_t SetCardBlacklist(uint8_t *uid, uint8_t blacklist)
{
    uint8_t index = FindCardInDatabase(uid);  // 查找卡
    if (index == 0xFF) {
        return 0;  // 卡不存在，设置失败
    }
    card_database[index].is_blacklist = blacklist;  // 设置黑名单状态
    return 1;  // 设置成功
}

/**
 * @brief  检查卡片是否在黑名单中
 * @param  uid: 待检查的 RFID 卡 UID
 * @retval 1: 在黑名单中；0: 不在黑名单中或卡不存在
 */
uint8_t IsCardBlacklisted(uint8_t *uid)
{
    uint8_t index = FindCardInDatabase(uid);  // 在卡数据库中查找
    if (index != 0xFF) {
        return card_database[index].is_blacklist;  // 返回黑名单状态
    }
    return 0;  // 卡不存在，返回0（不在黑名单）
}

/**
 * @brief  更新指定 RFID 卡的余额
 * @param  uid: 待更新的 RFID 卡 UID
 * @param  balance: 新的余额值
 * @retval 1: 更新成功；0: 更新失败（卡不存在）
 */
uint8_t UpdateCardBalance(uint8_t *uid, uint32_t balance)
{
    uint8_t index = FindCardInDatabase(uid);  // 查找卡
    if (index == 0xFF) {
        return 0;  // 卡不存在，更新失败
    }
    card_database[index].balance = balance;  // 更新余额
    return 1;  // 更新成功
}

// ==================== 车位管理函数 ====================
/**
 * @brief  查找一个空闲的车位
 * @retval 空闲车位的索引（0~MAX_PARKING_SPACES-1），无空车位返回 0xFF
 */
uint8_t FindEmptyParkingSpace(void)
{
    uint8_t i;
    // 遍历所有车位
    for (i = 0; i < MAX_PARKING_SPACES; i++) {
        if (!parking_spaces[i].is_occupied) {  // 检查车位是否空闲
            return i;  // 返回空闲车位索引
        }
    }
    return 0xFF;  // 无空车位
}

/**
 * @brief  根据 RFID 卡 UID 查找已停车的车位
 * @param  uid: 待查找的 RFID 卡 UID
 * @retval 车位索引（0~MAX_PARKING_SPACES-1），未找到返回 0xFF
 */
uint8_t FindCarByCard(uint8_t *uid)
{
    uint8_t i;
    // 遍历所有车位
    for (i = 0; i < MAX_PARKING_SPACES; i++) {
        // 检查车位是否被占用，且 UID 匹配
        if (parking_spaces[i].is_occupied &&
            parking_spaces[i].card_uid[0] == uid[0] &&
            parking_spaces[i].card_uid[1] == uid[1] &&
            parking_spaces[i].card_uid[2] == uid[2] &&
            parking_spaces[i].card_uid[3] == uid[3]) {
            return i;  // 找到匹配的车位
        }
    }
    return 0xFF;  // 未找到
}

/**
 * @brief  根据车牌号查找已注册的卡片
 */
uint8_t FindCardByLicensePlate(uint8_t *license_plate)
{
    uint8_t i;
    if (license_plate == NULL || license_plate[0] == 0) {
        return 0xFF;
    }

    for (i = 0; i < MAX_REGISTERED_CARDS; i++) {
        if (card_database[i].is_registered &&
            strncmp((char*)card_database[i].license_plate, (char*)license_plate, MAX_LICENSE_PLATE_LEN) == 0) {
            return i;
        }
    }
    return 0xFF;
}

/**
 * @brief  根据车牌号查找已停车的车位
 */
uint8_t FindCarByLicensePlate(uint8_t *license_plate)
{
    uint8_t i;
    if (license_plate == NULL || license_plate[0] == 0) {
        return 0xFF;
    }

    for (i = 0; i < MAX_PARKING_SPACES; i++) {
        if (parking_spaces[i].is_occupied &&
            strncmp((char*)parking_spaces[i].license_plate, (char*)license_plate, MAX_LICENSE_PLATE_LEN) == 0) {
            return i;
        }
    }
    return 0xFF;
}

// ==================== 车辆记录管理函数 ====================
/**
 * @brief  保存车辆进出记录
 * @param  uid: RFID 卡 UID（车牌路径可能全 0）
 * @param  license_plate: 车牌号（可为 NULL）
 * @param  time: 时间（epoch 秒）
 * @param  operation: 操作类型（OPERATION_IN=进场；OPERATION_OUT=出场）
 * @retval 记录索引（0~MAX_VEHICLE_RECORDS-1），保存失败返回 0xFF
 */
uint8_t SaveRecord(uint8_t *uid, uint8_t *license_plate, uint32_t time, uint8_t operation)
{
    uint8_t i;

    if (operation == OPERATION_IN) {
        // ==================== 处理进场记录 ====================
        for (i = 0; i < MAX_VEHICLE_RECORDS; i++) {
            if (vehicle_records[i].status == RECORD_EMPTY) {  // 找到空记录
                // 填写进场信息
                vehicle_records[i].card_uid[0] = uid[0];       // 保存 UID
                vehicle_records[i].card_uid[1] = uid[1];
                vehicle_records[i].card_uid[2] = uid[2];
                vehicle_records[i].card_uid[3] = uid[3];
                if (license_plate != NULL) {  // 如果提供了车牌号
                    memcpy(vehicle_records[i].license_plate, license_plate, MAX_LICENSE_PLATE_LEN);  // 保存车牌号
                } else {
                    memset(vehicle_records[i].license_plate, 0, MAX_LICENSE_PLATE_LEN);
                }
                vehicle_records[i].in_time = time;       // 保存进场时间
                vehicle_records[i].out_time = 0;          // 出场时间初始化为 0
                vehicle_records[i].fee = 0;                // 费用初始化为 0
                vehicle_records[i].status = RECORD_IN_PARKING;  // 标记为"在库中"
                vehicle_records[i].in_parking_lot = 1;    // 车辆在场内
                record_count++;  // 记录计数加 1
                return i;  // 返回记录索引
            }
        }
    } else {
        // ==================== 处理出场记录 ====================
        for (i = 0; i < MAX_VEHICLE_RECORDS; i++) {
            // 查找匹配的"在库中"记录
            if (vehicle_records[i].status == RECORD_IN_PARKING &&
                vehicle_records[i].card_uid[0] == uid[0] &&
                vehicle_records[i].card_uid[1] == uid[1] &&
                vehicle_records[i].card_uid[2] == uid[2] &&
                vehicle_records[i].card_uid[3] == uid[3]) {

                // 填写出场信息
                vehicle_records[i].out_time = time;  // 保存出场时间
                vehicle_records[i].fee = 0;  // 费用由上位机计算，初始化为0
                vehicle_records[i].status = RECORD_LEFT;  // 标记为"已出场"
                vehicle_records[i].in_parking_lot = 0;
                return i;  // 返回记录索引
            }
        }
    }
    return 0xFF;  // 保存失败（无空记录或未找到进场记录）
}

// ==================== 统一进出场处理（卡片 / 车牌共用） ====================
/**
 * @brief  从车辆记录中查找"在库中"的记录索引（出场记录索引修复的兜底逻辑）
 * @note   优先按 UID 查找；UID 全 0（纯车牌场景）时按车牌查找。
 * @retval 记录索引，未找到返回 0xFF
 */
static uint8_t FindInParkingRecordIndex(const uint8_t *uid, const uint8_t *license_plate)
{
    uint8_t i;

    if (!Uid_IsZero(uid)) {
        for (i = 0; i < MAX_VEHICLE_RECORDS; i++) {
            if (vehicle_records[i].status == RECORD_IN_PARKING &&
                Uid_IsEqual(vehicle_records[i].card_uid, uid)) {
                return i;
            }
        }
    }

    if (license_plate != NULL && license_plate[0] != 0) {
        for (i = 0; i < MAX_VEHICLE_RECORDS; i++) {
            if (vehicle_records[i].status == RECORD_IN_PARKING &&
                strncmp((char*)vehicle_records[i].license_plate, (char*)license_plate, MAX_LICENSE_PLATE_LEN) == 0) {
                return i;
            }
        }
    }

    return 0xFF;
}

/**
 * @brief  统一车辆入场处理（卡片与车牌两条路径共用）
 * @note   说明：
 *         1) 车牌优先级（卡片路径）：MaixCAM/上位机暂存车牌 -> 卡数据库车牌 -> "CAR%02X%02X%02X%02X"。
 *         2) 入场成功后保存停车位与车辆记录的对应关系（record_index），
 *            出场/费用回传优先使用它，修复 MaixCAM 出场把车位索引当记录索引的缺陷。
 *         3) OLED 只绘制不刷新，统一由 main.c 的 Display_Task() 调 OLED_Refresh()。
 */
uint8_t ParkingSystem_HandleEntry(const uint8_t *uid, const uint8_t *license_plate, uint8_t source)
{
    uint8_t space_id;
    uint8_t card_index = 0xFF;
    uint8_t record_index;
    uint8_t uid_buf[4];
    uint8_t plate_buf[MAX_LICENSE_PLATE_LEN];
    const uint8_t *display_plate;
    uint32_t current_time;

    memset(uid_buf, 0, 4);
    memset(plate_buf, 0, MAX_LICENSE_PLATE_LEN);
    display_plate = plate_buf;

    if (source == EVENT_SOURCE_CARD) {
        // ==================== 卡片路径 ====================
        if (uid == NULL) {
            return PARKING_ERR_INVALID;
        }
        memcpy(uid_buf, uid, 4);

        // 1. 未注册卡拒绝入场（记录 UID 文本到参数，保持原“未注册”界面显示 UID）
        card_index = FindCardInDatabase((uint8_t*)uid);
        if (card_index == 0xFF) {
            printf("[Parking] Entry denied: unregistered card %02X%02X%02X%02X\r\n",
                   uid_buf[0], uid_buf[1], uid_buf[2], uid_buf[3]);
            // 使用 UID 文本填充车牌参数（ShowDenied 的未注册界面显示 "UID:xxxxxxxx"）
            sprintf((char*)plate_buf, "%02X%02X%02X%02X", uid_buf[0], uid_buf[1], uid_buf[2], uid_buf[3]);
            ParkingSystem_ShowDenied(OLED_DISPLAY_UNREGISTERED, plate_buf);
            return PARKING_ERR_INVALID;
        }

        // 2. 黑名单拒绝入场
        if (card_database[card_index].is_blacklist) {
            printf("[Parking] Entry denied: blacklist card %02X%02X%02X%02X\r\n",
                   uid_buf[0], uid_buf[1], uid_buf[2], uid_buf[3]);
            if (card_database[card_index].license_plate[0] != 0) {
                ParkingSystem_ShowDenied(OLED_DISPLAY_BLACKLIST, card_database[card_index].license_plate);
            } else {
                ParkingSystem_ShowDenied(OLED_DISPLAY_BLACKLIST, NULL);
            }
            return PARKING_ERR_INVALID;
        }

        // 3. 已在场内：交由出场流程处理（此处仅防御，正常由 main.c 分发到 HandleExit）
        if (FindCarByCard((uint8_t*)uid) != 0xFF) {
            printf("[Parking] Entry ignored: card already in parking\r\n");
            return PARKING_ERR_INVALID;
        }

        // 4. 车牌优先级：暂存车牌 -> 卡数据库车牌 -> CAR+UID
        if (g_license_plate_received && g_pending_license_plate[0] != 0) {
            memcpy(plate_buf, g_pending_license_plate, MAX_LICENSE_PLATE_LEN);
            memset(g_pending_license_plate, 0, MAX_LICENSE_PLATE_LEN);
            g_license_plate_received = 0;
        } else if (card_database[card_index].license_plate[0] != 0) {
            memcpy(plate_buf, card_database[card_index].license_plate, MAX_LICENSE_PLATE_LEN);
        } else {
            sprintf((char*)plate_buf, "CAR%02X%02X%02X%02X", uid_buf[0], uid_buf[1], uid_buf[2], uid_buf[3]);
        }
        display_plate = plate_buf;
    } else if (source == EVENT_SOURCE_PLATE) {
        // ==================== 车牌路径 ====================
        if (license_plate == NULL || license_plate[0] == 0) {
            return PARKING_ERR_INVALID;
        }
        memcpy(plate_buf, license_plate, MAX_LICENSE_PLATE_LEN);
        plate_buf[MAX_LICENSE_PLATE_LEN - 1] = 0;

        // 1. 车牌已绑定的注册卡：取出 UID，便于记录/显示/余额关联
        card_index = FindCardByLicensePlate((uint8_t*)license_plate);
        if (card_index != 0xFF) {
            memcpy(uid_buf, card_database[card_index].card_uid, 4);
        }

        // 2. 已在场内：忽略重复入场
        if (FindCarByLicensePlate((uint8_t*)license_plate) != 0xFF) {
            printf("[Parking] Entry ignored: plate already in parking %s\r\n", (char*)plate_buf);
            return PARKING_ERR_INVALID;
        }

        // 3. 黑名单车牌拒绝入场
        if (card_index != 0xFF && card_database[card_index].is_blacklist) {
            printf("[Parking] Entry denied: blacklist plate %s\r\n", (char*)plate_buf);
            ParkingSystem_ShowDenied(OLED_DISPLAY_BLACKLIST, (uint8_t*)g_maixcam_plate_display);
            return PARKING_ERR_INVALID;
        }

        // 4. 中文车牌不能直接上 OLED，使用 MaixCAM 转换后的显示车牌（若可用）
        if (g_maixcam_plate_display[0] != 0) {
            display_plate = (const uint8_t*)g_maixcam_plate_display;
        }
    } else {
        return PARKING_ERR_INVALID;
    }

    // ==================== 分配车位 ====================
    space_id = FindEmptyParkingSpace();
    if (space_id == 0xFF) {
        printf("[Parking] Entry failed: parking full\r\n");
        ParkingSystem_ShowDenied(OLED_DISPLAY_FULL, display_plate);
        return PARKING_ERR_NO_SPACE;
    }

    current_time = Time_GetSeconds();

    // 1. 占用车位并保存车辆信息
    parking_spaces[space_id].is_occupied = 1;
    memcpy(parking_spaces[space_id].card_uid, uid_buf, 4);
    memcpy(parking_spaces[space_id].license_plate, plate_buf, MAX_LICENSE_PLATE_LEN);

    // 2. 保存进场记录，并把“车位 -> 记录索引”绑定起来
    record_index = SaveRecord(uid_buf, plate_buf, current_time, OPERATION_IN);
    parking_spaces[space_id].record_index = record_index;

    // 3. 入场界面数据（只绘制，刷新交给 Display_Task）
    g_disp_temp_data.space_id = space_id;
    g_disp_temp_data.fee = 0;
    memcpy(g_disp_temp_data.license_plate, display_plate, MAX_LICENSE_PLATE_LEN);
    g_disp_mode = DISP_MODE_IN;
    g_disp_start_time = timer_cnt;
    OLED_Clear();
    OLED_DisplayInInfo(space_id, (uint8_t*)display_plate, uid_buf);

    // 4. 开闸：非阻塞触发（320ms 开 -> 2000ms 保持 -> 310ms 关）
    Servo_360_KeyAction();

    // 5. 上报上位机（车牌路径上报原始车牌，保持串口协议不变）
    Protocol_SendParkingIn(uid_buf, plate_buf, current_time, space_id);
    Protocol_MarkSyncPending();
    printf("CAR IN: Space=%d, Plate=%s, UID=%02X%02X%02X%02X\r\n",
           space_id + 1, (char*)plate_buf, uid_buf[0], uid_buf[1], uid_buf[2], uid_buf[3]);

    if (record_index == 0xFF) {
        printf("[Parking] Entry warning: vehicle record save failed\r\n");
        return PARKING_ERR_NO_RECORD;
    }
    return PARKING_OK;
}

/**
 * @brief  统一车辆出场处理（卡片与车牌两条路径共用）
 * @note   出场时优先使用车位记录的 record_index 更新车辆记录；
 *         记录缺失时按 UID/车牌回退查找，并在无法回写时返回 PARKING_ERR_NO_RECORD。
 */
uint8_t ParkingSystem_HandleExit(const uint8_t *uid, const uint8_t *license_plate, uint8_t source)
{
    uint8_t space_id = 0xFF;
    uint8_t record_index = 0xFF;
    uint8_t uid_buf[4];
    uint8_t plate_buf[MAX_LICENSE_PLATE_LEN];
    uint8_t display_buf[MAX_LICENSE_PLATE_LEN];
    uint32_t current_time;
    uint32_t in_time = 0;

    memset(uid_buf, 0, 4);
    memset(plate_buf, 0, MAX_LICENSE_PLATE_LEN);
    memset(display_buf, 0, MAX_LICENSE_PLATE_LEN);

    if (source == EVENT_SOURCE_CARD) {
        // ==================== 卡片路径 ====================
        if (uid == NULL) {
            return PARKING_ERR_INVALID;
        }
        space_id = FindCarByCard((uint8_t*)uid);
        if (space_id == 0xFF) {
            printf("[Parking] Exit ignored: card not in parking\r\n");
            return PARKING_ERR_NOT_IN;
        }
        memcpy(uid_buf, parking_spaces[space_id].card_uid, 4);
        memcpy(plate_buf, parking_spaces[space_id].license_plate, MAX_LICENSE_PLATE_LEN);
        memcpy(display_buf, plate_buf, MAX_LICENSE_PLATE_LEN);
    } else if (source == EVENT_SOURCE_PLATE) {
        // ==================== 车牌路径 ====================
        if (license_plate == NULL || license_plate[0] == 0) {
            return PARKING_ERR_INVALID;
        }
        space_id = FindCarByLicensePlate((uint8_t*)license_plate);
        if (space_id == 0xFF) {
            printf("[Parking] Exit ignored: plate not in parking %s\r\n", (char*)license_plate);
            return PARKING_ERR_NOT_IN;
        }
        memcpy(uid_buf, parking_spaces[space_id].card_uid, 4);
        memcpy(plate_buf, parking_spaces[space_id].license_plate, MAX_LICENSE_PLATE_LEN);
        if (g_maixcam_plate_display[0] != 0) {
            memcpy(display_buf, g_maixcam_plate_display, MAX_LICENSE_PLATE_LEN);
        } else {
            memcpy(display_buf, plate_buf, MAX_LICENSE_PLATE_LEN);
        }
    } else {
        return PARKING_ERR_INVALID;
    }

    current_time = Time_GetSeconds();

    // 1. 优先使用车位保存的车辆记录索引（修复 MaixCAM 出场记录错位）
    record_index = parking_spaces[space_id].record_index;
    if (record_index < MAX_VEHICLE_RECORDS &&
        vehicle_records[record_index].status == RECORD_IN_PARKING &&
        (Uid_IsEqual(vehicle_records[record_index].card_uid, uid_buf) ||
         strncmp((char*)vehicle_records[record_index].license_plate, (char*)plate_buf, MAX_LICENSE_PLATE_LEN) == 0)) {
        // 索引有效，直接使用
    } else {
        // 2. 兼容异常/旧数据：按 UID 或车牌回退查找
        record_index = FindInParkingRecordIndex(uid_buf, plate_buf);
    }

    if (record_index != 0xFF) {
        in_time = vehicle_records[record_index].in_time;
        vehicle_records[record_index].out_time = current_time;
        vehicle_records[record_index].fee = 0;  // 费用由上位机计算后回传
        vehicle_records[record_index].status = RECORD_LEFT;
        vehicle_records[record_index].in_parking_lot = 0;
    } else {
        // 3. 找不到进场记录：仍然放行出场，但进场时间用当前时间，避免上位机按 0 计算天价费用
        in_time = current_time;
        printf("[Parking] Exit warning: in-parking record not found\r\n");
    }

    // 4. 释放车位（record_index 已取出，释放后置无效）
    parking_spaces[space_id].is_occupied = 0;
    memset(parking_spaces[space_id].card_uid, 0, 4);
    memset(parking_spaces[space_id].license_plate, 0, MAX_LICENSE_PLATE_LEN);
    parking_spaces[space_id].record_index = 0xFF;

    // 5. 保存等待上位机回传费用的临时数据
    memcpy(g_waiting_uid, uid_buf, 4);
    memcpy(g_waiting_license_plate, plate_buf, MAX_LICENSE_PLATE_LEN);
    g_waiting_record_index = record_index;
    g_waiting_fee_state = WAITING_FEE_PENDING;

    // 6. 显示“等待结算”界面（只绘制，刷新交给 Display_Task）
    memcpy(g_disp_temp_data.license_plate, display_buf, MAX_LICENSE_PLATE_LEN);
    g_disp_temp_data.space_id = space_id;
    g_disp_temp_data.fee = 0;
    g_disp_mode = DISP_MODE_OUT;
    g_disp_start_time = timer_cnt;
    OLED_Clear();
    OLED_ShowString(0, 0, (uint8_t *)"Wait Fee...", 16, 1);
    OLED_ShowString(0, 16, display_buf, 16, 1);

    // 7. 开闸：非阻塞触发
    Servo2_360_KeyAction();

    // 8. 上报上位机（费用填 0，表示等待上位机计算）
    Protocol_SendParkingOut(uid_buf, plate_buf, in_time, current_time, 0);
    Protocol_MarkSyncPending();
    printf("CAR OUT: Plate=%s, UID=%02X%02X%02X%02X, Waiting fee from host...\r\n",
           (char*)plate_buf, uid_buf[0], uid_buf[1], uid_buf[2], uid_buf[3]);

    if (record_index == 0xFF) {
        return PARKING_ERR_NO_RECORD;
    }
    return PARKING_OK;
}

/**
 * @brief  上位机回传出场费用（CMD_PARKING_OUT_FEE）
 * @note   契约 4.3：协议层直接调用本函数
 */
void ParkingSystem_HandleFeeResult(uint32_t fee)
{
    if (g_waiting_fee_state != WAITING_FEE_PENDING) {
        return;
    }

    // 1. 更新车辆记录费用（优先使用车位绑定的记录索引）
    if (g_waiting_record_index < MAX_VEHICLE_RECORDS &&
        vehicle_records[g_waiting_record_index].status == RECORD_LEFT) {
        vehicle_records[g_waiting_record_index].fee = fee;
    }

    // 2. 显示费用信息（只绘制，刷新交给 Display_Task）
    g_disp_temp_data.fee = fee;
    g_disp_mode = DISP_MODE_OUT;
    g_disp_start_time = timer_cnt;
    OLED_Clear();
    OLED_DisplayOutInfo(g_disp_temp_data.license_plate, fee, g_waiting_uid);

    printf("CAR OUT Fee: %d\r\n", fee);

    // 3. 重置等待状态
    g_waiting_fee_state = WAITING_FEE_NONE;
    g_waiting_record_index = 0xFF;
}

/**
 * @brief  显示拒绝入场提示（未注册 / 黑名单 / 车位已满）
 * @param  reason: OLED_DISPLAY_UNREGISTERED / OLED_DISPLAY_BLACKLIST / OLED_DISPLAY_FULL
 * @param  license_plate: 车牌号（可为 NULL）；未注册场景传入 UID 文本（"XXXXXXXX"）
 */
void ParkingSystem_ShowDenied(uint8_t reason, const uint8_t *license_plate)
{
    uint8_t buf[32];

    switch (reason) {
        case OLED_DISPLAY_UNREGISTERED:
            // 未注册卡片：保持原界面（时间 + UID + Unregistered + Access Denied）
            OLED_Clear();
            sprintf((char*)buf, "%02d/%02d %02d:%02d:%02d", mon, day, hour, min, sec);
            OLED_ShowString(0, 0, buf, 16, 1);
            if (license_plate != NULL && license_plate[0] != 0) {
                sprintf((char*)buf, "uid:%s", (char*)license_plate);
                OLED_ShowString(0, 16, buf, 16, 1);
            } else {
                OLED_ShowString(0, 16, (uint8_t *)"uid:--------", 16, 1);
            }
            OLED_ShowString(0, 32, (uint8_t *)"Unregistered", 16, 1);
            OLED_ShowString(0, 48, (uint8_t *)"Access Denied", 16, 1);
            g_disp_mode = DISP_MODE_FULL;
            g_disp_start_time = timer_cnt;
            break;

        case OLED_DISPLAY_BLACKLIST:
            OLED_DisplayBlacklist((uint8_t*)license_plate);
            break;

        case OLED_DISPLAY_FULL:
            OLED_DisplayParkingFull((uint8_t*)license_plate);
            break;

        default:
            break;
    }
}

// ==================== MaixCAM车牌处理函数 ====================
/**
 * @brief  MaixCAM 车牌入场：仅做参数/防抖检查，业务逻辑复用统一入场函数
 */
void MaixCAM_HandleEntry(uint8_t *license_plate)
{
    uint8_t status;
    uint32_t now;

    if (license_plate == NULL || license_plate[0] == 0) {
        printf("[MaixCAM] Entry ignored: empty plate\r\n");
        return;
    }

    now = Time_GetSeconds();
    if ((uint32_t)(now - last_maixcam_event_time) < 2) {  // 2 秒防抖（uint32_t 差值，无回绕问题）
        printf("[MaixCAM] Entry ignored: debounce %s\r\n", (char*)license_plate);
        return;
    }
    last_maixcam_event_time = now;

    status = ParkingSystem_HandleEntry(NULL, license_plate, EVENT_SOURCE_PLATE);
    if (status == PARKING_ERR_INVALID) {
        printf("[MaixCAM] Entry ignored: already in parking %s\r\n", (char*)license_plate);
    } else if (status == PARKING_ERR_NO_SPACE) {
        printf("[MaixCAM] Entry failed: no space for %s\r\n", (char*)license_plate);
    }
}

/**
 * @brief  MaixCAM 车牌出场：仅做参数/防抖检查，业务逻辑复用统一出场函数
 */
void MaixCAM_HandleExit(uint8_t *license_plate)
{
    uint8_t status;
    uint32_t now;

    if (license_plate == NULL || license_plate[0] == 0) {
        printf("[MaixCAM] Exit ignored: empty plate\r\n");
        return;
    }

    now = Time_GetSeconds();
    if ((uint32_t)(now - last_maixcam_event_time) < 2) {
        printf("[MaixCAM] Exit ignored: debounce %s\r\n", (char*)license_plate);
        return;
    }
    last_maixcam_event_time = now;

    status = ParkingSystem_HandleExit(NULL, license_plate, EVENT_SOURCE_PLATE);
    if (status == PARKING_ERR_NOT_IN) {
        printf("[MaixCAM] Exit ignored: not in parking %s\r\n", (char*)license_plate);
    }
}

// ==================== OLED 显示函数 ====================
/**
 * @brief  OLED 显示主界面（时间、车位状态、红外状态）
 * @note   由 main.c 迁入；内部不再调用 OLED_Refresh（由 Display_Task 统一刷新）
 */
void OLED_Display_Main_Info(void)
{
    char buf[32];
    uint8_t i;                   // 循环变量
    uint8_t occupied_count = 0;  // 已占用车位计数（通过入场记录）
    uint8_t free_count = 0;      // 空闲车位计数

    // 第 1 行：显示当前时间（如 "04/30 12:34:56"）
    sprintf(buf, "%02d/%02d %02d:%02d:%02d", mon, day, hour, min, sec);
    OLED_ShowString(0, 0, (uint8_t *)buf, 16, 1);

    // 第 2 行：显示车位标题
    OLED_ShowString(0, 16, (uint8_t *)"CW:P1  P2  P3", 16, 1);

    // 第 3 行：红外传感器状态反映P1到P3的车位情况
    // 使用红外检测的状态来显示单个车位状态
    if (ir_detected_parking[0]) {
        OLED_ShowString(24, 32, (uint8_t *)"YOU", 16, 1);  // 红外检测到车
    } else {
        OLED_ShowString(24, 32, (uint8_t *)"WU ", 16, 1);  // 红外未检测到
    }

    if (ir_detected_parking[1]) {
        OLED_ShowString(56, 32, (uint8_t *)"YOU", 16, 1);
    } else {
        OLED_ShowString(56, 32, (uint8_t *)"WU ", 16, 1);
    }

    if (ir_detected_parking[2]) {
        OLED_ShowString(88, 32, (uint8_t *)"YOU", 16, 1);
    } else {
        OLED_ShowString(88, 32, (uint8_t *)"WU ", 16, 1);
    }

    // 第 4 行：统计空闲车位
    // 空闲车位数量由入场车辆数决定（通过 parking_spaces 记录）
    for (i = 0; i < MAX_PARKING_SPACES; i++) {
        if (parking_spaces[i].is_occupied) {
            occupied_count++;  // 统计已入场的车辆数
        }
    }
    // 计算空闲车位 = 总车位 - 已入场车辆数
    free_count = MAX_PARKING_SPACES - occupied_count;

    // 格式化显示，固定长度避免字符残留
    // 格式："Free: 1/3" （空闲数/总车位数）
    sprintf(buf, "Free: %d/%d    ", free_count, MAX_PARKING_SPACES);
    OLED_ShowString(0, 48, (uint8_t *)buf, 16, 1);
}

/**
 * @brief  OLED 显示车辆进场信息
 * @note   内部不再调用 OLED_Refresh（由 Display_Task 统一刷新）
 */
void OLED_DisplayInInfo(uint8_t space_id, uint8_t *license_plate, uint8_t *uid)
{
    uint8_t buf[32];
    uint8_t card_index;

    OLED_Clear();
    OLED_ShowString(0, 0, (uint8_t *)"welcome", 16, 1);

    sprintf((char*)buf, "CP:%s", (char*)license_plate);
    OLED_ShowString(0, 16, buf, 16, 1);

    sprintf((char*)buf, "UID:%02X%02X%02X%02X", uid[0], uid[1], uid[2], uid[3]);
    OLED_ShowString(0, 32, buf, 16, 1);

    card_index = FindCardInDatabase(uid);
    if (card_index != 0xFF) {
        sprintf((char*)buf, "money:%d", card_database[card_index].balance);
    } else {
        sprintf((char*)buf, "money:0");
    }
    OLED_ShowString(0, 48, buf, 16, 1);
}

/**
 * @brief  OLED 显示车辆出场信息
 * @note   内部不再调用 OLED_Refresh（由 Display_Task 统一刷新）
 */
void OLED_DisplayOutInfo(uint8_t *license_plate, uint32_t fee, uint8_t *uid)
{
    uint8_t buf[32];
    uint8_t card_index;
    uint32_t balance = 0;

    OLED_Clear();
    OLED_ShowString(0, 0, (uint8_t *)"goodbye", 16, 1);

    sprintf((char*)buf, "CP:%s", (char*)license_plate);
    OLED_ShowString(0, 16, buf, 16, 1);

    sprintf((char*)buf, "UID:%02X%02X%02X%02X", uid[0], uid[1], uid[2], uid[3]);
    OLED_ShowString(0, 32, buf, 16, 1);

    card_index = FindCardInDatabase(uid);
    if (card_index != 0xFF) {
        balance = card_database[card_index].balance;
    }
    sprintf((char*)buf, "fee:%d bal:%d", fee, balance);
    OLED_ShowString(0, 48, buf, 16, 1);
}

/**
 * @brief  获取当前已占用的车位数量
 */
static uint8_t GetOccupiedSpaceCount(void)
{
    uint8_t count = 0;
    uint8_t i;

    for (i = 0; i < MAX_PARKING_SPACES; i++) {
        if (parking_spaces[i].is_occupied) {
            count++;
        }
    }
    return count;
}

/**
 * @brief  OLED 显示黑名单提示
 * @note   内部不再调用 OLED_Refresh（由 Display_Task 统一刷新）
 */
void OLED_DisplayBlacklist(uint8_t *license_plate)
{
    uint8_t buf[32];

    OLED_Clear();

    OLED_ShowString(0, 0, (uint8_t*)"BLACKLIST", 16, 1);
    if (license_plate != NULL && license_plate[0] != 0) {
        sprintf((char*)buf, "CP:%s", (char*)license_plate);
        OLED_ShowString(0, 16, buf, 16, 1);
    }
    OLED_ShowString(0, 32, (uint8_t*)"ACCESS DENIED", 16, 1);
    OLED_ShowString(0, 48, (uint8_t*)"Card Blocked", 16, 1);

    g_disp_mode = DISP_MODE_FULL;
    g_disp_start_time = timer_cnt;
}

/**
 * @brief  OLED 显示停车场已满提示
 * @note   内部不再调用 OLED_Refresh（由 Display_Task 统一刷新）
 */
void OLED_DisplayParkingFull(uint8_t *license_plate)
{
    uint8_t buf[32];
    uint8_t occupied = GetOccupiedSpaceCount();

    (void)license_plate;  // 保留形参以兼容原接口

    OLED_Clear();

    OLED_ShowString(0, 0, (uint8_t*)"PARKING FULL", 16, 1);
    OLED_ShowString(0, 16, (uint8_t*)"Sorry!", 16, 1);
    OLED_ShowString(0, 32, (uint8_t*)"Please leave", 16, 1);

    sprintf((char*)buf, "%d/%d Spaces", occupied, MAX_PARKING_SPACES);
    OLED_ShowString(0, 48, buf, 16, 1);

    printf("[Parking] Full! %d/%d spaces occupied\r\n", occupied, MAX_PARKING_SPACES);

    g_disp_mode = DISP_MODE_FULL;
    g_disp_start_time = timer_cnt;
}

/**
 * @brief  OLED 显示未注册卡片信息（拒绝入场）
 * @note   内部不再调用 OLED_Refresh（由 Display_Task 统一刷新）
 */
void OLED_DisplayUnregistered(uint8_t *uid)
{
    uint8_t buf[32];

    OLED_Clear();

    // 第一行：显示时间
    sprintf((char*)buf, "%02d/%02d %02d:%02d:%02d", mon, day, hour, min, sec);
    OLED_ShowString(0, 0, (uint8_t *)buf, 16, 1);

    // 第二行：显示UID
    sprintf((char*)buf, "uid:%02X%02X%02X%02X", uid[0], uid[1], uid[2], uid[3]);
    OLED_ShowString(0, 16, buf, 16, 1);

    // 第三行：显示未注册提示
    OLED_ShowString(0, 32, (uint8_t *)"Unregistered", 16, 1);

    // 第四行：显示拒绝入场
    OLED_ShowString(0, 48, (uint8_t *)"Access Denied", 16, 1);

    g_disp_mode = DISP_MODE_FULL;
    g_disp_start_time = timer_cnt;
}

/**
 * @brief  OLED 显示错误信息
 * @param  error_type: 错误类型（0~3）
 * @note   0=卡不存在；1=未注册；2=黑名单；3=无车位
 */
void OLED_DisplayError(uint8_t error_type)
{
    OLED_Clear();  // 清屏
    // 根据错误类型显示对应信息
    switch (error_type) {
        case 0:
            OLED_ShowString(0, 16, (uint8_t*)"Card Not Exist", 16, 1);  // 卡不存在
            break;
        case 1:
            OLED_ShowString(0, 16, (uint8_t*)"Unregistered", 16, 1);    // 未注册
            break;
        case 2:
            OLED_ShowString(0, 16, (uint8_t*)"Blacklist Card", 16, 1);  // 黑名单
            break;
        case 3:
            OLED_ShowString(0, 16, (uint8_t*)"No Space", 16, 1);        // 无车位
            break;
        default:
            OLED_ShowString(0, 16, (uint8_t*)"Error", 16, 1);           // 未知错误
            break;
    }
}

// ==================== 模式管理函数 ====================
/**
 * @brief  进入配置模式
 * @note   界面绘制与刷新由 main.c 的 Display_Task() 负责
 */
void Mode_EnterConfig(void)
{
    if (g_config_mode_state.mode != MODE_CONFIG) {
        g_config_mode_state.mode = MODE_CONFIG;
        g_config_mode_state.last_activity_time = Time_GetSeconds();
        g_disp_mode = DISP_MODE_CONFIG;
        g_disp_start_time = timer_cnt;
        printf("[Mode] Enter CONFIG MODE\n");
    }
}

/**
 * @brief  退出配置模式，恢复正常模式
 * @note   界面绘制与刷新由 main.c 的 Display_Task() 负责
 */
void Mode_ExitConfig(void)
{
    if (g_config_mode_state.mode != MODE_NORMAL) {
        g_config_mode_state.mode = MODE_NORMAL;
        g_config_mode_state.card_on_reader = 0;  // 重置卡片状态
        memset(g_config_mode_state.current_card_uid, 0, 4);
        CardData_Init(&g_config_mode_state.current_card_data);

        PcdHalt();  // 强制让卡片休眠

        printf("[Mode] Exit CONFIG MODE\n");

        // 回到主界面：清屏并交给 Display_Task 重绘
        g_disp_mode = DISP_MODE_MAIN;
        g_disp_start_time = timer_cnt;
        OLED_Clear();
    }
}

/**
 * @brief  更新配置模式的活动时间（刷新超时计时）
 */
void Mode_UpdateActivityTime(void)
{
    if (g_config_mode_state.mode == MODE_CONFIG) {
        g_config_mode_state.last_activity_time = Time_GetSeconds();
    }
}

/**
 * @brief  检查配置模式超时
 */
void Mode_CheckTimeout(void)
{
    if (g_config_mode_state.mode == MODE_CONFIG) {
        uint32_t current_time = Time_GetSeconds();
        uint32_t elapsed = current_time - g_config_mode_state.last_activity_time;

        // 超过设定时间无活动，自动退出配置模式
        if (elapsed >= CONFIG_TIMEOUT_SECONDS) {
            printf("[Mode] CONFIG MODE timeout, exit to normal\n");
            Mode_ExitConfig();
        }
    }
}

// ==================== 配置模式显示函数 ====================
/**
 * @brief  显示配置模式主界面（含超时倒计时）
 * @note   内部不再调用 OLED_Refresh（由 Display_Task 统一刷新）
 */
void OLED_DisplayConfigMode(void)
{
    uint8_t buf[32];
    uint32_t elapsed;
    uint32_t remaining;

    OLED_Clear();
    OLED_ShowString(0, 0, (uint8_t *)"CONFIG MODE", 16, 1);
    OLED_ShowString(0, 16, (uint8_t *)"Place card to", 16, 1);
    OLED_ShowString(0, 32, (uint8_t *)"read/write", 16, 1);

    // 显示超时倒计时
    elapsed = Time_GetSeconds() - g_config_mode_state.last_activity_time;
    if (elapsed > CONFIG_TIMEOUT_SECONDS) {
        elapsed = CONFIG_TIMEOUT_SECONDS;
    }
    remaining = CONFIG_TIMEOUT_SECONDS - elapsed;
    sprintf((char*)buf, "Timeout:%ds", (int)remaining);
    OLED_ShowString(0, 48, buf, 16, 1);
}

/**
 * @brief  显示配置模式下的卡片信息
 * @note   内部不再调用 OLED_Refresh（由 Display_Task 统一刷新）
 */
void OLED_DisplayConfigCardInfo(void)
{
    uint8_t buf[32];

    if (!g_config_mode_state.card_on_reader) {
        OLED_DisplayConfigMode();  // 无卡，显示主界面
        return;
    }

    OLED_Clear();
    OLED_ShowString(0, 0, (uint8_t *)"CARD INFO", 16, 1);

    // 显示UID
    sprintf((char*)buf, "UID:%02X%02X%02X%02X",
        g_config_mode_state.current_card_uid[0],
        g_config_mode_state.current_card_uid[1],
        g_config_mode_state.current_card_uid[2],
        g_config_mode_state.current_card_uid[3]);
    OLED_ShowString(0, 16, buf, 16, 1);

    // 显示车牌号或状态
    if (g_config_mode_state.current_card_data.is_valid) {
        OLED_ShowString(0, 32, (uint8_t *)"Valid", 16, 1);
        if (g_config_mode_state.current_card_data.license_plate[0] != 0) {
            OLED_ShowString(0, 48, g_config_mode_state.current_card_data.license_plate, 16, 1);
        }
    } else {
        OLED_ShowString(0, 32, (uint8_t *)"Invalid Card", 16, 1);
    }
}

// ==================== 卡片数据操作函数 ====================
/**
 * @brief  初始化卡片数据结构
 */
void CardData_Init(CardData_t *data)
{
    data->is_valid = 0;
    memset(data->license_plate, 0, 16);
    data->balance = 0;
    data->is_blacklist = 0;
    data->valid_date = 0xFFFFFFFF;  // 永久有效
    memset(data->reserved, 0, 6);
}

/**
 * @brief  写数据到 Ultralight 卡（6 个 page）
 */
static uint8_t CardData_WriteToUltralight(CardData_t *data)
{
    uint8_t page_data[4];

    page_data[0] = data->is_valid;
    memcpy(&page_data[1], data->license_plate, 3);
    if (PcdUltralightWrite(0, page_data) != MI_OK) return 0;

    memcpy(page_data, &data->license_plate[3], 4);
    if (PcdUltralightWrite(1, page_data) != MI_OK) return 0;

    memcpy(page_data, &data->license_plate[7], 4);
    if (PcdUltralightWrite(2, page_data) != MI_OK) return 0;

    memcpy(page_data, &data->license_plate[11], 4);
    if (PcdUltralightWrite(3, page_data) != MI_OK) return 0;

    page_data[0] = (uint8_t)(data->balance & 0xFF);
    page_data[1] = (uint8_t)((data->balance >> 8) & 0xFF);
    page_data[2] = (uint8_t)((data->balance >> 16) & 0xFF);
    page_data[3] = (uint8_t)((data->balance >> 24) & 0xFF);
    if (PcdUltralightWrite(4, page_data) != MI_OK) return 0;

    page_data[0] = data->is_blacklist;
    page_data[1] = (uint8_t)(data->valid_date & 0xFF);
    page_data[2] = (uint8_t)((data->valid_date >> 8) & 0xFF);
    page_data[3] = (uint8_t)((data->valid_date >> 16) & 0xFF);
    if (PcdUltralightWrite(5, page_data) != MI_OK) return 0;

    return 1;
}

/**
 * @brief  写数据到Mifare卡
 * @param  uid: 卡UID
 * @param  data: 要写入的数据
 * @retval 1: 成功, 0: 失败
 * @note   选卡统一使用 PcdSelectEx() 取 SAK（先选卡再取 FIFO 中的 SAK）
 */
uint8_t CardData_WriteToCard(uint8_t *uid, CardData_t *data)
{
    uint8_t key[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t block_data[16];
    uint8_t sak = 0;
    uint8_t i;

    if (PcdSelectEx(uid, &sak) != MI_OK) return 0;

    if ((sak & 0x08) == 0) {
        if (CardData_WriteToUltralight(data)) {
            return 1;
        }
    }

    if (PcdAuthState(PICC_AUTHENT1B, 6, key, uid) != MI_OK) return 0;

    block_data[0] = data->is_valid;
    memcpy(&block_data[1], data->license_plate, 15);
    if (PcdWrite(6, block_data) != MI_OK) return 0;

    // 写第 7 块前需要重新选卡并认证
    if (PcdSelectEx(uid, NULL) != MI_OK) return 0;
    if (PcdAuthState(PICC_AUTHENT1B, 7, key, uid) != MI_OK) return 0;

    block_data[0] = (uint8_t)(data->balance & 0xFF);
    block_data[1] = (uint8_t)((data->balance >> 8) & 0xFF);
    block_data[2] = (uint8_t)((data->balance >> 16) & 0xFF);
    block_data[3] = (uint8_t)((data->balance >> 24) & 0xFF);
    block_data[4] = data->is_blacklist;
    block_data[5] = (uint8_t)(data->valid_date & 0xFF);
    block_data[6] = (uint8_t)((data->valid_date >> 8) & 0xFF);
    block_data[7] = (uint8_t)((data->valid_date >> 16) & 0xFF);
    block_data[8] = (uint8_t)((data->valid_date >> 24) & 0xFF);
    for (i = 9; i < 16; i++) {
        block_data[i] = 0;
    }

    if (PcdWrite(7, block_data) != MI_OK) return 0;

    return 1;
}

/**
 * @brief  从 Ultralight 卡读数据（6 个 page）
 */
static uint8_t CardData_ReadFromUltralight(CardData_t *data)
{
    uint8_t page_data[4];
    uint8_t i;

    CardData_Init(data);

    for (i = 0; i < 6; i++) {
        if (PcdUltralightRead(i, page_data) != MI_OK) {
            return 0;
        }
        if (i == 0) {
            data->is_valid = page_data[0];
            memcpy(data->license_plate, &page_data[1], 3);
        } else if (i == 1) {
            memcpy(&data->license_plate[3], page_data, 4);
        } else if (i == 2) {
            memcpy(&data->license_plate[7], page_data, 4);
        } else if (i == 3) {
            memcpy(&data->license_plate[11], page_data, 4);
            data->license_plate[15] = 0;
        } else if (i == 4) {
            data->balance = (uint32_t)page_data[0] |
                            ((uint32_t)page_data[1] << 8) |
                            ((uint32_t)page_data[2] << 16) |
                            ((uint32_t)page_data[3] << 24);
        } else if (i == 5) {
            data->is_blacklist = page_data[0];
            data->valid_date = (uint32_t)page_data[1] |
                               ((uint32_t)page_data[2] << 8) |
                               ((uint32_t)page_data[3] << 16);
        }
    }

    return 1;
}

/**
 * @brief  从Mifare卡读数据
 * @param  uid: 卡UID
 * @param  data: 读取的数据输出
 * @retval 1: 成功, 0: 失败
 * @note   选卡统一使用 PcdSelectEx() 取 SAK；由于调用时卡片已在感应区内（主循环已 PcdAnticoll），
 *         这里直接 SELECT，不再重复发 REQA。
 */
uint8_t CardData_ReadFromCard(uint8_t *uid, CardData_t *data)
{
    uint8_t key[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    uint8_t block_data[16];
    uint8_t sak = 0;

    CardData_Init(data);

    /* 直接对已知 UID 的卡片发 SELECT：
     * 配置模式下卡片已被主循环 PcdAnticoll 激活，不能再发 REQA（REQA 只在 IDLE 状态响应），
     * 因此这里不再 PcdRequest + PcdAnticoll，而是直接 PcdSelectEx 取 SAK 后读数据。*/
    if (PcdSelectEx(uid, &sak) != MI_OK) {
        return 0;
    }

    if ((sak & 0x08) == 0) {
        if (CardData_ReadFromUltralight(data)) {
            return 1;
        }
    }

    if (PcdAuthState(PICC_AUTHENT1B, 6, key, uid) != MI_OK) {
        return 0;
    }

    if (PcdRead(6, block_data) != MI_OK) {
        return 0;
    }

    data->is_valid = block_data[0];
    memcpy(data->license_plate, &block_data[1], 15);
    data->license_plate[15] = 0;

    if (PcdRead(7, block_data) != MI_OK) {
        return 0;
    }

    data->balance = (uint32_t)block_data[0] |
                    ((uint32_t)block_data[1] << 8) |
                    ((uint32_t)block_data[2] << 16) |
                    ((uint32_t)block_data[3] << 24);
    data->is_blacklist = block_data[4];
    data->valid_date = (uint32_t)block_data[5] |
                       ((uint32_t)block_data[6] << 8) |
                       ((uint32_t)block_data[7] << 16) |
                       ((uint32_t)block_data[8] << 24);

    return 1;
}

// ==================== 红外检测与车位状态同步 ====================
/**
 * @brief  根据红外检测更新车位状态并上报状态变化
 * @note   IR1/IR2/IR3_Check() 由 drivers-fw 改为读取去抖缓存（无 delay），
 *         这里每轮只做状态比较与上报，不再直接阻塞采样。
 */
void UpdateParkingSpacesByIR(void)
{
    uint8_t i;
    uint8_t current_ir_state;
    uint8_t state_changed = 0;
    uint8_t (*ir_check_functions[MAX_PARKING_SPACES])(void) = {
        IR1_Check,
        IR2_Check,
        IR3_Check
    };

    for (i = 0; i < MAX_PARKING_SPACES; i++) {
        // 获取当前红外状态（去抖后的缓存值）
        current_ir_state = ir_check_functions[i]();

        // 状态变化处理
        if (current_ir_state != ir_detected_parking[i]) {
            ir_detected_parking[i] = current_ir_state;
            state_changed = 1;

            if (current_ir_state) {
                printf("IR%d: Car detected\r\n", i + 1);
            } else {
                printf("IR%d: Car left\r\n", i + 1);
            }
        }
    }

    // 状态变化时发送状态更新（限频 100ms）
    if (state_changed && (timer_cnt - last_status_send_time >= 100)) {
        last_status_send_time = timer_cnt;
        Protocol_SendStatus();
    }
}
