#ifndef _BSP_RC522_H
#define _BSP_RC522_H

#include "stm32f4xx.h"

#ifndef u8
#define u8 uint8_t 
#endif

#ifndef u16
#define u16 uint16_t 
#endif

#ifndef u32
#define u32 uint32_t 
#endif

//SDA
#define RCC_CS          RCC_AHB1Periph_GPIOA
#define PORT_CS         GPIOA
#define GPIO_CS         GPIO_Pin_5

//SCK
#define RCC_SCK         RCC_AHB1Periph_GPIOA
#define PORT_SCK        GPIOA
#define GPIO_SCK        GPIO_Pin_7
//MOSI
#define RCC_MOSI         RCC_AHB1Periph_GPIOC
#define PORT_MOSI        GPIOC
#define GPIO_MOSI        GPIO_Pin_4

//RST
#define RCC_RST          RCC_AHB1Periph_GPIOA
#define PORT_RST         GPIOA
#define GPIO_RST         GPIO_Pin_6

//MISO
#define RCC_MISO         RCC_AHB1Periph_GPIOA
#define PORT_MISO        GPIOA
#define GPIO_MISO        GPIO_Pin_4



/* IO????? */
#define   RC522_CS_Enable()         GPIO_WriteBit(PORT_CS, GPIO_CS,Bit_RESET)
#define   RC522_CS_Disable()        GPIO_WriteBit(PORT_CS, GPIO_CS,Bit_SET)

#define   RC522_Reset_Enable()      GPIO_WriteBit(PORT_RST, GPIO_RST,Bit_RESET)
#define   RC522_Reset_Disable()     GPIO_WriteBit(PORT_RST, GPIO_RST,Bit_SET)

#define   RC522_SCK_0()             GPIO_WriteBit(PORT_SCK, GPIO_SCK,Bit_RESET)
#define   RC522_SCK_1()             GPIO_WriteBit(PORT_SCK, GPIO_SCK,Bit_SET)

#define   RC522_MOSI_0()            GPIO_WriteBit(PORT_MOSI, GPIO_MOSI,Bit_RESET)
#define   RC522_MOSI_1()            GPIO_WriteBit(PORT_MOSI, GPIO_MOSI,Bit_SET)

#define   RC522_MISO_GET()          GPIO_ReadInputDataBit(PORT_MISO, GPIO_MISO)   


//RC522???
#define PCD_IDLE              0x00               //??????
#define PCD_AUTHENT           0x0E               //????
#define PCD_RECEIVE           0x08               //????
#define PCD_TRANSMIT          0x04               //????
#define PCD_TRANSCEIVE        0x0C               //???????
#define PCD_RESETPHASE        0x0F               //??
#define PCD_CALCCRC           0x03               //CRC??

//Mifare_One?????
#define PICC_REQIDL           0x26               //????????????
#define PICC_REQALL           0x52               //????????
#define PICC_ANTICOLL1        0x93               //???
#define PICC_ANTICOLL2        0x95               //???
#define PICC_AUTHENT1A        0x60               //??A??
#define PICC_AUTHENT1B        0x61               //??B??
#define PICC_READ             0x30               //??
#define PICC_WRITE            0xA0               //??
#define PICC_DECREMENT        0xC0               //??
#define PICC_INCREMENT        0xC1               //??
#define PICC_RESTORE          0xC2               //????????
#define PICC_TRANSFER         0xB0               //????????
#define PICC_HALT             0x50               //??

/* RC522  FIFO???? */
#define DEF_FIFO_LENGTH       64                 //FIFO size=64byte
#define MAXRLEN  18


/* RC522????? */
// PAGE 0
#define     RFU00                 0x00    //??
#define     CommandReg            0x01    //??????????
#define     ComIEnReg             0x02    //?????????(Enable/Disable)
#define     DivlEnReg             0x03    //?????????
#define     ComIrqReg             0x04    //????????
#define     DivIrqReg             0x05    //????????
#define     ErrorReg              0x06    //????,??????????????
#define     Status1Reg            0x07    //?????????
#define     Status2Reg            0x08    //??????????????
#define     FIFODataReg           0x09    //64??FIFO?????????
#define     FIFOLevelReg          0x0A    //??FIFO???????
#define     WaterLevelReg         0x0B    //??FIFO????????FIFO??
#define     ControlReg            0x0C    //????????
#define     BitFramingReg         0x0D    //????????
#define     CollReg               0x0E    //RF??????????????????
#define     RFU0F                 0x0F    //??
// PAGE 1     
#define     RFU10                 0x10    //??
#define     ModeReg               0x11    //????????????
#define     TxModeReg             0x12    //?????????????
#define     RxModeReg             0x13    //??????????????
#define     TxControlReg          0x14    //?????????TX1?TX2?????
#define     TxAutoReg             0x15    //??????????
#define     TxSelReg              0x16    //???????????
#define     RxSelReg              0x17    //??????????
#define     RxThresholdReg        0x18    //?????????
#define     DemodReg              0x19    //????????
#define     RFU1A                 0x1A    //??
#define     RFU1B                 0x1B    //??
#define     MifareReg             0x1C    //??ISO 14443/MIFARE???106kbit/s???
#define     RFU1D                 0x1D    //??
#define     RFU1E                 0x1E    //??
#define     SerialSpeedReg        0x1F    //????UART?????
// PAGE 2    
#define     RFU20                 0x20    //??
#define     CRCResultRegM         0x21    //??CRC?????MSB?
#define     CRCResultRegL         0x22    //??CRC?????LSB?
#define     RFU23                 0x23    //??
#define     ModWidthReg           0x24    //??ModWidth???
#define     RFU25                 0x25    //??
#define     RFCfgReg              0x26    //???????
#define     GsNReg                0x27    //?????????(TX1?TX2)?????
#define     CWGsCfgReg            0x28    //??????????????
#define     ModGsCfgReg           0x29    //??????????????
#define     TModeReg              0x2A    //??????????
#define     TPrescalerReg         0x2B    //??????????
#define     TReloadRegH           0x2C    //??16?????????
#define     TReloadRegL           0x2D    //??16?????????
#define     TCounterValueRegH     0x2E   
#define     TCounterValueRegL     0x2F    //??16?????????
// PAGE 3      
#define     RFU30                 0x30    //??
#define     TestSel1Reg           0x31    //????????
#define     TestSel2Reg           0x32    //?????????PRBS?? 
#define     TestPinEnReg          0x33    //D1-D7??????????(???????)
#define     TestPinValueReg       0x34    //??D1-D7??I/O?????
#define     TestBusReg            0x35    //???????????
#define     AutoTestReg           0x36    //???????
#define     VersionReg            0x37    //????
#define     AnalogTestReg         0x38    //????AUX1?AUX2
#define     TestDAC1Reg           0x39    //??TestDAC1????
#define     TestDAC2Reg           0x3A    //??TestDAC2????
#define     TestADCReg            0x3B    //??ADCI?Q??????
#define     RFU3C                 0x3C    //??
#define     RFU3D                 0x3D    //??
#define     RFU3E                 0x3E    //??
#define     RFU3F                                          0x3F    //??

/* ?RC522?????????? */
#define         MI_OK                 0x26
#define         MI_NOTAGERR           0xcc
#define         MI_ERR                0xbb

/**********************************************************************/

void RC522_Init(void);/* IO???? */
////////////////????SPI?RC522??///////////////////////////////////////////
void RC522_SPI_SendByte( uint8_t byte );/* ????SPI????????,???? */
uint8_t RC522_SPI_ReadByte( void );/* ????SPI????????,???? */
uint8_t RC522_Read_Register( uint8_t Address );//??RC522???????
void RC522_Write_Register( uint8_t Address, uint8_t data );//?RC522?????????????
void RC522_SetBit_Register( uint8_t Address, uint8_t mask );//??RC522?????????
void RC522_ClearBit_Register( uint8_t Address, uint8_t mask );//??RC522?????????
/////////////////////STM32?RC522?????///////////////////////////////////
void RC522_Antenna_On( void );//????
void RC522_Antenna_Off( void );//????
void RC522_Rese( void );//??RC522
void RC522_Config_Type( char Type );//??RC522?????
/////////////////////////STM32??RC522?M1???///////////////////////////////////////
char PcdComMF522 ( uint8_t ucCommand, uint8_t * pInData, uint8_t ucInLenByte, uint8_t * pOutData, uint32_t * pOutLenBit );//??RC522?ISO14443???
char PcdRequest ( uint8_t ucReq_code, uint8_t * pTagType );//??
char PcdAnticoll ( uint8_t * pSnr );//???
void CalulateCRC ( uint8_t * pIndata, u8 ucLen, uint8_t * pOutData );//?RC522??CRC16(??????)
char PcdSelect ( uint8_t * pSnr );//选定卡片（PcdSelectEx 的包装，SAK 丢弃）
char PcdSelectEx ( uint8_t * pSnr, uint8_t * pSak );//选定卡片并输出 SAK；pSak 可为 NULL；成功返回 MI_OK
char PcdAuthState ( uint8_t ucAuth_mode, uint8_t ucAddr, uint8_t * pKey, uint8_t * pSnr );//??????
char PcdWrite ( uint8_t ucAddr, uint8_t * pData );//?M1?????????????
char PcdRead ( uint8_t ucAddr, uint8_t * pData );//??M1????????
char PcdHalt( void );//????????
char PcdUltralightRead ( uint8_t ucPage, uint8_t * pData );
char PcdUltralightWrite ( uint8_t ucPage, uint8_t * pData );


#endif  


