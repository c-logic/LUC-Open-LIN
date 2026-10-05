/*
 * slcan_interface.c
 *
 *  Created on: Apr 2, 2016
 *      Author: Vostro1440
 *
 *  2026: Entkopplung von Interrupt- und Mainloop-Kontext.
 *  - USB-Empfang: der USB-ISR legt nur Bytes in rx_ring ab, die Zeilenauswertung
 *    laeuft komplett in der Mainloop (slCanCheckCommand).
 *  - USB-Senden: Erzeuger (auch ISRs) schreiben fertige Zeilen atomar in tx_ring,
 *    gesendet wird ausschliesslich aus der Mainloop (slCanOutputPump). Es gibt
 *    keine Warteschleife auf USB mehr in irgendeinem Interrupt.
 *  - LIN-Master-Schedule: SysTick zaehlt nur (slCanTickIsr), gearbeitet wird in
 *    der Mainloop (slCanServiceLin). Damit blockiert das blockierende
 *    HAL_UART_Transmit nicht mehr Tick, UART-Empfang und USB.
 */

#include "slcan.h"

#include <stdbool.h>
#include <string.h>
#include <sys/_stdint.h>

#include "usbd_cdc_if.h"
#include "stm32f0xx_hal.h"
#include "lin_slcan.h"

#define SLCAN_BELL 7
#define SLCAN_CR 13
#define SLCAN_LR 10

/* Ringpuffergroessen, jeweils Zweierpotenz */
#define SLCAN_RX_RING_SIZE 128u
#define SLCAN_TX_RING_SIZE 256u

/* laengste Ausgabezeile: 't' + Kompatibilitaets-Nibble + 2 Hex Id + Nibble Len
 * + 9 * 2 Hex Daten/Checksumme + Terminator */
#define SLCAN_LINE_OUT_MAX 24u

/* Statusflags fuer das 'F'-Kommando (slcan: Bit 3 = data overrun) */
#define SLCAN_FLAG_DATA_OVERRUN 0x08u

extern uint32_t serialNumber;

void RebootToBootloader();
volatile uint8_t slcan_state = SLCAN_STATE_CONFIG;
volatile LinType_t lin_type = LIN_MONITOR;
static uint8_t terminator = SLCAN_CR;

extern USBD_HandleTypeDef hUsbDeviceFS;

/* ------------------------------------------------------------------------- */
/* USB -> Geraet: beschrieben im USB-ISR, gelesen in der Mainloop            */
/* ------------------------------------------------------------------------- */
static volatile uint8_t  rx_ring[SLCAN_RX_RING_SIZE];
static volatile uint16_t rx_head = 0;
static volatile uint16_t rx_tail = 0;

/* ------------------------------------------------------------------------- */
/* Geraet -> USB: beschrieben von Mainloop und ISRs,                          */
/*                gesendet ausschliesslich aus der Mainloop                   */
/* ------------------------------------------------------------------------- */
static volatile uint8_t  tx_ring[SLCAN_TX_RING_SIZE];
static volatile uint16_t tx_head = 0;
static volatile uint16_t tx_tail = 0;

/* gesetzt, wenn ein Ringpuffer uebergelaufen ist; wird von 'F' gemeldet */
static volatile uint8_t slcan_overrun = 0;

/* vom SysTick hochgezaehlte, noch nicht verarbeitete Millisekunden */
static volatile uint8_t lin_ms_pending = 0;

/**
  * @brief  Zustand beim Zusammenbauen einer Ausgabezeile
  */
typedef struct {
	uint8_t *buf;
	uint8_t len;
	uint8_t cap;
} sl_line_t;

static void sl_putc(sl_line_t *l, uint8_t c)
{
	if (l->len < l->cap)
	{
		l->buf[l->len] = c;
		l->len++;
	}
}

/**
  * @brief  Nibble als Hexziffer anhaengen
  */
static void sl_put_nibble(sl_line_t *l, uint8_t ch)
{
	sl_putc(l, (uint8_t)(ch > 9u ? (ch - 10u + (uint8_t)'A') : (ch + (uint8_t)'0')));
}

/**
  * @brief  Byte als zwei Hexziffern anhaengen
  */
static void sl_put_hex(sl_line_t *l, uint8_t ch)
{
	sl_put_nibble(l, (uint8_t)(ch >> 4));
	sl_put_nibble(l, (uint8_t)(ch & 0x0Fu));
}

/**
  * @brief  Fertige Zeile atomar in den Sende-Ringpuffer uebernehmen.
  *         Passt sie nicht komplett hinein, wird sie komplett verworfen -
  *         so entstehen keine halben Zeilen auf der Leitung.
  */
static void sl_commit(const sl_line_t *l)
{
	uint32_t primask = __get_PRIMASK();
	uint16_t used;

	__disable_irq();
	used = (uint16_t)(tx_head - tx_tail);
	if ((uint16_t)(SLCAN_TX_RING_SIZE - used) >= (uint16_t)l->len)
	{
		uint8_t i;
		for (i = 0u; i < l->len; i++)
		{
			tx_ring[(uint16_t)(tx_head + i) & (SLCAN_TX_RING_SIZE - 1u)] = l->buf[i];
		}
		tx_head = (uint16_t)(tx_head + l->len);
	} else
	{
		slcan_overrun = 1u;
	}
	__set_PRIMASK(primask);
}

/**
  * @brief  Ein USB-Paket aus dem Ringpuffer absenden.
  *         Nur aus der Mainloop aufrufen. Blockiert nicht.
  */
void slCanOutputPump(void)
{
	USBD_CDC_HandleTypeDef *hcdc;
	uint16_t used;
	uint16_t off;
	uint16_t chunk;

	if (hUsbDeviceFS.dev_state != USBD_STATE_CONFIGURED)
		return;
	hcdc = (USBD_CDC_HandleTypeDef*) hUsbDeviceFS.pClassData;
	if (hcdc == 0)
		return;
	if (hcdc->TxState != 0u)
		return; /* vorheriges Paket noch unterwegs */

	used = (uint16_t)(tx_head - tx_tail);
	if (used == 0u)
		return;

	/* nur bis zum Ringende senden, dann braucht es keine Kopie */
	off = (uint16_t)(tx_tail & (SLCAN_TX_RING_SIZE - 1u));
	chunk = (uint16_t)(SLCAN_TX_RING_SIZE - off);
	if (chunk > used)
		chunk = used;
	if (chunk > CDC_DATA_FS_MAX_PACKET_SIZE)
		chunk = CDC_DATA_FS_MAX_PACKET_SIZE;

	if (CDC_Transmit_FS((uint8_t*) &tx_ring[off], (uint16_t) chunk) == USBD_OK)
		tx_tail = (uint16_t)(tx_tail + chunk);
}

/**
  * @brief  LIN-Zeitbasis. Wird aus dem SysTick aufgerufen und zaehlt nur.
  */
void slCanTickIsr(void)
{
	if (lin_ms_pending < 255u)
		lin_ms_pending++;
}

void slCanHandler(uint8_t time_passed_ms)
{
    if (slcan_state == SLCAN_STATE_OPEN)
    {
        if (lin_type == LIN_MASTER)
        	open_lin_master_dl_handler(time_passed_ms);
    }
}

extern uint32_t lin_baund_rate;

/**
  * @brief  Empfangs-Timeout in ms, abhaengig von der Baudrate.
  *         Etwa vier Zeichenzeiten (40 Bit), mindestens 2 ms.
  *         19200 Bd -> 3 ms, 9600 Bd -> 5 ms.
  */
static uint32_t lin_rx_timeout_ms(void)
{
	uint32_t ms = (40000u + lin_baund_rate - 1u) / lin_baund_rate;
	return (ms < 2u) ? 2u : ms;
}

/**
  * @brief  LIN-Zeitscheibe abarbeiten. Nur aus der Mainloop aufrufen.
  *         Hier laufen die blockierenden HAL_UART_Transmit-Aufrufe des
  *         Master-Schedules - bewusst ausserhalb jedes Interrupts.
  */
void slCanServiceLin(void)
{
	uint32_t primask;
	uint8_t ms;

	primask = __get_PRIMASK();
	__disable_irq();
	ms = lin_ms_pending;
	lin_ms_pending = 0u;
	__set_PRIMASK(primask);

	if (ms == 0u)
		return;

	slCanHandler(ms);

	/* Der Timeout-Handler fasst denselben Zustand an wie lin_slcan_monitor_rx()
	 * in der USART1-ISR. Deshalb Pruefung und Auswertung zusammen unter
	 * gesperrten Interrupts - das dauert nur wenige Mikrosekunden, also
	 * deutlich weniger als eine Zeichenzeit. */
	if (slcan_lin_timeout_counter != 0u)
	{
		primask = __get_PRIMASK();
		__disable_irq();
		if ((slcan_lin_timeout_counter != 0u)
				&& ((HAL_GetTick() - slcan_lin_timeout_counter) > lin_rx_timeout_ms()))
		{
			lin_slcan_rx_timeout_handler();
		}
		__set_PRIMASK(primask);
	}
}

/**
  * @brief  Byte aus dem USB-Interrupt annehmen. Macht nur noch das Einreihen,
  *         die Auswertung passiert in slCanCheckCommand().
  * @param  ch - empfangenes Zeichen
  * @retval immer 0
  */
int slCanProccesInput(uint8_t ch)
{
	if ((uint16_t)(rx_head - rx_tail) < SLCAN_RX_RING_SIZE)
	{
		rx_ring[rx_head & (SLCAN_RX_RING_SIZE - 1u)] = ch;
		rx_head++;
	} else
	{
		slcan_overrun = 1u;
	}
	return 0;
}


/**
  * @brief  Parse hex value of given string
  * @param  canmsg - line Input string
  * 		len    - of characters to interpret
  * 		value  - Pointer to variable for the resulting decoded value
  * @retval 0 on error, 1 on success
  */
uint8_t parseHex(uint8_t* line, uint8_t len, uint32_t* value) {
    *value = 0;
    while (len--) {
        if (*line == 0) return 0;
        *value <<= 4;
        if ((*line >= '0') && (*line <= '9')) {
           *value += *line - '0';
        } else if ((*line >= 'A') && (*line <= 'F')) {
           *value += *line - 'A' + 10;
        } else if ((*line >= 'a') && (*line <= 'f')) {
           *value += *line - 'a' + 10;
        } else return 0;
        line++;
    }
    return 1;
}


/**
 * @brief  Interprets given line and transmit can message
 * @param  line Line string which contains the transmit command
 * @retval HAL status
 */
static uint8_t transmitStd(uint8_t* line, uint8_t classicChecksum) {
    uint32_t temp;
    open_lin_frame_slot_t slot;
    uint8_t data_buff[8];
    uint8_t offset = 0;

    bool lin_data = ((line[0] == 't') || (line[0] == 'T'));

    slot.data_ptr = data_buff;
    slot.checksum_type = classicChecksum ?
    		OPEN_LIN_CHECKSUM_TYPE_CLASSIC : OPEN_LIN_CHECKSUM_TYPE_EXTENDED;
    if (line[0] < 'Z')
		offset = 5;
    // id
    if (!parseHex(&line[2 + offset], 2, &temp)) return 0;
    	slot.pid = open_lin_data_layer_parity((open_lin_pid_t)temp); // add parity
    // len
    if (!parseHex(&line[4 + offset], 1, &temp)) return 0;
    slot.data_length = temp;

    if (slot.data_length > 8) return 0;
    if (lin_data)
    {
        uint8_t i;
        for (i = 0; i < slot.data_length; i++) {
            if (!parseHex(&line[5 + offset + i*2], 2, &temp)) return 0;
            slot.data_ptr[i] = temp;
        }
    }

    if (offset == 0)
    {
    	open_lin_master_data_tx_header(&slot);
    }
    if (lin_data)
    {
    	open_lin_master_data_tx_data(&slot);
    }
    /* set data recepcion state machine */
	lin_slcan_skip_header_reception(slot.pid, slot.checksum_type);

    return 1;
}


/**
 * @brief  Parse given command line
 * @param  line Line string to parse
 * @retval None
 */
extern void MX_USART1_UART_Init(void);

static void slcan_execute(uint8_t *line)
{
	uint8_t outbuf[16];
	sl_line_t out;
	uint8_t result = SLCAN_BELL;

	out.buf = outbuf;
	out.len = 0u;
	out.cap = (uint8_t) sizeof(outbuf);

    switch (line[0]) {
    	case 'a':
    	{
    		if (terminator == SLCAN_CR)
    			terminator = SLCAN_LR;
    		else
    			terminator = SLCAN_CR;
    		result = terminator;
    		break;
    	}
        case 'S':
        case 'G':
        case 'W':
        case 's':
        	if (line[1] == '2')
        	{
        		lin_baund_rate = 9600;
        	} else
        	{
        		lin_baund_rate = 19200;

        	}
        	MX_USART1_UART_Init();
        	open_lin_hw_reset();
        	result = terminator;
        	break;
        case 'F': // Read status flags
        	/* Antwort bewusst unveraendert (nur Terminator), damit sich das
        	 * Protokoll gegenueber dem Host nicht aendert. Wer die Ringpuffer-
        	 * Ueberlaeufe auswerten will, kommentiert die drei Zeilen ein:
        	 *   sl_putc(&out, 'F');
        	 *   sl_put_hex(&out, slcan_overrun ? SLCAN_FLAG_DATA_OVERRUN : 0u);
        	 *   slcan_overrun = 0u;
        	 * (slcan: Bit 3 = data overrun) */
      		result = terminator;
            break;
        case 'V': // Get hardware version
            {
                sl_putc(&out, 'V');
//                sl_put_hex(&out, VERSION_HARDWARE_MAJOR);
                sl_put_hex(&out, VERSION_HARDWARE_MINOR);
                result = terminator;
            }
            break;
        case 'v': // Get firmware version
            {
                sl_putc(&out, 'v');
                sl_put_hex(&out, VERSION_FIRMWARE_MAJOR);
                sl_put_hex(&out, VERSION_FIRMWARE_MINOR);
                result = terminator;
            }
            break;
        case 'N': // Get serial number
            {

                sl_putc(&out, 'N');
                sl_put_hex(&out, (uint8_t)(serialNumber));
                sl_put_hex(&out, (uint8_t)(serialNumber>>8));
                sl_put_hex(&out, (uint8_t)(serialNumber>>16));
                sl_put_hex(&out, (uint8_t)(serialNumber>>24));
                result = terminator;
            }
            break;
        case 'o':  // master mode
        case 'O':
            if (slcan_state == SLCAN_STATE_CONFIG)
            {
                lin_type = LIN_MASTER;
                result = terminator;
            }
            break;
        case 'L': // slave mode
        	 if (slcan_state == SLCAN_STATE_CONFIG){
        		 result = terminator;
				 lin_type = LIN_SLAVE;
				 slcan_state = SLCAN_STATE_OPEN;
				 open_lin_hw_reset();
				 lin_slcan_reset();
        	 }
        	 break;
        case 'l':  // monitor
            if (slcan_state == SLCAN_STATE_CONFIG)
            {
				result = terminator;
                lin_type = LIN_MONITOR;
                slcan_state = SLCAN_STATE_OPEN;
            	open_lin_hw_reset();
            	lin_slcan_reset();
            }
            break;

        case 'C': // Close LIN channel
            slcan_state = SLCAN_STATE_CONFIG;
            result = terminator;
            lin_type = LIN_MASTER;
            break;

        case 'R':
        case 'r': // Transmit header
        case 'T':
        case 't': // Transmit full frame
        {
            // shame on you that you put this code here ...
        	uint8_t classicChecksum = 0;
        	if(line[2]=='8' || line[2]=='9') {
				classicChecksum  = 1;
				line[2]-='8'-'0';
        	} else if(line[2]=='A' || line[2]=='B' || line[2]=='a' || line[2]=='b') {
				classicChecksum  = 1;
				line[2]=(line[2] & 0x4f) - ('A'-'2');
        	}

        	switch (lin_type)
        	{
				case LIN_MASTER:
				case LIN_SLAVE:
	                if (addLinMasterRow(line, classicChecksum) == 1){
	                	if (line[0] < 'Z')
	                		sl_putc(&out, 'Z');
	                	else
	                		sl_putc(&out, 'z');
	                }
	                result = terminator;
					break;
				case LIN_MONITOR:
	                if (slcan_state == SLCAN_STATE_OPEN)
	                {
	                    if (transmitStd(line, classicChecksum) == 1) {
	                        if (line[0] < 'Z')
	                        	sl_putc(&out, 'Z');
	                        else
	                        	sl_putc(&out, 'z');
	                        result = terminator;
	                    }
	                }
					break;
				default:
					break;
        	}
            break;
        }
        default:
        	break;
    }

    if ((line[0] == 'b') && (line[1] == 'o') && (line[2] == 'o') && (line[3] == 't'))
    {
    	//RebootToBootloader();
    }

	sl_putc(&out, result);
	sl_commit(&out);
}

/**
  * @brief  Empfangene USB-Bytes zu Zeilen zusammensetzen und ausfuehren.
  *         Nur aus der Mainloop aufrufen. Der Zeilenpuffer wird ausschliesslich
  *         hier benutzt, damit gibt es keinen Wettlauf mit dem USB-Interrupt.
  *
  *         Multiline-kompatibel: CR, LF und CRLF (bzw. LFCR) werden alle als
  *         Zeilenende akzeptiert, ebenso mehrere Befehle in einem USB-Paket.
  *         Eine leere Zeile (z.B. das LF nach dem CR bei CRLF) wird uebersprungen,
  *         erzeugt also keinen zweiten, leeren Befehl.
  */
void slCanCheckCommand(void)
{
	static uint8_t line[LINE_MAXLEN];
	static uint8_t linepos = 0;

	while (rx_tail != rx_head)
	{
		uint8_t ch = rx_ring[rx_tail & (SLCAN_RX_RING_SIZE - 1u)];
		rx_tail++;

		if ((ch == SLCAN_CR) || (ch == SLCAN_LR))
		{
			/* Zeilenende - egal ob CR, LF oder eine beliebige Kombination */
			line[linepos] = 0;
			linepos = 0;
			if (line[0] != 0)
				slcan_execute(line);
		} else
		{
			line[linepos] = ch;
			if (linepos < (LINE_MAXLEN - 1u))
				linepos++;
		}
	}
}


/**
 * @brief  reciving CAN frame
 * @param  canmsg Pointer to can message
 * 			step Current step
 * @retval Next character to print out
 */
uint8_t slcanReciveCanFrame(open_lin_frame_slot_t *pRxMsg)
{
	uint8_t buf[SLCAN_LINE_OUT_MAX];
	sl_line_t out;
	uint8_t i;
	uint8_t len = (uint8_t) pRxMsg->data_length;

	out.buf = buf;
	out.len = 0u;
	out.cap = (uint8_t) sizeof(buf);

	sl_putc(&out, 't');
	sl_put_nibble(&out, 0); // for slcan compatibility
	sl_put_hex(&out, (uint8_t)(pRxMsg->pid & 0x3Fu));
	sl_put_nibble(&out, len);
	for (i = 0u; i < len; i++)
	{
		sl_put_hex(&out, pRxMsg->data_ptr[i]);
	}
	sl_putc(&out, terminator);
	sl_commit(&out);
	return 0;
}
