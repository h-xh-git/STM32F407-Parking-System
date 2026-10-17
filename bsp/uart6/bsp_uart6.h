#ifndef __BSP_UART6_H
#define __BSP_UART6_H

#include "stm32f4xx.h"

#define MAX_PLATE_LEN 16

void uart6_init(uint32_t baudRate);
void USART6_IRQHandler(void);
void MaixCAM_ProcessByte(uint8_t byte);

extern volatile uint8_t g_maixcam_data_ready;
extern char g_maixcam_plate[16];            // 原始中文车牌（用于发送给上位机）
extern char g_maixcam_plate_display[16];    // 转换后的车牌（用于OLED显示）
extern volatile uint8_t g_maixcam_entry_flag;
extern volatile uint8_t g_maixcam_exit_flag;

#endif
