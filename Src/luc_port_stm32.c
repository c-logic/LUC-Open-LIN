/*
 * luc_port_stm32.c - LUC-LIN-Port fuer STM32F042 (USART1 im LIN-Modus, USB-CDC)
 *
 * USART1: eigener Interrupt-Handler statt HAL (Break-Erkennung LBDF, RXNE).
 *         Gesendet wird direkt ueber TDR; die LIN-Engine legt das naechste
 *         Byte erst nach dem Echo nach, das TX-Register ist dann immer frei.
 * USB:    Empfang im USB-ISR in einen Ring, Auswertung in der Hauptschleife.
 *         Ausgabe ueber einen Sendering, gepumpt aus der Hauptschleife.
 */
#include "main.h"
#include "usart.h"
#include "usbd_cdc_if.h"
#include "luc_port.h"
#include "luc_port_stm32.h"
#include "luc_lin.h"

extern USBD_HandleTypeDef hUsbDeviceFS;
extern uint32_t lin_baund_rate;

/* ------------------------------------------------------------------------ */
/* Ringpuffer (Groessen Zweierpotenzen)                                      */
/* ------------------------------------------------------------------------ */
#define IN_RING_SIZE  256u
#define OUT_RING_SIZE 512u

static volatile uint8_t in_ring[IN_RING_SIZE];
static volatile uint16_t in_head, in_tail;
static volatile uint8_t out_ring[OUT_RING_SIZE];
static volatile uint16_t out_head, out_tail;

/* ------------------------------------------------------------------------ */
/* Zeit                                                                      */
/* ------------------------------------------------------------------------ */
uint32_t luc_port_now_us(void)
{
	uint32_t ms, val, pend, key;
	uint32_t load = SysTick->LOAD + 1u;

	key = __get_PRIMASK();
	__disable_irq();
	ms = uwTick;
	val = SysTick->VAL;
	pend = SCB->ICSR & SCB_ICSR_PENDSTSET_Msk;
	__set_PRIMASK(key);

	/* SysTick schon uebergelaufen, Tick aber noch nicht gezaehlt
	 * (USART-Interrupt hat hoehere Prioritaet als SysTick) */
	if (pend && val > (load / 2u))
		ms++;
	return ms * 1000u + ((load - 1u - val) * 1000u) / load;
}

uint32_t luc_port_lock(void)
{
	uint32_t key = __get_PRIMASK();
	__disable_irq();
	return key;
}

void luc_port_unlock(uint32_t key)
{
	__set_PRIMASK(key);
}

uint32_t luc_port_serial(void)
{
	const uint32_t *uid = (const uint32_t *)UID_BASE;
	return uid[0] ^ uid[1] ^ uid[2];
}

/* ------------------------------------------------------------------------ */
/* LIN / USART1                                                              */
/* ------------------------------------------------------------------------ */
static void usart_enable_irqs(void)
{
	USART1->ICR = USART_ICR_LBDCF | USART_ICR_ORECF | USART_ICR_FECF |
			USART_ICR_NCF | USART_ICR_PECF;
	(void)USART1->RDR;
	USART1->CR2 |= USART_CR2_LBDIE;
	USART1->CR1 |= USART_CR1_RXNEIE;
	HAL_NVIC_SetPriority(USART1_IRQn, 0, 0);
	HAL_NVIC_EnableIRQ(USART1_IRQn);
}

void luc_port_tx_byte(uint8_t b)
{
	USART1->TDR = b;
}

void luc_port_send_break(void)
{
	USART1->RQR = USART_RQR_SBKRQ;
}

void luc_port_set_baud(uint32_t baud)
{
	uint32_t key = luc_port_lock();
	lin_baund_rate = baud;
	MX_USART1_UART_Init();       /* HAL_LIN_Init loescht die Interrupt-Bits */
	usart_enable_irqs();
	luc_port_unlock(key);
}

void luc_port_usart1_irq(void)
{
	uint32_t isr = USART1->ISR;

	/* Break zuerst: das folgende Byte (Sync) gehoert schon zum neuen Frame */
	if (isr & USART_ISR_LBDF) {
		USART1->ICR = USART_ICR_LBDCF;
		luc_lin_isr_break();
	}
	if (isr & USART_ISR_RXNE) {
		uint8_t b = (uint8_t)USART1->RDR;
		luc_lin_isr_byte(b, (isr & USART_ISR_FE) ? 1u : 0u);
	}
	if (isr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE | USART_ISR_PE))
		USART1->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NCF | USART_ICR_PECF;
}

/* ------------------------------------------------------------------------ */
/* USB                                                                       */
/* ------------------------------------------------------------------------ */
/* aus dem USB-Interrupt (CDC_Receive_FS) */
void luc_port_usb_rx(const uint8_t *buf, uint32_t len)
{
	uint32_t i;
	for (i = 0; i < len; i++) {
		if ((uint16_t)(in_head - in_tail) < IN_RING_SIZE) {
			in_ring[in_head & (IN_RING_SIZE - 1u)] = buf[i];
			in_head++;
		}
	}
}

int luc_port_in_getc(void)
{
	int c;
	if (in_tail == in_head)
		return -1;
	c = in_ring[in_tail & (IN_RING_SIZE - 1u)];
	in_tail++;
	return c;
}

uint16_t luc_port_out_space(void)
{
	return (uint16_t)(OUT_RING_SIZE - (uint16_t)(out_head - out_tail));
}

void luc_port_out_write(const char *s, uint16_t len)
{
	uint16_t i;
	if (luc_port_out_space() < len)
		return;                  /* Aufrufer prueft vorher den Platz */
	for (i = 0; i < len; i++)
		out_ring[(uint16_t)(out_head + i) & (OUT_RING_SIZE - 1u)] = (uint8_t)s[i];
	out_head = (uint16_t)(out_head + len);
}

/* ein USB-Paket aus dem Ring senden, nicht blockierend (Hauptschleife) */
void luc_port_usb_pump(void)
{
	USBD_CDC_HandleTypeDef *hcdc;
	uint16_t used, off, chunk;

	if (hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED)
		return;
	hcdc = (USBD_CDC_HandleTypeDef *)hUsbDeviceFS.pClassData;
	if (hcdc == 0 || hcdc->TxState != 0u)
		return;
	used = (uint16_t)(out_head - out_tail);
	if (used == 0u)
		return;
	off = (uint16_t)(out_tail & (OUT_RING_SIZE - 1u));
	chunk = (uint16_t)(OUT_RING_SIZE - off);
	if (chunk > used)
		chunk = used;
	if (chunk > CDC_DATA_FS_MAX_PACKET_SIZE)
		chunk = CDC_DATA_FS_MAX_PACKET_SIZE;
	if (CDC_Transmit_FS((uint8_t *)&out_ring[off], chunk) == USBD_OK)
		out_tail = (uint16_t)(out_tail + chunk);
}

void luc_port_init(void)
{
	in_head = in_tail = 0;
	out_head = out_tail = 0;
	usart_enable_irqs();
}
