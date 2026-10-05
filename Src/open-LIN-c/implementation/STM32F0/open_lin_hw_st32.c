/*
 * open_lin_frame_st32.c
 *
 *  Created on: 25.01.2018
 *      Author: ay7vi2
 */

#include "../../open_lin_data_layer.h"
#include "../../open_lin_types.h"
#include "../../open_lin_network_layer.h"
#include "../../open_lin_hw.h"
#include "slcan.h"
#include <stm32f0xx_hal.h>
#include <stm32f0xx_hal_uart_ex.h>
#include <stm32f0xx_hal_uart.h>
#include <stm32f0xx.h>
extern UART_HandleTypeDef huart1;

void open_lin_error_handler(t_open_lin_error error_code)
{
	/* no error handling */
}

l_bool open_lin_hw_check_for_break(void)
{
	l_bool result = __HAL_UART_GET_FLAG(&huart1,UART_FLAG_LBDF);
	if (result == true)
	{
		__HAL_UART_CLEAR_FLAG(&huart1,UART_CLEAR_LBDF);
	}

	return result;
}

void open_lin_frame_set_auto_baud(void)
{
	//	UART_AUTOBAUD_REQUEST
}
extern uint8_t Uart2RxFifo;
void open_lin_hw_reset(void) {
	open_lin_hw_check_for_break();
	HAL_LIN_Init(&huart1, UART_LINBREAKDETECTLENGTH_11B);
	HAL_UART_Receive_IT(&huart1, &Uart2RxFifo, 1);
    NVIC_EnableIRQ(USART1_IRQn);

}


/**
  * @brief  Empfang scharf machen bzw. stilllegen.
  *
  *         Vorher lief hier im true-Zweig ein komplettes HAL_LIN_Init
  *         (USART aus, umkonfigurieren, an, Warten auf TEACK/REACK). Das dauert
  *         ~100-200 us und wurde nach dem Header aufgerufen - also genau dann,
  *         wenn der Slave zu antworten anfaengt. Die ersten Antwortbytes gingen
  *         dabei verloren und das Frame lief in den Timeout.
  *
  *         Der false-Zweig legt jetzt auch die Fehlerinterrupts still und
  *         verwirft das Echo der eigenen Sendebytes, damit kein ORE auflaeuft,
  *         das die HAL beim naechsten Byte als "blocking error" behandelt.
  */
void open_lin_set_rx_enabled(l_bool v)
{
	if (v == l_true)
	{
		/* Altlasten wegwerfen: Break-Flag, Fehlerflags und ein evtl.
		 * stehengebliebenes Byte. Das Break-Flag muss mit weg, sonst meldet
		 * open_lin_hw_check_for_break() beim ersten Antwortbyte einen Break und
		 * die Monitor-/Slave-Statemachine verwirft das Frame. */
		__HAL_UART_CLEAR_FLAG(&huart1, UART_CLEAR_LBDF);
		__HAL_UART_CLEAR_FLAG(&huart1,
				UART_CLEAR_OREF | UART_CLEAR_FEF | UART_CLEAR_NEF | UART_CLEAR_PEF);
		(void) huart1.Instance->RDR;
		huart1.ErrorCode = HAL_UART_ERROR_NONE;

		if (huart1.RxState != HAL_UART_STATE_BUSY_RX)
		{
			/* HAL-Empfang war abgebrochen (z.B. durch ORE) -> neu armieren */
			huart1.RxState = HAL_UART_STATE_READY;
			(void) HAL_UART_Receive_IT(&huart1, &Uart2RxFifo, 1);
		} else
		{
			ATOMIC_SET_BIT(huart1.Instance->CR3, USART_CR3_EIE);
			ATOMIC_SET_BIT(huart1.Instance->CR1, USART_CR1_RXNEIE);
		}
	} else
	{
		ATOMIC_CLEAR_BIT(huart1.Instance->CR1, (USART_CR1_RXNEIE | USART_CR1_PEIE));
		ATOMIC_CLEAR_BIT(huart1.Instance->CR3, USART_CR3_EIE);
		__HAL_UART_CLEAR_FLAG(&huart1, UART_CLEAR_OREF | UART_CLEAR_FEF);
		(void) huart1.Instance->RDR;
	}
}

l_bool open_lin_hw_tx_byte(l_u8 byte){

	if (HAL_UART_Transmit(&huart1,&byte,1,1000) == HAL_OK)
		return true;
	return false;
}

l_bool open_lin_hw_tx_break(void){

	if (HAL_LIN_SendBreak(&huart1) == HAL_OK)
		return true;
	return false;
}

l_bool open_lin_hw_tx_data(l_u8* data, l_u8 len)
{
	if (HAL_UART_Transmit(&huart1,data,len,1000) == HAL_OK)
		return true;
	else
		return false;
//	add buffers to use
//	HAL_UART_Transmit_IT(huart1,data,len);

}

//void open_lin_on_rx_frame(open_lin_frame_slot_t *slot)
//{
//
//}

void open_lin_master_dl_rx_callback(open_lin_frame_slot_t* slot)
{
	slcanReciveCanFrame(slot);
}
