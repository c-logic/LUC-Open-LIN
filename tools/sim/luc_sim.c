/*
 * luc_sim.c - LUC-LIN-Firmware auf dem PC mit virtuellem LIN-Bus
 *
 * Der unveraenderte Firmware-Kern (luc_lin.c, luc_proto.c) laeuft gegen einen
 * simulierten Bus mit Echo. Der Host verbindet sich ueber ein Pseudo-Terminal,
 * genau wie mit dem echten Adapter ueber COM-Port. Auf dem Bus koennen
 * virtuelle Knoten haengen:
 *
 *   --slave ID=HEX[,C]      Slave, beantwortet ID (C = classic checksum);
 *                            ID mit '+' (z.B. 21+=...) zaehlt Byte 0 hoch
 *   --master ID[/ms],...     fremder Master mit Schedule
 *   --diag NAD               Diagnose-Slave (B2 Read-by-ID, 22 RDBI mit
 *                            Mehrfach-Frame-Antwort und einmal 'pending')
 *   --link PFAD              Symlink auf das Pseudo-Terminal anlegen
 *   --nobus                  kein Echo (Transceiver fehlt)
 *   --gap US                 Antwortpause des Slaves nach der PID in us
 *   --ibgap US               Pause zwischen den Antwortbytes des Slaves in us
 *
 * Bauen:  cc -O2 -I../../Inc -o luc_sim luc_sim.c ../../Src/luc_lin.c ../../Src/luc_proto.c
 */
#define _XOPEN_SOURCE 700
#define _DEFAULT_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <termios.h>

#include "luc_lin.h"
#include "luc_proto.h"
#include "luc_port.h"

/* ------------------------------------------------------------------------ */
/* Zeit                                                                      */
/* ------------------------------------------------------------------------ */
static uint64_t t0;
static uint64_t mono_us(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}
/* virtuelle Zeit: laeuft der Echtzeit hinterher und wird in kleinen Schritten
 * (genau bis zu jedem Busereignis) nachgezogen. So bleibt das Timing auch dann
 * exakt, wenn der Prozess vom Betriebssystem kurz angehalten wird. */
static uint64_t vnow;
static uint64_t now64(void) { return vnow; }

/* ------------------------------------------------------------------------ */
/* virtueller Bus                                                            */
/* ------------------------------------------------------------------------ */
#define NODE_DUT    0
#define NODE_SLAVE  1
#define NODE_MASTER 2
#define NODE_DIAG   3
#define NODES       4
#define SYM_BREAK   0x100

static uint32_t bus_baud = 19200;
static int nobus;

typedef struct { int q[16]; int head, tail; } symq_t;
static symq_t txq[NODES];
static uint64_t node_ready[NODES]; /* Knoten darf erst ab hier senden */
static int sym_node;             /* Knoten, der das aktuelle Symbol sendet */
static uint32_t slave_gap_us, slave_ibgap_us;
static int sym_active;           /* Symbol auf dem Bus */
static int sym_value;            /* Byte (wired-AND) oder SYM_BREAK */
static uint64_t sym_end;

static void q_put(int node, int sym)
{
	symq_t *q = &txq[node];
	int n = (q->head + 1) % 16;
	if (n == q->tail)
		return;
	q->q[q->head] = sym;
	q->head = n;
}
static int q_get(int node, int *sym)
{
	symq_t *q = &txq[node];
	if (q->tail == q->head)
		return 0;
	*sym = q->q[q->tail];
	q->tail = (q->tail + 1) % 16;
	return 1;
}

static uint64_t bit_time(void) { return (1000000u + bus_baud / 2) / bus_baud; }

static void nodes_on_symbol(int sym);   /* virtuelle Knoten */

static void bus_service(void)
{
	uint64_t now = now64();
	if (sym_active && now >= sym_end) {
		sym_active = 0;
		if (sym_node == NODE_SLAVE || sym_node == NODE_DIAG)
			node_ready[sym_node] = now + slave_ibgap_us;
		if (!nobus) {
			if (sym_value == SYM_BREAK) {
				luc_lin_isr_byte(0x00, 1);
				luc_lin_isr_break();
			} else {
				luc_lin_isr_byte((uint8_t)sym_value, 0);
			}
		}
		nodes_on_symbol(sym_value);
	}
	if (!sym_active) {
		int i, any = 0, val = 0xFF, brk = 0, s;
		for (i = 0; i < NODES; i++) {
			if (now < node_ready[i])
				continue;
			if (q_get(i, &s)) {
				sym_node = i;
				any = 1;
				if (s == SYM_BREAK)
					brk = 1;
				else
					val &= s;
			}
		}
		if (any) {
			sym_active = 1;
			sym_value = brk ? SYM_BREAK : val;
			sym_end = now + bit_time() * (brk ? 14u : 10u);
		}
	}
}

static int bus_busy(void)
{
	int i;
	if (sym_active)
		return 1;
	for (i = 0; i < NODES; i++)
		if (txq[i].head != txq[i].tail)
			return 1;
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Port-Funktionen fuer den Firmware-Kern                                    */
/* ------------------------------------------------------------------------ */
static int pty_fd = -1;
static uint8_t in_ring[4096];
static int in_head, in_tail;

uint32_t luc_port_now_us(void) { return (uint32_t)now64(); }
void luc_port_tx_byte(uint8_t b) { q_put(NODE_DUT, b); }
void luc_port_send_break(void) { q_put(NODE_DUT, SYM_BREAK); }
void luc_port_set_baud(uint32_t baud) { bus_baud = baud; }
uint32_t luc_port_lock(void) { return 0; }
void luc_port_unlock(uint32_t key) { (void)key; }
uint32_t luc_port_serial(void) { return 0x51A0C0DEu; }
uint16_t luc_port_out_space(void) { return 2048; }

void luc_port_out_write(const char *s, uint16_t len)
{
	while (len) {
		ssize_t w = write(pty_fd, s, len);
		if (w > 0) {
			s += w;
			len = (uint16_t)(len - w);
		} else if (w < 0 && errno != EAGAIN && errno != EIO) {
			return;
		} else {
			usleep(200);
			if (w < 0 && errno == EIO)
				return;   /* kein Host verbunden: verwerfen */
		}
	}
}

int luc_port_in_getc(void)
{
	int c;
	if (in_tail == in_head)
		return -1;
	c = in_ring[in_tail];
	in_tail = (in_tail + 1) % (int)sizeof(in_ring);
	return c;
}

/* ------------------------------------------------------------------------ */
/* virtuelle Knoten                                                          */
/* ------------------------------------------------------------------------ */
typedef struct { int used, len, classic, count; uint8_t data[8]; } vslave_t;
static vslave_t vslave[64];

static int vm_n, vm_idx;
static uint8_t vm_id[32], vm_ms[32];
static uint64_t vm_next;

static int vdiag_nad = -1;

/* Empfangszustand je virtuellem Knoten (alle sehen denselben Bus) */
static int nst;                  /* 0 idle, 1 sync, 2 pid, 3 data */
static int nid;
static uint8_t nbuf[9];
static int nn;
static int responder;            /* -1 oder Knoten, der gerade antwortet */

/* Diagnose-Slave */
static uint8_t dreq[64];
static int dreq_len, dreq_got, dreq_sn;
static uint8_t drsp[64];
static int drsp_len, drsp_off, drsp_sn, drsp_pending_once = 1, drsp_ready;
static int drsp_sent_pending;

static void respond(int node, const uint8_t *d, int len, int classic, int id)
{
	int i;
	for (i = 0; i < len; i++)
		q_put(node, d[i]);
	q_put(node, luc_lin_checksum((uint8_t)id, d, (uint8_t)len, (uint8_t)classic));
	responder = node;
	node_ready[node] = now64() + slave_gap_us;
}

static void diag_make_response(void)
{
	uint8_t sid = dreq[0];
	drsp_off = 0;
	drsp_sn = 1;
	drsp_ready = 1;
	drsp_sent_pending = 0;
	if (sid == 0xB2) {          /* Read by Identifier: Supplier/Function/Variant */
		uint8_t r[] = { 0xF2, 0x34, 0x12, 0x78, 0x56, 0x01 };
		memcpy(drsp, r, sizeof(r));
		drsp_len = sizeof(r);
	} else if (sid == 0x22) {   /* RDBI: lange Antwort -> FF + CF */
		int i;
		drsp[0] = 0x62;
		drsp[1] = dreq[1];
		drsp[2] = dreq[2];
		for (i = 0; i < 17; i++)
			drsp[3 + i] = (uint8_t)('A' + i);
		drsp_len = 20;
	} else if (sid == 0xB0) {   /* Assign NAD */
		drsp[0] = 0xF0;
		drsp_len = 1;
	} else {
		drsp[0] = 0x7F;
		drsp[1] = sid;
		drsp[2] = 0x11;         /* service not supported */
		drsp_len = 3;
	}
}

static void diag_on_3c(const uint8_t *b)
{
	int pci = b[1] >> 4, i;
	if (b[0] != vdiag_nad && b[0] != 0x7F)
		return;
	if (pci == 0) {
		dreq_len = b[1] & 0x0F;
		for (i = 0; i < dreq_len; i++)
			dreq[i] = b[2 + i];
		diag_make_response();
	} else if (pci == 1) {
		dreq_len = ((b[1] & 0x0F) << 8) | b[2];
		for (i = 0; i < 5; i++)
			dreq[i] = b[3 + i];
		dreq_got = 5;
		dreq_sn = 1;
		drsp_ready = 0;
	} else if (pci == 2) {
		if ((b[1] & 0x0F) != (dreq_sn & 0x0F))
			return;
		dreq_sn++;
		for (i = 0; i < 6 && dreq_got < dreq_len && dreq_got < 64; i++)
			dreq[dreq_got++] = b[2 + i];
		if (dreq_got >= dreq_len)
			diag_make_response();
	}
}

static void diag_on_3d_header(void)
{
	uint8_t f[8];
	int i;
	if (!drsp_ready)
		return;
	memset(f, 0xFF, 8);
	f[0] = (uint8_t)vdiag_nad;
	if (dreq[0] == 0x22 && drsp_pending_once && !drsp_sent_pending) {
		f[1] = 0x03; f[2] = 0x7F; f[3] = 0x22; f[4] = 0x78;
		drsp_sent_pending = 1;
		drsp_pending_once = 0;
	} else if (drsp_len <= 6) {
		f[1] = (uint8_t)drsp_len;
		for (i = 0; i < drsp_len; i++)
			f[2 + i] = drsp[i];
		drsp_ready = 0;
	} else if (drsp_off == 0) {
		f[1] = (uint8_t)(0x10 | (drsp_len >> 8));
		f[2] = (uint8_t)drsp_len;
		for (i = 0; i < 5; i++)
			f[3 + i] = drsp[i];
		drsp_off = 5;
	} else {
		f[1] = (uint8_t)(0x20 | (drsp_sn & 0x0F));
		drsp_sn++;
		for (i = 0; i < 6 && drsp_off < drsp_len; i++)
			f[2 + i] = drsp[drsp_off++];
		if (drsp_off >= drsp_len)
			drsp_ready = 0;
	}
	respond(NODE_DIAG, f, 8, 1, 0x3D);
}

static void node_frame_done(void)
{
	/* kompletter Frame gesehen: 3C-Requests an den Diagnose-Slave */
	if (nid == 0x3C && nn == 9 && vdiag_nad >= 0 &&
			nbuf[8] == luc_lin_checksum(0x3C, nbuf, 8, 1))
		diag_on_3c(nbuf);
}

static void nodes_on_symbol(int sym)
{
	if (sym == SYM_BREAK) {
		if (nst == 3 && nn)
			node_frame_done();
		nst = 1;
		nn = 0;
		responder = -1;
		return;
	}
	switch (nst) {
	case 1:
		nst = (sym == 0x55) ? 2 : 0;
		break;
	case 2:
		if (luc_lin_pid((uint8_t)(sym & 0x3F)) != sym) {
			nst = 0;
			break;
		}
		nid = sym & 0x3F;
		nst = 3;
		nn = 0;
		if (vslave[nid].used) {
			vslave_t *v = &vslave[nid];
			respond(NODE_SLAVE, v->data, v->len, v->classic, nid);
			if (v->count)
				v->data[0]++;
		} else if (nid == 0x3D && vdiag_nad >= 0) {
			diag_on_3d_header();
		}
		break;
	case 3:
		if (nn < 9)
			nbuf[nn++] = (uint8_t)sym;
		if (nn == 9) {
			node_frame_done();
			nst = 0;
		}
		break;
	default:
		break;
	}
}

static void vmaster_service(void)
{
	uint64_t now = now64();
	if (!vm_n || now < vm_next || bus_busy())
		return;
	{
		uint8_t id = vm_id[vm_idx];
		if (nst == 3 && nn)
			node_frame_done();
		q_put(NODE_MASTER, SYM_BREAK);
		q_put(NODE_MASTER, 0x55);
		q_put(NODE_MASTER, luc_lin_pid(id));
		vm_next = now + (uint64_t)vm_ms[vm_idx] * 1000u;
		vm_idx = (vm_idx + 1) % vm_n;
	}
}

/* ------------------------------------------------------------------------ */
/* main                                                                      */
/* ------------------------------------------------------------------------ */
static int hexbytes(const char *s, uint8_t *out, int max)
{
	int n = 0;
	while (s[0] && s[1] && n < max) {
		unsigned v;
		if (sscanf(s, "%2x", &v) != 1)
			break;
		out[n++] = (uint8_t)v;
		s += 2;
	}
	return n;
}

static volatile int running = 1;
static void on_sig(int s) { (void)s; running = 0; }

int main(int argc, char **argv)
{
	const char *link = NULL;
	int i;
	struct termios tio;

	t0 = mono_us();
	signal(SIGINT, on_sig);
	signal(SIGTERM, on_sig);

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--slave") && i + 1 < argc) {
			char *a = argv[++i], *eq = strchr(a, '=');
			unsigned id;
			int cnt = 0;
			if (!eq)
				continue;
			sscanf(a, "%x", &id);
			if (strchr(a, '+') && strchr(a, '+') < eq)
				cnt = 1;
			id &= 0x3F;
			vslave[id].used = 1;
			vslave[id].count = cnt;
			vslave[id].len = hexbytes(eq + 1, vslave[id].data, 8);
			vslave[id].classic = strstr(eq, ",C") || strstr(eq, ",c");
		} else if (!strcmp(argv[i], "--master") && i + 1 < argc) {
			char *a = strdup(argv[++i]), *tok = strtok(a, ",");
			while (tok && vm_n < 32) {
				unsigned id, ms = 20;
				sscanf(tok, "%x/%u", &id, &ms);
				vm_id[vm_n] = (uint8_t)(id & 0x3F);
				vm_ms[vm_n] = (uint8_t)ms;
				vm_n++;
				tok = strtok(NULL, ",");
			}
		} else if (!strcmp(argv[i], "--diag") && i + 1 < argc) {
			unsigned nad;
			sscanf(argv[++i], "%x", &nad);
			vdiag_nad = (int)nad;
		} else if (!strcmp(argv[i], "--link") && i + 1 < argc) {
			link = argv[++i];
		} else if (!strcmp(argv[i], "--gap") && i + 1 < argc) {
			slave_gap_us = (uint32_t)atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--ibgap") && i + 1 < argc) {
			slave_ibgap_us = (uint32_t)atoi(argv[++i]);
		} else if (!strcmp(argv[i], "--nobus")) {
			nobus = 1;
		}
	}

	pty_fd = posix_openpt(O_RDWR | O_NOCTTY);
	if (pty_fd < 0 || grantpt(pty_fd) || unlockpt(pty_fd)) {
		perror("pty");
		return 1;
	}
	{
		int sfd = open(ptsname(pty_fd), O_RDWR | O_NOCTTY);
		if (sfd >= 0) {
			tcgetattr(sfd, &tio);
			cfmakeraw(&tio);
			tcsetattr(sfd, TCSANOW, &tio);
			close(sfd);
		}
	}
	fcntl(pty_fd, F_SETFL, O_NONBLOCK);
	if (link) {
		unlink(link);
		if (symlink(ptsname(pty_fd), link))
			perror("symlink");
	}
	printf("%s\n", ptsname(pty_fd));
	fflush(stdout);

	luc_lin_init();
	luc_proto_init();

	while (running) {
		uint8_t buf[256];
		ssize_t r = read(pty_fd, buf, sizeof(buf));
		if (r > 0) {
			ssize_t k;
			for (k = 0; k < r; k++) {
				int n = (in_head + 1) % (int)sizeof(in_ring);
				if (n != in_tail) {
					in_ring[in_head] = buf[k];
					in_head = n;
				}
			}
		}
		{
			uint64_t real = mono_us() - t0;
			while (vnow < real) {
				uint64_t step = vnow + 50u;
				if (step > real)
					step = real;
				if (sym_active && sym_end < step)
					step = sym_end;
				if (step <= vnow)
					step = vnow + 1u;
				vnow = step;
				vmaster_service();
				bus_service();
				luc_lin_poll();
				luc_proto_poll();
				bus_service();
			}
		}
		usleep(50);
	}
	if (link)
		unlink(link);
	return 0;
}
