#ifndef __BSP_HONGWAI_H
#define __BSP_HONGWAI_H

#include "stm32f4xx.h"

/* ======================== 3 路红外避障：PB0 / PB1 / PB4 ======================== */
#define  IR1_PIN               GPIO_Pin_0    /* 车位1（PB0） */
#define  IR2_PIN               GPIO_Pin_1    /* 车位2（PB1） */
#define  IR3_PIN               GPIO_Pin_4    /* 车位3（PB4，注意 JTAG NJTRST 复用） */
#define  IR_PORT               GPIOB
#define  RCC_IR                RCC_AHB1Periph_GPIOB

void IR_Init(void);      /* 上拉输入初始化 */
void IR_Task(void);      /* 主循环调用：约 10ms 采样 + 20ms 去抖，更新缓存 */

uint8_t IR1_Check(void); /* 返回去抖后的缓存值：1=检测到车，0=无车（无 delay） */
uint8_t IR2_Check(void);
uint8_t IR3_Check(void);

#endif
