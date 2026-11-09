/*
 * luc_lin.h - LIN-Engine fuer Master, Slave, Monitor und Diagnose
 *
 * Eine einzige Empfangs-Statemachine (im UART-Interrupt) verarbeitet jeden
 * Frame auf dem Bus - auch die eigenen, denn der Transceiver liefert jedes
 * gesendete Byte als Echo zurueck. Gesendet wird echo-gesteuert: das naechste
 * Byte geht erst raus, wenn das Echo des vorherigen geprueft ist. Damit gibt
 * es kein blockierendes Senden im Interrupt und Kollisionen werden erkannt.
 */
#ifndef LUC_LIN_H_
#define LUC_LIN_H_

#include <stdint.h>

#define LUC_FW_VERSION   "4.0"
#define LUC_PROTO_VERSION 1

/* Modi */
#define LUC_MODE_OFF     0u
#define LUC_MODE_MASTER  1u
#define LUC_MODE_SLAVE   2u
#define LUC_MODE_MONITOR 3u
#define LUC_MODE_DIAG    4u

/* Frame-Richtung aus Sicht dieses Geraets */
#define LUC_DIR_NONE 0u
#define LUC_DIR_PUB  1u   /* Geraet liefert die Antwort */
#define LUC_DIR_ONCE 2u   /* wie PUB, einmal je DATA */
#define LUC_DIR_SUB  3u   /* anderer Knoten liefert die Antwort */

/* Frame-Status in Ereignissen */
#define LUC_ST_E   0u     /* gueltig, enhanced checksum */
#define LUC_ST_C   1u     /* gueltig, classic checksum */
#define LUC_ST_CS  2u     /* Checksummenfehler */
#define LUC_ST_NR  3u     /* keine Antwort */
#define LUC_ST_TO  4u     /* unvollstaendig */
#define LUC_ST_BIT 5u     /* Bitfehler beim eigenen Senden */

/* Diagnose-Ergebnis */
#define LUC_DG_OK  0u
#define LUC_DG_NRC 1u
#define LUC_DG_TO  2u

/* Fehlercodes der API (= Protokoll-Fehlercodes) */
#define LUC_OK        0
#define LUC_E_SYNTAX  1
#define LUC_E_RANGE   2
#define LUC_E_MODE    3
#define LUC_E_FULL    4
#define LUC_E_UNDEF   5
#define LUC_E_BUSY    6
#define LUC_E_UNKNOWN 7

#define LUC_SCHED_MAX    32u
#define LUC_DIAG_REQ_MAX 40u
#define LUC_DIAG_RSP_MAX 64u

typedef struct {
	uint8_t dir : 2;     /* LUC_DIR_* */
	uint8_t classic : 1; /* 1 = classic checksum */
	uint8_t armed : 1;   /* ONCE: Antwort bereit */
	uint8_t len : 4;     /* 1..8 */
	uint8_t data[8];
} luc_frame_t;           /* 9 Byte, 64 Eintraege = 576 Byte RAM */

typedef struct {
	uint32_t t_us;      /* Zeitstempel Break */
	uint8_t id;
	uint8_t src;        /* 'T', 'R' oder '-' */
	uint8_t n;          /* empfangene Bytes inkl. Checksumme */
	uint8_t st;         /* LUC_ST_* */
	uint8_t b[9];
	/* Zeitmessung (OPT TIMING), alle Werte in us, LUC_T_NONE = nicht vorhanden */
	uint16_t t_hdr;     /* Break-Erkennung bis Ende PID */
	int16_t t_rs;       /* Antwortpause: Stoppbit PID bis Startbit 1. Datenbyte */
	int16_t t_ib;       /* groesste Pause zwischen zwei Antwortbytes */
	uint16_t t_frm;     /* Break-Erkennung bis Ende letztes Byte */
} luc_event_t;

#define LUC_T_NONE (-32768)

typedef struct {
	uint8_t nad;
	uint8_t status;     /* LUC_DG_* */
	uint8_t len;
	uint8_t data[LUC_DIAG_RSP_MAX];
} luc_diag_result_t;

typedef struct {
	uint8_t echo;       /* eigene Antworten melden */
	uint8_t all;        /* undefinierte IDs melden */
	uint8_t nr;         /* Header ohne Antwort melden */
	uint8_t strict;     /* Checksummenart muss zur Frame-Tabelle passen */
	uint8_t timing;     /* Zeitmessung in !F ausgeben */
	uint8_t dslot_ms;   /* Abstand Diagnose-Frames */
	uint16_t p2_ms;     /* Diagnose-Timeout */
} luc_opt_t;

typedef struct {
	uint32_t frames;
	uint32_t errors;
	uint32_t overruns;
} luc_stats_t;

extern luc_opt_t luc_opt;
extern luc_stats_t luc_stats;

void luc_lin_init(void);
void luc_lin_reset(void);

uint8_t luc_lin_get_mode(void);
void luc_lin_set_mode(uint8_t mode);
uint32_t luc_lin_get_baud(void);
int luc_lin_set_baud(uint32_t baud);

int luc_lin_frame_set(uint8_t id, uint8_t dir, uint8_t len, uint8_t classic,
		const uint8_t *data, uint8_t n);
int luc_lin_frame_data(uint8_t id, const uint8_t *data, uint8_t n);
void luc_lin_frame_del(uint8_t id);
void luc_lin_frame_del_all(void);
void luc_lin_frame_get(uint8_t id, luc_frame_t *out);

int luc_lin_sched_set(const uint8_t *ids, const uint8_t *ms, uint8_t n);
uint8_t luc_lin_sched_count(void);
void luc_lin_sched_get(uint8_t i, uint8_t *id, uint8_t *ms);
uint8_t luc_lin_slot_ms(uint8_t len);

int luc_lin_send(uint8_t id, int8_t classic, const uint8_t *data, uint8_t n);
int luc_lin_wake(void);
int luc_lin_diag_request(uint8_t nad, const uint8_t *data, uint8_t n);

int luc_lin_event_pop(luc_event_t *e);
int luc_lin_diag_pop(luc_diag_result_t *r);

/* Hauptschleife */
void luc_lin_poll(void);

/* aus dem UART-Interrupt */
void luc_lin_isr_break(void);
void luc_lin_isr_byte(uint8_t b, uint8_t frame_err);

/* Hilfsfunktionen */
uint8_t luc_lin_pid(uint8_t id);
uint8_t luc_lin_checksum(uint8_t id, const uint8_t *data, uint8_t n, uint8_t classic);

#endif /* LUC_LIN_H_ */
