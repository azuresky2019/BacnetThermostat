#include <string.h>
#include <stdio.h>
#include "sdcard.h"
#include "delay.h"

#define SDIO_CLKEN_SW(X)	(BIT_ADDR(SDIO->CLKCR, 8) = X)


static u8 CardType = SDIO_STD_CAPACITY_SD_CARD_V1_1;		//SD�����ͣ�Ĭ��Ϊ1.x����
static u32 CSD_Tab[4], CID_Tab[4], RCA=0;					//SD��CSD,CID�Լ���Ե�ַ(RCA)����
static u8 DeviceMode = SD_DMA_MODE;		   					//����ģʽ,ע��,����ģʽ����ͨ��SD_SetDeviceMode,�������.����ֻ�Ƕ���һ��Ĭ�ϵ�ģʽ(SD_DMA_MODE)
static u8 StopCondition = 0; 								//�Ƿ���ֹͣ�����־λ,DMA����д��ʱ���õ�  
volatile SD_Error TransferError = SD_OK;					//���ݴ�������־,DMA��дʱʹ��	    
volatile u8 TransferEnd = 0;								//���������־,DMA��дʱʹ��
SD_CardInfo SDCardInfo;										//SD����Ϣ

//SD_ReadDisk/SD_WriteDisk����ר��buf,�����������������ݻ�������ַ����4�ֽڶ����ʱ��,
//��Ҫ�õ�������,ȷ�����ݻ�������ַ��4�ֽڶ����.
__align(4) u8 SDIO_DATA_BUFFER[512];


void Sdio_GPIO_Init(void)
{
//	GPIO_InitTypeDef GPIO_InitStructure;
	NVIC_InitTypeDef NVIC_InitStructure;

	//SDIO NVIC ����
    NVIC_InitStructure.NVIC_IRQChannel = SDIO_IRQn;
	NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 0;	//��ռ���ȼ�0
	NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0;			//�����ȼ�2
	NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;				//IRQͨ��ʹ��
	NVIC_Init(&NVIC_InitStructure);								//����ָ���Ĳ�����ʼ��NVIC�Ĵ���

	RCC->APB2ENR |= (1 << 4) | (1 << 5);    //ʹ��PORTC,PORTDʱ��	   	 
	/* Configure PC.08, PC.09, PC.10, PC.11, PC.12 pin: D0, D1, D2, D3, CLK pin */  	 
	GPIOC->CRH &= 0XFFF00000; 
	GPIOC->CRH |= 0X000BBBBB;				//PC8,PC9,PC10,PC11,PC12�����������   	 
    GPIOC->ODR |= (1 << 8) | (1 << 9) | (1 << 10) | (1 << 11) | (1 << 12);//��ʼ�����
	/* Configure PD2,    pin: cmd */										  
	GPIOD->CRL &= 0XFFFFF0FF;
	GPIOD->CRL |= 0X00000B00;				//PD.2�����������
	GPIOD->ODR |= (1 << 2);

	RCC->AHBENR |= (1 << 1) | (1 << 10);    //ʹ��DMA2��SDIOʱ��


//	RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOC | RCC_APB2Periph_GPIOD, ENABLE);

//	/* Configure PC.08, PC.09, PC.10, PC.11, PC.12 pin: D0, D1, D2, D3, CLK pin */  	 
//	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_8 | GPIO_Pin_9 | GPIO_Pin_10 | GPIO_Pin_11 | GPIO_Pin_12;
//	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
//	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
//	GPIO_Init(GPIOC, &GPIO_InitStructure);
//	GPIO_SetBits(GPIOC, GPIO_Pin_8 | GPIO_Pin_9 | GPIO_Pin_10 | GPIO_Pin_11 | GPIO_Pin_12);
//	
//	/* Configure PD2,    pin: cmd */
//	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_2;
//	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP;
//	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
//	GPIO_Init(GPIOD, &GPIO_InitStructure);
//	GPIO_SetBits(GPIOD, GPIO_Pin_2);

//	RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA2 | RCC_AHBPeriph_SDIO, ENABLE);
}

//SDIOʱ�ӳ�ʼ������
//clkdiv:ʱ�ӷ�Ƶϵ��
//CKʱ��=SDIOCLK/[clkdiv+2];(SDIOCLKʱ��ֱ�Ӿ���AHBʱ��,һ��Ϊ72Mhz)
void SDIO_Clock_Set(u8 clkdiv)
{
  	SDIO->CLKCR &= 0XFFFFFF00;
 	SDIO->CLKCR |= clkdiv; 
} 

//SDIO���������
//cmdindex:��������,����λ��Ч
//waitrsp:�ڴ�����Ӧ.00/10,����Ӧ;01,����Ӧ;11,����Ӧ
//arg:����
void SDIO_Send_Cmd(u8 cmdindex, u8 waitrsp, u32 arg)
{						    
	SDIO->ARG = arg;
	SDIO->CMD &= 0XFFFFF800;		//���index��waitrsp
	SDIO->CMD |= cmdindex & 0X3F;	//�����µ�index			 
	SDIO->CMD |= waitrsp << 6;		//�����µ�wait rsp 
	SDIO->CMD |= 0 << 8;			//�޵ȴ�
  	SDIO->CMD |= 1 << 10;			//����ͨ��״̬��ʹ��
}

//SDIO�����������ú���
//datatimeout:��ʱʱ������
//datalen:�������ݳ���,��25λ��Ч,����Ϊ���С��������
//blksize:���С.ʵ�ʴ�СΪ:2^blksize�ֽ�
//dir:���ݴ��䷽��:0,����������;1,����������;
void SDIO_Send_Data_Cfg(u32 datatimeout, u32 datalen, u8 blksize, u8 dir)
{
	SDIO->DTIMER = datatimeout;
  	SDIO->DLEN = datalen & 0X1FFFFFF;	//��25λ��Ч
	SDIO->DCTRL &= 0xFFFFFF08;			//���֮ǰ������.
	SDIO->DCTRL |= blksize << 4;		//���ÿ��С
	SDIO->DCTRL |= 0 << 2;				//�����ݴ���
	SDIO->DCTRL |= (dir & 0X01) << 1;	//�������
	SDIO->DCTRL |= 1 << 0;				//���ݴ���ʹ��,DPSM״̬��
}  

//����SDIO DMA  
//mbuf:�洢����ַ
//bufsize:����������
//dir:����;1,�洢��-->SDIO(д����);0,SDIO-->�洢��(������);
void SD_DMA_Config(u32*mbuf, u32 bufsize, u8 dir)
{				  
 	DMA2->IFCR |= (0XF << 12);				//���DMA2ͨ��4�ĸ��ֱ��
 	DMA2_Channel4->CCR &= ~(1 << 0);		//�ر�DMA ͨ��4
  	DMA2_Channel4->CCR &= ~(0X7FF << 4);	//���֮ǰ������,DIR,CIRC,PINC,MINC,PSIZE,MSIZE,PL,MEM2MEM
 	DMA2_Channel4->CCR |= dir << 4;  		//�Ӵ洢����   
	DMA2_Channel4->CCR |= 0 << 5;  			//��ͨģʽ
	DMA2_Channel4->CCR |= 0 << 6; 			//�����ַ������ģʽ
	DMA2_Channel4->CCR |= 1 << 7;  			//�洢������ģʽ
	DMA2_Channel4->CCR |= 2 << 8;  			//�������ݿ���Ϊ32λ
	DMA2_Channel4->CCR |= 2 << 10; 			//�洢�����ݿ���32λ
	DMA2_Channel4->CCR |= 2 << 12; 			//�����ȼ�	  
  	DMA2_Channel4->CNDTR = bufsize / 4;   	//DMA2,����������	  
 	DMA2_Channel4->CPAR = (u32)&SDIO->FIFO;	//DMA2 �����ַ 
	DMA2_Channel4->CMAR = (u32)mbuf; 		//DMA2,�洢����ַ
 	DMA2_Channel4->CCR |= 1 << 0; 			//����DMAͨ��		
}

//���CMD0��ִ��״̬
//����ֵ:sd��������
SD_Error CmdError(void)
{
	SD_Error errorstatus = SD_OK;
	u32 timeout = SDIO_CMD0TIMEOUT;
	
	while(timeout--)
	{
		if(SDIO->STA & (1 << 7)) break;	//�����ѷ���(������Ӧ)	 
	}
	
	if(timeout == 0)
		return SD_CMD_RSP_TIMEOUT; 
	
	SDIO->ICR = 0X5FF;					//������
	return errorstatus;
}	 
//���R7��Ӧ�Ĵ���״̬
//����ֵ:sd��������
SD_Error CmdResp7Error(void)
{
	SD_Error errorstatus = SD_OK;
	u32 status;
	u32 timeout = SDIO_CMD0TIMEOUT;
 	while(timeout--)
	{
		status = SDIO->STA;
		if(status & ((1 << 0) | (1 << 2) | (1 << 6))) break;	//CRC����/������Ӧ��ʱ/�Ѿ��յ���Ӧ(CRCУ��ɹ�)	
	}
	
 	if((timeout == 0) || (status & (1 << 2)))					//��Ӧ��ʱ
	{																				    
		errorstatus = SD_CMD_RSP_TIMEOUT;						//��ǰ������2.0���ݿ�,���߲�֧���趨�ĵ�ѹ��Χ
		SDIO->ICR |= 1 << 2;									//���������Ӧ��ʱ��־
		return errorstatus;
	}
	
	if(status & (1 << 6))										//�ɹ����յ���Ӧ
	{								   
		errorstatus = SD_OK;
		SDIO->ICR |= 1 << 6;									//�����Ӧ��־
 	}
	return errorstatus;
}	   
//���R1��Ӧ�Ĵ���״̬
//cmd:��ǰ����
//����ֵ:sd��������
SD_Error CmdResp1Error(u8 cmd)
{	  
   	u32 status;
	while(1)
	{
		status = SDIO->STA;
		if(status & ((1 << 0) | (1 << 2) | (1 << 6)))break;//CRC����/������Ӧ��ʱ/�Ѿ��յ���Ӧ(CRCУ��ɹ�)	
	}
	
 	if(status & (1 << 2))					//��Ӧ��ʱ
	{																				    
 		SDIO->ICR = 1 << 2;					//���������Ӧ��ʱ��־
		return SD_CMD_RSP_TIMEOUT;
	}
	
 	if(status & (1 << 0))					//CRC����
	{																				    
 		SDIO->ICR = 1<<0;					//�����־
		return SD_CMD_CRC_FAIL;
	}
	
	if(SDIO->RESPCMD != cmd)
		return SD_ILLEGAL_CMD;				//���ƥ�� 
	
  	SDIO->ICR = 0X5FF;	 					//������
	return (SD_Error)(SDIO->RESP1 & SD_OCR_ERRORBITS);//���ؿ���Ӧ
}
//���R3��Ӧ�Ĵ���״̬
//����ֵ:����״̬
SD_Error CmdResp3Error(void)
{
	u32 status;						 
 	while(1)
	{
		status = SDIO->STA;
		if(status & ((1 << 0) | (1 << 2) | (1 << 6)))break;//CRC����/������Ӧ��ʱ/�Ѿ��յ���Ӧ(CRCУ��ɹ�)	
	}
	
 	if(status & (1 << 2))					//��Ӧ��ʱ
	{											 
		SDIO->ICR |= 1 << 2;				//���������Ӧ��ʱ��־
		return SD_CMD_RSP_TIMEOUT;
	}
	
   	SDIO->ICR = 0X5FF;	 				//������
 	return SD_OK;								  
}
//���R2��Ӧ�Ĵ���״̬
//����ֵ:����״̬
SD_Error CmdResp2Error(void)
{
	SD_Error errorstatus = SD_OK;
	u32 status;
	u32 timeout = SDIO_CMD0TIMEOUT;
 	while(timeout--)
	{
		status = SDIO->STA;
		if(status & ((1 << 0) | (1 << 2) | (1 << 6)))break;//CRC����/������Ӧ��ʱ/�Ѿ��յ���Ӧ(CRCУ��ɹ�)	
	}
	
  	if((timeout == 0) || (status & (1 << 2)))	//��Ӧ��ʱ
	{																				    
		errorstatus = SD_CMD_RSP_TIMEOUT; 
		SDIO->ICR |= 1 << 2;				//���������Ӧ��ʱ��־
		return errorstatus;
	}	
	
	if(status & (1 << 0))						//CRC����
	{								   
		errorstatus = SD_CMD_CRC_FAIL;
		SDIO->ICR |= 1 << 0;				//�����Ӧ��־
 	}
	
	SDIO->ICR = 0X5FF;	 				//������
 	return errorstatus;								    		 
} 
//���R6��Ӧ�Ĵ���״̬
//cmd:֮ǰ���͵�����
//prca:�����ص�RCA��ַ
//����ֵ:����״̬
SD_Error CmdResp6Error(u8 cmd, u16*prca)
{
	SD_Error errorstatus = SD_OK;
	u32 status;					    
	u32 rspr1;
 	while(1)
	{
		status = SDIO->STA;
		if(status & ((1 << 0) | (1 << 2) | (1 << 6)))break;//CRC����/������Ӧ��ʱ/�Ѿ��յ���Ӧ(CRCУ��ɹ�)	
	}
	
	if(status & (1 << 2))					//��Ӧ��ʱ
	{																				    
 		SDIO->ICR |= 1 << 2;				//���������Ӧ��ʱ��־
		return SD_CMD_RSP_TIMEOUT;
	}
	
	if(status & (1 << 0))					//CRC����
	{								   
		SDIO->ICR |= (1 << 0);				//�����Ӧ��־
 		return SD_CMD_CRC_FAIL;
	}
	
	if(SDIO->RESPCMD != cmd)				//�ж��Ƿ���Ӧcmd����
	{
 		return SD_ILLEGAL_CMD; 		
	}
	
	SDIO->ICR = 0X5FF;	 				//������б��
	rspr1 = SDIO->RESP1;				//�õ���Ӧ 	 
	if(SD_ALLZERO == (rspr1 & (SD_R6_GENERAL_UNKNOWN_ERROR | SD_R6_ILLEGAL_CMD | SD_R6_COM_CRC_FAILED)))
	{
		*prca = (u16)(rspr1 >> 16);			//����16λ�õ�,rca
		return errorstatus;
	}
	
   	if(rspr1 & SD_R6_GENERAL_UNKNOWN_ERROR)
		return SD_GENERAL_UNKNOWN_ERROR;
	
   	if(rspr1 & SD_R6_ILLEGAL_CMD)
		return SD_ILLEGAL_CMD;
	
   	if(rspr1 & SD_R6_COM_CRC_FAILED)
		return SD_COM_CRC_FAILED;
	
	return errorstatus;
}

//���ϵ�
//��ѯ����SDIO�ӿ��ϵĿ��豸,����ѯ���ѹ������ʱ��
//����ֵ:�������;(0,�޴���)
SD_Error SD_PowerON(void)
{
	u8 i = 0;
	SD_Error errorstatus = SD_OK;
	u32 response = 0, count = 0, validvoltage = 0;
	u32 SDType = SD_STD_CAPACITY;
	
	printf("Get into SD_PowerON()\r\n");
	
	// ����CLKCR�Ĵ��� 
	SDIO->CLKCR = 0;					//���CLKCR֮ǰ������
	SDIO->CLKCR |= 0 << 9;				//��ʡ��ģʽ
	SDIO->CLKCR |= 0 << 10;				//�ر���·,CK���ݷ�Ƶ�������
	SDIO->CLKCR |= 0 << 11;				//1λ���ݿ���
	SDIO->CLKCR |= 0 << 13;				//SDIOCLK�����ز���SDIOCK
	SDIO->CLKCR |= 0 << 14;				//�ر�Ӳ��������    
	SDIO_Clock_Set(SDIO_INIT_CLK_DIV);	//����ʱ��Ƶ��(��ʼ����ʱ��,���ܳ���400Khz)			 
 	SDIO->POWER = 0X03;					//�ϵ�״̬,������ʱ��    
  	SDIO->CLKCR |= 1 << 8;				//SDIOCKʹ��
	
	for(i = 0; i < 74; i++)
	{
		SDIO_Send_Cmd(SD_CMD_GO_IDLE_STATE, 0, 0);	//����CMD0����IDLE STAGEģʽ����.												  
		errorstatus = CmdError();
		if(errorstatus == SD_OK)
			break;
	}
	if(errorstatus != SD_OK)
		return errorstatus;//���ش���״̬
	
	printf("SD_CMD_GO_IDLE_STATE successful\r\n");

	SDIO_Send_Cmd(SDIO_SEND_IF_COND, 1, SD_CHECK_PATTERN);	//����CMD8,����Ӧ,���SD���ӿ�����.
															//arg[11:8]:01,֧�ֵ�ѹ��Χ,2.7~3.6V
															//arg[7:0]:Ĭ��0XAA
															//������Ӧ7
  	errorstatus = CmdResp7Error();							//�ȴ�R7��Ӧ
 	if(errorstatus == SD_OK) 								//R7��Ӧ����
	{
		CardType = SDIO_STD_CAPACITY_SD_CARD_V2_0;			//SD 2.0��
		SDType = SD_HIGH_CAPACITY;			   				//��������
		printf("SDIO_SEND_IF_COND successful\r\n");
	}
	else
	{
		printf("SDIO_SEND_IF_COND failure\r\n");
	}

	
	SDIO_Send_Cmd(SD_CMD_APP_CMD, 1, 0);					//����CMD55,����Ӧ	 
	errorstatus = CmdResp1Error(SD_CMD_APP_CMD); 		 	//�ȴ�R1��Ӧ   
	if(errorstatus == SD_OK)								//SD2.0/SD 1.1,����ΪMMC��
	{																  
		//SD��,����ACMD41 SD_APP_OP_COND,����Ϊ:0x80100000 
		while((!validvoltage) && (count < SD_MAX_VOLT_TRIAL))
		{	   										   
			SDIO_Send_Cmd(SD_CMD_APP_CMD, 1, 0);				//����CMD55,����Ӧ	 
			errorstatus = CmdResp1Error(SD_CMD_APP_CMD); 	 	//�ȴ�R1��Ӧ   
 			if(errorstatus != SD_OK)
				return errorstatus;   							//��Ӧ���

			SDIO_Send_Cmd(SD_CMD_SD_APP_OP_COND, 1, SD_VOLTAGE_WINDOW_SD | SDType);//����ACMD41,����Ӧ	 
			errorstatus = CmdResp3Error(); 						//�ȴ�R3��Ӧ   
 			if(errorstatus != SD_OK)
				return errorstatus;   							//��Ӧ���� 
			
			response = SDIO->RESP1;;			   				//�õ���Ӧ
			validvoltage = (((response >> 31) == 1) ? 1 : 0);	//�ж�SD���ϵ��Ƿ����
			count++;
		}
		
		if(count >= SD_MAX_VOLT_TRIAL)
		{
			errorstatus = SD_INVALID_VOLTRANGE;
			return errorstatus;
		}
		
		if(response &= SD_HIGH_CAPACITY)
		{
			CardType = SDIO_HIGH_CAPACITY_SD_CARD;
		}
 	}
	else//MMC��
	{
		//MMC��,����CMD1 SDIO_SEND_OP_COND,����Ϊ:0x80FF8000 
		while((!validvoltage) && (count < SD_MAX_VOLT_TRIAL))
		{	   										   				   
			SDIO_Send_Cmd(SD_CMD_SEND_OP_COND, 1, SD_VOLTAGE_WINDOW_MMC);	//����CMD1,����Ӧ	 
			errorstatus = CmdResp3Error(); 									//�ȴ�R3��Ӧ   
 			if(errorstatus != SD_OK)
				return errorstatus;   										//��Ӧ����
			
			response = SDIO->RESP1;;			   							//�õ���Ӧ
			validvoltage = (((response >> 31) == 1) ? 1 : 0);
			count++;
		}
		
		if(count >= SD_MAX_VOLT_TRIAL)
		{
			errorstatus = SD_INVALID_VOLTRANGE;
			return errorstatus;
		}
		
		CardType = SDIO_MULTIMEDIA_CARD;	  
  	}
	
	return errorstatus;
}

//SD�� Power OFF
//����ֵ:�������;(0,�޴���)
SD_Error SD_PowerOFF(void)
{
  	SDIO->POWER &= ~(3 << 0);	//SDIO��Դ�ر�,ʱ��ֹͣ	
	return SD_OK;		  
}

//��ʼ�����еĿ�,���ÿ��������״̬
//����ֵ:�������
SD_Error SD_InitializeCards(void)
{
 	SD_Error errorstatus = SD_OK;
	u16 rca = 0x01;
	
 	if((SDIO->POWER & 0X03) == 0)
		return SD_REQUEST_NOT_APPLICABLE;				//����Դ״̬,ȷ��Ϊ�ϵ�״̬
	
 	if(SDIO_SECURE_DIGITAL_IO_CARD != CardType)			//��SECURE_DIGITAL_IO_CARD
	{
		SDIO_Send_Cmd(SD_CMD_ALL_SEND_CID, 3, 0);		//����CMD2,ȡ��CID,����Ӧ	 
		errorstatus = CmdResp2Error(); 					//�ȴ�R2��Ӧ   
		if(errorstatus != SD_OK)
			return errorstatus;   						//��Ӧ����
	    
 		CID_Tab[0] = SDIO->RESP1;
		CID_Tab[1] = SDIO->RESP2;
		CID_Tab[2] = SDIO->RESP3;
		CID_Tab[3] = SDIO->RESP4;
	}
	
	//�жϿ�����
	if((SDIO_STD_CAPACITY_SD_CARD_V1_1 == CardType) || (SDIO_STD_CAPACITY_SD_CARD_V2_0 == CardType) || (SDIO_SECURE_DIGITAL_IO_COMBO_CARD == CardType) || (SDIO_HIGH_CAPACITY_SD_CARD == CardType))
	{
		SDIO_Send_Cmd(SD_CMD_SET_REL_ADDR, 1, 0);					//����CMD3,����Ӧ 
		errorstatus = CmdResp6Error(SD_CMD_SET_REL_ADDR, &rca);		//�ȴ�R6��Ӧ 
		if(errorstatus != SD_OK)
			return errorstatus;   									//��Ӧ����		    
	}
	
    if(SDIO_MULTIMEDIA_CARD == CardType)
    {
 		SDIO_Send_Cmd(SD_CMD_SET_REL_ADDR, 1, (u32)(rca << 16));	//����CMD3,����Ӧ 	   
		errorstatus = CmdResp2Error(); 								//�ȴ�R2��Ӧ   
		if(errorstatus != SD_OK)
			return errorstatus;   									//��Ӧ����	 
    }
	
	if(SDIO_SECURE_DIGITAL_IO_CARD != CardType)						//��SECURE_DIGITAL_IO_CARD
	{
		RCA = rca;
		SDIO_Send_Cmd(SD_CMD_SEND_CSD, 3, (u32)(rca << 16));		//����CMD9+��RCA,ȡ��CSD,����Ӧ 	   
		errorstatus = CmdResp2Error(); 								//�ȴ�R2��Ӧ   
		if(errorstatus != SD_OK)
			return errorstatus;   									//��Ӧ����	
	    
  		CSD_Tab[0] = SDIO->RESP1;
		CSD_Tab[1] = SDIO->RESP2;
		CSD_Tab[2] = SDIO->RESP3;						
		CSD_Tab[3] = SDIO->RESP4;					    
	}
	
	return SD_OK;//����ʼ���ɹ�
}

//�õ�����Ϣ
//cardinfo:����Ϣ�洢��
//����ֵ:����״̬
SD_Error SD_GetCardInfo(SD_CardInfo *cardinfo)
{
 	SD_Error errorstatus = SD_OK;
	u8 tmp = 0;
	
	cardinfo->CardType = (u8)CardType; 						//������
	cardinfo->RCA = (u16)RCA;								//��RCAֵ
	tmp = (u8)((CSD_Tab[0] & 0xFF000000) >> 24);
	cardinfo->SD_csd.CSDStruct = (tmp & 0xC0) >> 6;			//CSD�ṹ
	cardinfo->SD_csd.SysSpecVersion = (tmp & 0x3C) >> 2;	//2.0Э�黹û�����ⲿ��(Ϊ����),Ӧ���Ǻ���Э�鶨���
	cardinfo->SD_csd.Reserved1 = tmp & 0x03;				//2������λ
	tmp = (u8)((CSD_Tab[0] & 0x00FF0000) >> 16);			//��1���ֽ�
	cardinfo->SD_csd.TAAC = tmp;				   			//���ݶ�ʱ��1
	tmp = (u8)((CSD_Tab[0] & 0x0000FF00) >> 8);	  			//��2���ֽ�
	cardinfo->SD_csd.NSAC = tmp;		  					//���ݶ�ʱ��2
	tmp = (u8)(CSD_Tab[0] & 0x000000FF);					//��3���ֽ�
	cardinfo->SD_csd.MaxBusClkFrec = tmp;		  			//�����ٶ�	
	tmp = (u8)((CSD_Tab[1] & 0xFF000000) >> 24);			//��4���ֽ�
	cardinfo->SD_csd.CardComdClasses = tmp << 4;    		//��ָ�������λ
	tmp = (u8)((CSD_Tab[1] & 0x00FF0000) >> 16);	 		//��5���ֽ�
	cardinfo->SD_csd.CardComdClasses |= (tmp & 0xF0) >> 4;	//��ָ�������λ
	cardinfo->SD_csd.RdBlockLen = tmp & 0x0F;	    		//����ȡ���ݳ���
	tmp = (u8)((CSD_Tab[1] & 0x0000FF00) >> 8);				//��6���ֽ�
	cardinfo->SD_csd.PartBlockRead = (tmp & 0x80) >> 7;		//�����ֿ��
	cardinfo->SD_csd.WrBlockMisalign = (tmp & 0x40) >> 6;	//д���λ
	cardinfo->SD_csd.RdBlockMisalign = (tmp & 0x20) >> 5;	//�����λ
	cardinfo->SD_csd.DSRImpl = (tmp & 0x10) >> 4;
	cardinfo->SD_csd.Reserved2 = 0; 						//����
	
 	if((CardType == SDIO_STD_CAPACITY_SD_CARD_V1_1) || (CardType == SDIO_STD_CAPACITY_SD_CARD_V2_0) || (SDIO_MULTIMEDIA_CARD == CardType))//��׼1.1/2.0��/MMC��
	{
		cardinfo->SD_csd.DeviceSize = (tmp & 0x03) << 10;						//C_SIZE(12λ)
	 	tmp = (u8)(CSD_Tab[1] & 0x000000FF); 									//��7���ֽ�	
		cardinfo->SD_csd.DeviceSize |= (tmp) << 2;
 		tmp = (u8)((CSD_Tab[2] & 0xFF000000) >> 24);							//��8���ֽ�	
		cardinfo->SD_csd.DeviceSize |= (tmp & 0xC0) >> 6;
 		cardinfo->SD_csd.MaxRdCurrentVDDMin = (tmp & 0x38) >> 3;
		cardinfo->SD_csd.MaxRdCurrentVDDMax = (tmp & 0x07);
 		tmp = (u8)((CSD_Tab[2]&0x00FF0000) >> 16);								//��9���ֽ�	
		cardinfo->SD_csd.MaxWrCurrentVDDMin = (tmp & 0xE0) >> 5;
		cardinfo->SD_csd.MaxWrCurrentVDDMax = (tmp & 0x1C) >> 2;
		cardinfo->SD_csd.DeviceSizeMul = (tmp & 0x03) << 1;						//C_SIZE_MULT
 		tmp = (u8)((CSD_Tab[2] & 0x0000FF00) >> 8);	  							//��10���ֽ�	
		cardinfo->SD_csd.DeviceSizeMul |= (tmp & 0x80) >> 7;
 		cardinfo->CardCapacity = (cardinfo->SD_csd.DeviceSize + 1);				//���㿨����
		cardinfo->CardCapacity *= (1 << (cardinfo->SD_csd.DeviceSizeMul + 2));
		cardinfo->CardBlockSize = 1 << (cardinfo->SD_csd.RdBlockLen);			//���С
		cardinfo->CardCapacity *= cardinfo->CardBlockSize;
	}
	else if(CardType == SDIO_HIGH_CAPACITY_SD_CARD)								//��������
	{
 		tmp = (u8)(CSD_Tab[1] & 0x000000FF); 									//��7���ֽ�	
		cardinfo->SD_csd.DeviceSize = (tmp & 0x3F) << 16;						//C_SIZE
 		tmp = (u8)((CSD_Tab[2] & 0xFF000000) >> 24); 							//��8���ֽ�	
 		cardinfo->SD_csd.DeviceSize |= (tmp << 8);
 		tmp = (u8)((CSD_Tab[2] & 0x00FF0000) >> 16);							//��9���ֽ�	
 		cardinfo->SD_csd.DeviceSize |= (tmp);
 		tmp = (u8)((CSD_Tab[2] & 0x0000FF00) >> 8); 							//��10���ֽ�	
 		cardinfo->CardCapacity = (long long)(cardinfo->SD_csd.DeviceSize + 1) * 512 * 1024;	//���㿨����
		cardinfo->CardBlockSize = 512; 											//���С�̶�Ϊ512�ֽ�
	}	  
	cardinfo->SD_csd.EraseGrSize = (tmp & 0x40) >> 6;
	cardinfo->SD_csd.EraseGrMul = (tmp & 0x3F) << 1;	   
	tmp = (u8)(CSD_Tab[2] & 0x000000FF);										//��11���ֽ�	
	cardinfo->SD_csd.EraseGrMul |= (tmp & 0x80) >> 7;
	cardinfo->SD_csd.WrProtectGrSize = (tmp & 0x7F);
 	tmp = (u8)((CSD_Tab[3] & 0xFF000000) >> 24);								//��12���ֽ�	
	cardinfo->SD_csd.WrProtectGrEnable = (tmp & 0x80) >> 7;
	cardinfo->SD_csd.ManDeflECC = (tmp & 0x60) >> 5;
	cardinfo->SD_csd.WrSpeedFact = (tmp & 0x1C) >> 2;
	cardinfo->SD_csd.MaxWrBlockLen = (tmp & 0x03) << 2;	 
	tmp = (u8)((CSD_Tab[3] & 0x00FF0000) >> 16);								//��13���ֽ�
	cardinfo->SD_csd.MaxWrBlockLen |= (tmp & 0xC0) >> 6;
	cardinfo->SD_csd.WriteBlockPaPartial = (tmp & 0x20) >> 5;
	cardinfo->SD_csd.Reserved3 = 0;
	cardinfo->SD_csd.ContentProtectAppli = (tmp & 0x01);  
	tmp = (u8)((CSD_Tab[3] & 0x0000FF00) >> 8);									//��14���ֽ�
	cardinfo->SD_csd.FileFormatGrouop = (tmp & 0x80) >> 7;
	cardinfo->SD_csd.CopyFlag = (tmp & 0x40) >> 6;
	cardinfo->SD_csd.PermWrProtect = (tmp & 0x20) >> 5;
	cardinfo->SD_csd.TempWrProtect = (tmp & 0x10) >> 4;
	cardinfo->SD_csd.FileFormat = (tmp & 0x0C) >> 2;
	cardinfo->SD_csd.ECC = (tmp & 0x03);  
	tmp = (u8)(CSD_Tab[3] & 0x000000FF);										//��15���ֽ�
	cardinfo->SD_csd.CSD_CRC = (tmp & 0xFE) >> 1;
	cardinfo->SD_csd.Reserved4 = 1;		 
	tmp = (u8)((CID_Tab[0] & 0xFF000000) >> 24);								//��0���ֽ�
	cardinfo->SD_cid.ManufacturerID = tmp;		    
	tmp = (u8)((CID_Tab[0] & 0x00FF0000) >> 16);								//��1���ֽ�
	cardinfo->SD_cid.OEM_AppliID = tmp << 8;	  
	tmp = (u8)((CID_Tab[0] & 0x000000FF00) >> 8);								//��2���ֽ�
	cardinfo->SD_cid.OEM_AppliID |= tmp;	    
	tmp = (u8)(CID_Tab[0] & 0x000000FF);										//��3���ֽ�	
	cardinfo->SD_cid.ProdName1 = tmp << 24;				  
	tmp = (u8)((CID_Tab[1] & 0xFF000000) >> 24); 								//��4���ֽ�
	cardinfo->SD_cid.ProdName1 |= tmp << 16;	  
	tmp = (u8)((CID_Tab[1] & 0x00FF0000) >> 16);	   							//��5���ֽ�
	cardinfo->SD_cid.ProdName1 |= tmp << 8;		 
	tmp = (u8)((CID_Tab[1] & 0x0000FF00) >> 8);									//��6���ֽ�
	cardinfo->SD_cid.ProdName1 |= tmp;		   
	tmp = (u8)(CID_Tab[1] & 0x000000FF);	  									//��7���ֽ�
	cardinfo->SD_cid.ProdName2 = tmp;			  
	tmp = (u8)((CID_Tab[2] & 0xFF000000) >> 24); 								//��8���ֽ�
	cardinfo->SD_cid.ProdRev = tmp;		 
	tmp = (u8)((CID_Tab[2] & 0x00FF0000) >> 16);								//��9���ֽ�
	cardinfo->SD_cid.ProdSN = tmp << 24;	   
	tmp = (u8)((CID_Tab[2] & 0x0000FF00) >> 8); 								//��10���ֽ�
	cardinfo->SD_cid.ProdSN |= tmp << 16;	   
	tmp = (u8)(CID_Tab[2] & 0x000000FF);   										//��11���ֽ�
	cardinfo->SD_cid.ProdSN |= tmp << 8;		   
	tmp = (u8)((CID_Tab[3] & 0xFF000000) >> 24); 								//��12���ֽ�
	cardinfo->SD_cid.ProdSN |= tmp;			     
	tmp = (u8)((CID_Tab[3] & 0x00FF0000) >> 16);	 							//��13���ֽ�
	cardinfo->SD_cid.Reserved1 |= (tmp & 0xF0) >> 4;
	cardinfo->SD_cid.ManufactDate = (tmp & 0x0F) << 8;    
	tmp = (u8)((CID_Tab[3] & 0x0000FF00) >> 8);									//��14���ֽ�
	cardinfo->SD_cid.ManufactDate |= tmp;		 	  
	tmp = (u8)(CID_Tab[3] & 0x000000FF);										//��15���ֽ�
	cardinfo->SD_cid.CID_CRC=(tmp & 0xFE) >> 1;
	cardinfo->SD_cid.Reserved2 = 1;	 
	return errorstatus;
}

//SDIOʹ�ܿ�����ģʽ
//enx:0,��ʹ��;1,ʹ��;
//����ֵ:����״̬
SD_Error SDEnWideBus(u8 enx)
{
	SD_Error errorstatus = SD_OK;
 	u32 scr[2] = {0, 0};
	u8 arg = 0X00;
	
	printf("Get in SDEnWideBus(%d)\r\n", enx);
	
	if(enx)
		arg = 0X02;
	else
		arg = 0X00;
	
 	if(SDIO->RESP1 & SD_CARD_LOCKED)
	{
		printf("return SD_LOCK_UNLOCK_FAILED\r\n");
		return SD_LOCK_UNLOCK_FAILED;						//SD������LOCKED״̬
	}
	
	printf("Start FindSCR(RCA, scr)\r\n");
 	errorstatus = FindSCR(RCA, scr);						//�õ�SCR�Ĵ�������
 	if(errorstatus != SD_OK)
	{
		printf("FindSCR(RCA, scr) failure\r\n");
		return errorstatus;
	}
	else
	{
		printf("FindSCR(RCA, scr) successful\r\n");
	}
	
	if((scr[1] & SD_WIDE_BUS_SUPPORT) != SD_ALLZERO)		//֧�ֿ�����
	{
		printf("(scr[1] & SD_WIDE_BUS_SUPPORT) != SD_ALLZERO\r\n");
	 	SDIO_Send_Cmd(SD_CMD_APP_CMD, 1, (u32)RCA << 16);	//����CMD55+RCA,����Ӧ											  
	 	errorstatus = CmdResp1Error(SD_CMD_APP_CMD);
	 	if(errorstatus != SD_OK)
		{
			printf("CmdResp1Error(SD_CMD_APP_CMD) failure\r\n");
			return errorstatus;
		}
		
	 	SDIO_Send_Cmd(SD_CMD_APP_SD_SET_BUSWIDTH, 1, arg);	//����ACMD6,����Ӧ,����:10,4λ;00,1λ.											  
		errorstatus = CmdResp1Error(SD_CMD_APP_SD_SET_BUSWIDTH);
		printf("CmdResp1Error(SD_CMD_APP_SD_SET_BUSWIDTH) return %d\r\n", errorstatus);
		return errorstatus;
	}
	else
	{
		printf("return SD_REQUEST_NOT_APPLICABLE\r\n");
		return SD_REQUEST_NOT_APPLICABLE;				//��֧�ֿ���������
	}		
}												   

//����SDIO���߿���(MMC����֧��4bitģʽ)
//wmode:λ��ģʽ.0,1λ���ݿ���;1,4λ���ݿ���;2,8λ���ݿ���
//����ֵ:SD������״̬
SD_Error SD_EnableWideBusOperation(u32 wmode)
{
  	SD_Error errorstatus = SD_OK;
	
	printf("Get in SSD_EnableWideBusOperation(%d)\r\n", wmode);
	
 	if(SDIO_MULTIMEDIA_CARD == CardType)
	{
		printf("return SD_UNSUPPORTED_FEATURE\r\n");
		return SD_UNSUPPORTED_FEATURE;				//MMC����֧��
	}
 	else if((SDIO_STD_CAPACITY_SD_CARD_V1_1 == CardType) || (SDIO_STD_CAPACITY_SD_CARD_V2_0 == CardType) || (SDIO_HIGH_CAPACITY_SD_CARD == CardType))
	{
		if(wmode >= 2)
		{
			return SD_UNSUPPORTED_FEATURE;			//��֧��8λģʽ
		}
 		else   
		{
			errorstatus = SDEnWideBus(wmode);
 			if(SD_OK == errorstatus)
			{
				SDIO->CLKCR &= ~(3 << 11);			//���֮ǰ��λ������    
				SDIO->CLKCR |= (u16)wmode << 11;	//1λ/4λ���߿��� 
				SDIO->CLKCR |= 0 << 14;				//������Ӳ��������
			}
			else
			{
				printf("SDEnWideBus(wmode) == %d\r\n", errorstatus);
			}
		}  
	}
	
	return errorstatus; 
}

//����SD������ģʽ
//Mode:
//����ֵ:����״̬
SD_Error SD_SetDeviceMode(u32 Mode)
{
	SD_Error errorstatus = SD_OK;
 	if((Mode == SD_DMA_MODE) || (Mode == SD_POLLING_MODE))
		DeviceMode = Mode;
	else
		errorstatus = SD_INVALID_PARAMETER;
	
	return errorstatus;	    
}

//ѡ��
//����CMD7,ѡ����Ե�ַ(rca)Ϊaddr�Ŀ�,ȡ��������.���Ϊ0,�򶼲�ѡ��.
//addr:����RCA��ַ
SD_Error SD_SelectDeselect(u32 addr)
{
 	SDIO_Send_Cmd(SD_CMD_SEL_DESEL_CARD, 1, addr);	//����CMD7,ѡ��,����Ӧ	 	   
   	return CmdResp1Error(SD_CMD_SEL_DESEL_CARD);	  
}

//�õ�NumberOfBytes��2Ϊ�׵�ָ��.
//NumberOfBytes:�ֽ���.
//����ֵ:��2Ϊ�׵�ָ��ֵ
u8 convert_from_bytes_to_power_of_two(u16 NumberOfBytes)
{
	u8 count = 0;
	while(NumberOfBytes != 1)
	{
		NumberOfBytes >>= 1;
		count++;
	}
	return count;
} 

//SD����ȡһ���� 
//buf:�����ݻ�����(����4�ֽڶ���!!)
//addr:��ȡ��ַ
//blksize:���С
SD_Error SD_ReadBlock(u8 *buf, u32 addr, u16 blksize)
{	  
	SD_Error errorstatus = SD_OK;
	u8 power;
   	u32 count = 0, *tempbuff = (u32*)buf;	//ת��Ϊu32ָ�� 
	u32 timeout = 0;
	
   	if(NULL == buf)
		return SD_INVALID_PARAMETER;
	
   	SDIO->DCTRL = 0x0;								//���ݿ��ƼĴ�������(��DMA)   
	if(CardType == SDIO_HIGH_CAPACITY_SD_CARD)		//��������
	{
		blksize = 512;
		addr >>= 9;
	}   
  	SDIO_Send_Data_Cfg(SD_DATATIMEOUT, 0, 0, 0);	//���DPSM״̬������
	if(SDIO->RESP1 & SD_CARD_LOCKED)
		return SD_LOCK_UNLOCK_FAILED;				//������
	
	if((blksize > 0) && (blksize <= 2048) && ((blksize & (blksize - 1)) == 0))
	{
		power = convert_from_bytes_to_power_of_two(blksize);	    	   
		SDIO_Send_Cmd(SD_CMD_SET_BLOCKLEN, 1, blksize);		//����CMD16+�������ݳ���Ϊblksize,����Ӧ 	   
		errorstatus = CmdResp1Error(SD_CMD_SET_BLOCKLEN);	//�ȴ�R1��Ӧ   
		if(errorstatus != SD_OK)
			return errorstatus;   							//��Ӧ����	 
	}
	else
	{
		return SD_INVALID_PARAMETER;	
	}
	
  	SDIO_Send_Data_Cfg(SD_DATATIMEOUT, blksize, power, 1);	//blksize,����������	  
   	SDIO_Send_Cmd(SD_CMD_READ_SINGLE_BLOCK, 1, addr);		//����CMD17+��addr��ַ����ȡ����,����Ӧ 	   
	errorstatus = CmdResp1Error(SD_CMD_READ_SINGLE_BLOCK);	//�ȴ�R1��Ӧ   
	if(errorstatus != SD_OK)
		return errorstatus;   								//��Ӧ����
	
	if(DeviceMode == SD_POLLING_MODE)						//��ѯģʽ,��ѯ����	 
	{
		while(!(SDIO->STA & ((1 << 5) | (1 << 1) | (1 << 3) | (1 << 10) | (1 << 9))))	//������/CRC/��ʱ/���(��־)/��ʼλ����
		{
			if(SDIO->STA & (1 << 15))						//����������,��ʾ���ٴ���8����
			{
				for(count = 0; count < 8; count++)			//ѭ����ȡ����
				{
					*(tempbuff + count) = SDIO->FIFO;	 
				}
				tempbuff += 8;
			}
		}
		
		if(SDIO->STA & (1 << 3))		//���ݳ�ʱ����
		{										   
	 		SDIO->ICR |= 1 << 3; 		//������־
			return SD_DATA_TIMEOUT;
	 	}
		else if(SDIO->STA & (1 << 1))	//���ݿ�CRC����
		{
	 		SDIO->ICR |= 1 << 1; 		//������־
			return SD_DATA_CRC_FAIL;		   
		}
		else if(SDIO->STA & (1 << 5)) 	//����fifo�������
		{
	 		SDIO->ICR |= 1 << 5; 		//������־
			return SD_RX_OVERRUN;		 
		}
		else if(SDIO->STA & (1 << 9)) 	//������ʼλ����
		{
	 		SDIO->ICR |= 1 << 9; 		//������־
			return SD_START_BIT_ERR;		 
		}
		
		while(SDIO->STA & (1 << 21))	//FIFO����,�����ڿ�������
		{
			*tempbuff = SDIO->FIFO;		//ѭ����ȡ����
			tempbuff++;
		}
		SDIO->ICR = 0X5FF;	 			//������б��
	}
	else if(DeviceMode == SD_DMA_MODE)
	{
 		TransferError = SD_OK;
		StopCondition = 0;				//�����,����Ҫ����ֹͣ����ָ��
		TransferEnd = 0;				//�����������λ�����жϷ�����1
		SDIO->MASK |= (1 << 1) | (1 << 3) | (1 << 8) | (1 << 5) | (1 << 9);	//������Ҫ���ж� 
	 	SDIO->DCTRL |= 1 << 3;		 	//SDIO DMAʹ�� 
 	    SD_DMA_Config((u32*)buf, blksize, 0);
		timeout = SDIO_DATATIMEOUT;
 		while(((DMA2->ISR & 0X2000) == RESET) && (TransferEnd == 0) && (TransferError == SD_OK) && timeout)
		{
			timeout--;					//�ȴ�������� 
		}
		if(timeout == 0)
			return SD_DATA_TIMEOUT;		//��ʱ
		
		if(TransferError != SD_OK)
			errorstatus = TransferError;  
    }   
 	return errorstatus; 
}

//SD����ȡ����� 
//buf:�����ݻ�����
//addr:��ȡ��ַ
//blksize:���С
//nblks:Ҫ��ȡ�Ŀ���
//����ֵ:����״̬
SD_Error SD_ReadMultiBlocks(u8 *buf, u32 addr, u16 blksize, u32 nblks)
{
  	SD_Error errorstatus = SD_OK;
	u8 power;
   	u32 count = 0, *tempbuff = (u32*)buf;			//ת��Ϊu32ָ��
	u32 timeout = 0;  
    SDIO->DCTRL = 0x0;								//���ݿ��ƼĴ�������(��DMA)
	
	if(CardType == SDIO_HIGH_CAPACITY_SD_CARD)		//��������
	{
		blksize = 512;
		addr >>= 9;
	}
	
   	SDIO_Send_Data_Cfg(SD_DATATIMEOUT, 0, 0, 0);	//���DPSM״̬������
	if(SDIO->RESP1 & SD_CARD_LOCKED)
		return SD_LOCK_UNLOCK_FAILED;				//������
	
	if((blksize > 0) && (blksize <= 2048) && ((blksize & (blksize - 1)) == 0))
	{
		power = convert_from_bytes_to_power_of_two(blksize);	    
		SDIO_Send_Cmd(SD_CMD_SET_BLOCKLEN, 1, blksize);		//����CMD16+�������ݳ���Ϊblksize,����Ӧ 	   
		errorstatus = CmdResp1Error(SD_CMD_SET_BLOCKLEN);	//�ȴ�R1��Ӧ   
		if(errorstatus != SD_OK)
			return errorstatus;   							//��Ӧ����	 
	}
	else
	{
		return SD_INVALID_PARAMETER;	  
	}
	
	if(nblks > 1)											//����  
	{									    
 	  	if(nblks * blksize > SD_MAX_DATA_LENGTH)
			return SD_INVALID_PARAMETER;					//�ж��Ƿ񳬹������ճ���
		
		SDIO_Send_Data_Cfg(SD_DATATIMEOUT, nblks * blksize, power, 1);	//nblks*blksize,512���С,����������	  
	  	SDIO_Send_Cmd(SD_CMD_READ_MULT_BLOCK, 1, addr);		//����CMD18+��addr��ַ����ȡ����,����Ӧ 	   
		errorstatus = CmdResp1Error(SD_CMD_READ_MULT_BLOCK);//�ȴ�R1��Ӧ   
		if(errorstatus != SD_OK)
			return errorstatus;   							//��Ӧ����	  
		
		if(DeviceMode == SD_POLLING_MODE)
		{
			while(!(SDIO->STA & ((1 << 5) | (1 << 1) | (1 << 3) | (1 << 8) | (1 << 9))))//������/CRC/��ʱ/���(��־)/��ʼλ����
			{
				if(SDIO->STA & (1 << 15))					//����������,��ʾ���ٴ���8����
				{
					for(count = 0; count < 8; count++)		//ѭ����ȡ����
					{
						*(tempbuff + count) = SDIO->FIFO;	 
					}
					tempbuff += 8;
				}
			}
			
			if(SDIO->STA & (1 << 3))		//���ݳ�ʱ����
			{										   
		 		SDIO->ICR |= 1 << 3; 		//������־
				return SD_DATA_TIMEOUT;
		 	}
			else if(SDIO->STA & (1 << 1))	//���ݿ�CRC����
			{
		 		SDIO->ICR |= 1 << 1; 		//������־
				return SD_DATA_CRC_FAIL;		   
			}
			else if(SDIO->STA & (1 << 5)) 	//����fifo�������
			{
		 		SDIO->ICR |= 1 << 5; 		//������־
				return SD_RX_OVERRUN;		 
			}
			else if(SDIO->STA & (1 << 9)) 	//������ʼλ����
			{
		 		SDIO->ICR |= 1 << 9; 		//������־
				return SD_START_BIT_ERR;		 
			}
			
			while(SDIO->STA & (1 << 21))	//FIFO����,�����ڿ�������
			{
				*tempbuff = SDIO->FIFO;		//ѭ����ȡ����
				tempbuff++;
			}
			
	 		if(SDIO->STA & (1 << 8))		//���ս���
			{
				if((SDIO_STD_CAPACITY_SD_CARD_V1_1 == CardType) || (SDIO_STD_CAPACITY_SD_CARD_V2_0 == CardType) || (SDIO_HIGH_CAPACITY_SD_CARD == CardType))
				{
					SDIO_Send_Cmd(SD_CMD_STOP_TRANSMISSION, 1, 0);			//����CMD12+�������� 	   
					errorstatus = CmdResp1Error(SD_CMD_STOP_TRANSMISSION);	//�ȴ�R1��Ӧ   
					if(errorstatus != SD_OK)
						return errorstatus;	 
				}
 			}
	 		SDIO->ICR = 0X5FF;	 											//������б�� 
 		}
		else if(DeviceMode == SD_DMA_MODE)
		{
	   		TransferError = SD_OK;
			StopCondition = 1;									//����,��Ҫ����ֹͣ����ָ�� 
			TransferEnd = 0;									//�����������λ�����жϷ�����1
			SDIO->MASK |= (1 << 1) | (1 << 3) | (1 << 8) | (1 << 5) | (1 << 9);	//������Ҫ���ж� 
		 	SDIO->DCTRL |= 1 << 3;		 						//SDIO DMAʹ�� 
	 	    SD_DMA_Config((u32*)buf, nblks * blksize, 0);
			timeout = SDIO_DATATIMEOUT;
	 		while(((DMA2->ISR & 0X2000) == RESET) && timeout)
			{
				timeout--;										//�ȴ�������� 
			}
			
			if(timeout == 0)
				return SD_DATA_TIMEOUT;							//��ʱ
			
			while((TransferEnd == 0) && (TransferError == SD_OK));
			
			if(TransferError != SD_OK)
				errorstatus = TransferError;  	 
		}		 
  	}
	return errorstatus;
}

//��鿨�Ƿ�����ִ��д����
//pstatus:��ǰ״̬.
//����ֵ:�������
SD_Error IsCardProgramming(u8 *pstatus)
{
 	vu32 respR1 = 0, status = 0; 
	
  	SDIO_Send_Cmd(SD_CMD_SEND_STATUS, 1, (u32)RCA << 16);		//����CMD13 	   
  	status = SDIO->STA;
	while(!(status & ((1 << 0) | (1 << 6) | (1 << 2))))
		status = SDIO->STA;										//�ȴ��������
	
   	if(status & (1 << 0))			//CRC���ʧ��
	{
		SDIO->ICR |= 1 << 0;		//���������
		return SD_CMD_CRC_FAIL;
	}
	
   	if(status & (1 << 2))			//���ʱ 
	{
		SDIO->ICR |= 1 << 2;		//���������
		return SD_CMD_RSP_TIMEOUT;
	}
	
 	if(SDIO->RESPCMD != SD_CMD_SEND_STATUS)
		return SD_ILLEGAL_CMD;
	
	SDIO->ICR = 0X5FF;	 			//������б��
	
	respR1 = SDIO->RESP1;
	*pstatus = (u8)((respR1 >> 9) & 0x0000000F);
	
	return SD_OK;
}

//SD��д1���� 
//buf:���ݻ�����
//addr:д��ַ
//blksize:���С	  
//����ֵ:����״̬
SD_Error SD_WriteBlock(u8 *buf, u32 addr, u16 blksize)
{
	SD_Error errorstatus = SD_OK;
	u8 power = 0, cardstate = 0;
	u32 timeout = 0, bytestransferred = 0;
	u32 cardstatus = 0, count = 0, restwords = 0;
	u32	tlen = blksize;							//�ܳ���(�ֽ�)
	u32 *tempbuff = (u32*)buf;								 
	
 	if(buf == NULL)
		return SD_INVALID_PARAMETER;			//��������   
	
  	SDIO->DCTRL = 0x0;							//���ݿ��ƼĴ�������(��DMA)   
  	SDIO_Send_Data_Cfg(SD_DATATIMEOUT, 0, 0, 0);//���DPSM״̬������
	if(SDIO->RESP1 & SD_CARD_LOCKED)
		return SD_LOCK_UNLOCK_FAILED;			//������
	
 	if(CardType == SDIO_HIGH_CAPACITY_SD_CARD)	//��������
	{
		blksize = 512;
		addr >>= 9;
	}
	
	if((blksize > 0) && (blksize <= 2048) && ((blksize & (blksize - 1)) == 0))
	{
		power = convert_from_bytes_to_power_of_two(blksize);	    
		SDIO_Send_Cmd(SD_CMD_SET_BLOCKLEN, 1, blksize);		//����CMD16+�������ݳ���Ϊblksize,����Ӧ 	   
		errorstatus = CmdResp1Error(SD_CMD_SET_BLOCKLEN);	//�ȴ�R1��Ӧ   
		if(errorstatus != SD_OK)
			return errorstatus;   							//��Ӧ����	 
	}
	else
	{
		return SD_INVALID_PARAMETER;	 
	}
	
   	SDIO_Send_Cmd(SD_CMD_SEND_STATUS, 1, (u32)RCA << 16);	//����CMD13,��ѯ����״̬,����Ӧ 	   
	errorstatus = CmdResp1Error(SD_CMD_SEND_STATUS);		//�ȴ�R1��Ӧ   		   
	if(errorstatus != SD_OK)
		return errorstatus;
	
	cardstatus = SDIO->RESP1;													  
	timeout = SD_DATATIMEOUT;
   	while(((cardstatus & 0x00000100) == 0) && (timeout > 0))//���READY_FOR_DATAλ�Ƿ���λ
	{
		timeout--;
	   	SDIO_Send_Cmd(SD_CMD_SEND_STATUS, 1, (u32)RCA << 16);//����CMD13,��ѯ����״̬,����Ӧ 	   
		errorstatus = CmdResp1Error(SD_CMD_SEND_STATUS);	//�ȴ�R1��Ӧ   		   
		if(errorstatus != SD_OK)
			return errorstatus;				    
		
		cardstatus = SDIO->RESP1;													  
	}
	if(timeout == 0)
		return SD_ERROR;
	
   	SDIO_Send_Cmd(SD_CMD_WRITE_SINGLE_BLOCK, 1, addr);		//����CMD24,д����ָ��,����Ӧ 	   
	errorstatus = CmdResp1Error(SD_CMD_WRITE_SINGLE_BLOCK);	//�ȴ�R1��Ӧ   		   
	if(errorstatus != SD_OK)
		return errorstatus;   	  
	
	StopCondition = 0;										//����д,����Ҫ����ֹͣ����ָ�� 
 	SDIO_Send_Data_Cfg(SD_DATATIMEOUT, blksize, power, 0);	//blksize, ����������	  
	if (DeviceMode == SD_POLLING_MODE)
	{
		while(!(SDIO->STA & ((1 << 10) | (1 << 4) | (1 << 1) | (1 << 3) | (1 << 9))))//���ݿ鷢�ͳɹ�/����/CRC/��ʱ/��ʼλ����
		{
			if(SDIO->STA & (1 << 14))							//���������,��ʾ���ٴ���8����
			{
				if((tlen - bytestransferred) < SD_HALFFIFOBYTES)//����32�ֽ���
				{
					restwords = ((tlen - bytestransferred) % 4 == 0) ? ((tlen - bytestransferred) / 4) : ((tlen - bytestransferred) / 4 + 1);
					
					for(count = 0; count < restwords; count++, tempbuff++, bytestransferred += 4)
					{
						SDIO->FIFO = *tempbuff;
					}
				}
				else
				{
					for(count = 0; count < 8; count++)
					{
						SDIO->FIFO = *(tempbuff + count);
					}
					tempbuff += 8;
					bytestransferred += 32;
				}
			}
		}
		
		if(SDIO->STA & (1 << 3))		//���ݳ�ʱ����
		{										   
	 		SDIO->ICR |= 1 << 3; 		//������־
			return SD_DATA_TIMEOUT;
	 	}
		else if(SDIO->STA & (1 << 1))	//���ݿ�CRC����
		{
	 		SDIO->ICR |= 1 << 1; 		//������־
			return SD_DATA_CRC_FAIL;		   
		}
		else if(SDIO->STA & (1 << 4)) 	//����fifo�������
		{
	 		SDIO->ICR |= 1 << 4; 		//������־
			return SD_TX_UNDERRUN;		 
		}
		else if(SDIO->STA & (1 << 9)) 	//������ʼλ����
		{
	 		SDIO->ICR |= 1 << 9; 		//������־
			return SD_START_BIT_ERR;		 
		}   
		SDIO->ICR = 0X5FF;	 			//������б��	  
	}
	else if(DeviceMode == SD_DMA_MODE)
	{
   		TransferError = SD_OK;
		StopCondition = 0;				//����д,����Ҫ����ֹͣ����ָ�� 
		TransferEnd = 0;				//�����������λ�����жϷ�����1
		SDIO->MASK |= (1 << 1) | (1 << 3) | (1 << 8) | (1 << 4) | (1 << 9);	//���ò������ݽ�������ж�
		SD_DMA_Config((u32*)buf, blksize, 1);	//SDIO DMA����
 	 	SDIO->DCTRL |= 1 << 3;					//SDIO DMAʹ��. 
		timeout = SDIO_DATATIMEOUT;
 		while(((DMA2->ISR & 0X2000) == RESET) && timeout)
		{
			timeout--;							//�ȴ�������� 
		}
		if(timeout==0)
		{
  			SD_Init();	 						//���³�ʼ��SD��,���Խ��д������������
			return SD_DATA_TIMEOUT;				//��ʱ	 
 		}
		
		timeout = SDIO_DATATIMEOUT;
		while((TransferEnd == 0) && (TransferError == SD_OK) && timeout)
		{
			timeout--;
		}
 		if(timeout == 0)
			return SD_DATA_TIMEOUT;				//��ʱ
		
  		if(TransferError != SD_OK)
			return TransferError;
 	}
	
 	SDIO->ICR = 0X5FF;	 						//������б��
 	errorstatus = IsCardProgramming(&cardstate);
 	while((errorstatus == SD_OK) && ((cardstate == SD_CARD_PROGRAMMING) || (cardstate == SD_CARD_RECEIVING)))
	{
		errorstatus = IsCardProgramming(&cardstate);
	}
	
	return errorstatus;
}
//SD��д����� 
//buf:���ݻ�����
//addr:д��ַ
//blksize:���С
//nblks:Ҫд��Ŀ���
//����ֵ:����״̬												   
SD_Error SD_WriteMultiBlocks(u8 *buf,u32 addr,u16 blksize,u32 nblks)
{
	SD_Error errorstatus = SD_OK;
	u8 power = 0, cardstate = 0;
	u32 timeout = 0, bytestransferred = 0;
	u32 count = 0, restwords = 0;
	u32 tlen = nblks * blksize;				//�ܳ���(�ֽ�)
	u32 *tempbuff = (u32*)buf;  
  	if(buf == NULL)
		return SD_INVALID_PARAMETER;		//��������  
	
  	SDIO->DCTRL = 0x0;								//���ݿ��ƼĴ�������(��DMA)   
  	SDIO_Send_Data_Cfg(SD_DATATIMEOUT, 0, 0, 0);	//���DPSM״̬������
	if(SDIO->RESP1 & SD_CARD_LOCKED)
		return SD_LOCK_UNLOCK_FAILED;				//������
	
 	if(CardType == SDIO_HIGH_CAPACITY_SD_CARD)		//��������
	{
		blksize = 512;
		addr >>= 9;
	}
	
	if((blksize > 0) && (blksize <= 2048) && ((blksize & (blksize - 1)) == 0))
	{
		power = convert_from_bytes_to_power_of_two(blksize);	    
		SDIO_Send_Cmd(SD_CMD_SET_BLOCKLEN, 1, blksize);		//����CMD16+�������ݳ���Ϊblksize,����Ӧ 	   
		errorstatus = CmdResp1Error(SD_CMD_SET_BLOCKLEN);	//�ȴ�R1��Ӧ   
		if(errorstatus != SD_OK)
			return errorstatus;   							//��Ӧ����	 
		
	}
	else
	{
		return SD_INVALID_PARAMETER;	 
	}
	
	if(nblks > 1)
	{					  
		if(nblks * blksize > SD_MAX_DATA_LENGTH)
			return SD_INVALID_PARAMETER;   
		
     	if((SDIO_STD_CAPACITY_SD_CARD_V1_1 == CardType) || (SDIO_STD_CAPACITY_SD_CARD_V2_0 == CardType) || (SDIO_HIGH_CAPACITY_SD_CARD == CardType))
    	{
			//�������
	 	   	SDIO_Send_Cmd(SD_CMD_APP_CMD, 1, (u32)RCA << 16);	//����ACMD55,����Ӧ 	   
			errorstatus = CmdResp1Error(SD_CMD_APP_CMD);		//�ȴ�R1��Ӧ   		   
			if(errorstatus != SD_OK)
				return errorstatus;				    
			
	 	   	SDIO_Send_Cmd(SD_CMD_SET_BLOCK_COUNT, 1, nblks);	//����CMD23,���ÿ�����,����Ӧ 	   
			errorstatus = CmdResp1Error(SD_CMD_SET_BLOCK_COUNT);//�ȴ�R1��Ӧ   		   
			if(errorstatus != SD_OK)
				return errorstatus;				    
		}
		
		SDIO_Send_Cmd(SD_CMD_WRITE_MULT_BLOCK, 1, addr);		//����CMD25,���дָ��,����Ӧ 	   
		errorstatus = CmdResp1Error(SD_CMD_WRITE_MULT_BLOCK);	//�ȴ�R1��Ӧ   		   
		if(errorstatus != SD_OK)
			return errorstatus;
		
 	 	SDIO_Send_Data_Cfg(SD_DATATIMEOUT, nblks * blksize, power, 0);	//blksize, ����������	
	    if(DeviceMode == SD_POLLING_MODE)
	    {
			while(!(SDIO->STA & ((1 << 4) | (1 << 1) | (1 << 8) | (1 << 3) | (1 << 9))))//����/CRC/���ݽ���/��ʱ/��ʼλ����
			{
				if(SDIO->STA & (1 << 14))							//���������,��ʾ���ٴ���8��(32�ֽ�)
				{	  
					if((tlen - bytestransferred) < SD_HALFFIFOBYTES)//����32�ֽ���
					{
						restwords = ((tlen - bytestransferred) % 4 == 0) ? ((tlen - bytestransferred) / 4) : ((tlen - bytestransferred) / 4 + 1);
						for(count = 0; count < restwords; count++, tempbuff++, bytestransferred += 4)
						{
							SDIO->FIFO = *tempbuff;
						}
					}
					else 										//���������,���Է�������8��(32�ֽ�)����
					{
						for(count = 0; count < SD_HALFFIFO; count++)
						{
							SDIO->FIFO = *(tempbuff + count);
						}
						
						tempbuff += SD_HALFFIFO;
						bytestransferred += SD_HALFFIFOBYTES;
					} 
				}
			}
			
			if(SDIO->STA & (1 << 3))		//���ݳ�ʱ����
			{										   
		 		SDIO->ICR |= 1 << 3; 		//������־
				return SD_DATA_TIMEOUT;
		 	}
			else if(SDIO->STA & (1 << 1))	//���ݿ�CRC����
			{
		 		SDIO->ICR |= 1 << 1; 		//������־
				return SD_DATA_CRC_FAIL;		   
			}
			else if(SDIO->STA & (1 << 4)) 	//����fifo�������
			{
		 		SDIO->ICR |= 1 << 4; 		//������־
				return SD_TX_UNDERRUN;		 
			}
			else if(SDIO->STA & (1 << 9)) 	//������ʼλ����
			{
		 		SDIO->ICR |= 1 << 9; 		//������־
				return SD_START_BIT_ERR;		 
			}
			
			if(SDIO->STA & (1 << 8))		//���ͽ���
			{															 
				if((SDIO_STD_CAPACITY_SD_CARD_V1_1 == CardType) || (SDIO_STD_CAPACITY_SD_CARD_V2_0 == CardType) || (SDIO_HIGH_CAPACITY_SD_CARD == CardType))
				{
					SDIO_Send_Cmd(SD_CMD_STOP_TRANSMISSION, 1, 0);			//����CMD12+�������� 	   
					errorstatus = CmdResp1Error(SD_CMD_STOP_TRANSMISSION);	//�ȴ�R1��Ӧ   
					if(errorstatus != SD_OK)
						return errorstatus;	 
				}
			}
	 		SDIO->ICR = 0X5FF;	 			//������б�� 
	    }
		else if(DeviceMode == SD_DMA_MODE)
		{
	   		TransferError = SD_OK;
			StopCondition = 1;				//���д,��Ҫ����ֹͣ����ָ�� 
			TransferEnd = 0;				//�����������λ�����жϷ�����1
			SDIO->MASK |= (1 << 1) | (1 << 3) | (1 << 8) | (1 << 4) | (1 << 9);	//���ò������ݽ�������ж�
			SD_DMA_Config((u32*)buf, nblks * blksize, 1);		//SDIO DMA����
	 	 	SDIO->DCTRL |= 1 << 3;								//SDIO DMAʹ��. 
			timeout = SDIO_DATATIMEOUT;
	 		while(((DMA2->ISR & 0X2000) == RESET) && timeout)
			{
				timeout--;					//�ȴ�������� 
			}
			if(timeout==0)	 				//��ʱ
			{									  
  				SD_Init();	 				//���³�ʼ��SD��,���Խ��д������������
	 			return SD_DATA_TIMEOUT;		//��ʱ	 
	 		}
			
			timeout = SDIO_DATATIMEOUT;
			while((TransferEnd == 0) && (TransferError == SD_OK) && timeout)
			{
				timeout--;
			}
	 		if(timeout == 0)
				return SD_DATA_TIMEOUT;		//��ʱ	 
			
	 		if(TransferError != SD_OK)
				return TransferError;	 
		}
  	}
 	SDIO->ICR = 0X5FF;	 		//������б��
 	errorstatus = IsCardProgramming(&cardstate);
 	while((errorstatus == SD_OK) && ((cardstate == SD_CARD_PROGRAMMING) || (cardstate == SD_CARD_RECEIVING)))
	{
		errorstatus = IsCardProgramming(&cardstate);
	}
	return errorstatus;	   
}
 																    
//SDIO�жϴ�������
//����SDIO��������еĸ����ж�����
//����ֵ:�������
SD_Error SD_ProcessIRQSrc(void)
{
	if(SDIO->STA & (1 << 8))	//��������ж�
	{	 
		if(StopCondition == 1)
		{
			SDIO_Send_Cmd(SD_CMD_STOP_TRANSMISSION, 1, 0);			//����CMD12,�������� 	   
			TransferError = CmdResp1Error(SD_CMD_STOP_TRANSMISSION);
		}
		else
		{
			TransferError = SD_OK;	
		}
		
 		SDIO->ICR |= 1 << 8;										//�������жϱ��
		SDIO->MASK &= ~((1 << 1) | (1 << 3) | (1 << 8) | (1 << 14) | (1 << 15) | (1 << 4) | (1 << 5) | (1 << 9));//�ر�����ж�
 		TransferEnd = 1;
		return(TransferError);
	}
	
 	if(SDIO->STA & (1 << 1))	//����CRC����
	{
		SDIO->ICR |= 1 << 1;	//����жϱ��
		SDIO->MASK &= ~((1 << 1) | (1 << 3) | (1 << 8) | (1 << 14) | (1 << 15) | (1 << 4) | (1 << 5) | (1 << 9));//�ر�����ж�
	    TransferError = SD_DATA_CRC_FAIL;
	    return(SD_DATA_CRC_FAIL);
	}
 	if(SDIO->STA & (1 << 3))	//���ݳ�ʱ����
	{
		SDIO->ICR |= 1 << 3;	//����жϱ��
		SDIO->MASK &= ~((1 << 1) | (1 << 3) | (1 << 8) | (1 << 14) | (1 << 15) | (1 << 4) | (1 << 5) | (1 << 9));//�ر�����ж�
	    TransferError = SD_DATA_TIMEOUT;
	    return(SD_DATA_TIMEOUT);
	}
  	if(SDIO->STA & (1 << 5))	//FIFO�������
	{
		SDIO->ICR |= 1 << 5;	//����жϱ��
		SDIO->MASK &= ~((1 << 1) | (1 << 3) | (1 << 8) | (1 << 14) | (1 << 15) | (1 << 4) | (1 << 5) | (1 << 9));//�ر�����ж�
	    TransferError = SD_RX_OVERRUN;
	    return(SD_RX_OVERRUN);
	}
   	if(SDIO->STA & (1 << 4))	//FIFO�������
	{
		SDIO->ICR |= 1 << 4;	//����жϱ��
		SDIO->MASK &= ~((1 << 1) | (1 << 3) | (1 << 8) | (1 << 14) | (1 << 15) | (1 << 4) | (1 << 5) | (1 << 9));//�ر�����ж�
	    TransferError = SD_TX_UNDERRUN;
	    return(SD_TX_UNDERRUN);
	}
	if(SDIO->STA & (1 << 9))	//��ʼλ����
	{
		SDIO->ICR |= 1 << 9;	//����жϱ��
		SDIO->MASK &= ~((1 << 1) | (1 << 3) | (1 << 8) | (1 << 14) | (1 << 15) | (1 << 4) | (1 << 5) | (1 << 9));//�ر�����ж�
	    TransferError = SD_START_BIT_ERR;
	    return(SD_START_BIT_ERR);
	}
	return(SD_OK);
}

//SDIO�жϷ�����		  
void SDIO_IRQHandler(void) 
{											
 	SD_ProcessIRQSrc();			//��������SDIO����ж�
}

//����SD����SCR�Ĵ���ֵ
//rca:����Ե�ַ
//pscr:���ݻ�����(�洢SCR����)
//����ֵ:����״̬		   
SD_Error FindSCR(u16 rca, u32 *pscr)
{ 
	u32 index = 0;
	SD_Error errorstatus = SD_OK;
	u32 tempscr[2] = {0, 0};
	
 	SDIO_Send_Cmd(SD_CMD_SET_BLOCKLEN, 1, 8);				//����CMD16,����Ӧ,����Block SizeΪ8�ֽ�											  
 	errorstatus = CmdResp1Error(SD_CMD_SET_BLOCKLEN);
 	if(errorstatus != SD_OK)
		return errorstatus;
	else
		printf("SD_CMD_SET_BLOCKLEN successful\r\n");
	
	delay_ms(5);
	
  	SDIO_Send_Cmd(SD_CMD_APP_CMD, 1, (u32)rca << 16);		//����CMD55,����Ӧ 									  
 	errorstatus = CmdResp1Error(SD_CMD_APP_CMD);
 	if(errorstatus != SD_OK)
		return errorstatus;
	else
		printf("SD_CMD_APP_CMD successful\r\n");
	
	SDIO_Send_Data_Cfg(SD_DATATIMEOUT, 8, 3, 1);			//8���ֽڳ���,blockΪ8�ֽ�,SD����SDIO.
   	SDIO_Send_Cmd(SD_CMD_SD_APP_SEND_SCR, 1, 0);			//����ACMD51,����Ӧ,����Ϊ0											  
 	errorstatus = CmdResp1Error(SD_CMD_SD_APP_SEND_SCR);
 	if(errorstatus != SD_OK)
		return errorstatus;
	else
		printf("SD_CMD_SD_APP_SEND_SCR successful\r\n");
	
 	while(!(SDIO->STA & (SDIO_FLAG_RXOVERR | SDIO_FLAG_DCRCFAIL | SDIO_FLAG_DTIMEOUT | SDIO_FLAG_DBCKEND | SDIO_FLAG_STBITERR)))
	{
		if(SDIO->STA & (1 << 21))							//����FIFO���ݿ���
		{
			*(tempscr + index) = SDIO->FIFO;				//��ȡFIFO����
			index++;
			if(index >= 2)
				break;
		}
	}
	
 	if(SDIO->STA & (1 << 3))								//�������ݳ�ʱ
	{										 
 		SDIO->ICR |= 1 << 3;								//������
		return SD_DATA_TIMEOUT;
	}
	else if(SDIO->STA & (1 << 1))							//�ѷ���/���յ����ݿ�CRCУ�����
	{
 		SDIO->ICR |= 1 << 1;								//������
		return SD_DATA_CRC_FAIL;   
	}
	else if(SDIO->STA & (1 << 5))							//����FIFO���
	{
 		SDIO->ICR |= 1 << 5;								//������
		return SD_RX_OVERRUN;   	   
	}
	else if(SDIO->STA & (1 << 9))							//��ʼλ������
	{
 		SDIO->ICR |= 1 << 9;								//������
		return SD_START_BIT_ERR;    
	}
   	SDIO->ICR = 0X5FF;								 		//������	 
	
	//������˳��8λΪ��λ������.   	
	*(pscr + 1) = ((tempscr[0] & SD_0TO7BITS) << 24) | ((tempscr[0] & SD_8TO15BITS) << 8) | ((tempscr[0] & SD_16TO23BITS) >> 8) | ((tempscr[0] & SD_24TO31BITS) >> 24);
	*(pscr) = ((tempscr[1] & SD_0TO7BITS) << 24) | ((tempscr[1] & SD_8TO15BITS) << 8) | ((tempscr[1] & SD_16TO23BITS) >> 8) | ((tempscr[1] & SD_24TO31BITS) >> 24);
 	return errorstatus;
}

//��SD��
//buf:�����ݻ�����
//sector:������ַ
//cnt:��������	
//����ֵ:����״̬;0,����;����,�������;				  				 
u8 SD_ReadDisk(u8*buf, u32 sector, u8 cnt)
{
	u8 sta = SD_OK;
	u8 n;
	
	if(CardType != SDIO_STD_CAPACITY_SD_CARD_V1_1)
		sector <<= 9;
	
	if((u32)buf % 4 != 0)
	{
	 	for(n = 0; n < cnt; n++)
		{
		 	sta = SD_ReadBlock(SDIO_DATA_BUFFER, sector + 512 * n, 512);		//����sector�Ķ�����
			memcpy(buf, SDIO_DATA_BUFFER, 512);
			buf += 512;
		} 
	}
	else
	{
		if(cnt == 1)
			sta = SD_ReadBlock(buf, sector, 512);  							  	//����sector�Ķ�����
		else
			sta = SD_ReadMultiBlocks(buf, sector, 512, cnt);					//���sector  
	}
	return sta;
}

//дSD��
//buf:д���ݻ�����
//sector:������ַ
//cnt:��������	
//����ֵ:����״̬;0,����;����,�������;	
u8 SD_WriteDisk(u8*buf, u32 sector, u8 cnt)
{
	u8 sta = SD_OK;
	u8 n;
	
	if(CardType != SDIO_STD_CAPACITY_SD_CARD_V1_1)
		sector <<= 9;
	
	if((u32)buf % 4 != 0)
	{
	 	for(n = 0; n < cnt; n++)
		{
			memcpy(SDIO_DATA_BUFFER, buf, 512);
		 	sta = SD_WriteBlock(SDIO_DATA_BUFFER, sector + 512 * n, 512);		//����sector��д����
			buf += 512;
		} 
	}
	else
	{
		if(cnt == 1)
			sta = SD_WriteBlock(buf, sector, 512);						    	//����sector��д����
		else
			sta = SD_WriteMultiBlocks(buf, sector, 512, cnt);					//���sector  
	}
	return sta;
}

SD_Error SD_Init(void)
{
	SD_Error errorstatus = SD_OK;

	SDIO_DeInit();
	Sdio_GPIO_Init();
	
	errorstatus = SD_PowerON();
	if(errorstatus == SD_OK)
	{
		printf("SD_PowerON() == SD_OK\r\n");
		errorstatus = SD_InitializeCards();					//��ʼ��SD��
	}
	
  	if(errorstatus == SD_OK)
	{
		printf("SD_InitializeCards() == SD_OK\r\n");
		errorstatus = SD_GetCardInfo(&SDCardInfo);			//��ȡ����Ϣ
	}
	
 	if(errorstatus == SD_OK)
	{
		printf("SD_GetCardInfo() == SD_OK\r\n");
		errorstatus = SD_SelectDeselect((u32)(SDCardInfo.RCA << 16));//ѡ��SD��  
	}
	
   	if(errorstatus == SD_OK)
	{
		printf("SD_SelectDeselect() == SD_OK\r\n");
		errorstatus = SD_EnableWideBusOperation(1);			//4λ����,�����MMC��,������4λģʽ 
	}
	
  	if((errorstatus == SD_OK) || (SDIO_MULTIMEDIA_CARD == CardType))
	{  		    
		SDIO_Clock_Set(SDIO_TRANSFER_CLK_DIV + 5);				//����ʱ��Ƶ��,SDIOʱ�Ӽ��㹫ʽ:SDIO_CKʱ��=SDIOCLK/[clkdiv+2];����,SDIOCLKһ��Ϊ72Mhz 
		errorstatus = SD_SetDeviceMode(SD_DMA_MODE);		//����ΪDMAģʽ
		//errorstatus = SD_SetDeviceMode(SD_POLLING_MODE);	//����Ϊ��ѯģʽ
 	}
	
	printf("SD_SetDeviceMode() == %d\r\n", errorstatus);
	return errorstatus;
}