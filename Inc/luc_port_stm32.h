/*
 * luc_port_stm32.h - Einbindung des LUC-LIN-Ports in die CubeMX-Dateien
 */
#ifndef LUC_PORT_STM32_H_
#define LUC_PORT_STM32_H_

#include <stdint.h>

void luc_port_init(void);
void luc_port_usart1_irq(void);                        /* USART1_IRQHandler */
void luc_port_usb_rx(const uint8_t *buf, uint32_t len); /* CDC_Receive_FS */
void luc_port_usb_pump(void);                          /* Hauptschleife */

#endif /* LUC_PORT_STM32_H_ */
