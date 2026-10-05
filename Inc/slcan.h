#ifndef SLCAN_H_
#define SLCAN_H_

#include "stdint.h"

#include "open_lin_cfg.h"
#include "open_lin_data_layer.h"
#include "open_lin_master_data_layer.h"
#include "open_lin_network_layer.h"
#include "open_lin_hw.h"
#include "open_lin_slave_data_layer.h"

#define VERSION_FIRMWARE_MAJOR 3
#define VERSION_FIRMWARE_MINOR 1

#define VERSION_HARDWARE_MAJOR 0
#define VERSION_HARDWARE_MINOR 1

#define LINE_MAXLEN 62
#define MAX_SLAVES_COUNT 64
#define LIN_SLAVE_SIGNALS 5

#define SLCAN_STATE_CONFIG 0
#define SLCAN_STATE_OPEN 2

/* wird in der Mainloop geschrieben und in ISRs gelesen */
extern volatile uint8_t slcan_state;

uint8_t slcanReciveCanFrame(open_lin_frame_slot_t *pRxMsg);
int slCanProccesInput(uint8_t ch);
void slCanHandler(uint8_t time_passed_ms);

/* --- Mainloop --- */
void slCanCheckCommand(void);   /* USB-Kommandos einlesen und ausfuehren */
void slCanServiceLin(void);     /* LIN-Master-Schedule und Empfangs-Timeout */
void slCanOutputPump(void);     /* ein USB-Paket aus dem Sendering absenden */

/* --- Interrupt --- */
void slCanTickIsr(void);        /* aus SysTick_Handler, zaehlt nur */

uint8_t parseHex(uint8_t* line, uint8_t len, uint32_t* value) ;

typedef enum {
    LIN_MASTER,
    LIN_MONITOR,
	LIN_SLAVE
} LinType_t ;

extern volatile LinType_t lin_type;


#endif /* SLCAN_H_ */
