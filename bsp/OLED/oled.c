#include "oled.h"
#include "stdlib.h"
#include "oledfont.h"  	 
#include "oled_cn14.h"
#include "board.h"

u8 OLED_GRAM[128][8];

/* ==================== 脏页机制（局部刷新） ==================== */
/* bit0~bit7 对应 page0~page7；置 1 表示该页 GRAM 有改动、待刷新 */
static u8 OLED_DirtyPages = 0xFF;   /* 上电先整屏刷一次 */

/* 单次 OLED_Refresh 最多刷新的页数：2 页约 25ms，保证主循环单次刷新 < 50ms */
#define OLED_REFRESH_PAGES_PER_CALL  2

/* 标记单页脏 */
static void OLED_MarkDirty(u8 page)
{
	if(page < 8)
	{
		OLED_DirtyPages |= (u8)(1U << page);
	}
}

/* 标记全部 8 页脏 */
static void OLED_MarkAllDirty(void)
{
	OLED_DirtyPages = 0xFF;
}

//���Ժ���
void OLED_ColorTurn(u8 i)
{
	if(i==0)
		{
			OLED_WR_Byte(0xA6,OLED_CMD);//������ʾ
		}
	if(i==1)
		{
			OLED_WR_Byte(0xA7,OLED_CMD);//��ɫ��ʾ
		}
}

//��Ļ��ת180��
void OLED_DisplayTurn(u8 i)
{
	if(i==0)
		{
			OLED_WR_Byte(0xC8,OLED_CMD);//������ʾ
			OLED_WR_Byte(0xA1,OLED_CMD);
		}
	if(i==1)
		{
			OLED_WR_Byte(0xC0,OLED_CMD);//��ת��ʾ
			OLED_WR_Byte(0xA0,OLED_CMD);
		}
}

//��ʱ
void IIC_delay(void)
{
        delay_us(5);
}

//��ʼ�ź�
void I2C_Start(void)
{
	OLED_SDA_Set();
	OLED_SCL_Set();
	IIC_delay();
	OLED_SDA_Clr();
	IIC_delay();
	OLED_SCL_Clr();
	IIC_delay();
}

//�����ź�
void I2C_Stop(void)
{
	OLED_SDA_Clr();
	OLED_SCL_Set();
	IIC_delay();
	OLED_SDA_Set();
}

//�ȴ��ź���Ӧ
void I2C_WaitAck(void) //�������źŵĵ�ƽ
{
	OLED_SDA_Set();
	IIC_delay();
	OLED_SCL_Set();
	IIC_delay();
	OLED_SCL_Clr();
	IIC_delay();
}

//д��һ���ֽ�
void Send_Byte(u8 dat)
{
	u8 i;
	for(i=0;i<8;i++)
	{
		if(dat&0x80)//��dat��8λ�����λ����д��
		{
			OLED_SDA_Set();
    }
		else
		{
			OLED_SDA_Clr();
    }
		IIC_delay();
		OLED_SCL_Set();
		IIC_delay();
		OLED_SCL_Clr();//��ʱ���ź�����Ϊ�͵�ƽ
		dat<<=1;
  }
}

//����һ���ֽ�
//mode:����/�����־ 0,��ʾ����;1,��ʾ����;
void OLED_WR_Byte(u8 dat,u8 mode)
{
	I2C_Start();
	Send_Byte(0x78);
	I2C_WaitAck();
	if(mode){Send_Byte(0x40);}
  else{Send_Byte(0x00);}
	I2C_WaitAck();
	Send_Byte(dat);
	I2C_WaitAck();
	I2C_Stop();
}

//����OLED��ʾ 
void OLED_DisPlay_On(void)
{
	OLED_WR_Byte(0x8D,OLED_CMD);//��ɱ�ʹ��
	OLED_WR_Byte(0x14,OLED_CMD);//������ɱ�
	OLED_WR_Byte(0xAF,OLED_CMD);//������Ļ
}

//�ر�OLED��ʾ 
void OLED_DisPlay_Off(void)
{
	OLED_WR_Byte(0x8D,OLED_CMD);//��ɱ�ʹ��
	OLED_WR_Byte(0x10,OLED_CMD);//�رյ�ɱ�
	OLED_WR_Byte(0xAE,OLED_CMD);//�ر���Ļ
}

//�����Դ浽OLED	
void OLED_Refresh(void)
{
	u8 i,n,dirty,flushed;
	if(OLED_DirtyPages == 0) return;   /* 无脏页：立即返回，不做任何 I2C 操作 */
	dirty = OLED_DirtyPages;
	OLED_DirtyPages = 0;
	flushed = 0;
	for(i=0;i<8;i++)
	{
		if(!(dirty & (u8)(1U << i))) continue;   /* 该页不脏，跳过 */
		if(flushed >= OLED_REFRESH_PAGES_PER_CALL)
		{
			OLED_MarkDirty(i);   /* 本轮刷不完的脏页重新标记，留到下次继续 */
			continue;
		}
		OLED_WR_Byte(0xb0+i,OLED_CMD); //设置页地址
		OLED_WR_Byte(0x00,OLED_CMD);   //设置列低地址
		OLED_WR_Byte(0x10,OLED_CMD);   //设置列高地址
		I2C_Start();
		Send_Byte(0x78);
		I2C_WaitAck();
		Send_Byte(0x40);
		I2C_WaitAck();
		for(n=0;n<128;n++)
		{
			Send_Byte(OLED_GRAM[n][i]);
			I2C_WaitAck();
		}
		I2C_Stop();
		flushed++;
	}
}
//��������
void OLED_Clear(void)
{
	u8 i,n;
	for(i=0;i<8;i++)
	{
		for(n=0;n<128;n++)
		{
			OLED_GRAM[n][i]=0;   /* 清空显示缓存 */
		}
	}
	/* 只标记全部脏页，不在此处刷屏；主循环统一调用 OLED_Refresh() 生效 */
	OLED_MarkAllDirty();
}

//���� 
//x:0~127
//y:0~63
//t:1 ��� 0,���	
void OLED_DrawPoint(u8 x,u8 y,u8 t)
{
	u8 i,m,n,old_val,new_val;
	if(x >= 128 || y >= 64) return;   /* 越界保护：GRAM 只有 128x64 */
	i=y/8;
	m=y%8;
	n=(u8)(1U<<m);
	old_val=OLED_GRAM[x][i];
	if(t){new_val=(u8)(old_val|n);}
	else {new_val=(u8)(old_val&(~n));}
	if(new_val==old_val) return;   /* 像素值没变化：不标记脏页（仅内容变化时刷新） */
	OLED_GRAM[x][i]=new_val;
	OLED_MarkDirty(i);   /* 标记所在页为脏页，由 OLED_Refresh 局部刷新 */
}

//����
//x1,y1:�������
//x2,y2:��������
void OLED_DrawLine(u8 x1,u8 y1,u8 x2,u8 y2,u8 mode)
{
	u16 t; 
	int xerr=0,yerr=0,delta_x,delta_y,distance;
	int incx,incy,uRow,uCol;
	delta_x=x2-x1; //������������ 
	delta_y=y2-y1;
	uRow=x1;//�����������
	uCol=y1;
	if(delta_x>0)incx=1; //���õ������� 
	else if (delta_x==0)incx=0;//��ֱ�� 
	else {incx=-1;delta_x=-delta_x;}
	if(delta_y>0)incy=1;
	else if (delta_y==0)incy=0;//ˮƽ�� 
	else {incy=-1;delta_y=-delta_x;}
	if(delta_x>delta_y)distance=delta_x; //ѡȡ�������������� 
	else distance=delta_y;
	for(t=0;t<distance+1;t++)
	{
		OLED_DrawPoint(uRow,uCol,mode);//����
		xerr+=delta_x;
		yerr+=delta_y;
		if(xerr>distance)
		{
			xerr-=distance;
			uRow+=incx;
		}
		if(yerr>distance)
		{
			yerr-=distance;
			uCol+=incy;
		}
	}
}
//x,y:Բ������
//r:Բ�İ뾶
void OLED_DrawCircle(u8 x,u8 y,u8 r)
{
	int a, b,num;
    a = 0;
    b = r;
    while(2 * b * b >= r * r)      
    {
        OLED_DrawPoint(x + a, y - b,1);
        OLED_DrawPoint(x - a, y - b,1);
        OLED_DrawPoint(x - a, y + b,1);
        OLED_DrawPoint(x + a, y + b,1);
 
        OLED_DrawPoint(x + b, y + a,1);
        OLED_DrawPoint(x + b, y - a,1);
        OLED_DrawPoint(x - b, y - a,1);
        OLED_DrawPoint(x - b, y + a,1);
        
        a++;
        num = (a * a + b * b) - r*r;//���㻭�ĵ���Բ�ĵľ���
        if(num > 0)
        {
            b--;
            a--;
        }
    }
}



//��ָ��λ����ʾһ���ַ�,���������ַ�
//x:0~127
//y:0~63
//size1:ѡ������ 6x8/6x12/8x16/12x24
//mode:0,��ɫ��ʾ;1,������ʾ
void OLED_ShowChar(u8 x,u8 y,u8 chr,u8 size1,u8 mode)
{
	u8 i,m,temp,size2,chr1;
	u8 x0=x,y0=y;
	if(size1==8)size2=6;
	else size2=(size1/8+((size1%8)?1:0))*(size1/2);  //�õ�����һ���ַ���Ӧ������ռ���ֽ���
	chr1=chr-' ';  //����ƫ�ƺ��ֵ
	for(i=0;i<size2;i++)
	{
		if(size1==8)
			  {temp=asc2_0806[chr1][i];} //����0806����
		else if(size1==12)
        {temp=asc2_1206[chr1][i];} //����1206����
		else if(size1==16)
        {temp=asc2_1608[chr1][i];} //����1608����
		else if(size1==24)
        {temp=asc2_2412[chr1][i];} //����2412����
		else return;
		for(m=0;m<8;m++)
		{
			if(temp&0x01)OLED_DrawPoint(x,y,mode);
			else OLED_DrawPoint(x,y,!mode);
			temp>>=1;
			y++;
		}
		x++;
		if((size1!=8)&&((x-x0)==size1/2))
		{x=x0;y0=y0+8;}
		y=y0;
  }
}


//��ʾ�ַ���
//x,y:�������  
//size1:�����С 
//*chr:�ַ�����ʼ��ַ 
//mode:0,��ɫ��ʾ;1,������ʾ
void OLED_ShowString(u8 x,u8 y,u8 *chr,u8 size1,u8 mode)
{
	while((*chr>=' ')&&(*chr<='~'))//�ж��ǲ��ǷǷ��ַ�!
	{
		OLED_ShowChar(x,y,*chr,size1,mode);
		if(size1==8)x+=6;
		else x+=size1/2;
		chr++;
  }
}

//m^n
u32 OLED_Pow(u8 m,u8 n)
{
	u32 result=1;
	while(n--)
	{
	  result*=m;
	}
	return result;
}

//��ʾ����
//x,y :�������
//num :Ҫ��ʾ������
//len :���ֵ�λ��
//size:�����С
//mode:0,��ɫ��ʾ;1,������ʾ
void OLED_ShowNum(u8 x,u8 y,u32 num,u8 len,u8 size1,u8 mode)
{
	u8 t,temp,m=0;
	if(size1==8)m=2;
	for(t=0;t<len;t++)
	{
		temp=(num/OLED_Pow(10,len-t-1))%10;
			if(temp==0)
			{
				OLED_ShowChar(x+(size1/2+m)*t,y,'0',size1,mode);
      }
			else 
			{
			  OLED_ShowChar(x+(size1/2+m)*t,y,temp+'0',size1,mode);
			}
  }
}

//��ʾ����
//x,y:�������
//num:���ֶ�Ӧ�����
//mode:0,��ɫ��ʾ;1,������ʾ
void OLED_ShowChinese(u8 x,u8 y,u8 num,u8 size1,u8 mode)
{
	u8 m,temp;
	u8 x0=x,y0=y;
	u16 i,size3=(size1/8+((size1%8)?1:0))*size1;  //�õ�����һ���ַ���Ӧ������ռ���ֽ���
	for(i=0;i<size3;i++)
	{
		if(size1==16)
				{temp=Hzk1[num][i];}//����16*16����
		else if(size1==24)
				{temp=Hzk2[num][i];}//����24*24����
		else if(size1==32)       
				{temp=Hzk3[num][i];}//����32*32����
		else if(size1==64)
				{temp=Hzk4[num][i];}//����64*64����
		else return;
		for(m=0;m<8;m++)
		{
			if(temp&0x01)OLED_DrawPoint(x,y,mode);
			else OLED_DrawPoint(x,y,!mode);
			temp>>=1;
			y++;
		}
		x++;
		if((x-x0)==size1)
		{x=x0;y0=y0+8;}
		y=y0;
	}
}

//num ��ʾ���ֵĸ���
//space ÿһ����ʾ�ļ��
//mode:0,��ɫ��ʾ;1,������ʾ
void OLED_ScrollDisplay(u8 num,u8 space,u8 mode)
{
	u8 i,n,t=0,m=0,r;
	while(1)
	{
		if(m==0)
		{
			OLED_ShowChinese(112,24,t,16,mode); //写入一个字到最右侧（GRAM 只有 128 列）
			t++;
		}
		if(t==num)
		{
			for(r=0;r<16*space;r++)      //显示间隔
			{
				for(i=1;i<128;i++)
				{
					for(n=0;n<8;n++)
					{
						OLED_GRAM[i-1][n]=OLED_GRAM[i][n];
					}
				}
				OLED_MarkAllDirty();
				OLED_Refresh();
			}
			t=0;
		}
		m++;
		if(m==16){m=0;}
		for(i=1;i<128;i++)   //整屏左移
		{
			for(n=0;n<8;n++)
			{
				OLED_GRAM[i-1][n]=OLED_GRAM[i][n];
			}
		}
		OLED_MarkAllDirty();
		OLED_Refresh();
	}
}

//x,y���������
//sizex,sizey,ͼƬ����
//BMP[]��Ҫд���ͼƬ����
//mode:0,��ɫ��ʾ;1,������ʾ
void OLED_ShowPicture(u8 x,u8 y,u8 sizex,u8 sizey,u8 BMP[],u8 mode)
{
	u16 j=0;
	u8 i,n,temp,m;
	u8 x0=x,y0=y;
	sizey=sizey/8+((sizey%8)?1:0);
	for(n=0;n<sizey;n++)
	{
		 for(i=0;i<sizex;i++)
		 {
				temp=BMP[j];
				j++;
				for(m=0;m<8;m++)
				{
					if(temp&0x01)OLED_DrawPoint(x,y,mode);
					else OLED_DrawPoint(x,y,!mode);
					temp>>=1;
					y++;
				}
				x++;
				if((x-x0)==sizex)
				{
					x=x0;
					y0=y0+8;
				}
				y=y0;
     }
	 }
}
//OLED�ĳ�ʼ��
//OLED????
void OLED_Init(void)
{
    GPIO_InitTypeDef  GPIO_InitStructure;
    u8 i;
    
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB,ENABLE);//??GPIO??
      
    //GPIO?????
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_10|GPIO_Pin_11;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;//??????
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;//????
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;//100MHz
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;//??
    GPIO_Init(GPIOB, &GPIO_InitStructure);//???
    
    delay_ms(200);

    OLED_WR_Byte(0xAE,OLED_CMD);//--turn off oled panel
    OLED_WR_Byte(0x00,OLED_CMD);//---set low column address
    OLED_WR_Byte(0x10,OLED_CMD);//---set high column address
    OLED_WR_Byte(0x40,OLED_CMD);//--set start line address  Set Mapping RAM Display Start Line (0x00~0x3F)
    OLED_WR_Byte(0x81,OLED_CMD);//--set contrast control register
    OLED_WR_Byte(0xCF,OLED_CMD);// Set SEG Output Current Brightness
    OLED_WR_Byte(0xA1,OLED_CMD);//--Set SEG/Column Mapping     0xa0???? 0xa1??
    OLED_WR_Byte(0xC8,OLED_CMD);//Set COM/Row Scan Direction   0xc0???? 0xc8??
    OLED_WR_Byte(0xA6,OLED_CMD);//--set normal display
    OLED_WR_Byte(0xA8,OLED_CMD);//--set multiplex ratio(1 to 64)
    OLED_WR_Byte(0x3f,OLED_CMD);//--1/64 duty
    OLED_WR_Byte(0xD3,OLED_CMD);//-set display offset        Shift Mapping RAM Counter (0x00~0x3F)
    OLED_WR_Byte(0x00,OLED_CMD);//-not offset
    OLED_WR_Byte(0xd5,OLED_CMD);//--set display clock divide ratio/oscillator frequency
    OLED_WR_Byte(0x80,OLED_CMD);//--set divide ratio, Set Clock as 100 Frames/Sec
    OLED_WR_Byte(0xD9,OLED_CMD);//--set pre-charge period
    OLED_WR_Byte(0xF1,OLED_CMD);//Set Pre-Charge as 15 Clocks & Discharge as 1 Clock
    OLED_WR_Byte(0xDA,OLED_CMD);//--set com pins hardware configuration
    OLED_WR_Byte(0x12,OLED_CMD);
    OLED_WR_Byte(0xDB,OLED_CMD);//--set vcomh
    OLED_WR_Byte(0x40,OLED_CMD);//Set VCOM Deselect Level
    OLED_WR_Byte(0x20,OLED_CMD);//-Set Page Addressing Mode (0x00/0x01/0x02)
    OLED_WR_Byte(0x02,OLED_CMD);//
    OLED_WR_Byte(0x8D,OLED_CMD);//--set Charge Pump enable/disable
    OLED_WR_Byte(0x14,OLED_CMD);//--set(0x10) disable
    OLED_WR_Byte(0xA4,OLED_CMD);// Disable Entire Display On (0xa4/0xa5)
    OLED_WR_Byte(0xA6,OLED_CMD);// Disable Inverse Display On (0xa6/a7) 
    OLED_Clear();
    /* 初始化阶段允许阻塞：把整屏（8 页）刷完，之后主循环按脏页局部刷新 */
    for(i=0;i<8;i++)
    {
        if(OLED_DirtyPages == 0) break;
        OLED_Refresh();
    }
    OLED_WR_Byte(0xAF,OLED_CMD);
}

/* ==================== 14x14 中文字库显示（oled_cn14.h） ==================== */
/**
 * @brief  按 Unicode 码点查找 14x14 中文字模
 * @retval 字模首地址（28 字节），未收录返回 NULL
 * @note   字库按 Unicode 升序排列，使用二分查找，O(log n)。
 */
static const uint8_t *OLED_CN14_GetBitmap(uint16_t unicode)
{
    uint16_t lo = 0;
    uint16_t hi = OLED_CN14_COUNT;
    uint16_t mid;

    while (lo < hi)
    {
        mid = (uint16_t)(lo + (hi - lo) / 2u);
        if (oled_cn14_map[mid].unicode < unicode)
        {
            lo = (uint16_t)(mid + 1u);
        }
        else
        {
            hi = mid;
        }
    }

    if ((lo < OLED_CN14_COUNT) && (oled_cn14_map[lo].unicode == unicode))
    {
        return oled_cn14_bitmap[oled_cn14_map[lo].index];
    }
    return 0;
}

/**
 * @brief  GB2312 双字节 -> Unicode（仅字库中收录的省份/特殊汉字）
 */
static uint16_t OLED_CN14_FromGB2312(uint8_t hi, uint8_t lo)
{
    uint16_t i;

    for (i = 0; i < OLED_GB2312_COUNT; i++)
    {
        if ((oled_gb2312_map[i].hi == hi) && (oled_gb2312_map[i].lo == lo))
        {
            return oled_gb2312_map[i].unicode;
        }
    }
    return 0;
}

/**
 * @brief  解码一个 UTF-8 字符
 * @param  pp: 输入/输出指针（成功时指向下一个字符）
 * @param  cp: 输出 Unicode 码点
 * @retval 字节数（1/2/3），非法字节返回 0
 */
static uint8_t OLED_UTF8_Decode(const uint8_t **pp, uint16_t *cp)
{
    const uint8_t *p = *pp;
    uint8_t b = p[0];

    if (b < 0x80u)
    {
        *cp = b;
        *pp = p + 1;
        return 1;
    }
    if (((b & 0xE0u) == 0xC0u) && (p[1] != 0))
    {
        *cp = (uint16_t)(((uint16_t)(b & 0x1Fu) << 6) | (uint16_t)(p[1] & 0x3Fu));
        *pp = p + 2;
        return 2;
    }
    if (((b & 0xF0u) == 0xE0u) && (p[1] != 0) && (p[2] != 0))
    {
        *cp = (uint16_t)(((uint16_t)(b & 0x0Fu) << 12) |
                         ((uint16_t)(p[1] & 0x3Fu) << 6) |
                         (uint16_t)(p[2] & 0x3Fu));
        *pp = p + 3;
        return 3;
    }

    *pp = p + 1;
    *cp = 0;
    return 0;
}

/**
 * @brief  在指定位置绘制一个 14x14 汉字
 * @param  x,y: 字符左上角（y 为 16px 行高内的顶部坐标）
 * @note   未收录字符不绘制；越界由 OLED_DrawPoint 保护。
 */
void OLED_ShowCN14Char(u8 x, u8 y, uint16_t unicode, u8 mode)
{
    const uint8_t *bmp = OLED_CN14_GetBitmap(unicode);
    uint8_t r;
    uint8_t c;
    uint16_t bits;

    if (bmp == 0)
    {
        return;
    }

    for (r = 0; r < OLED_CN14_HEIGHT; r++)
    {
        bits = (uint16_t)(((uint16_t)bmp[r * 2u] << 8) | (uint16_t)bmp[r * 2u + 1u]);
        if (bits == 0u)
        {
            continue;
        }
        for (c = 0; c < OLED_CN14_WIDTH; c++)
        {
            if (bits & (uint16_t)(0x8000u >> c))
            {
                OLED_DrawPoint((u8)(x + c), (u8)(y + r), mode);
            }
        }
    }
}

/**
 * @brief  显示混合中英文/数字字符串
 * @param  x,y: 起始位置（建议 y 取 0/16/32/48，中文占 14px、ASCII 用 12px 字库）
 * @param  str: UTF-8 字符串（ASCII 部分使用 6x12 字库）
 * @retval 下一个字符的 x 坐标
 * @note   ASCII 使用 6x12 字库（垂直居中于 16px 行），汉字使用 14x14 字库。
 */
u8 OLED_ShowCNString(u8 x, u8 y, const char *str, u8 mode)
{
    const uint8_t *p = (const uint8_t *)str;
    const uint8_t *next;
    uint16_t cp;
    uint8_t len;

    if (str == 0)
    {
        return x;
    }

    while (*p != 0u)
    {
        next = p;
        len = OLED_UTF8_Decode(&next, &cp);

        if (len == 1u)
        {
            if ((cp >= 0x20u) && (cp <= 0x7Eu))
            {
                OLED_ShowChar(x, (u8)(y + 2u), (u8)cp, 12, mode);
                x = (u8)(x + 6u);
            }
        }
        else if (len == 3u)
        {
            if (OLED_CN14_GetBitmap(cp) != 0)
            {
                OLED_ShowCN14Char(x, (u8)(y + 1u), cp, mode);
                x = (u8)(x + 14u);
            }
            else
            {
                OLED_ShowChar(x, (u8)(y + 2u), (u8)'?', 12, mode);
                x = (u8)(x + 6u);
            }
        }
        else if (len == 2u)
        {
            OLED_ShowChar(x, (u8)(y + 2u), (u8)'?', 12, mode);
            x = (u8)(x + 6u);
        }

        p = next;
    }

    return x;
}

/**
 * @brief  计算混合字符串的像素宽度（汉字 14px、ASCII 6px）
 */
u8 OLED_CNStringWidth(const char *str)
{
    const uint8_t *p = (const uint8_t *)str;
    const uint8_t *next;
    uint16_t cp;
    uint8_t len;
    uint16_t width = 0;

    if (str == 0)
    {
        return 0;
    }

    while (*p != 0u)
    {
        next = p;
        len = OLED_UTF8_Decode(&next, &cp);
        if (len == 1u)
        {
            if ((cp >= 0x20u) && (cp <= 0x7Eu))
            {
                width = (uint16_t)(width + 6u);
            }
        }
        else if (len == 3u)
        {
            width = (uint16_t)(width + ((OLED_CN14_GetBitmap(cp) != 0) ? 14u : 6u));
        }
        else if (len == 2u)
        {
            width = (uint16_t)(width + 6u);
        }
        p = next;
    }

    return (u8)((width > 255u) ? 255u : width);
}

/**
 * @brief  居中显示混合字符串
 */
void OLED_ShowCNStringCenter(u8 y, const char *str, u8 mode)
{
    u8 width = OLED_CNStringWidth(str);
    u8 x = (width < 128u) ? (u8)((128u - width) / 2u) : 0u;
    (void)OLED_ShowCNString(x, y, str, mode);
}

/**
 * @brief  显示车牌（自动识别 UTF-8 汉字 / GB2312 汉字 / ASCII）
 * @param  plate: 以 0 结尾的车牌缓冲区；支持：
 *                - MaixCAM 原始车牌：UTF-8 编码（如 E4 BA AC A1 32 33...）
 *                - 上位机卡片数据库车牌：GB2312 编码（如 BE A9 A1 32 33...）
 *                - ASCII（CAR12345 等）
 * @retval 下一个字符 x 坐标
 * @note   只转换第一个汉字（省份），其余按 ASCII 显示。
 */
u8 OLED_ShowPlate(u8 x, u8 y, const u8 *plate, u8 mode)
{
    uint16_t cp;

    if ((plate == 0) || (plate[0] == 0u))
    {
        return x;
    }

    /* 1) UTF-8 汉字（MaixCAM 车牌）：直接走混合字符串 */
    if (((plate[0] & 0xF0u) == 0xE0u) &&
        ((plate[1] & 0xC0u) == 0x80u) &&
        ((plate[2] & 0xC0u) == 0x80u))
    {
        return OLED_ShowCNString(x, y, (const char *)plate, mode);
    }

    /* 2) GB2312 汉字（上位机数据库车牌）：查表转换第一个汉字 */
    if ((plate[0] >= 0xA1u) && (plate[0] <= 0xF7u) &&
        (plate[1] >= 0xA1u) && (plate[1] <= 0xFEu))
    {
        cp = OLED_CN14_FromGB2312(plate[0], plate[1]);
        if (cp != 0u)
        {
            OLED_ShowCN14Char(x, (u8)(y + 1u), cp, mode);
            x = (u8)(x + 14u);
        }
        else
        {
            OLED_ShowChar(x, (u8)(y + 2u), (u8)'?', 12, mode);
            x = (u8)(x + 6u);
        }
        return OLED_ShowCNString(x, y, (const char *)(plate + 2), mode);
    }

    /* 3) ASCII 车牌 */
    return OLED_ShowCNString(x, y, (const char *)plate, mode);
}


