#include "bsp_sg90.h"
#include "board.h"
#include "app_state.h"

/* ======================== 舵机动作时序（与旧阻塞版保持一致） ======================== */
#define SERVO_OPEN_TIME_MS      320U    /* 开闸转动时间 */
#define SERVO_HOLD_TIME_MS      2000U   /* 开闸后保持（停转）时间 */
#define SERVO_CLOSE_TIME_MS     310U    /* 关闸转动时间 */

/* ======================== 按键参数 ======================== */
#define KEY_SCAN_NUM            4U      /* KEY1 ~ KEY4 */
#define KEY_DEBOUNCE_MS         20U     /* 按键去抖时间 */

/* ======================== 舵机非阻塞状态机 ======================== */
typedef enum {
    SERVO_STATE_IDLE = 0,   /* 空闲（输出 0=停转） */
    SERVO_STATE_OPEN,       /* 正在开闸（+100，320ms） */
    SERVO_STATE_HOLD,       /* 开闸后保持（0，2000ms） */
    SERVO_STATE_CLOSE       /* 正在关闸（-100，310ms） */
} ServoState_t;

typedef struct {
    ServoState_t state;
    uint32_t     state_start_time;   /* 进入当前状态时的 timer_cnt（1ms） */
} ServoStateMachine_t;

static ServoStateMachine_t servo1_sm;
static ServoStateMachine_t servo2_sm;

/* ======================== 按键去抖状态 ======================== */
/* key_stable：去抖后的电平，1=按下（低电平有效） */
static uint8_t  key_stable[KEY_SCAN_NUM];
/* key_candidate：最近一次采样值；key_candidate_time：该值开始出现的时刻 */
static uint8_t  key_candidate[KEY_SCAN_NUM];
static uint32_t key_candidate_time[KEY_SCAN_NUM];

/******************************************************************
 * 函 数 名 称：Servo_SM_Init
 * 函 数 说 明：复位单个舵机状态机
 ******************************************************************/
static void Servo_SM_Init(ServoStateMachine_t *sm)
{
    sm->state = SERVO_STATE_IDLE;
    sm->state_start_time = timer_cnt;
}

/******************************************************************
 * 函 数 名 称：KEY_ReadLevel
 * 函 数 说 明：读取一个按键的当前电平（1=按下，0=松开）
 * 函 数 形 参：index 0~3 对应 KEY1~KEY4
 ******************************************************************/
static uint8_t KEY_ReadLevel(uint8_t index)
{
    switch (index)
    {
        case 0:  return (uint8_t)((GPIO_ReadInputDataBit(KEY1_PORT, KEY1_PIN) == 0) ? 1 : 0);
        case 1:  return (uint8_t)((GPIO_ReadInputDataBit(KEY2_PORT, KEY2_PIN) == 0) ? 1 : 0);
        case 2:  return (uint8_t)((GPIO_ReadInputDataBit(KEY3_PORT, KEY3_PIN) == 0) ? 1 : 0);
        case 3:  return (uint8_t)((GPIO_ReadInputDataBit(KEY4_PORT, KEY4_PIN) == 0) ? 1 : 0);
        default: return 0;
    }
}

/******************************************************************
 * 函 数 名 称：KEY_Sample
 * 函 数 说 明：非阻塞采样 + 20ms 去抖（只更新状态，不延时）
 ******************************************************************/
static void KEY_Sample(uint8_t index)
{
    uint8_t  level;
    uint32_t now;

    if (index >= KEY_SCAN_NUM)
    {
        return;
    }

    now = timer_cnt;
    level = KEY_ReadLevel(index);

    if (level != key_candidate[index])
    {
        /* 电平刚变化：记录候选值，等待去抖时间 */
        key_candidate[index] = level;
        key_candidate_time[index] = now;
    }
    else if (level != key_stable[index])
    {
        /* 电平已稳定超过 20ms：更新去抖后的电平 */
        if ((uint32_t)(now - key_candidate_time[index]) >= KEY_DEBOUNCE_MS)
        {
            key_stable[index] = level;
        }
    }
}

/******************************************************************
 * 函 数 名 称：SG90_Init
 * 函 数 说 明：初始化两个 360° 舵机 PWM（50Hz / 20ms），上电停止
 ******************************************************************/
void SG90_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    TIM_TimeBaseInitTypeDef  TIM_TimeBaseStructure;
    TIM_OCInitTypeDef  TIM_OCInitStructure;

    /* 开启时钟 */
    RCC_APB1PeriphClockCmd(RCC_SIG_TIMER, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_SIG2_TIMER, ENABLE);
    RCC_AHB1PeriphClockCmd(RCC_SIG, ENABLE);

    /* 配置 PB5 + PB6 复用功能（先 StructInit，避免使用栈垃圾值） */
    GPIO_StructInit(&GPIO_InitStructure);
    GPIO_InitStructure.GPIO_Pin = GPIO_SIG | GPIO_SIG2;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_NOPULL;
    GPIO_Init(PORT_SIG, &GPIO_InitStructure);

    /* 引脚复用映射 */
    GPIO_PinAFConfig(PORT_SIG, GPIO_SIG_SOURCE, AF_SIG);    /* PB5 -> TIM3_CH2 */
    GPIO_PinAFConfig(PORT_SIG, GPIO_SIG2_SOURCE, AF_SIG2);  /* PB6 -> TIM4_CH1 */

    /* PWM 50Hz 配置 */
    TIM_TimeBaseStructure.TIM_Period = 200-1;
    TIM_TimeBaseStructure.TIM_Prescaler = 8400-1;
    TIM_TimeBaseStructure.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseStructure.TIM_RepetitionCounter = 0;

    /* 初始化 TIM3 / TIM4 */
    TIM_TimeBaseInit(BSP_SIG_TIMER, &TIM_TimeBaseStructure);
    TIM_TimeBaseInit(BSP_SIG2_TIMER, &TIM_TimeBaseStructure);

    /* PWM 模式配置（先 StructInit，避免未使用字段残留栈垃圾） */
    TIM_OCStructInit(&TIM_OCInitStructure);
    TIM_OCInitStructure.TIM_OCMode = TIM_OCMode_PWM1;
    TIM_OCInitStructure.TIM_OutputState = TIM_OutputState_Enable;
    TIM_OCInitStructure.TIM_OCPolarity = TIM_OCPolarity_High;
    TIM_OCInitStructure.TIM_Pulse = 15;

    /* 舵机1：TIM3_CH2 */
    TIM_OC2Init(BSP_SIG_TIMER, &TIM_OCInitStructure);
    TIM_OC2PreloadConfig(BSP_SIG_TIMER, TIM_OCPreload_Enable);

    /* 舵机2：TIM4_CH1 */
    TIM_OC1Init(BSP_SIG2_TIMER, &TIM_OCInitStructure);
    TIM_OC1PreloadConfig(BSP_SIG2_TIMER, TIM_OCPreload_Enable);

    /* 启动定时器 */
    TIM_Cmd(BSP_SIG_TIMER, ENABLE);
    TIM_Cmd(BSP_SIG2_TIMER, ENABLE);

    /* 上电停止 */
    Servo_360_SetSpeed(0);
    Servo2_360_SetSpeed(0);

    /* 复位两个非阻塞状态机 */
    Servo_SM_Init(&servo1_sm);
    Servo_SM_Init(&servo2_sm);
}

/******************************************************************
 * 舵机1：速度设置 -100~100
 ******************************************************************/
void Servo_360_SetSpeed(int speed)
{
    int cmp;
    if(speed > 100) speed = 100;
    if(speed < -100) speed = -100;
    cmp = 15 + (speed * 5 / 100);
    TIM_SetCompare2(BSP_SIG_TIMER, cmp);
}

/******************************************************************
 * 舵机1：正转90度（阻塞版，仅兼容保留）
 ******************************************************************/
void Servo_360_Rotate90(void)
{
    Servo_360_SetSpeed(100);
    delay_ms(320);
    Servo_360_SetSpeed(0);
}

/******************************************************************
 * 舵机1：反转复位90度（阻塞版，仅兼容保留）
 ******************************************************************/
void Servo_360_Reset90(void)
{
    Servo_360_SetSpeed(-100);
    delay_ms(310);
    Servo_360_SetSpeed(0);
}

/******************************************************************
 * 舵机2：速度设置 -100~100
 ******************************************************************/
void Servo2_360_SetSpeed(int speed)
{
    int cmp;
    if(speed > 100) speed = 100;
    if(speed < -100) speed = -100;
    cmp = 15 + (speed * 5 / 100);
    TIM_SetCompare1(BSP_SIG2_TIMER, cmp);
}

/******************************************************************
 * 舵机2：正转90度（阻塞版，仅兼容保留）
 ******************************************************************/
void Servo2_360_Rotate90(void)
{
    Servo2_360_SetSpeed(100);
    delay_ms(320);
    Servo2_360_SetSpeed(0);
}

/******************************************************************
 * 舵机2：反转复位90度（阻塞版，仅兼容保留）
 ******************************************************************/
void Servo2_360_Reset90(void)
{
    Servo2_360_SetSpeed(-100);
    delay_ms(310);
    Servo2_360_SetSpeed(0);
}

/******************************************************************
 * 函 数 名 称：Servo_360_KeyAction
 * 函 数 说 明：触发舵机1 开闸动作（非阻塞）：开闸 320ms -> 保持 2000ms -> 关闸 310ms
 * 备       注：动作进行中再次触发会被忽略
 ******************************************************************/
void Servo_360_KeyAction(void)
{
    if (servo1_sm.state != SERVO_STATE_IDLE)
    {
        return;     /* 忙时忽略 */
    }

    Servo_360_SetSpeed(100);            /* 开闸 */
    servo1_sm.state = SERVO_STATE_OPEN;
    servo1_sm.state_start_time = timer_cnt;
}

/******************************************************************
 * 函 数 名 称：Servo2_360_KeyAction
 * 函 数 说 明：触发舵机2 开闸动作（非阻塞），时序同舵机1
 * 备       注：动作进行中再次触发会被忽略
 ******************************************************************/
void Servo2_360_KeyAction(void)
{
    if (servo2_sm.state != SERVO_STATE_IDLE)
    {
        return;     /* 忙时忽略 */
    }

    Servo2_360_SetSpeed(100);           /* 开闸 */
    servo2_sm.state = SERVO_STATE_OPEN;
    servo2_sm.state_start_time = timer_cnt;
}

/******************************************************************
 * 函 数 名 称：Servo_Task
 * 函 数 说 明：舵机非阻塞状态机，主循环每轮调用一次
 ******************************************************************/
void Servo_Task(void)
{
    uint32_t now = timer_cnt;

    /* ---------------- 舵机1 ---------------- */
    switch (servo1_sm.state)
    {
        case SERVO_STATE_OPEN:
            if ((uint32_t)(now - servo1_sm.state_start_time) >= SERVO_OPEN_TIME_MS)
            {
                Servo_360_SetSpeed(0);                  /* 停止，进入保持 */
                servo1_sm.state = SERVO_STATE_HOLD;
                servo1_sm.state_start_time = now;
            }
            break;

        case SERVO_STATE_HOLD:
            if ((uint32_t)(now - servo1_sm.state_start_time) >= SERVO_HOLD_TIME_MS)
            {
                Servo_360_SetSpeed(-100);               /* 关闸 */
                servo1_sm.state = SERVO_STATE_CLOSE;
                servo1_sm.state_start_time = now;
            }
            break;

        case SERVO_STATE_CLOSE:
            if ((uint32_t)(now - servo1_sm.state_start_time) >= SERVO_CLOSE_TIME_MS)
            {
                Servo_360_SetSpeed(0);                  /* 停止，回到空闲 */
                servo1_sm.state = SERVO_STATE_IDLE;
                servo1_sm.state_start_time = now;
            }
            break;

        default:
            break;
    }

    /* ---------------- 舵机2 ---------------- */
    switch (servo2_sm.state)
    {
        case SERVO_STATE_OPEN:
            if ((uint32_t)(now - servo2_sm.state_start_time) >= SERVO_OPEN_TIME_MS)
            {
                Servo2_360_SetSpeed(0);
                servo2_sm.state = SERVO_STATE_HOLD;
                servo2_sm.state_start_time = now;
            }
            break;

        case SERVO_STATE_HOLD:
            if ((uint32_t)(now - servo2_sm.state_start_time) >= SERVO_HOLD_TIME_MS)
            {
                Servo2_360_SetSpeed(-100);
                servo2_sm.state = SERVO_STATE_CLOSE;
                servo2_sm.state_start_time = now;
            }
            break;

        case SERVO_STATE_CLOSE:
            if ((uint32_t)(now - servo2_sm.state_start_time) >= SERVO_CLOSE_TIME_MS)
            {
                Servo2_360_SetSpeed(0);
                servo2_sm.state = SERVO_STATE_IDLE;
                servo2_sm.state_start_time = now;
            }
            break;

        default:
            break;
    }
}

/******************************************************************
 * 函 数 名 称：KEY_Init
 * 函 数 说 明：按键初始化（上拉输入）
 ******************************************************************/
void KEY_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    uint8_t i;
    uint8_t level;

    RCC_AHB1PeriphClockCmd(RCC_KEY, ENABLE);

    /* 先 StructInit，避免 OType/PuPd 等字段使用栈垃圾值 */
    GPIO_StructInit(&GPIO_InitStructure);
    GPIO_InitStructure.GPIO_Pin = KEY1_PIN | KEY2_PIN | KEY3_PIN | KEY4_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(KEY1_PORT, &GPIO_InitStructure);

    /* 用当前电平初始化去抖状态，避免上电误触发 */
    for (i = 0; i < KEY_SCAN_NUM; i++)
    {
        level = KEY_ReadLevel(i);
        key_stable[i] = level;
        key_candidate[i] = level;
        key_candidate_time[i] = timer_cnt;
    }
}

/******************************************************************
 * 函 数 名 称：Key_Task
 * 函 数 说 明：按键非阻塞采样任务，主循环每轮调用一次
 *             纯 GPIO 读 + timer_cnt 计时，无任何 delay
 ******************************************************************/
void Key_Task(void)
{
    uint8_t i;

    for (i = 0; i < KEY_SCAN_NUM; i++)
    {
        KEY_Sample(i);
    }
}

/******************************************************************
 * 函 数 名 称：KEY1_Press
 * 函 数 说 明：按键1 去抖后电平：1=按下，0=松开（无 delay）
 ******************************************************************/
uint8_t KEY1_Press(void)
{
    KEY_Sample(0);      /* 容错：即使主循环漏调 Key_Task 也能工作 */
    return key_stable[0];
}

uint8_t KEY2_Press(void)
{
    KEY_Sample(1);
    return key_stable[1];
}

uint8_t KEY3_Press(void)
{
    KEY_Sample(2);
    return key_stable[2];
}

uint8_t KEY4_Press(void)
{
    KEY_Sample(3);
    return key_stable[3];
}
