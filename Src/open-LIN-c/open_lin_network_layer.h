/**
 * @file open_lin_network_layer.h
 * @author LJ
 * @date 06.07.2018
 * @brief Open LIN network layer
 */

#ifndef OPEN_LIN_NETWORK_LAYER_H_
#define OPEN_LIN_NETWORK_LAYER_H_

#include "open_lin_types.h"

#define OPEN_LIN_NET_SLOT_EMPTY ((void*)0)

typedef struct {
	open_lin_pid_t pid;
	open_lin_frame_type_t frame_type:1;
	open_lin_checksum_type_t checksum_type:1;
	l_u8 data_length:6;
	l_u8 *data_ptr;
} __attribute__((packed)) open_lin_frame_slot_t;

#ifdef OPEN_LIN_DYNAMIC_IDS
	typedef struct {
		open_lin_pid_t id_in_lin_table;
		open_lin_pid_t input_id;
	}open_lin_id_translation_item_t;
#endif

open_lin_frame_slot_t* open_lin_net_get_slot(open_lin_pid_t pid);
void open_lin_net_rx_handler(open_lin_pid_t pid);
void open_lin_net_init(open_lin_frame_slot_t *a_slot_array, l_u8 a_slot_array_len);
void open_lin_on_rx_frame(open_lin_frame_slot_t *slot);

#endif /* OPEN_LIN_NETWORK_LAYER_H_ */
