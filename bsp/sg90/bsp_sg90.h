#ifndef __BSP_SG90_H
#define __BSP_SG90_H

#include "stm32f4xx.h"

/* ======================== 舵机1：PB5 (TIM3_CH2) ======================== */
#define  BSP_SIG_TIMER            TIM3
#define  RCC_SIG_TIMER            RCC_APB1Periph_TIM3

#define  PORT_SIG                 GPIOB
#define  RCC_SIG                  RCC_AHB1Periph_GPIOB
#define  GPIO_SIG                 GPIO_Pin_5
#define  GPIO_SIG_SOURCE          GPIO_PinSource5
#define  AF_SIG                   GPIO_AF_TIM3

/* ======================== 舵机2：PB6 (TIM4_CH1) ======================== */
#define  BSP_SIG2_TIMER           TIM4
#define  RCC_SIG2_TIMER           RCC_APB1Periph_TIM4
#define  GPIO_SIG2                GPIO_Pin_6
#define  GPIO_SIG2_SOURCE         GPIO_PinSource6
#define  AF_SIG2                  GPIO_AF_TIM4

/* ======================== 按键：PA0 / PA1 / PA2 / PA3 ======================== */
#define  KEY1_PORT            GPIOA
#define  KEY1_PIN             GPIO_Pin_0
#define  KEY2_PORT            GPIOA
#define  KEY2_PIN             GPIO_Pin_1
#define  KEY3_PORT            GPIOA
#define  KEY3_PIN             GPIO_Pin_2
#define  KEY4_PORT            GPIOA
#define  KEY4_PIN             GPIO_Pin_3
#define  RCC_KEY              RCC_AHB1Periph_GPIOA

/* ======================== 初始化 ======================== */
void SG90_Init(void);    /* 舵机 PWM 初始化（50Hz，上电停止） */
void KEY_Init(void);     /* 按键上拉输入初始化 */

/* ======================== 舵机1 接口 ======================== */
void Servo_360_SetSpeed(int speed);
void Servo_360_Rotate90(void);
void Servo_360_Reset90(void);
void Servo_360_KeyAction(void);  /* 触发舵机1 开闸动作（非阻塞，忙时忽略） */

/* ======================== 舵机2 接口 ======================== */
void Servo2_360_SetSpeed(int speed);
void Servo2_360_Rotate90(void);
void Servo2_360_Reset90(void);
void Servo2_360_KeyAction(void); /* 触发舵机2 开闸动作（非阻塞，忙时忽略） */

/* ======================== 非阻塞任务（主循环调用） ======================== */
void Servo_Task(void);   /* 舵机状态机：开闸 320ms -> 保持 2000ms -> 关闸 310ms */
void Key_Task(void);     /* 按键 20ms 去抖采样（纯 GPIO 读 + timer_cnt，无 delay） */

/* ======================== 按键（返回去抖后的电平，1=按下） ======================== */
uint8_t KEY1_Press(void);
uint8_t KEY2_Press(void);
uint8_t KEY3_Press(void);
uint8_t KEY4_Press(void);

#endif
