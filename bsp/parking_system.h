#ifndef __PARKING_SYSTEM_H__
#define __PARKING_SYSTEM_H__

#include "stm32f4xx.h"

#define MAX_PARKING_SPACES     3
#define MAX_REGISTERED_CARDS   100
#define MAX_VEHICLE_RECORDS    200
#define MAX_LICENSE_PLATE_LEN  16

// 收费配置结构体
typedef struct {
    uint8_t fee_unit;         // 计费单位: 0=按小时, 1=按分钟
    uint32_t fee_duration;    // 计费时长（分钟）
    uint32_t fee_amount;      // 收费金额（元）
    uint32_t min_fee;         // 最低收费（元）
    uint32_t free_duration;   // 免费时长（分钟）
} FeeConfig_t;

extern FeeConfig_t g_fee_config;

// ==================== 配置模式相关定义 ====================
// 系统工作模式
typedef enum {
    MODE_NORMAL = 0,        // 正常模式：停车场正常运行
    MODE_CONFIG = 1         // 配置模式：暂停业务，响应写卡/读卡指令
} SystemMode_t;

// 配置模式超时时间（单位：秒）
#define CONFIG_TIMEOUT_SECONDS    60

// Mifare 卡数据存储块结构（用户区数据）
typedef struct {
    uint8_t is_valid;              // 标志：0=无效，1=有效
    uint8_t license_plate[16];     // 绑定车牌号
    uint32_t balance;              // 余额（单位：分）
    uint8_t is_blacklist;          // 黑名单标志
    uint32_t valid_date;           // 有效期（时间戳）
    uint8_t reserved[6];           // 保留字段
} CardData_t;

// 配置模式状态
typedef struct {
    SystemMode_t mode;             // 当前模式
    uint32_t last_activity_time;   // 最后一次活动时间（秒，epoch=2000-01-01）
    uint8_t card_on_reader;        // 卡片是否在感应区（0=否，1=是）
    uint8_t current_card_uid[4];   // 当前卡的UID
    CardData_t current_card_data;  // 当前卡的数据
} ConfigModeState_t;

typedef enum {
    CARD_NO_EXIST = 0,
    CARD_UNREGISTERED,
    CARD_VALID,
    CARD_EXPIRED,
    CARD_BLACKLIST
} CardStatusType;

typedef enum {
    OPERATION_IN = 0,
    OPERATION_OUT
} OperationType;

typedef enum {
    RECORD_EMPTY = 0,
    RECORD_IN_PARKING,
    RECORD_LEFT
} RecordStatusType;

// 费用等待状态
typedef enum {
    WAITING_FEE_NONE,
    WAITING_FEE_PENDING,
} WaitingFeeState_t;

extern volatile WaitingFeeState_t g_waiting_fee_state;
extern uint8_t g_waiting_uid[4];
extern uint8_t g_waiting_license_plate[16];
extern uint8_t g_waiting_record_index;

// ==================== 统一进出场事件来源（契约 4.2） ====================
typedef enum {
    EVENT_SOURCE_CARD = 0,   // 事件来源：RFID 刷卡
    EVENT_SOURCE_PLATE = 1   // 事件来源：MaixCAM 车牌识别
} EventSource_t;

// 统一进出场处理返回码
#define PARKING_OK              0   // 处理成功
#define PARKING_ERR_NO_SPACE    1   // 车位已满
#define PARKING_ERR_NOT_IN      2   // 车辆不在场内（出场时）
#define PARKING_ERR_INVALID     3   // 参数非法 / 未注册 / 黑名单 / 重复入场
#define PARKING_ERR_NO_RECORD   4   // 车辆记录缺失（车位已释放，费用无法回写）

// ParkingSystem_ShowDenied 的拒绝原因
#define OLED_DISPLAY_UNREGISTERED  1   // 未注册卡片
#define OLED_DISPLAY_BLACKLIST     2   // 黑名单
#define OLED_DISPLAY_FULL          3   // 车位已满

// 卡片入场时暂存的车牌（例如 MaixCAM 识别结果，优先于卡数据库车牌）
extern uint8_t g_pending_license_plate[MAX_LICENSE_PLATE_LEN];
extern uint8_t g_license_plate_received;

typedef struct {
    uint8_t card_uid[4];
    uint8_t is_registered;
    uint8_t is_blacklist;
    uint8_t license_plate[MAX_LICENSE_PLATE_LEN];
    uint32_t balance;
    uint32_t valid_date;
} RegisteredCardType;

typedef struct {
    uint8_t card_uid[4];
    uint8_t license_plate[MAX_LICENSE_PLATE_LEN];
    uint32_t in_time;
    uint32_t out_time;
    uint32_t fee;
    uint8_t status;
    uint8_t in_parking_lot;
} VehicleRecordType;

typedef struct {
    uint8_t is_occupied;
    uint8_t card_uid[4];
    uint8_t license_plate[MAX_LICENSE_PLATE_LEN];
    // 该车位对应 vehicle_records 的索引（0xFF=无效），入场时保存，出场/费用回传优先使用；
    // 修复 MaixCAM 出场把“车位索引”当作“记录索引”的缺陷。
    uint8_t record_index;
} ParkingSpaceType;

extern ParkingSpaceType parking_spaces[MAX_PARKING_SPACES];
extern RegisteredCardType card_database[MAX_REGISTERED_CARDS];
extern VehicleRecordType vehicle_records[MAX_VEHICLE_RECORDS];
extern uint32_t record_count;
extern ConfigModeState_t g_config_mode_state;  // 配置模式状态
extern uint8_t ir_detected_parking[MAX_PARKING_SPACES];  // 红外检测到的车位状态

void ParkingSystem_Init(void);
uint8_t FindCardInDatabase(uint8_t *uid);
uint8_t AddCardToDatabase(uint8_t *uid, uint8_t *license_plate);
uint8_t RemoveCardFromDatabase(uint8_t *uid);
uint8_t SetCardBlacklist(uint8_t *uid, uint8_t blacklist);
uint8_t IsCardBlacklisted(uint8_t *uid);
uint8_t UpdateCardBalance(uint8_t *uid, uint32_t balance);
uint8_t FindEmptyParkingSpace(void);
uint8_t FindCarByCard(uint8_t *uid);
uint8_t FindCardByLicensePlate(uint8_t *license_plate);
uint8_t FindCarByLicensePlate(uint8_t *license_plate);
uint8_t SaveRecord(uint8_t *uid, uint8_t *license_plate, uint32_t time, uint8_t operation);

// ==================== 统一进出场接口（卡片 / 车牌共用） ====================
/**
 * @brief  统一车辆入场处理（卡片与车牌两条路径共用）
 * @param  uid:           卡片 UID；source=EVENT_SOURCE_PLATE 时可为 NULL
 * @param  license_plate: 车牌号；source=EVENT_SOURCE_CARD 时可为 NULL
 * @param  source:        EventSource_t
 * @retval PARKING_OK / PARKING_ERR_NO_SPACE / PARKING_ERR_INVALID / PARKING_ERR_NO_RECORD
 */
uint8_t ParkingSystem_HandleEntry(const uint8_t *uid, const uint8_t *license_plate, uint8_t source);

/**
 * @brief  统一车辆出场处理（卡片与车牌两条路径共用）
 * @retval PARKING_OK / PARKING_ERR_NOT_IN / PARKING_ERR_INVALID / PARKING_ERR_NO_RECORD
 */
uint8_t ParkingSystem_HandleExit(const uint8_t *uid, const uint8_t *license_plate, uint8_t source);

/**
 * @brief  上位机回传出场费用（CMD_PARKING_OUT_FEE）：更新记录并显示费用
 */
void ParkingSystem_HandleFeeResult(uint32_t fee);

/**
 * @brief  显示拒绝入场提示（未注册 / 黑名单 / 车位已满）
 * @param  reason: OLED_DISPLAY_UNREGISTERED / OLED_DISPLAY_BLACKLIST / OLED_DISPLAY_FULL
 * @param  license_plate: 车牌号（可为 NULL）；未注册场景传入 UID 文本（"XXXXXXXX"）以保持原界面显示
 */
void ParkingSystem_ShowDenied(uint8_t reason, const uint8_t *license_plate);

// ==================== MaixCAM车牌处理函数 ====================
void MaixCAM_HandleEntry(uint8_t *license_plate);  // 处理MaixCAM入场
void MaixCAM_HandleExit(uint8_t *license_plate);   // 处理MaixCAM出场
void OLED_Display_Main_Info(void);                 // 主界面（由 main.c 迁入，供 Display_Task 调用）
void OLED_Display_Main_TimeLine(void);             // 主界面第 1 行时间（每秒局部重绘）
void OLED_DisplayInInfo(uint8_t space_id, uint8_t *license_plate, uint8_t *uid);
void OLED_DisplayOutInfo(uint8_t *license_plate, uint32_t fee, uint8_t *uid);
void OLED_DisplayUnregistered(uint8_t *uid);
void OLED_DisplayError(uint8_t error_type);
void OLED_DisplayParkingFull(uint8_t *license_plate);
void OLED_DisplayBlacklist(uint8_t *license_plate);

// ==================== 模式管理函数 ====================
void Mode_EnterConfig(void);                  // 进入配置模式
void Mode_ExitConfig(void);                   // 退出配置模式
void Mode_UpdateActivityTime(void);          // 更新活动时间（刷新超时计时）
void Mode_CheckTimeout(void);                // 检查配置模式超时

// ==================== 配置模式显示函数 ====================
void OLED_DisplayConfigMode(void);           // 显示配置模式界面
void OLED_DisplayConfigCardInfo(void);       // 显示卡片信息

// ==================== 卡片数据操作函数 ====================
void CardData_Init(CardData_t *data);       // 初始化卡片数据结构
uint8_t CardData_WriteToCard(uint8_t *uid, CardData_t *data); // 写数据到卡片
uint8_t CardData_ReadFromCard(uint8_t *uid, CardData_t *data);  // 从卡片读数据

// ==================== 红外检测与车位状态同步函数 ====================
void UpdateParkingSpacesByIR(void);         // 根据红外检测更新车位状态

#endif
