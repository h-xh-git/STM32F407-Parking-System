#include "bsp_uart6.h"
#include "stdio.h"
#include "string.h"

volatile uint8_t g_maixcam_data_ready = 0;
volatile uint8_t g_maixcam_entry_flag = 0;
volatile uint8_t g_maixcam_exit_flag = 0;
char g_maixcam_plate[MAX_PLATE_LEN] = {0};           // 原始中文车牌（用于发送给上位机）
char g_maixcam_plate_display[MAX_PLATE_LEN] = {0};   // 转换后的车牌（用于OLED显示）

static char rx_buffer[64];
static uint8_t rx_index = 0;
static uint8_t rx_overflow = 0;   /* 当前行超长：丢弃到行结束符为止 */

/* ==================== 省份 Unicode 码点 -> 拼音缩写 ==================== */
/* 表放在 Flash（static const），不占 1KB 栈/有限 RAM。
 * 码点已用 Python 的 ord()/unicodedata 全部核对，与 LPR 的 UTF-8 车牌一致。 */
typedef struct {
    uint16_t chinese_char;
    const char *pinyin;
} ProvinceCode;

static const ProvinceCode province_codes[] = {
    {0x4EAC, "BJ"}, /* 京 - 北京 */
    {0x6D25, "TJ"}, /* 津 - 天津 */
    {0x6CAA, "SH"}, /* 沪 - 上海 */
    {0x6E1D, "CQ"}, /* 渝 - 重庆 */
    {0x5180, "HE"}, /* 冀 - 河北 */
    {0x664B, "SX"}, /* 晋 - 山西 */
    {0x8499, "NM"}, /* 蒙 - 内蒙古 */
    {0x8FBD, "LN"}, /* 辽 - 辽宁 */
    {0x5409, "JL"}, /* 吉 - 吉林 */
    {0x9ED1, "HL"}, /* 黑 - 黑龙江 */
    {0x82CF, "JS"}, /* 苏 - 江苏 */
    {0x6D59, "ZJ"}, /* 浙 - 浙江 */
    {0x7696, "AH"}, /* 皖 - 安徽 */
    {0x95FD, "FJ"}, /* 闽 - 福建 */
    {0x8D63, "JX"}, /* 赣 - 江西 */
    {0x9C81, "SD"}, /* 鲁 - 山东 */
    {0x8C6B, "HA"}, /* 豫 - 河南 */
    {0x9102, "HB"}, /* 鄂 - 湖北 */
    {0x6E58, "HN"}, /* 湘 - 湖南 */
    {0x7CA4, "GD"}, /* 粤 - 广东 */
    {0x6842, "GX"}, /* 桂 - 广西 */
    {0x743C, "HI"}, /* 琼 - 海南 */
    {0x5DDD, "SC"}, /* 川 - 四川 */
    {0x8700, "SC"}, /* 蜀 - 四川 */
    {0x8D35, "GZ"}, /* 贵 - 贵州 */
    {0x9ED4, "GZ"}, /* 黔 - 贵州 */
    {0x4E91, "YN"}, /* 云 - 云南 */
    {0x6EC7, "YN"}, /* 滇 - 云南 */
    {0x85CF, "XZ"}, /* 藏 - 西藏 */
    {0x9655, "SN"}, /* 陕 - 陕西 */
    {0x79E6, "SN"}, /* 秦 - 陕西 */
    {0x7518, "GS"}, /* 甘 - 甘肃 */
    {0x9647, "GS"}, /* 陇 - 甘肃 */
    {0x9752, "QH"}, /* 青 - 青海 */
    {0x5B81, "NX"}, /* 宁 - 宁夏 */
    {0x65B0, "XJ"}, /* 新 - 新疆 */
    {0x6E2F, "HK"}, /* 港 - 香港 */
    {0x6FB3, "MO"}, /* 澳 - 澳门 */
    {0x53F0, "TW"}, /* 台 - 台湾 */
    {0x0000, 0}     /* 结束标记 */
};

/******************************************************************
 * 函 数 名 称：ConvertChineseProvinceToPinyin
 * 函 数 说 明：把 UTF-8 车牌首字（省份汉字）原地替换为两位拼音缩写
 *             例："京A12345" -> "BJA12345"
 * 函 数 形 参：plate：UTF-8 车牌字符串（原地修改，缓冲区需 >= 原长度）
 * 备       注：未收录的省份/非汉字首字保持原样，不越界、不写坏字符串
 ******************************************************************/
void ConvertChineseProvinceToPinyin(char *plate)
{
    uint16_t unicode_char;
    uint16_t i;
    uint8_t char_len;
    int remaining_len;
    const char *pinyin;

    if (plate == 0 || plate[0] == 0) return;

    unicode_char = 0;
    char_len = 1;

    /* UTF-8 三字节汉字：1110xxxx 10xxxxxx 10xxxxxx */
    if (((uint8_t)plate[0] & 0xF0) == 0xE0)
    {
        if (plate[1] == 0 || plate[2] == 0) return;
        if ((((uint8_t)plate[1] & 0xC0) != 0x80) || (((uint8_t)plate[2] & 0xC0) != 0x80)) return;
        unicode_char = (uint16_t)((((uint8_t)plate[0] & 0x0F) << 12) |
                                  (((uint8_t)plate[1] & 0x3F) << 6) |
                                  (((uint8_t)plate[2] & 0x3F)));
        char_len = 3;
    }
    /* UTF-8 两字节字符（省份表里没有，仅为解析合法性判断） */
    else if (((uint8_t)plate[0] & 0xE0) == 0xC0)
    {
        if (plate[1] == 0) return;
        if (((uint8_t)plate[1] & 0xC0) != 0x80) return;
        unicode_char = (uint16_t)((((uint8_t)plate[0] & 0x1F) << 6) |
                                  ((uint8_t)plate[1] & 0x3F));
        char_len = 2;
    }
    else
    {
        return;     /* ASCII 或非法 UTF-8：无需转换 */
    }

    for (i = 0; province_codes[i].chinese_char != 0; i++)
    {
        if (province_codes[i].chinese_char != unicode_char)
        {
            continue;
        }

        pinyin = province_codes[i].pinyin;
        remaining_len = (int)strlen(plate) - (int)char_len;
        plate[0] = pinyin[0];
        plate[1] = pinyin[1];

        if (remaining_len > 0)
        {
            memmove(plate + 2, plate + char_len, (size_t)remaining_len);
            plate[2 + remaining_len] = '\0';
        }
        else
        {
            plate[2] = '\0';
        }
        return;
    }
}

/******************************************************************
 * 函 数 名 称：MaixCAM_IsValidPlate
 * 函 数 说 明：车牌合法性/噪声防护：长度 1~15、无控制字符、UTF-8 结构完整
 * 函 数 形 参：plate：待检查字符串；len：字节长度
 * 函 数 返 回：1=合法，0=非法（整行丢弃）
 ******************************************************************/
static uint8_t MaixCAM_IsValidPlate(const char *plate, uint8_t len)
{
    uint8_t i;
    uint8_t j;
    uint8_t need;

    if (len == 0 || len > (MAX_PLATE_LEN - 1)) return 0;

    i = 0;
    while (i < len)
    {
        if (((uint8_t)plate[i] < 0x20) || ((uint8_t)plate[i] == 0x7F)) return 0;

        if ((uint8_t)plate[i] < 0x80)
        {
            i++;
            continue;
        }

        if (((uint8_t)plate[i] & 0xE0) == 0xC0)
        {
            need = 1;
        }
        else if (((uint8_t)plate[i] & 0xF0) == 0xE0)
        {
            need = 2;
        }
        else if (((uint8_t)plate[i] & 0xF8) == 0xF0)
        {
            need = 3;
        }
        else
        {
            return 0;
        }

        if ((uint16_t)i + need >= (uint16_t)len) return 0;
        for (j = 1; j <= need; j++)
        {
            if (((uint8_t)plate[i + j] & 0xC0) != 0x80) return 0;
        }
        i = (uint8_t)(i + need + 1);
    }

    return 1;
}

/******************************************************************
 * 函 数 名 称：MaixCAM_HandleLine
 * 函 数 说 明：解析一行完整数据（IN:车牌 / OUT:车牌），更新全局车牌与事件标志
 * 备       注：只由 MaixCAM_ProcessByte 在收到行结束符时调用（USART6 中断上下文）
 ******************************************************************/
static void MaixCAM_HandleLine(void)
{
    uint8_t is_entry;
    uint8_t payload_len;
    const char *payload;

    if (rx_index < 4) return;           /* 最短合法行 "IN:x" */
    rx_buffer[rx_index] = '\0';

    if ((rx_buffer[0] == 'I') && (rx_buffer[1] == 'N') && (rx_buffer[2] == ':'))
    {
        is_entry = 1;
        payload = &rx_buffer[3];
    }
    else if ((rx_buffer[0] == 'O') && (rx_buffer[1] == 'U') &&
             (rx_buffer[2] == 'T') && (rx_buffer[3] == ':'))
    {
        is_entry = 0;
        payload = &rx_buffer[4];
    }
    else
    {
        return;                         /* 前缀非法：丢弃噪声行 */
    }

    payload_len = (uint8_t)(rx_index - (uint8_t)(payload - rx_buffer));
    if (!MaixCAM_IsValidPlate(payload, payload_len))
    {
        return;                         /* 长度/编码非法：丢弃噪声行 */
    }

    memcpy(g_maixcam_plate, payload, payload_len);
    g_maixcam_plate[payload_len] = '\0';

    memcpy(g_maixcam_plate_display, g_maixcam_plate, (size_t)payload_len + 1);
    ConvertChineseProvinceToPinyin(g_maixcam_plate_display);

    if (is_entry)
    {
        g_maixcam_entry_flag = 1;
    }
    else
    {
        g_maixcam_exit_flag = 1;
    }
}

/******************************************************************
 * 函 数 名 称：uart6_init
 * 函 数 说 明：USART6 初始化（PC6=TX, PC7=RX, 115200-8N1，RXNE 中断）
 ******************************************************************/
void uart6_init(uint32_t baudRate)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    USART_InitTypeDef USART_InitStructure;
    NVIC_InitTypeDef NVIC_InitStructure;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC, ENABLE);
    RCC_APB2PeriphClockCmd(RCC_APB2Periph_USART6, ENABLE);

    GPIO_PinAFConfig(GPIOC, GPIO_PinSource6, GPIO_AF_USART6);
    GPIO_PinAFConfig(GPIOC, GPIO_PinSource7, GPIO_AF_USART6);

    GPIO_StructInit(&GPIO_InitStructure);
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_6;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(GPIOC, &GPIO_InitStructure);

    GPIO_StructInit(&GPIO_InitStructure);
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_7;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(GPIOC, &GPIO_InitStructure);

    USART_DeInit(USART6);
    USART_StructInit(&USART_InitStructure);
    USART_InitStructure.USART_BaudRate = baudRate;
    USART_InitStructure.USART_WordLength = USART_WordLength_8b;
    USART_InitStructure.USART_StopBits = USART_StopBits_1;
    USART_InitStructure.USART_Parity = USART_Parity_No;
    USART_InitStructure.USART_Mode = USART_Mode_Rx | USART_Mode_Tx;
    USART_InitStructure.USART_HardwareFlowControl = USART_HardwareFlowControl_None;
    USART_Init(USART6, &USART_InitStructure);

    USART_ClearFlag(USART6, USART_FLAG_RXNE);
    USART_ITConfig(USART6, USART_IT_RXNE, ENABLE);
    USART_Cmd(USART6, ENABLE);

    NVIC_InitStructure.NVIC_IRQChannel = USART6_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 2;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);

    printf("[UART6] USART6 initialized (PC6=TX, PC7=RX) @ %d baud\r\n", baudRate);
}

/******************************************************************
 * 函 数 名 称：MaixCAM_ProcessByte
 * 函 数 说 明：USART6 中断里逐字节组帧（只组帧，不做 printf/OLED/延时）
 *             行格式："IN:车牌\n" 或 "OUT:车牌\n"
 * 备       注：超长行直接丢弃到行尾，避免半行数据触发错误事件
 ******************************************************************/
void MaixCAM_ProcessByte(uint8_t byte)
{
    if ((byte == '\n') || (byte == '\r'))
    {
        if (rx_overflow)
        {
            /* 本行超长（噪声）：整行丢弃 */
            rx_overflow = 0;
            rx_index = 0;
            rx_buffer[0] = '\0';
            return;
        }

        if (rx_index > 0)
        {
            MaixCAM_HandleLine();
        }
        rx_index = 0;
        rx_buffer[0] = '\0';
        return;
    }

    if (rx_overflow)
    {
        return;     /* 已经在丢弃模式：忽略到行尾 */
    }

    if (rx_index < (uint8_t)(sizeof(rx_buffer) - 1))
    {
        rx_buffer[rx_index] = (char)byte;
        rx_index++;
    }
    else
    {
        /* 行超过 63 字节：视为噪声，丢弃整行 */
        rx_overflow = 1;
        rx_index = 0;
        rx_buffer[0] = '\0';
    }
}

/******************************************************************
 * 函 数 名 称：USART6_IRQHandler
 * 函 数 说 明：USART6 接收中断：只取字节，交给 MaixCAM_ProcessByte 组帧
 ******************************************************************/
void USART6_IRQHandler(void)
{
    if (USART_GetITStatus(USART6, USART_IT_RXNE) == SET) {
        uint8_t byte = USART_ReceiveData(USART6);
        MaixCAM_ProcessByte(byte);
        USART_ClearITPendingBit(USART6, USART_IT_RXNE);
    }
}
