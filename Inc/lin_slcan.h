/*
 * lin_slcan.h
 *
 *  Created on: 10.02.2018
 *      Author: ay7vi2
 */

#ifndef LIN_SLCAN_H_
#define LIN_SLCAN_H_

#include <stdint.h>
#include "open_lin_types.h"
#include "open_lin_data_layer.h"
#include "open_lin_network_layer.h"
#include "open_lin_master_data_layer.h"

/* im USART1-ISR gestellt, in der Mainloop ausgewertet; 0 = kein Timeout aktiv */
extern volatile uint32_t slcan_lin_timeout_counter;

/* Liefert die Zeile zu einer Id, legt bei Bedarf eine neue an.
 * Rueckgabe 0 (und *out_index = -1), wenn die Tabelle voll ist. */
t_master_frame_table_item* slcan_get_master_table_row(open_lin_pid_t id, int8_t* out_index);

uint8_t addLinMasterRow(uint8_t* line, uint8_t classicChecksum);
void lin_slcan_monitor_rx(l_u8 rx_byte);
void lin_slcan_rx_timeout_handler(void);
void lin_slcan_skip_header_reception(uint8_t pid, open_lin_checksum_type_t cs_type);
void lin_slcan_reset(void);

#endif /* LIN_SLCAN_H_ */
