/*
 * luc_port.h - Hardware-Schnittstelle des LUC-LIN-Kerns
 *
 * Der Kern (luc_lin.c, luc_proto.c) ist hardwareunabhaengig. Diese Funktionen
 * stellt der jeweilige Port bereit (STM32: luc_port_stm32.c, PC-Simulator: sim).
 */
#ifndef LUC_PORT_H_
#define LUC_PORT_H_

#include <stdint.h>

/* freilaufende Zeit in Mikrosekunden (32 bit, darf ueberlaufen) */
uint32_t luc_port_now_us(void);

/* ein Byte auf den LIN-Bus legen (TX-Register muss frei sein) */
void luc_port_tx_byte(uint8_t b);

/* Break anfordern; ein danach geschriebenes Byte folgt dem Break */
void luc_port_send_break(void);

/* LIN-UART auf neue Baudrate umstellen */
void luc_port_set_baud(uint32_t baud);

/* Interrupts sperren / Zustand wiederherstellen */
uint32_t luc_port_lock(void);
void luc_port_unlock(uint32_t key);

/* Seriennummer des Geraets */
uint32_t luc_port_serial(void);

/* Host-Ausgabe: freier Platz und atomares Schreiben einer kompletten Zeile */
uint16_t luc_port_out_space(void);
void luc_port_out_write(const char *s, uint16_t len);

/* Host-Eingabe: naechstes Zeichen oder -1 */
int luc_port_in_getc(void);

#endif /* LUC_PORT_H_ */
