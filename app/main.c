#include "board.h"              // 板级初始化头文件（时钟、GPIO等底层配置）
#include "bsp_uart.h"           // 串口驱动头文件（用于调试输出）
#include "bsp_uart6.h"          // USART6驱动头文件（用于MaixCAM通信）
#include <stdio.h>              // 标准输入输出库（用于 sprintf、printf 等）
#include <string.h>             // 字符串操作库（用于 memcpy、memset 等）
#include "stm32f4xx.h"          // STM32F4 固件库头文件
#include "oled.h"               // OLED 显示屏驱动头文件
#include "bsp_rc522.h"          // RC522 RFID 读卡模块驱动头文件
#include "bsp_sg90.h"           // SG90 舵机/按键驱动头文件（道闸控制）
#include "bsp_hongwai.h"        // 红外传感器驱动头文件（车位检测）
#include "parking_system.h"     // 停车场业务逻辑头文件（车位、记录、统一进出场接口）
#include "app_state.h"          // 应用共享状态（timer_cnt / 时间服务 / 显示模式）
#include "protocol.h"           // 通信协议头文件（与上位机交互）

// 说明：oled.h 已把 u8/u16/u32 定义为宏，本文件直接使用 uint8_t 等标准类型，避免 typedef 重定义告警。

// ==================== 全局变量定义 ====================
// 注意（契约 4.0）：
//   timer_cnt / year / mon / day / hour / min / sec 定义在本文件；
//   g_disp_mode / g_disp_start_time / g_disp_temp_data 定义在 bsp/parking_system.c。
volatile uint32_t timer_cnt = 0;  // 定时器计数（1ms 加 1，volatile 防止编译器优化）
volatile uint8_t  sec_flag  = 0;  // 秒标志（每 1s 置 1，TIM2 中断中置位）

// 软件时钟（由于未使用硬件 RTC，用软件模拟；epoch = 2000-01-01 00:00:00 本地时间）
uint16_t year = 2024;     // 年
uint8_t mon  = 4;         // 月
uint8_t day  = 30;        // 日
uint8_t hour = 0;         // 时
uint8_t min  = 0;         // 分
uint8_t sec  = 0;         // 秒

// 内部保存的 epoch 秒数：时间的唯一基准，保证跨月/跨年单调递增
static uint32_t g_time_seconds = 0;
static uint8_t  g_time_initialized = 0;

// RFID 读卡状态
static uint8_t ucArray_ID[4];         // 存放 RFID 卡的 UID（卡号）
static uint8_t ucStatusReturn;        // RC522 操作返回状态
static uint8_t last_card_uid[4] = {0};  // 记录上一次读卡的 UID（防止重复处理）
static uint8_t last_card_uid_valid = 0; // 上一次 UID 是否有效
static uint8_t g_card_changed = 0;      // 配置模式下卡片信息变化标志（供 Display_Task 重绘）

// ==================== 时间服务（契约 4.0，实现在本文件） ====================
#define TIME_YEAR_BASE    2000U   // epoch 起始年份
#define SECS_PER_MINUTE   60UL
#define SECS_PER_HOUR     3600UL
#define SECS_PER_DAY      86400UL

static const uint8_t s_days_in_month[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

/**
 * @brief  判断是否闰年（2000~2099 范围内；仍按标准公式实现）
 */
static uint8_t Time_IsLeapYear(uint16_t y)
{
    if ((y % 400U) == 0U) {
        return 1;
    }
    if (((y % 4U) == 0U) && ((y % 100U) != 0U)) {
        return 1;
    }
    return 0;
}

/**
 * @brief  返回指定年月的天数（真实日历：大小月 + 闰年 2 月）
 */
static uint8_t Time_GetDaysInMonth(uint16_t y, uint8_t m)
{
    if (m < 1 || m > 12) {
        return 31;
    }
    if (m == 2 && Time_IsLeapYear(y)) {
        return 29;
    }
    return s_days_in_month[m - 1];
}

/**
 * @brief  由年月日时分秒计算 epoch 秒数（epoch = 2000-01-01 00:00:00 本地时间）
 */
static uint32_t Time_CalcSeconds(uint16_t y, uint8_t mo, uint8_t d,
                                 uint8_t h, uint8_t mi, uint8_t s)
{
    uint32_t days = 0;
    uint16_t yy;
    uint8_t mm;

    for (yy = (uint16_t)TIME_YEAR_BASE; yy < y; yy++) {
        days += Time_IsLeapYear(yy) ? 366UL : 365UL;
    }
    for (mm = 1; mm < mo; mm++) {
        days += (uint32_t)Time_GetDaysInMonth(y, mm);
    }
    days += (uint32_t)(d - 1);

    return days * SECS_PER_DAY
         + (uint32_t)h * SECS_PER_HOUR
         + (uint32_t)mi * SECS_PER_MINUTE
         + (uint32_t)s;
}

/**
 * @brief  由 epoch 秒数反算年月日时分秒并写回全局时间变量
 */
static void Time_ApplySeconds(uint32_t total)
{
    uint32_t days;
    uint32_t rem;
    uint16_t y;
    uint16_t days_in_year;
    uint8_t m;
    uint8_t dm;

    days = total / SECS_PER_DAY;
    rem  = total % SECS_PER_DAY;

    hour = (uint8_t)(rem / SECS_PER_HOUR);
    rem %= SECS_PER_HOUR;
    min  = (uint8_t)(rem / SECS_PER_MINUTE);
    sec  = (uint8_t)(rem % SECS_PER_MINUTE);

    // 1. 从 2000 年开始逐年扣减，得到年份与当年内天数
    y = (uint16_t)TIME_YEAR_BASE;
    for (;;) {
        days_in_year = Time_IsLeapYear(y) ? 366U : 365U;
        if (days < (uint32_t)days_in_year) {
            break;
        }
        days -= (uint32_t)days_in_year;
        y++;
        if (y > 2099U) {   // 防御：32 位秒数范围内不会到这里
            y = 2099U;
            days = 0;
            break;
        }
    }
    year = y;

    // 2. 从 1 月开始逐月扣减，得到月份与日
    m = 1;
    for (;;) {
        dm = Time_GetDaysInMonth(y, m);
        if (days < (uint32_t)dm) {
            break;
        }
        days -= (uint32_t)dm;
        m++;
        if (m > 12U) {     // 防御
            m = 12U;
            days = 0;
            break;
        }
    }
    mon = m;
    day = (uint8_t)(days + 1U);
}

/**
 * @brief  首次使用时按当前全局时间初始化 epoch 秒数
 */
static void Time_EnsureInit(void)
{
    if (!g_time_initialized) {
        g_time_seconds = Time_CalcSeconds(year, mon, day, hour, min, sec);
        g_time_initialized = 1;
    }
}

/**
 * @brief  主循环调用的时钟任务：检测 sec_flag，递增 1 秒并维护年月日时分秒
 * @note   基于 epoch 秒数反算，跨月/跨年/闰年正确，且时间严格单调前进
 */
void Time_Update(void)
{
    Time_EnsureInit();

    if (sec_flag) {          // 检查秒标志（TIM2 中断每 1s 置位）
        sec_flag = 0;
        g_time_seconds++;    // epoch 秒数 +1（uint32_t，约 136 年后才回绕）
        Time_ApplySeconds(g_time_seconds);
    }
}

/**
 * @brief  设置软件时钟（上位机 CMD_SET_TIME 使用）
 * @retval 1 成功；0 参数越界（越界时不修改时钟）
 */
uint8_t Time_SetDateTime(uint16_t y, uint8_t mo, uint8_t d,
                         uint8_t h, uint8_t mi, uint8_t s)
{
    // 1. 按真实日历校验（年份 2000~2099，大小月/闰年）
    if (y < (uint16_t)TIME_YEAR_BASE || y > 2099U) {
        return 0;
    }
    if (mo < 1U || mo > 12U) {
        return 0;
    }
    if (d < 1U || d > Time_GetDaysInMonth(y, mo)) {
        return 0;
    }
    if (h > 23U || mi > 59U || s > 59U) {
        return 0;
    }

    // 2. 参数合法才修改时钟
    year = y;
    mon  = mo;
    day  = d;
    hour = h;
    min  = mi;
    sec  = s;
    g_time_seconds = Time_CalcSeconds(y, mo, d, h, mi, s);
    g_time_initialized = 1;
    return 1;
}

/**
 * @brief  获取当前时间戳（秒，epoch = 2000-01-01 00:00:00 本地时间）
 */
uint32_t Time_GetSeconds(void)
{
    Time_EnsureInit();
    return g_time_seconds;
}

/**
 * @brief  获取当前日期时间
 */
void Time_GetDateTime(uint16_t *y, uint8_t *mo, uint8_t *d,
                      uint8_t *h, uint8_t *mi, uint8_t *s)
{
    if (y != NULL)  { *y  = year; }
    if (mo != NULL) { *mo = mon; }
    if (d != NULL)  { *d  = day; }
    if (h != NULL)  { *h  = hour; }
    if (mi != NULL) { *mi = min; }
    if (s != NULL)  { *s  = sec; }
}

// ==================== 定时器驱动函数 ====================
/**
 * @brief  TIM2 初始化函数
 * @note   配置为 1ms 中断一次，用于系统时基
 *         假设系统时钟为 84MHz：
 *         - 预分频 Prescaler = 83：84MHz / (83+1) = 1MHz 计数频率
 *         - 重装载值 Period = 999：1MHz / (999+1) = 1kHz（即 1ms 一次中断）
 */
void TIM2_Init(void)
{
    TIM_TimeBaseInitTypeDef TIM_TimeBaseStructure;
    NVIC_InitTypeDef NVIC_InitStructure;

    // 使能 TIM2 时钟
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2, ENABLE);

    // 配置 TIM2 时基
    TIM_TimeBaseStructure.TIM_Period = 999;                  // 重装载值
    TIM_TimeBaseStructure.TIM_Prescaler = 83;                // 预分频系数
    TIM_TimeBaseStructure.TIM_ClockDivision = TIM_CKD_DIV1;  // 时钟分频
    TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_Up; // 向上计数模式
    TIM_TimeBaseInit(TIM2, &TIM_TimeBaseStructure);

    // 配置 TIM2 中断
    NVIC_InitStructure.NVIC_IRQChannel = TIM2_IRQn;          // 中断通道
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 2; // 抢占优先级
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;        // 响应优先级
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;           // 使能中断
    NVIC_Init(&NVIC_InitStructure);

    TIM_ITConfig(TIM2, TIM_IT_Update, ENABLE);  // 使能更新中断
    TIM_Cmd(TIM2, ENABLE);                       // 启动 TIM2
}

/**
 * @brief  TIM2 中断服务函数
 * @note   每 1ms 触发一次，用于计数和产生秒标志
 */
void TIM2_IRQHandler(void)
{
    if (TIM_GetITStatus(TIM2, TIM_IT_Update) != RESET)  // 检查更新中断标志
    {
        timer_cnt++;                    // 1ms 计数加 1
        if ((timer_cnt % 1000U) == 0U)  // 每 1000ms（1s）
        {
            sec_flag = 1;               // 置位秒标志
        }
        TIM_ClearITPendingBit(TIM2, TIM_IT_Update);  // 清除中断标志
    }
}

// ==================== RFID 卡处理核心函数 ====================
/**
 * @brief  处理 RC522 读卡逻辑（寻卡、防冲突、统一进出场/配置模式读写卡）
 * @note   契约 4.2：正常模式不做 PcdAuthState，仅用 PcdSelectEx 取 SAK；
 *         未注册/黑名单/在库/入场分别调用统一的业务函数。
 */
void Process_RC522_Card(void)
{
    uint8_t same_card;
    uint8_t sak = 0;
    uint8_t card_index;
    uint8_t uid_text[9];

    // 1. 寻卡（检测是否有卡靠近），失败重试 1 次
    ucStatusReturn = PcdRequest(PICC_REQALL, ucArray_ID);
    if (ucStatusReturn != MI_OK) {
        ucStatusReturn = PcdRequest(PICC_REQALL, ucArray_ID);
    }

    if (ucStatusReturn != MI_OK) {
        // 无卡：清理“卡在感应区”状态
        if (g_config_mode_state.mode == MODE_CONFIG) {
            if (g_config_mode_state.card_on_reader) {
                g_config_mode_state.card_on_reader = 0;
                CardData_Init(&g_config_mode_state.current_card_data);
                g_card_changed = 1;   // 通知 Display_Task 重绘配置界面
            }
        } else {
            last_card_uid_valid = 0;  // 正常模式下清除 UID 有效标记
        }
        return;
    }

    // 2. 防冲突（获取卡的 UID，防止多张卡同时靠近）
    if (PcdAnticoll(ucArray_ID) != MI_OK) {
        return;
    }

    if (g_config_mode_state.mode == MODE_CONFIG) {
        // ==================== 配置模式：只读卡，不触发道闸 ====================
        same_card = 0;
        if (g_config_mode_state.card_on_reader &&
            g_config_mode_state.current_card_uid[0] == ucArray_ID[0] &&
            g_config_mode_state.current_card_uid[1] == ucArray_ID[1] &&
            g_config_mode_state.current_card_uid[2] == ucArray_ID[2] &&
            g_config_mode_state.current_card_uid[3] == ucArray_ID[3]) {
            same_card = 1;
        }

        if (!same_card) {
            memcpy(g_config_mode_state.current_card_uid, ucArray_ID, 4);
            g_config_mode_state.card_on_reader = 1;
            g_config_mode_state.last_activity_time = Time_GetSeconds();
            g_card_changed = 1;

            // 读卡函数内部会 SELECT 并读取 SAK，区分 Mifare Classic / Ultralight；
            // 不要在这里先 PcdSelectEx 再让读卡函数发 REQA（激活态卡片不响应寻卡，会读卡失败）。
            CardData_ReadFromCard(ucArray_ID, &g_config_mode_state.current_card_data);
            printf("Card detected in config mode, UID=%02X%02X%02X%02X\r\n",
                   ucArray_ID[0], ucArray_ID[1], ucArray_ID[2], ucArray_ID[3]);
        }
        PcdHalt();
        return;
    }

    // ==================== 正常模式：统一进出场 ====================
    same_card = 0;
    if (last_card_uid_valid &&
        last_card_uid[0] == ucArray_ID[0] &&
        last_card_uid[1] == ucArray_ID[1] &&
        last_card_uid[2] == ucArray_ID[2] &&
        last_card_uid[3] == ucArray_ID[3]) {
        same_card = 1;
    }
    if (same_card) {   // 同一张卡仍停留在感应区，不重复处理
        return;
    }

    memcpy(last_card_uid, ucArray_ID, 4);  // 记录本次 UID
    last_card_uid_valid = 1;               // 标记 UID 有效

    // 3. 选卡（PcdSelectEx 内部把 SAK 从 FIFO 取出，避免读走 FIFO 后 SAK 恒为 0）
    if (PcdSelectEx(ucArray_ID, &sak) != MI_OK) {
        PcdHalt();
        return;
    }

    // 4. 未注册卡：拒绝入场
    card_index = FindCardInDatabase(ucArray_ID);
    if (card_index == 0xFF) {
        sprintf((char*)uid_text, "%02X%02X%02X%02X",
                ucArray_ID[0], ucArray_ID[1], ucArray_ID[2], ucArray_ID[3]);
        ParkingSystem_ShowDenied(OLED_DISPLAY_UNREGISTERED, uid_text);
        printf("[Unregistered] Card denied: %s\r\n", (char*)uid_text);
    } else if (card_database[card_index].is_blacklist) {
        // 5. 黑名单卡：拒绝入场
        if (card_database[card_index].license_plate[0] != 0) {
            ParkingSystem_ShowDenied(OLED_DISPLAY_BLACKLIST, card_database[card_index].license_plate);
        } else {
            ParkingSystem_ShowDenied(OLED_DISPLAY_BLACKLIST, NULL);
        }
        printf("[Blacklist] Card blocked: %02X%02X%02X%02X\r\n",
               ucArray_ID[0], ucArray_ID[1], ucArray_ID[2], ucArray_ID[3]);
    } else if (FindCarByCard(ucArray_ID) != 0xFF) {
        // 6. 在库内：统一出场
        ParkingSystem_HandleExit(ucArray_ID, NULL, EVENT_SOURCE_CARD);
    } else {
        // 7. 不在库内：统一入场（车位满/未注册等由统一函数内部处理）
        ParkingSystem_HandleEntry(ucArray_ID, NULL, EVENT_SOURCE_CARD);
    }

    PcdHalt();  // 让卡片进入休眠状态（防止重复读卡）
}

// ==================== 主循环任务函数 ====================
/**
 * @brief  MaixCAM 事件处理：处理 entry/exit flag，调用统一的车牌进出场接口
 * @note   本函数在 main.c 内实现为 static，避免与其他模块符号重名
 */
static void MaixCAM_ProcessEvents(void)
{
    if (g_maixcam_entry_flag) {
        g_maixcam_entry_flag = 0;
        MaixCAM_HandleEntry((uint8_t*)g_maixcam_plate);
    }
    if (g_maixcam_exit_flag) {
        g_maixcam_exit_flag = 0;
        MaixCAM_HandleExit((uint8_t*)g_maixcam_plate);
    }
}

/**
 * @brief  按键动作处理（KEY_Task 由 drivers-fw 在 bsp_sg90.c 提供，仅负责扫描/去抖）
 * @note   KEY3：切换配置模式；KEY1/KEY2：正常模式下测试入口/出口舵机。
 *         按键判断沿用 500ms 防连按，动作本身均为非阻塞触发。
 */
static void Process_KeyActions(void)
{
    static uint32_t key1_last_press_time = 0;
    static uint32_t key2_last_press_time = 0;
    static uint32_t key3_last_press_time = 0;

    // KEY3：切换 普通模式 <-> 配置模式
    if (KEY3_Press() && (uint32_t)(timer_cnt - key3_last_press_time) > 500UL) {
        key3_last_press_time = timer_cnt;
        if (g_config_mode_state.mode == MODE_NORMAL) {
            Mode_EnterConfig();
            printf("Enter config mode via KEY3\r\n");
        } else {
            Mode_ExitConfig();
            printf("Exit config mode via KEY3\r\n");
        }
    }

    // KEY1 / KEY2：只有正常模式下才响应舵机测试
    if (g_config_mode_state.mode == MODE_NORMAL) {
        if (KEY1_Press() && (uint32_t)(timer_cnt - key1_last_press_time) > 500UL) {
            key1_last_press_time = timer_cnt;
            Servo_360_KeyAction();  // 非阻塞触发入口道闸
            printf("Servo1 triggered by KEY1\r\n");
        }

        if (KEY2_Press() && (uint32_t)(timer_cnt - key2_last_press_time) > 500UL) {
            key2_last_press_time = timer_cnt;
            Servo2_360_KeyAction(); // 非阻塞触发出口道闸
            printf("Servo2 triggered by KEY2\r\n");
        }
    }
}

/**
 * @brief  显示调度任务（契约 4.1）
 * @note   1) 配置模式：秒变化时刷新倒计时，卡片变化时刷新卡信息；
 *         2) MAIN 模式：仅在回到主界面或秒变化时重绘；
 *         3) IN/OUT 5s、FULL 10s 后切回 MAIN 并 OLED_Clear()；
 *         4) 末尾统一 OLED_Refresh()（只刷脏页，无脏页立即返回）。
 */
static void Display_Task(void)
{
    static DisplayMode_t last_disp_mode = DISP_MODE_MAIN;   // 上一轮显示模式
    static uint8_t last_config_mode = (uint8_t)MODE_NORMAL; // 上一轮系统模式
    static uint8_t last_drawn_sec = 0xFF;                   // 上次绘制时的秒
    static uint8_t force_redraw = 1;                        // 强制重绘标志（上电/切界面）
    static uint8_t last_ir_state[MAX_PARKING_SPACES] = {0xFF, 0xFF, 0xFF};  // 上次主界面红外状态
    static uint8_t last_occupied_count = 0xFF;              // 上次主界面占用车位数
    uint8_t cur_sec;
    uint8_t cur_config_mode;
    uint8_t occupied_count;
    uint8_t need_full_redraw;
    uint8_t i;
    char buf[24];

    // 1. 配置模式超时检查（超时后 Mode_ExitConfig 会切回普通模式）
    if (g_config_mode_state.mode == MODE_CONFIG) {
        Mode_CheckTimeout();
    }

    // 2. 普通 <-> 配置 模式切换：清屏、强制重绘
    cur_config_mode = (uint8_t)g_config_mode_state.mode;
    if (cur_config_mode != last_config_mode) {
        last_config_mode = cur_config_mode;
        g_card_changed = 0;
        force_redraw = 1;
        last_drawn_sec = 0xFF;
        OLED_Clear();
    }

    if (g_config_mode_state.mode == MODE_CONFIG) {
        // ==================== 配置模式界面 ====================
        cur_sec = sec;
        if (g_card_changed) {
            g_card_changed = 0;
            force_redraw = 1;
        }
        if (g_config_mode_state.card_on_reader) {
            // 有卡：只在卡片信息变化时重绘
            if (force_redraw) {
                force_redraw = 0;
                last_drawn_sec = cur_sec;
                OLED_DisplayConfigCardInfo();
            }
        } else {
            // 无卡：每秒刷新倒计时
            if (force_redraw || cur_sec != last_drawn_sec) {
                force_redraw = 0;
                last_drawn_sec = cur_sec;
                OLED_DisplayConfigMode();
            }
        }
    } else {
        // ==================== 正常模式界面 ====================
        switch (g_disp_mode) {
            case DISP_MODE_MAIN:
                cur_sec = sec;
                // 统计占用车位数，并与上一轮比较，判断是否需要整屏重绘
                occupied_count = 0;
                for (i = 0; i < MAX_PARKING_SPACES; i++) {
                    if (parking_spaces[i].is_occupied) {
                        occupied_count++;
                    }
                }
                need_full_redraw = (uint8_t)(force_redraw || (last_disp_mode != DISP_MODE_MAIN));
                if (occupied_count != last_occupied_count) {
                    need_full_redraw = 1;
                }
                if (!need_full_redraw) {
                    for (i = 0; i < MAX_PARKING_SPACES; i++) {
                        if (ir_detected_parking[i] != last_ir_state[i]) {
                            need_full_redraw = 1;
                            break;
                        }
                    }
                }

                if (need_full_redraw) {
                    // 回到主界面 / 车位或红外状态变化：整屏重绘一次
                    force_redraw = 0;
                    last_drawn_sec = cur_sec;
                    last_occupied_count = occupied_count;
                    for (i = 0; i < MAX_PARKING_SPACES; i++) {
                        last_ir_state[i] = ir_detected_parking[i];
                    }
                    OLED_Display_Main_Info();
                } else if (cur_sec != last_drawn_sec) {
                    // 只有时间变化：仅重绘第 1 行（2 页），减少 IIC 刷新耗时
                    last_drawn_sec = cur_sec;
                    sprintf(buf, "%02d/%02d %02d:%02d:%02d", mon, day, hour, min, sec);
                    OLED_ShowString(0, 0, (uint8_t *)buf, 16, 1);
                }
                break;

            case DISP_MODE_FULL:
                // 车位已满/未注册/黑名单提示：10 秒后切回主界面
                if ((uint32_t)(timer_cnt - g_disp_start_time) >= 10000UL) {
                    g_disp_mode = DISP_MODE_MAIN;
                    g_disp_start_time = timer_cnt;
                    OLED_Clear();
                    force_redraw = 1;
                    last_drawn_sec = 0xFF;
                }
                break;

            case DISP_MODE_IN:
            case DISP_MODE_OUT:
            case DISP_MODE_CONFIG:
            default:
                // 入场/出场界面：5 秒后切回主界面
                if ((uint32_t)(timer_cnt - g_disp_start_time) >= 5000UL) {
                    g_disp_mode = DISP_MODE_MAIN;
                    g_disp_start_time = timer_cnt;
                    OLED_Clear();
                    force_redraw = 1;
                    last_drawn_sec = 0xFF;
                }
                break;
        }
    }

    last_disp_mode = g_disp_mode;

    // 3. 统一刷新：只刷脏页（IIC 100kHz 下典型 2 页约 25ms）
    OLED_Refresh();
}

// ==================== 主函数 ====================
int main(void)
{
    // 0. 软件时钟按真实日历初始化（保持与全局初值一致）
    Time_SetDateTime(2024, 4, 30, 0, 0, 0);

    // 1. 初始化底层硬件
    board_init();              // 板级初始化（时钟、GPIO等）
    uart1_init(115200U);       // 串口 1 初始化，波特率 115200
    TIM2_Init();               // 定时器 2 初始化（系统时基）

    // 2. 初始化外设模块
    SG90_Init();               // 舵机初始化
    KEY_Init();                // 按键初始化
    IR_Init();                 // 红外传感器初始化
    OLED_Init();               // OLED 初始化
    OLED_Clear();              // OLED 清屏（只清 GRAM 并标记脏页）

    // 3. 初始化业务逻辑
    ParkingSystem_Init();      // 停车场系统初始化（车位、记录等）
    Protocol_Init();           // 通信协议初始化

    // 4. 初始化 RC522 RFID 模块
    printf("Init....\r\n");
    RC522_Init();              // RC522 初始化
    RC522_Rese();              // RC522 复位
    printf("RC522 Init OK\r\n");

    // 5. 初始化 USART6 (MaixCAM通信)
    uart6_init(115200);
    printf("MaixCAM UART6 Ready\r\n");

    OLED_Refresh();            // 上电先真正清一次屏（之后由 Display_Task 统一刷新）
    printf("Parking System Ready\r\n");

    // 6. 主循环（前后台系统的后台，全部任务非阻塞）
    while (1)
    {
        Time_Update();               // 1. 软件时钟（真实日历，1s 进位）
        Servo_Task();                // 2. 舵机非阻塞状态机（drivers-fw）
        IR_Task();                   // 3. 红外周期采样 + 去抖（drivers-fw）
        UpdateParkingSpacesByIR();   // 4. 红外状态 -> 车位状态/上报（只调用一次）
        Protocol_Task();             // 5. 协议解析/后台分片发送/心跳（comm-fw）
        Process_RC522_Card();        // 6. 刷卡：配置模式读写卡 / 正常模式统一进出场
        MaixCAM_ProcessEvents();     // 7. MaixCAM 车牌 event flag -> 统一进出场
        Key_Task();                  // 8. 按键非阻塞扫描/去抖（drivers-fw）
        Process_KeyActions();        // 9. 按键动作（配置模式切换 / 舵机测试）
        Display_Task();              // 10. 显示调度（局部刷新，末尾统一 OLED_Refresh）
    }
}
