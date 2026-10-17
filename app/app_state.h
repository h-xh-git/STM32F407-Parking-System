#ifndef __APP_STATE_H__
#define __APP_STATE_H__

/**
 * @file    app_state.h
 * @brief   应用层共享状态与时间服务声明。
 *
 * 说明：
 *   - 本文件是 main / parking_system / protocol 之间共享的“应用状态”接口。
 *   - 时间戳统一使用 epoch = 2000-01-01 00:00:00（本地时间）的 32 位秒数，
 *     与上位机 上位机/parking_client.py 的 timestamp_to_str() 保持一致。
 *   - 显示模式与显示缓存集中在本文件，避免散落的 extern 声明。
 */

#include "stm32f4xx.h"

/* ==================== 显示模式 ==================== */
typedef enum {
    DISP_MODE_MAIN = 0,   /* 主界面：时间 / 车位 / 红外状态 */
    DISP_MODE_IN,         /* 车辆入场提示界面 */
    DISP_MODE_OUT,        /* 车辆出场提示界面 */
    DISP_MODE_CONFIG,     /* 配置模式界面 */
    DISP_MODE_FULL        /* 车位已满 / 未注册 / 黑名单等提示界面 */
} DisplayMode_t;

/* 入场/出场界面的临时显示数据 */
typedef struct {
    uint8_t  space_id;
    uint8_t  license_plate[16];
    uint32_t fee;
} DispTempData_t;

/* ==================== 系统节拍 ==================== */
/* 1ms 递增的系统节拍（TIM2 中断中维护，volatile 不可省略） */
extern volatile uint32_t timer_cnt;

/* ==================== 软件时钟（本地时间） ==================== */
extern uint16_t year;                       /* 2000 ~ 2099 */
extern uint8_t  mon, day, hour, min, sec;   /* mon: 1~12, day: 1~31, hour: 0~23 */

/* ==================== 显示状态 ==================== */
extern volatile DisplayMode_t g_disp_mode;
extern volatile uint32_t      g_disp_start_time;   /* 进入当前显示界面的 timer_cnt */
extern DispTempData_t         g_disp_temp_data;

/* ==================== 时间服务 ==================== */
/**
 * @brief  主循环调用的时钟任务：检测 sec_flag，递增 1 秒并维护年月日时分秒。
 */
void Time_Update(void);

/**
 * @brief  设置软件时钟（上位机 CMD_SET_TIME 使用）。
 * @param  y 年，mo 月，d 日，h 时，mi 分，s 秒
 * @retval 1 成功；0 参数越界（越界时不修改时钟）
 */
uint8_t Time_SetDateTime(uint16_t y, uint8_t mo, uint8_t d,
                         uint8_t h, uint8_t mi, uint8_t s);

/**
 * @brief  获取当前时间戳（秒，epoch = 2000-01-01 00:00:00 本地时间）。
 */
uint32_t Time_GetSeconds(void);

/**
 * @brief  获取当前日期时间。
 * @param  输出指针均不可为 NULL。
 */
void Time_GetDateTime(uint16_t *y, uint8_t *mo, uint8_t *d,
                      uint8_t *h, uint8_t *mi, uint8_t *s);

#endif /* __APP_STATE_H__ */
