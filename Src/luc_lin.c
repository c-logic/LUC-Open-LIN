/*
 * luc_lin.c - LIN-Engine (Master, Slave, Monitor, Diagnose)
 *
 * Kontexte:
 *  - luc_lin_isr_break / luc_lin_isr_byte: UART-Interrupt
 *  - alles andere: Hauptschleife; Zugriffe auf gemeinsame Daten unter
 *    luc_port_lock() (nur wenige Mikrosekunden).
 */
#include "luc_lin.h"
#include "luc_port.h"
#include <string.h>

luc_opt_t luc_opt;
luc_stats_t luc_stats;

/* ------------------------------------------------------------------------ */
/* Konfiguration                                                             */
/* ------------------------------------------------------------------------ */
static luc_frame_t frames[64];
static volatile uint8_t mode = LUC_MODE_OFF;
static uint32_t baud = 19200u;
static uint32_t bit_us = 52u;

static uint8_t sched_id[LUC_SCHED_MAX];
static uint8_t sched_ms[LUC_SCHED_MAX];
static uint8_t sched_n;
static uint8_t sched_idx;

/* Einmal-Frame (SEND) */
static struct {
	uint8_t pending;
	uint8_t id;
	int8_t classic;     /* -1: nur Header */
	uint8_t n;
	uint8_t data[8];
} oneshot;
static uint8_t wake_pending;

/* Slot-Steuerung der Hauptschleife */
static uint32_t slot_start_us;
static uint32_t slot_len_us;

/* ------------------------------------------------------------------------ */
/* Empfangs-/Sende-Zustand (gemeinsam mit dem Interrupt)                     */
/* ------------------------------------------------------------------------ */
#define RX_IDLE 0u
#define RX_SYNC 1u
#define RX_PID  2u
#define RX_DATA 3u

static volatile uint8_t rx_state = RX_IDLE;
static volatile uint32_t t_break;     /* Zeitpunkt Break */
static volatile uint32_t t_pid;       /* Zeitpunkt PID */
static volatile uint32_t t_last;      /* letztes Byte */
static uint32_t t_first;              /* erstes Antwortbyte */
static uint32_t t_prev;               /* vorheriges Antwortbyte */
static int32_t ib_max;                /* groesste Pause zwischen Antwortbytes */
static uint8_t cur_id;
static uint8_t cur_len;               /* erwartete Datenlaenge, 0 = unbekannt */
static uint8_t cur_n;
static uint8_t cur_b[9];
static uint8_t cur_src;               /* 'T', 'R', 0 */
static uint8_t cur_bit;               /* Bitfehler */
static uint8_t cur_own;               /* Header von uns: 1 Schedule/Diag, 2 SEND */

static uint8_t tx_buf[11];
static volatile uint8_t tx_active;
static uint8_t tx_len;
static uint8_t tx_sent;
static uint8_t tx_echo;
static uint8_t tx_master;             /* tx_buf beginnt mit 0x55, PID */
static uint8_t tx_kind;               /* 1 Schedule/Diag, 2 SEND */
static volatile uint32_t t_tx_start;

/* Ereignis-Queue Interrupt -> Hauptschleife */
#define EVQ_SIZE 10u
static luc_event_t evq[EVQ_SIZE];
static volatile uint8_t evq_head, evq_tail;

/* Diagnose */
#define DG_IDLE 0u
#define DG_REQ  1u
#define DG_POLL 2u
#define DG_DONE 3u
static struct {
	uint8_t state;
	uint8_t nad;
	uint8_t req[LUC_DIAG_REQ_MAX];
	uint8_t req_n;
	uint8_t req_off;
	uint8_t sn;
	uint16_t rsp_len;
	uint16_t rsp_got;
	uint8_t rsp_sn;
	uint32_t t_next;
	uint32_t t_deadline;
	luc_diag_result_t res;
} dg;
static volatile uint8_t dg_mail_flag;
static uint8_t dg_mail[8];

/* ------------------------------------------------------------------------ */
/* Hilfsfunktionen                                                           */
/* ------------------------------------------------------------------------ */
#define BIT(x, n) (((x) >> (n)) & 1u)

uint8_t luc_lin_pid(uint8_t id)
{
	uint8_t p0, p1;
	id &= 0x3Fu;
	p0 = (uint8_t)(BIT(id, 0) ^ BIT(id, 1) ^ BIT(id, 2) ^ BIT(id, 4));
	p1 = (uint8_t)(~(BIT(id, 1) ^ BIT(id, 3) ^ BIT(id, 4) ^ BIT(id, 5)) & 1u);
	return (uint8_t)(id | (p0 << 6) | (p1 << 7));
}

uint8_t luc_lin_checksum(uint8_t id, const uint8_t *data, uint8_t n, uint8_t classic)
{
	uint16_t s = 0;
	uint8_t i;
	if (!classic && ((id & 0x3Fu) < 0x3Cu))
		s = luc_lin_pid(id);
	for (i = 0; i < n; i++) {
		s = (uint16_t)(s + data[i]);
		if (s > 0xFFu)
			s = (uint16_t)(s - 0xFFu);
	}
	return (uint8_t)(~s);
}

static uint32_t char_us(void)      { return 10u * bit_us; }
static uint32_t char_max_us(void)  { return 14u * bit_us; }   /* 1.4 x Zeichenzeit */

uint8_t luc_lin_slot_ms(uint8_t len)
{
	/* T_frame_max = 1.4 * (34 + 10 * (len + 1)) Bitzeiten, plus 1 ms */
	uint32_t us;
	if (len == 0u || len > 8u)
		len = 8u;
	us = ((34u + 10u * ((uint32_t)len + 1u)) * 14u * bit_us) / 10u;
	us = us / 1000u + 2u;
	return (uint8_t)(us > 255u ? 255u : us);
}

/* ------------------------------------------------------------------------ */
/* Ereignisse                                                                */
/* ------------------------------------------------------------------------ */
static void evq_push(const luc_event_t *e)
{
	uint8_t next = (uint8_t)((evq_head + 1u) % EVQ_SIZE);
	if (next == evq_tail) {
		luc_stats.overruns++;
		return;
	}
	evq[evq_head] = *e;
	evq_head = next;
}

int luc_lin_event_pop(luc_event_t *e)
{
	uint32_t key;
	int ok = 0;
	key = luc_port_lock();
	if (evq_tail != evq_head) {
		*e = evq[evq_tail];
		evq_tail = (uint8_t)((evq_tail + 1u) % EVQ_SIZE);
		ok = 1;
	}
	luc_port_unlock(key);
	return ok;
}

/* Frame abschliessen und melden. Aufruf im Interrupt oder unter Lock. */
static void finalize(void)
{
	luc_event_t e;
	const luc_frame_t *f = &frames[cur_id];
	uint8_t report = 1;

	e.t_us = t_break;
	e.id = cur_id;
	e.n = cur_n;
	e.src = cur_n ? (cur_src ? cur_src : 'R') : '-';
	memcpy(e.b, cur_b, sizeof(e.b));

	/* Zeitmessung. Die Zeitstempel liegen jeweils am Ende des Stoppbits
	 * (RXNE); die Pause bis zum naechsten Startbit ist daher Abstand minus
	 * eine Zeichenzeit. */
	if (rx_state == RX_DATA) {
		uint32_t h = t_pid - t_break;
		e.t_hdr = (uint16_t)(h > 0xFFFFu ? 0xFFFFu : h);
	} else {
		e.t_hdr = 0xFFFFu;
	}
	if (cur_n) {
		int32_t rs = (int32_t)(t_first - t_pid) - (int32_t)char_us();
		uint32_t fr = t_last - t_break;
		e.t_rs = (int16_t)(rs > 32767 ? 32767 : (rs < -32767 ? -32767 : rs));
		e.t_ib = (cur_n > 1u) ? (int16_t)(ib_max > 32767 ? 32767 : ib_max) : (int16_t)LUC_T_NONE;
		e.t_frm = (uint16_t)(fr > 0xFFFFu ? 0xFFFFu : fr);
	} else {
		e.t_rs = (int16_t)LUC_T_NONE;
		e.t_ib = (int16_t)LUC_T_NONE;
		e.t_frm = 0xFFFFu;
	}

	if (cur_bit)
		e.st = LUC_ST_BIT;
	else if (cur_n == 0u)
		e.st = LUC_ST_NR;
	else if ((cur_len && cur_n < (uint8_t)(cur_len + 1u)) || cur_n < 2u)
		e.st = LUC_ST_TO;
	else {
		uint8_t l = (uint8_t)(cur_n - 1u);
		uint8_t cs = cur_b[l];
		if (cur_id < 0x3Cu && cs == luc_lin_checksum(cur_id, cur_b, l, 0))
			e.st = LUC_ST_E;
		else if (cs == luc_lin_checksum(cur_id, cur_b, l, 1))
			e.st = LUC_ST_C;
		else
			e.st = LUC_ST_CS;

		/* STRICT: Checksummenart muss zur Definition passen (nicht fuer SEND
		 * mit eigenen Daten - dort gilt die beim Senden gewaehlte Art) */
		if (luc_opt.strict && f->dir != LUC_DIR_NONE && !(cur_own == 2u && e.src == 'T') &&
				e.st <= LUC_ST_C && cur_id < 0x3Cu &&
				(uint8_t)(e.st == LUC_ST_C) != f->classic)
			e.st = LUC_ST_CS;
	}

	if (e.st >= LUC_ST_CS && e.st != LUC_ST_NR)
		luc_stats.errors++;

	/* Diagnose-Antwort an die Hauptschleife */
	if (cur_id == 0x3Du && cur_n == 9u && e.st <= LUC_ST_C && dg.state == DG_POLL) {
		memcpy(dg_mail, cur_b, 8);
		dg_mail_flag = 1u;
	}

	if (cur_own != 2u) {                  /* SEND-Ergebnisse immer melden */
		if (e.src == 'T' && !luc_opt.echo)
			report = 0;
		if (!cur_own && mode != LUC_MODE_MONITOR && f->dir == LUC_DIR_NONE && !luc_opt.all)
			report = 0;
		if (e.st == LUC_ST_NR && !luc_opt.nr)
			report = 0;
	}

	if (report) {
		luc_stats.frames++;
		evq_push(&e);
	}

	rx_state = RX_IDLE;
	tx_active = 0;
	cur_bit = 0;
	cur_own = 0;
	cur_src = 0;
}

/* ------------------------------------------------------------------------ */
/* Interrupt                                                                 */
/* ------------------------------------------------------------------------ */
void luc_lin_isr_break(void)
{
	uint32_t now = luc_port_now_us();
	uint8_t own = (tx_active && tx_master && tx_echo == 0u);

	if (mode == LUC_MODE_OFF)
		return;

	if (rx_state == RX_DATA) {
		if (tx_active && !own)
			cur_bit = 1u;
		finalize();
	} else if (rx_state != RX_IDLE) {
		luc_stats.errors++;
	}

	if (own) {
		tx_active = 1u;   /* finalize() hat es geloescht */
		cur_own = tx_kind;
	} else {
		tx_active = 0;
		cur_own = 0;
	}
	rx_state = RX_SYNC;
	t_break = now;
	t_last = now;
	cur_n = 0;
	cur_bit = 0;
	cur_src = 0;
}

static void slave_respond(const luc_frame_t *f)
{
	uint8_t i;
	for (i = 0; i < f->len; i++)
		tx_buf[i] = f->data[i];
	tx_buf[f->len] = luc_lin_checksum(cur_id, f->data, f->len, f->classic);
	tx_len = (uint8_t)(f->len + 1u);
	tx_echo = 0;
	tx_master = 0;
	tx_sent = 1u;
	tx_active = 1u;
	cur_src = 'T';
	luc_port_tx_byte(tx_buf[0]);
}

void luc_lin_isr_byte(uint8_t b, uint8_t frame_err)
{
	uint32_t now;

	if (mode == LUC_MODE_OFF)
		return;
	if (frame_err && b == 0u)
		return;                    /* Break-Zeichen */

	now = luc_port_now_us();
	t_last = now;

	/* Echo des eigenen Sendens pruefen und naechstes Byte nachlegen */
	if (tx_active) {
		if (b != tx_buf[tx_echo]) {
			tx_active = 0;
			cur_bit = 1u;
		} else {
			tx_echo++;
			if (tx_sent < tx_len)
				luc_port_tx_byte(tx_buf[tx_sent++]);
			else if (tx_echo >= tx_len)
				tx_active = 0;
		}
	}

	switch (rx_state) {
	case RX_SYNC:
		if (b == 0x55u) {
			rx_state = RX_PID;
		} else {
			luc_stats.errors++;
			rx_state = RX_IDLE;
			tx_active = 0;
			cur_own = 0;
		}
		break;

	case RX_PID:
	{
		const luc_frame_t *f;
		if (luc_lin_pid(b) != b || cur_bit) {
			luc_stats.errors++;
			rx_state = RX_IDLE;
			tx_active = 0;
			cur_own = 0;
			break;
		}
		cur_id = (uint8_t)(b & 0x3Fu);
		f = &frames[cur_id];
		cur_len = (f->dir != LUC_DIR_NONE) ? f->len : 0u;
		cur_n = 0;
		t_pid = now;
		rx_state = RX_DATA;
		if (cur_own) {
			if (tx_len > 2u) {
				cur_src = 'T';
				cur_len = (uint8_t)(tx_len - 3u);
			}
		} else if (mode == LUC_MODE_SLAVE) {
			if (f->dir == LUC_DIR_PUB ||
					(f->dir == LUC_DIR_ONCE && f->armed)) {
				if (f->dir == LUC_DIR_ONCE)
					frames[cur_id].armed = 0;
				slave_respond(f);
			}
		}
		break;
	}

	case RX_DATA:
		if (cur_n == 0u) {
			t_first = now;
			ib_max = 0;
		} else {
			int32_t gap = (int32_t)(now - t_prev) - (int32_t)char_us();
			if (gap > ib_max)
				ib_max = gap;
		}
		t_prev = now;
		if (cur_n < 9u)
			cur_b[cur_n++] = b;
		if (!cur_src)
			cur_src = 'R';
		if (cur_bit || (cur_len && cur_n == (uint8_t)(cur_len + 1u)) || cur_n == 9u)
			finalize();
		break;

	default:
		break;
	}
}

/* ------------------------------------------------------------------------ */
/* Master: Header (+ Antwort) starten                                        */
/* ------------------------------------------------------------------------ */
static void master_frame(uint8_t kind, uint8_t id, int8_t classic, const uint8_t *data, uint8_t n)
{
	uint32_t key;
	const luc_frame_t *f = &frames[id];
	uint8_t i, len = 2u;

	tx_buf[0] = 0x55u;
	tx_buf[1] = luc_lin_pid(id);
	if (classic >= 0 && n > 0u) {
		for (i = 0; i < n; i++)
			tx_buf[2u + i] = data[i];
		tx_buf[2u + n] = luc_lin_checksum(id, data, n, (uint8_t)classic);
		len = (uint8_t)(3u + n);
	} else if (f->dir == LUC_DIR_PUB || (f->dir == LUC_DIR_ONCE && f->armed)) {
		/* eigene Slave-Task des Masters */
		for (i = 0; i < f->len; i++)
			tx_buf[2u + i] = f->data[i];
		tx_buf[2u + f->len] = luc_lin_checksum(id, f->data, f->len, f->classic);
		len = (uint8_t)(3u + f->len);
		if (f->dir == LUC_DIR_ONCE)
			frames[id].armed = 0;
	}

	key = luc_port_lock();
	tx_len = len;
	tx_echo = 0;
	tx_master = 1u;
	tx_kind = kind;
	tx_sent = 1u;
	tx_active = 1u;
	cur_bit = 0;
	t_tx_start = luc_port_now_us();
	luc_port_send_break();
	luc_port_tx_byte(tx_buf[0]);
	luc_port_unlock(key);
}

/* ------------------------------------------------------------------------ */
/* Diagnose                                                                  */
/* ------------------------------------------------------------------------ */
static void dg_finish(uint8_t status)
{
	dg.res.nad = dg.nad;
	dg.res.status = status;
	dg.state = DG_DONE;
}

/* liefert 1 und ein 3C-Frame (8 Byte) oder 3D-Header (n=0) */
static uint8_t dg_next(uint32_t now, uint8_t *id, uint8_t *buf, uint8_t *n)
{
	uint8_t i;
	if (dg.state != DG_REQ && dg.state != DG_POLL)
		return 0;
	if ((int32_t)(now - dg.t_next) < 0)
		return 0;
	dg.t_next = now + (uint32_t)luc_opt.dslot_ms * 1000u;

	if (dg.state == DG_POLL) {
		*id = 0x3Du;
		*n = 0;
		return 1u;
	}

	memset(buf, 0xFF, 8);
	buf[0] = dg.nad;
	if (dg.req_off == 0u && dg.req_n <= 6u) {
		buf[1] = dg.req_n;
		for (i = 0; i < dg.req_n; i++)
			buf[2u + i] = dg.req[i];
		dg.req_off = dg.req_n;
	} else if (dg.req_off == 0u) {
		buf[1] = 0x10u;
		buf[2] = dg.req_n;
		for (i = 0; i < 5u; i++)
			buf[3u + i] = dg.req[i];
		dg.req_off = 5u;
		dg.sn = 1u;
	} else {
		buf[1] = (uint8_t)(0x20u | (dg.sn & 0x0Fu));
		for (i = 0; i < 6u && dg.req_off < dg.req_n; i++)
			buf[2u + i] = dg.req[dg.req_off++];
		dg.sn++;
	}
	if (dg.req_off >= dg.req_n) {
		dg.state = DG_POLL;
		dg.t_deadline = now + (uint32_t)luc_opt.p2_ms * 1000u;
	}
	*id = 0x3Cu;
	*n = 8u;
	return 1u;
}

static void dg_rx(uint32_t now, const uint8_t *b)
{
	uint8_t pci = (uint8_t)(b[1] >> 4);
	uint8_t i;

	if (dg.state != DG_POLL)
		return;
	if (b[0] != dg.nad && dg.nad != 0x7Fu)
		return;

	dg.t_deadline = now + (uint32_t)luc_opt.p2_ms * 1000u;
	if (pci == 0u) {
		uint8_t l = (uint8_t)(b[1] & 0x0Fu);
		if (l == 0u || l > 6u)
			return;
		if (l >= 3u && b[2] == 0x7Fu && b[4] == 0x78u) {
			dg.t_deadline = now + 5000000u;     /* response pending */
			return;
		}
		dg.res.len = l;
		for (i = 0; i < l; i++)
			dg.res.data[i] = b[2u + i];
		dg_finish((l >= 3u && b[2] == 0x7Fu) ? LUC_DG_NRC : LUC_DG_OK);
	} else if (pci == 1u) {
		dg.rsp_len = (uint16_t)(((uint16_t)(b[1] & 0x0Fu) << 8) | b[2]);
		dg.rsp_got = 0;
		for (i = 0; i < 5u && dg.rsp_got < LUC_DIAG_RSP_MAX; i++)
			dg.res.data[dg.rsp_got++] = b[3u + i];
		dg.rsp_sn = 1u;
	} else if (pci == 2u && dg.rsp_len) {
		if ((b[1] & 0x0Fu) != (dg.rsp_sn & 0x0Fu))
			return;                         /* falsche Sequenz: ignorieren */
		dg.rsp_sn++;
		for (i = 0; i < 6u && dg.rsp_got < dg.rsp_len; i++) {
			if (dg.rsp_got < LUC_DIAG_RSP_MAX)
				dg.res.data[dg.rsp_got] = b[2u + i];
			dg.rsp_got++;
		}
		if (dg.rsp_got >= dg.rsp_len) {
			dg.res.len = (uint8_t)(dg.rsp_len > LUC_DIAG_RSP_MAX ? LUC_DIAG_RSP_MAX : dg.rsp_len);
			dg_finish(LUC_DG_OK);
		}
	}
}

int luc_lin_diag_request(uint8_t nad, const uint8_t *data, uint8_t n)
{
	if (mode != LUC_MODE_MASTER && mode != LUC_MODE_DIAG)
		return LUC_E_MODE;
	if (n == 0u || n > LUC_DIAG_REQ_MAX)
		return LUC_E_RANGE;
	if (dg.state == DG_REQ || dg.state == DG_POLL)
		return LUC_E_BUSY;
	memcpy(dg.req, data, n);
	dg.req_n = n;
	dg.req_off = 0;
	dg.nad = nad;
	dg.rsp_len = 0;
	dg.rsp_got = 0;
	dg.res.len = 0;
	dg.t_next = luc_port_now_us();
	dg_mail_flag = 0;
	dg.state = DG_REQ;
	return LUC_OK;
}

int luc_lin_diag_pop(luc_diag_result_t *r)
{
	if (dg.state != DG_DONE)
		return 0;
	*r = dg.res;
	dg.state = DG_IDLE;
	return 1;
}

/* ------------------------------------------------------------------------ */
/* Hauptschleife                                                             */
/* ------------------------------------------------------------------------ */
static void check_timeouts(uint32_t now)
{
	uint32_t key = luc_port_lock();
	uint8_t s = rx_state;

	if (s == RX_SYNC || s == RX_PID) {
		if (now - t_last > 2u * char_max_us() + 1000u) {
			/* Header unvollstaendig (oder nur Wakeup-Break) */
			rx_state = RX_IDLE;
			if (tx_active)
				luc_stats.errors++;
			tx_active = 0;
			cur_own = 0;
		}
	} else if (s == RX_DATA) {
		uint32_t lim;
		uint8_t l = cur_len ? cur_len : 8u;
		uint8_t done = 0;
		if (cur_n == 0u) {
			lim = (uint32_t)(l + 1u) * char_max_us() + 1000u;
			done = (now - t_pid) > lim;
		} else {
			/* Antwort laeuft: erst bei einer echten Pause abschliessen, nicht
			 * nach der Gesamtzeit - sonst wuerde ein zu langsamer Slave
			 * mitten im Frame abgeschnitten. Ueberlange Frames zeigt
			 * die Zeitmessung (F). */
			if (now - t_last > 2u * char_us() + 1000u)
				done = 1u;
		}
		if (done)
			finalize();
	} else if (tx_active) {
		/* gesendet, aber kein Echo (kein Transceiver / keine Busspannung) */
		if (now - t_tx_start > 14u * char_max_us() + 2000u) {
			tx_active = 0;
			luc_stats.errors++;
		}
	}
	luc_port_unlock(key);
}

static uint8_t bus_idle(void)
{
	return (rx_state == RX_IDLE && !tx_active) ? 1u : 0u;
}

void luc_lin_poll(void)
{
	uint32_t now = luc_port_now_us();
	uint8_t m = mode;

	check_timeouts(now);

	/* Diagnose: empfangene 3D-Antwort und Timeout */
	if (dg_mail_flag) {
		uint8_t b[8];
		uint32_t key = luc_port_lock();
		memcpy(b, dg_mail, 8);
		dg_mail_flag = 0;
		luc_port_unlock(key);
		dg_rx(now, b);
	}
	if (dg.state == DG_POLL && (int32_t)(now - dg.t_deadline) > 0)
		dg_finish(LUC_DG_TO);

	if (m == LUC_MODE_OFF || m == LUC_MODE_SLAVE)
		return;
	if (!bus_idle())
		return;
	if (now - slot_start_us < slot_len_us)
		return;

	slot_start_us = now;

	if (wake_pending) {
		uint32_t key = luc_port_lock();
		wake_pending = 0;
		luc_port_send_break();
		luc_port_unlock(key);
		slot_len_us = 100000u;           /* 100 ms Ruhe nach Wakeup */
		return;
	}

	if (oneshot.pending) {
		oneshot.pending = 0;
		master_frame(2u, oneshot.id, oneshot.classic, oneshot.data, oneshot.n);
		slot_len_us = (uint32_t)luc_lin_slot_ms(oneshot.n ? oneshot.n : frames[oneshot.id].len) * 1000u;
		return;
	}

	if (m == LUC_MODE_MASTER || m == LUC_MODE_DIAG) {
		uint8_t id, n, buf[8];
		if (dg_next(now, &id, buf, &n)) {
			master_frame(1u, id, n ? 1 : -1, buf, n);
			slot_len_us = (uint32_t)luc_lin_slot_ms(8) * 1000u;
			return;
		}
	}

	if (m == LUC_MODE_MASTER && sched_n) {
		uint8_t i = sched_idx;
		if (i >= sched_n)
			i = 0;
		sched_idx = (uint8_t)(i + 1u);
		master_frame(1u, sched_id[i], -1, 0, 0);
		slot_len_us = (uint32_t)sched_ms[i] * 1000u;
		return;
	}

	slot_len_us = 1000u;                 /* nichts zu tun: bald wieder schauen */
}

/* ------------------------------------------------------------------------ */
/* Konfiguration                                                             */
/* ------------------------------------------------------------------------ */
static void stop_bus(void)
{
	uint32_t key = luc_port_lock();
	rx_state = RX_IDLE;
	tx_active = 0;
	cur_own = 0;
	cur_bit = 0;
	cur_src = 0;
	luc_port_unlock(key);
}

uint8_t luc_lin_get_mode(void)
{
	return mode;
}

void luc_lin_set_mode(uint8_t m)
{
	stop_bus();
	mode = m;
	sched_idx = 0;
	oneshot.pending = 0;
	wake_pending = 0;
	dg.state = DG_IDLE;
	slot_start_us = luc_port_now_us();
	slot_len_us = 0;
}

uint32_t luc_lin_get_baud(void)
{
	return baud;
}

int luc_lin_set_baud(uint32_t b)
{
	if (b < 1000u || b > 115200u)
		return LUC_E_RANGE;
	stop_bus();
	baud = b;
	bit_us = (1000000u + b / 2u) / b;
	luc_port_set_baud(b);
	return LUC_OK;
}

int luc_lin_frame_set(uint8_t id, uint8_t dir, uint8_t len, uint8_t classic,
		const uint8_t *data, uint8_t n)
{
	luc_frame_t f;
	uint32_t key;
	if (id > 0x3Fu || len == 0u || len > 8u || n > len)
		return LUC_E_RANGE;
	if (dir != LUC_DIR_PUB && dir != LUC_DIR_ONCE && dir != LUC_DIR_SUB)
		return LUC_E_RANGE;
	memset(&f, 0, sizeof(f));
	f.dir = dir;
	f.len = len;
	f.classic = (uint8_t)((classic || id >= 0x3Cu) ? 1u : 0u);
	f.armed = (uint8_t)((dir == LUC_DIR_ONCE && n > 0u) ? 1u : 0u);
	if (n)
		memcpy(f.data, data, n);
	key = luc_port_lock();
	frames[id] = f;
	luc_port_unlock(key);
	return LUC_OK;
}

int luc_lin_frame_data(uint8_t id, const uint8_t *data, uint8_t n)
{
	uint32_t key;
	if (id > 0x3Fu)
		return LUC_E_RANGE;
	if (frames[id].dir != LUC_DIR_PUB && frames[id].dir != LUC_DIR_ONCE)
		return LUC_E_UNDEF;
	if (n != frames[id].len)
		return LUC_E_RANGE;
	key = luc_port_lock();
	memcpy(frames[id].data, data, n);
	if (frames[id].dir == LUC_DIR_ONCE)
		frames[id].armed = 1u;
	luc_port_unlock(key);
	return LUC_OK;
}

void luc_lin_frame_del(uint8_t id)
{
	uint32_t key;
	if (id > 0x3Fu)
		return;
	key = luc_port_lock();
	memset(&frames[id], 0, sizeof(luc_frame_t));
	luc_port_unlock(key);
}

void luc_lin_frame_del_all(void)
{
	uint32_t key = luc_port_lock();
	memset(frames, 0, sizeof(frames));
	luc_port_unlock(key);
}

void luc_lin_frame_get(uint8_t id, luc_frame_t *out)
{
	uint32_t key = luc_port_lock();
	*out = frames[id & 0x3Fu];
	luc_port_unlock(key);
}

int luc_lin_sched_set(const uint8_t *ids, const uint8_t *ms, uint8_t n)
{
	uint8_t i;
	if (n > LUC_SCHED_MAX)
		return LUC_E_FULL;
	for (i = 0; i < n; i++) {
		if (ids[i] > 0x3Fu || ms[i] == 0u)
			return LUC_E_RANGE;
	}
	for (i = 0; i < n; i++) {
		sched_id[i] = ids[i];
		sched_ms[i] = ms[i];
	}
	sched_n = n;
	sched_idx = 0;
	return LUC_OK;
}

uint8_t luc_lin_sched_count(void)
{
	return sched_n;
}

void luc_lin_sched_get(uint8_t i, uint8_t *id, uint8_t *ms)
{
	*id = sched_id[i];
	*ms = sched_ms[i];
}

int luc_lin_send(uint8_t id, int8_t classic, const uint8_t *data, uint8_t n)
{
	if (mode != LUC_MODE_MASTER && mode != LUC_MODE_MONITOR && mode != LUC_MODE_DIAG)
		return LUC_E_MODE;
	if (id > 0x3Fu || n > 8u)
		return LUC_E_RANGE;
	if (oneshot.pending)
		return LUC_E_BUSY;
	oneshot.id = id;
	oneshot.n = n;
	oneshot.classic = (int8_t)(n ? (classic < 0 ? (int8_t)frames[id].classic : classic) : -1);
	if (n)
		memcpy(oneshot.data, data, n);
	oneshot.pending = 1u;
	return LUC_OK;
}

int luc_lin_wake(void)
{
	if (mode == LUC_MODE_OFF)
		return LUC_E_MODE;
	if (mode == LUC_MODE_SLAVE) {
		/* Slave darf jederzeit wecken, wenn der Bus ruhig ist */
		uint32_t key = luc_port_lock();
		if (rx_state == RX_IDLE && !tx_active)
			luc_port_send_break();
		luc_port_unlock(key);
		return LUC_OK;
	}
	wake_pending = 1u;
	return LUC_OK;
}

void luc_lin_reset(void)
{
	luc_lin_set_mode(LUC_MODE_OFF);
	luc_lin_frame_del_all();
	sched_n = 0;
	luc_opt.echo = 1u;
	luc_opt.all = 1u;
	luc_opt.nr = 1u;
	luc_opt.strict = 0;
	luc_opt.timing = 0;
	luc_opt.dslot_ms = 10u;
	luc_opt.p2_ms = 1000u;
	memset(&luc_stats, 0, sizeof(luc_stats));
	(void)luc_lin_set_baud(19200u);
}

void luc_lin_init(void)
{
	evq_head = evq_tail = 0;
	luc_lin_reset();
}
