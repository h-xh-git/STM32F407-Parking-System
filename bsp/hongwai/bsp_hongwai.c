#include "bsp_hongwai.h"
#include "app_state.h"

/* ======================== 红外采样参数 ======================== */
#define IR_CHANNEL_NUM          3U
#define IR_SAMPLE_PERIOD_MS     10U     /* 采样周期 */
#define IR_DEBOUNCE_MS          20U     /* 去抖时间 */

/* 每路红外的状态：raw=最近采样值，stable=去抖后缓存值（1=检测到车） */
typedef struct {
    uint8_t  raw;
    uint8_t  stable;
    uint32_t raw_change_time;   /* raw 最近一次变化的 timer_cnt */
} IRChannel_t;

static IRChannel_t ir_channel[IR_CHANNEL_NUM];
static uint32_t ir_last_sample_time;

/******************************************************************
 * 函 数 名 称：IR_ReadRaw
 * 函 数 说 明：读取一路红外的原始电平（0 = 检测到车 -> 返回 1）
 * 函 数 形 参：index 0~2 对应 IR1~IR3
 ******************************************************************/
static uint8_t IR_ReadRaw(uint8_t index)
{
    switch (index)
    {
        case 0:  return (uint8_t)((GPIO_ReadInputDataBit(IR_PORT, IR1_PIN) == 0) ? 1 : 0);
        case 1:  return (uint8_t)((GPIO_ReadInputDataBit(IR_PORT, IR2_PIN) == 0) ? 1 : 0);
        case 2:  return (uint8_t)((GPIO_ReadInputDataBit(IR_PORT, IR3_PIN) == 0) ? 1 : 0);
        default: return 0;
    }
}

/******************************************************************
 * 函 数 名 称：IR_Init
 * 函 数 说 明：3 路红外避障初始化（上拉输入）
 ******************************************************************/
void IR_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    uint8_t i;

    RCC_AHB1PeriphClockCmd(RCC_IR, ENABLE);

    GPIO_StructInit(&GPIO_InitStructure);
    GPIO_InitStructure.GPIO_Pin = IR1_PIN | IR2_PIN | IR3_PIN;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(IR_PORT, &GPIO_InitStructure);

    /* 用当前电平初始化缓存，首轮 IRx_Check 即返回合理值 */
    for (i = 0; i < IR_CHANNEL_NUM; i++)
    {
        ir_channel[i].raw = IR_ReadRaw(i);
        ir_channel[i].stable = ir_channel[i].raw;
        ir_channel[i].raw_change_time = timer_cnt;
    }
    ir_last_sample_time = timer_cnt;
}

/******************************************************************
 * 函 数 名 称：IR_Task
 * 函 数 说 明：红外非阻塞采样任务：约每 10ms 采样一次，20ms 去抖后更新缓存
 *             主循环每轮调用一次，无 delay
 ******************************************************************/
void IR_Task(void)
{
    uint32_t now;
    uint8_t  i;
    uint8_t  raw;

    now = timer_cnt;
    if ((uint32_t)(now - ir_last_sample_time) < IR_SAMPLE_PERIOD_MS)
    {
        return;     /* 未到采样时刻 */
    }
    ir_last_sample_time = now;

    for (i = 0; i < IR_CHANNEL_NUM; i++)
    {
        raw = IR_ReadRaw(i);

        if (raw != ir_channel[i].raw)
        {
            /* 原始电平变化：重新开始去抖计时 */
            ir_channel[i].raw = raw;
            ir_channel[i].raw_change_time = now;
        }
        else if (raw != ir_channel[i].stable)
        {
            /* 新电平稳定超过 20ms：更新缓存 */
            if ((uint32_t)(now - ir_channel[i].raw_change_time) >= IR_DEBOUNCE_MS)
            {
                ir_channel[i].stable = raw;
            }
        }
    }
}

/******************************************************************
 * 车位1检测：返回去抖后的缓存值（1=有车，0=无车，无 delay）
 ******************************************************************/
uint8_t IR1_Check(void)
{
    return ir_channel[0].stable;
}

uint8_t IR2_Check(void)
{
    return ir_channel[1].stable;
}

uint8_t IR3_Check(void)
{
    return ir_channel[2].stable;
}
