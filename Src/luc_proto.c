/*
 * luc_proto.c - ASCII-Host-Protokoll Version 1 (siehe docs/PROTOKOLL.md)
 *
 * Laeuft komplett in der Hauptschleife. Jeder Befehl erzeugt genau eine
 * '+'- oder '-'-Zeile. Mehrzeilige Ausgaben ('='-Zeilen) werden zeilenweise
 * ausgegeben, sobald im Sendepuffer Platz ist; solange wird kein neuer Befehl
 * angenommen, damit die Reihenfolge erhalten bleibt.
 */
#include "luc_proto.h"
#include "luc_lin.h"
#include "luc_port.h"
#include <string.h>

#define LINE_MAX  112u
#define TOK_MAX   20u
#define OUT_MAX   112u
#define OUT_RESERVE 120u   /* so viel Platz muss frei sein, bevor ein Befehl laeuft */

static char line[LINE_MAX];
static uint8_t line_len;
static uint8_t line_overflow;
static uint8_t line_ready;

/* Ausgabezeile */
static char ob[OUT_MAX];
static uint8_t ob_len;

/* mehrzeilige Ausgabe */
#define JOB_NONE  0u
#define JOB_LIST  1u
#define JOB_SCHED 2u
#define JOB_HELP  3u
static uint8_t job;
static uint8_t job_idx;
static uint8_t job_cnt;

static uint32_t last_overruns;

/* ------------------------------------------------------------------------ */
/* Ausgabe                                                                   */
/* ------------------------------------------------------------------------ */
static void o_begin(char c)
{
	ob[0] = c;
	ob_len = 1u;
}

static void o_ch(char c)
{
	if (ob_len < OUT_MAX - 2u)
		ob[ob_len++] = c;
}

static void o_str(const char *s)
{
	while (*s)
		o_ch(*s++);
}

static void o_hex8(uint8_t v)
{
	static const char hx[] = "0123456789ABCDEF";
	o_ch(hx[v >> 4]);
	o_ch(hx[v & 0x0Fu]);
}

static void o_hex32(uint32_t v)
{
	o_hex8((uint8_t)(v >> 24));
	o_hex8((uint8_t)(v >> 16));
	o_hex8((uint8_t)(v >> 8));
	o_hex8((uint8_t)v);
}

static void o_dec(uint32_t v)
{
	char tmp[10];
	uint8_t n = 0;
	do {
		tmp[n++] = (char)('0' + (v % 10u));
		v /= 10u;
	} while (v && n < sizeof(tmp));
	while (n)
		o_ch(tmp[--n]);
}

/* vorzeichenbehaftet, LUC_T_NONE als '-' */
static void o_sdec(int16_t v)
{
	if (v == (int16_t)LUC_T_NONE) {
		o_ch('-');
		return;
	}
	if (v < 0) {
		o_ch('-');
		v = (int16_t)-v;
	}
	o_dec((uint32_t)v);
}

static void o_hexbytes(const uint8_t *b, uint8_t n)
{
	uint8_t i;
	for (i = 0; i < n; i++)
		o_hex8(b[i]);
}

static void o_end(void)
{
	ob[ob_len++] = '\r';
	ob[ob_len++] = '\n';
	luc_port_out_write(ob, ob_len);
}

static void ok_simple(void)
{
	o_begin('+');
	o_str("OK");
	o_end();
}

static void err(int code)
{
	static const char *const txt[] = {
		"", "SYNTAX", "RANGE", "MODE", "FULL", "UNDEF", "BUSY", "UNKNOWN"
	};
	o_begin('-');
	o_str("ERR ");
	o_dec((uint32_t)code);
	o_ch(' ');
	o_str((code > 0 && code <= 7) ? txt[code] : "?");
	o_end();
}

static void reply(int rc)
{
	if (rc == LUC_OK)
		ok_simple();
	else
		err(rc);
}

/* ------------------------------------------------------------------------ */
/* Parser-Hilfen                                                             */
/* ------------------------------------------------------------------------ */
static int hexval(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

/* 1..2 Hexziffern */
static int p_hex8(const char *t, uint8_t *v)
{
	int a, b;
	if (!t || !t[0])
		return 0;
	a = hexval(t[0]);
	if (a < 0)
		return 0;
	if (!t[1]) {
		*v = (uint8_t)a;
		return 1;
	}
	b = hexval(t[1]);
	if (b < 0 || t[2])
		return 0;
	*v = (uint8_t)(a * 16 + b);
	return 1;
}

static int p_id(const char *t, uint8_t *v)
{
	return p_hex8(t, v) && *v <= 0x3Fu;
}

static int p_dec(const char *t, uint32_t *v)
{
	uint32_t r = 0;
	if (!t || !t[0])
		return 0;
	while (*t) {
		if (*t < '0' || *t > '9' || r > 100000000u)
			return 0;
		r = r * 10u + (uint32_t)(*t - '0');
		t++;
	}
	*v = r;
	return 1;
}

/* Hexbytes aus allen Tokens ab tok[k] (Tokens werden aneinandergehaengt) */
static int p_bytes(char **tok, uint8_t ntok, uint8_t k, uint8_t *out, uint8_t max, uint8_t *n)
{
	uint8_t cnt = 0;
	for (; k < ntok; k++) {
		const char *t = tok[k];
		while (*t) {
			int a = hexval(t[0]);
			int b = t[1] ? hexval(t[1]) : -1;
			if (a < 0 || b < 0 || cnt >= max)
				return 0;
			out[cnt++] = (uint8_t)(a * 16 + b);
			t += 2;
		}
	}
	*n = cnt;
	return 1;
}

static int is_word(const char *t, const char *w)
{
	return t && strcmp(t, w) == 0;
}

/* ------------------------------------------------------------------------ */
/* Befehle                                                                   */
/* ------------------------------------------------------------------------ */
static const char *const mode_names[] = { "OFF", "MASTER", "SLAVE", "MONITOR", "DIAG" };
static const char *const dir_names[] = { "-", "PUB", "ONCE", "SUB" };

static void cmd_mode(char **tok, uint8_t ntok)
{
	uint8_t i;
	if (ntok >= 2u) {
		for (i = 0; i < 5u; i++) {
			if (is_word(tok[1], mode_names[i]))
				break;
		}
		if (i == 5u) {
			err(LUC_E_RANGE);
			return;
		}
		luc_lin_set_mode(i);
	}
	o_begin('+');
	o_str("OK ");
	o_str(mode_names[luc_lin_get_mode()]);
	o_end();
}

static void cmd_baud(char **tok, uint8_t ntok)
{
	uint32_t b;
	if (ntok >= 2u) {
		int rc;
		if (!p_dec(tok[1], &b)) {
			err(LUC_E_SYNTAX);
			return;
		}
		rc = luc_lin_set_baud(b);
		if (rc != LUC_OK) {
			err(rc);
			return;
		}
	}
	o_begin('+');
	o_str("OK ");
	o_dec(luc_lin_get_baud());
	o_end();
}

/* FRAME <id> <dir> <len> [E|C] [data] */
static void cmd_frame(char **tok, uint8_t ntok)
{
	uint8_t id, dir, n, k = 4u, classic = 0;
	uint32_t len;
	uint8_t data[8];

	if (ntok < 4u || !p_id(tok[1], &id) || !p_dec(tok[3], &len)) {
		err(LUC_E_SYNTAX);
		return;
	}
	if (is_word(tok[2], "PUB"))
		dir = LUC_DIR_PUB;
	else if (is_word(tok[2], "ONCE"))
		dir = LUC_DIR_ONCE;
	else if (is_word(tok[2], "SUB"))
		dir = LUC_DIR_SUB;
	else {
		err(LUC_E_RANGE);
		return;
	}
	if (ntok > k && (is_word(tok[k], "E") || is_word(tok[k], "C"))) {
		classic = (uint8_t)(tok[k][0] == 'C');
		k++;
	}
	if (len < 1u || len > 8u) {
		err(LUC_E_RANGE);
		return;
	}
	if (!p_bytes(tok, ntok, k, data, (uint8_t)len, &n)) {
		err(LUC_E_SYNTAX);
		return;
	}
	reply(luc_lin_frame_set(id, dir, (uint8_t)len, classic, data, n));
}

static void cmd_data(char **tok, uint8_t ntok)
{
	uint8_t id, n, data[8];
	if (ntok < 3u || !p_id(tok[1], &id) || !p_bytes(tok, ntok, 2u, data, 8u, &n)) {
		err(LUC_E_SYNTAX);
		return;
	}
	reply(luc_lin_frame_data(id, data, n));
}

static void cmd_del(char **tok, uint8_t ntok)
{
	uint8_t id;
	if (ntok == 2u && is_word(tok[1], "*")) {
		luc_lin_frame_del_all();
		ok_simple();
		return;
	}
	if (ntok != 2u || !p_id(tok[1], &id)) {
		err(LUC_E_SYNTAX);
		return;
	}
	luc_lin_frame_del(id);
	ok_simple();
}

/* SCHED <id>[/ms] ... | SCHED ? | SCHED - */
static void cmd_sched(char **tok, uint8_t ntok)
{
	uint8_t ids[LUC_SCHED_MAX], ms[LUC_SCHED_MAX];
	uint8_t i, n = 0;
	int rc;

	if (ntok == 2u && is_word(tok[1], "?")) {
		job = JOB_SCHED;
		job_idx = 0;
		return;
	}
	if (!(ntok == 2u && is_word(tok[1], "-"))) {
		for (i = 1; i < ntok; i++) {
			char *t = tok[i];
			char *slash = strchr(t, '/');
			uint32_t v;
			luc_frame_t f;
			if (n >= LUC_SCHED_MAX) {
				err(LUC_E_FULL);
				return;
			}
			if (slash)
				*slash = 0;
			if (!p_id(t, &ids[n])) {
				err(LUC_E_SYNTAX);
				return;
			}
			if (slash) {
				if (!p_dec(slash + 1, &v) || v == 0u || v > 255u) {
					err(LUC_E_RANGE);
					return;
				}
				ms[n] = (uint8_t)v;
			} else {
				luc_lin_frame_get(ids[n], &f);
				ms[n] = luc_lin_slot_ms(f.len);
			}
			n++;
		}
	}
	rc = luc_lin_sched_set(ids, ms, n);
	if (rc != LUC_OK) {
		err(rc);
		return;
	}
	o_begin('+');
	o_str("OK ");
	o_dec(n);
	o_end();
}

/* SEND <id> [E|C] [data] */
static void cmd_send(char **tok, uint8_t ntok)
{
	uint8_t id, n, k = 2u, data[8];
	int8_t classic = -1;
	if (ntok < 2u || !p_id(tok[1], &id)) {
		err(LUC_E_SYNTAX);
		return;
	}
	if (ntok > k && (is_word(tok[k], "E") || is_word(tok[k], "C"))) {
		classic = (int8_t)(tok[k][0] == 'C');
		k++;
	}
	if (!p_bytes(tok, ntok, k, data, 8u, &n)) {
		err(LUC_E_SYNTAX);
		return;
	}
	reply(luc_lin_send(id, classic, data, n));
}

static void cmd_dreq(char **tok, uint8_t ntok)
{
	uint8_t nad, n, data[LUC_DIAG_REQ_MAX];
	if (ntok < 3u || !p_hex8(tok[1], &nad) ||
			!p_bytes(tok, ntok, 2u, data, LUC_DIAG_REQ_MAX, &n)) {
		err(LUC_E_SYNTAX);
		return;
	}
	reply(luc_lin_diag_request(nad, data, n));
}

static void cmd_opt(char **tok, uint8_t ntok)
{
	uint32_t v;
	if (ntok == 1u) {
		o_begin('+');
		o_str("OK ECHO=");
		o_dec(luc_opt.echo);
		o_str(" ALL=");
		o_dec(luc_opt.all);
		o_str(" NR=");
		o_dec(luc_opt.nr);
		o_str(" STRICT=");
		o_dec(luc_opt.strict);
		o_str(" TIMING=");
		o_dec(luc_opt.timing);
		o_str(" DSLOT=");
		o_dec(luc_opt.dslot_ms);
		o_str(" P2=");
		o_dec(luc_opt.p2_ms);
		o_end();
		return;
	}
	if (ntok != 3u || !p_dec(tok[2], &v)) {
		err(LUC_E_SYNTAX);
		return;
	}
	if (is_word(tok[1], "ECHO") && v <= 1u)
		luc_opt.echo = (uint8_t)v;
	else if (is_word(tok[1], "ALL") && v <= 1u)
		luc_opt.all = (uint8_t)v;
	else if (is_word(tok[1], "NR") && v <= 1u)
		luc_opt.nr = (uint8_t)v;
	else if (is_word(tok[1], "STRICT") && v <= 1u)
		luc_opt.strict = (uint8_t)v;
	else if (is_word(tok[1], "TIMING") && v <= 1u)
		luc_opt.timing = (uint8_t)v;
	else if (is_word(tok[1], "DSLOT") && v >= 1u && v <= 255u)
		luc_opt.dslot_ms = (uint8_t)v;
	else if (is_word(tok[1], "P2") && v >= 10u && v <= 60000u)
		luc_opt.p2_ms = (uint16_t)v;
	else {
		err(LUC_E_RANGE);
		return;
	}
	ok_simple();
}

static void cmd_stat(void)
{
	o_begin('+');
	o_str("OK FR=");
	o_dec(luc_stats.frames);
	o_str(" ERR=");
	o_dec(luc_stats.errors);
	o_str(" OVR=");
	o_dec(luc_stats.overruns);
	o_end();
}

/* ------------------------------------------------------------------------ */
/* Hilfe (liegt komplett im Flash, jede Zeile wird mit '=' ausgegeben)       */
/* ------------------------------------------------------------------------ */
static const char *const help_txt[] = {
	"VER SN RESET STAT HELP [befehl]",
	"BAUD [n]  MODE [OFF|MASTER|SLAVE|MONITOR|DIAG]",
	"FRAME id PUB|ONCE|SUB len [E|C] [data]",
	"DATA id data  DEL id|*  LIST",
	"SCHED id[/ms] ...|?|-  SEND id [E|C] [data]",
	"WAKE SLEEP DREQ nad data  OPT [name val]",
	"Details: HELP <befehl>   Ausgaben: HELP F, HELP D",
	0
};
static const char *const h_mode[] = {
	"MODE [m]  Betriebsart setzen/abfragen",
	" OFF      Bus passiv",
	" MASTER   Header nach Schedule, Antworten melden",
	" SLAVE    auf Header antworten (PUB/ONCE)",
	" MONITOR  nur mithoeren, SEND/WAKE erlaubt",
	" DIAG     Master nur fuer DREQ und SEND",
	0
};
static const char *const h_baud[] = {
	"BAUD [n]  LIN-Baudrate 1000..115200, Default 19200",
	" ohne Argument: abfragen. Bricht laufenden Frame ab.",
	0
};
static const char *const h_frame[] = {
	"FRAME id dir len [E|C] [data]  Frame definieren",
	" id 00..3F (hex), len 1..8",
	" PUB  Adapter liefert die Antwort",
	" ONCE wie PUB, aber nur einmal je DATA",
	" SUB  Antwort von anderem Knoten (Laenge/Checksumme)",
	" E enhanced (Default), C classic; 3C/3D immer classic",
	" fehlende Datenbytes = 00",
	0
};
static const char *const h_data[] = {
	"DATA id data  Daten von PUB/ONCE atomar ersetzen",
	" genau len Bytes, z.B. DATA 21 510005DC00140400",
	" ONCE: Antwort scharf schalten (LIST zeigt ARMED)",
	0
};
static const char *const h_list[] = {
	"LIST  Frame-Tabelle: =FRAME id dir len E|C data [ARMED]",
	"DEL id | DEL *  Frame bzw. alle Frames loeschen",
	0
};
static const char *const h_sched[] = {
	"SCHED id[/ms] ...  Master-Schedule ersetzen (max. 32)",
	" ms = Slotzeit, Default: max. Framezeit + Reserve",
	" SCHED ?  auslesen    SCHED -  leeren",
	0
};
static const char *const h_send[] = {
	"SEND id [E|C] [data]  Einmal-Frame (MASTER/MONITOR/DIAG)",
	" ohne data: nur Header, Antwort kommt vom Bus",
	" mit data:  kompletter Frame vom Adapter",
	" Ergebnis als !F, auch bei ECHO=0",
	"SLEEP  Go-to-Sleep (3C 00 FF..)    WAKE  Wakeup-Puls",
	0
};
static const char *const h_dreq[] = {
	"DREQ nad data  Diagnose-Request (MASTER/DIAG)",
	" data = SID + Parameter, 1..40 Bytes; NAD 7F = alle",
	" 3C als SF bzw. FF/CF, danach 3D-Polling im DSLOT-Abstand",
	" 7F sid 78 (response pending) verlaengert auf 5 s",
	" Ergebnis: !D nad OK data | NRC sid nrc | TO",
	0
};
static const char *const h_opt[] = {
	"OPT [name wert]  Optionen anzeigen/setzen (Default)",
	" ECHO 0|1    eigene Antworten melden (1)",
	" ALL 0|1     nicht definierte IDs melden (1)",
	" NR 0|1      Header ohne Antwort melden (1)",
	" STRICT 0|1  Checksummenart muss zu FRAME passen (0)",
	" TIMING 0|1  Zeitwerte an !F anhaengen (0)",
	" DSLOT ms    Abstand Diagnose-Frames (10)",
	" P2 ms       Diagnose-Timeout (1000)",
	0
};
static const char *const h_stat[] = {
	"STAT   FR=gemeldete Frames ERR=Busfehler OVR=verlorene Ereignisse",
	"VER    Firmware- und Protokollversion   SN  Seriennummer",
	"RESET  Modus OFF, Tabellen leer, Defaults",
	0
};
static const char *const h_f[] = {
	"!F t_us id src len data cs st [H R I F]",
	" src: T gesendet, R empfangen, - keine Antwort",
	" st:  E/C ok (enhanced/classic), CS Checksumme,",
	"      NR keine Antwort, TO unvollstaendig, BIT Bitfehler",
	" TIMING (us): H Header, R Antwortpause,",
	"      I max. Byte-Pause, F Framedauer",
	0
};
static const char *const h_d[] = {
	"!D nad OK data       Diagnose-Antwort (RSID + Daten)",
	"!D nad NRC sid nrc   negative Antwort",
	"!D nad TO            keine Antwort innerhalb P2",
	"!E OVR n             n Ereignisse verloren",
	0
};

static const struct {
	const char *name;
	const char *const *txt;
} help_tab[] = {
	{ "MODE", h_mode }, { "BAUD", h_baud }, { "FRAME", h_frame },
	{ "DATA", h_data }, { "LIST", h_list }, { "DEL", h_list },
	{ "SCHED", h_sched }, { "SEND", h_send }, { "SLEEP", h_send },
	{ "WAKE", h_send }, { "DREQ", h_dreq }, { "OPT", h_opt },
	{ "STAT", h_stat }, { "VER", h_stat }, { "SN", h_stat },
	{ "RESET", h_stat }, { "F", h_f }, { "!F", h_f }, { "D", h_d },
	{ "!D", h_d }, { "E", h_d }, { "!E", h_d },
};
static const char *const *job_help;

static void execute(void)
{
	char *tok[TOK_MAX];
	uint8_t ntok = 0;
	char *p = line;
	char *c0;

	/* Grossschreibung und Zerlegen */
	for (p = line; *p; p++) {
		if (*p >= 'a' && *p <= 'z')
			*p = (char)(*p - 'a' + 'A');
	}
	p = line;
	while (*p && ntok < TOK_MAX) {
		while (*p == ' ' || *p == '\t')
			*p++ = 0;
		if (!*p)
			break;
		tok[ntok++] = p;
		while (*p && *p != ' ' && *p != '\t')
			p++;
	}
	if (ntok == 0u)
		return;
	while (*p == ' ' || *p == '\t')
		p++;
	if (*p) {               /* zu viele Tokens */
		err(LUC_E_SYNTAX);
		return;
	}
	c0 = tok[0];

	if (is_word(c0, "VER")) {
		o_begin('+');
		o_str("OK LUC-LIN " LUC_FW_VERSION " P");
		o_dec(LUC_PROTO_VERSION);
		o_end();
	} else if (is_word(c0, "SN")) {
		o_begin('+');
		o_str("OK ");
		o_hex32(luc_port_serial());
		o_end();
	} else if (is_word(c0, "RESET")) {
		luc_lin_reset();
		ok_simple();
	} else if (is_word(c0, "BAUD")) {
		cmd_baud(tok, ntok);
	} else if (is_word(c0, "MODE")) {
		cmd_mode(tok, ntok);
	} else if (is_word(c0, "FRAME")) {
		cmd_frame(tok, ntok);
	} else if (is_word(c0, "DATA")) {
		cmd_data(tok, ntok);
	} else if (is_word(c0, "DEL")) {
		cmd_del(tok, ntok);
	} else if (is_word(c0, "LIST")) {
		job = JOB_LIST;
		job_idx = 0;
		job_cnt = 0;
	} else if (is_word(c0, "SCHED")) {
		cmd_sched(tok, ntok);
	} else if (is_word(c0, "SEND")) {
		cmd_send(tok, ntok);
	} else if (is_word(c0, "SLEEP")) {
		static const uint8_t gts[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
		reply(luc_lin_send(0x3Cu, 1, gts, 8u));
	} else if (is_word(c0, "WAKE")) {
		reply(luc_lin_wake());
	} else if (is_word(c0, "DREQ")) {
		cmd_dreq(tok, ntok);
	} else if (is_word(c0, "OPT")) {
		cmd_opt(tok, ntok);
	} else if (is_word(c0, "STAT")) {
		cmd_stat();
	} else if (is_word(c0, "HELP") || is_word(c0, "?")) {
		uint8_t i;
		job_help = help_txt;
		if (ntok >= 2u) {
			job_help = 0;
			for (i = 0; i < sizeof(help_tab) / sizeof(help_tab[0]); i++) {
				if (is_word(tok[1], help_tab[i].name)) {
					job_help = help_tab[i].txt;
					break;
				}
			}
		}
		if (job_help) {
			job = JOB_HELP;
			job_idx = 0;
		} else {
			err(LUC_E_RANGE);
		}
	} else {
		err(LUC_E_UNKNOWN);
	}
}

/* ------------------------------------------------------------------------ */
/* mehrzeilige Ausgaben                                                      */
/* ------------------------------------------------------------------------ */

static void job_step(void)
{
	if (job == JOB_LIST) {
		while (job_idx < 64u) {
			luc_frame_t f;
			uint8_t id = job_idx++;
			luc_lin_frame_get(id, &f);
			if (f.dir == LUC_DIR_NONE)
				continue;
			o_begin('=');
			o_str("FRAME ");
			o_hex8(id);
			o_ch(' ');
			o_str(dir_names[f.dir]);
			o_ch(' ');
			o_dec(f.len);
			o_str(f.classic ? " C " : " E ");
			o_hexbytes(f.data, f.len);
			if (f.dir == LUC_DIR_ONCE && f.armed)
				o_str(" ARMED");      /* Antwort liegt bereit, noch nicht abgeholt */
			o_end();
			job_cnt++;
			return;
		}
		o_begin('+');
		o_str("OK ");
		o_dec(job_cnt);
		o_end();
		job = JOB_NONE;
	} else if (job == JOB_SCHED) {
		uint8_t i, id, ms, n = luc_lin_sched_count();
		o_begin('=');
		o_str("SCHED");
		for (i = 0; i < n && ob_len < OUT_MAX - 12u; i++) {
			luc_lin_sched_get(i, &id, &ms);
			o_ch(' ');
			o_hex8(id);
			o_ch('/');
			o_dec(ms);
		}
		o_end();
		if (n > i)    /* sehr lange Schedules: Rest in weiterer Zeile */
		{
			o_begin('=');
			o_str("SCHED");
			for (; i < n; i++) {
				luc_lin_sched_get(i, &id, &ms);
				o_ch(' ');
				o_hex8(id);
				o_ch('/');
				o_dec(ms);
			}
			o_end();
		}
		o_begin('+');
		o_str("OK ");
		o_dec(n);
		o_end();
		job = JOB_NONE;
	} else if (job == JOB_HELP) {
		if (job_help[job_idx]) {
			o_begin('=');
			o_str(job_help[job_idx]);
			o_end();
			job_idx++;
		} else {
			ok_simple();
			job = JOB_NONE;
		}
	}
}

/* ------------------------------------------------------------------------ */
/* Ereignisse                                                                */
/* ------------------------------------------------------------------------ */
static void out_frame(const luc_event_t *e)
{
	static const char *const st[] = { "E", "C", "CS", "NR", "TO", "BIT" };
	uint8_t dlen = e->n ? (uint8_t)(e->n - 1u) : 0u;
	uint8_t has_cs = 1u;

	/* unvollstaendig ohne erkennbare Checksumme: alle Bytes als Daten */
	if (e->st == LUC_ST_TO || e->st == LUC_ST_BIT) {
		dlen = e->n;
		has_cs = 0;
	}
	if (dlen > 8u)
		dlen = 8u;

	o_begin('!');
	o_str("F ");
	o_dec(e->t_us);
	o_ch(' ');
	o_hex8(e->id);
	o_ch(' ');
	o_ch((char)e->src);
	o_ch(' ');
	o_dec(dlen);
	o_ch(' ');
	if (dlen)
		o_hexbytes(e->b, dlen);
	else
		o_ch('-');
	o_ch(' ');
	if (e->n && has_cs)
		o_hex8(e->b[e->n - 1u]);
	else
		o_str("--");
	o_ch(' ');
	o_str(st[e->st]);
	if (luc_opt.timing) {
		o_ch(' ');
		if (e->t_hdr != 0xFFFFu)
			o_dec(e->t_hdr);
		else
			o_ch('-');
		o_ch(' ');
		o_sdec(e->t_rs);
		o_ch(' ');
		o_sdec(e->t_ib);
		o_ch(' ');
		if (e->t_frm != 0xFFFFu)
			o_dec(e->t_frm);
		else
			o_ch('-');
	}
	o_end();
}

static void out_diag(const luc_diag_result_t *r)
{
	o_begin('!');
	o_str("D ");
	o_hex8(r->nad);
	if (r->status == LUC_DG_TO) {
		o_str(" TO");
	} else if (r->status == LUC_DG_NRC) {
		o_str(" NRC ");
		o_hex8(r->len > 1u ? r->data[1] : 0u);
		o_ch(' ');
		o_hex8(r->len > 2u ? r->data[2] : 0u);
	} else {
		o_str(" OK ");
		o_hexbytes(r->data, r->len > 48u ? 48u : r->len);
	}
	o_end();
}

/* ------------------------------------------------------------------------ */
/* Hauptschleife                                                             */
/* ------------------------------------------------------------------------ */
void luc_proto_init(void)
{
	line_len = 0;
	line_overflow = 0;
	line_ready = 0;
	job = JOB_NONE;
	last_overruns = 0;
}

void luc_proto_poll(void)
{
	luc_event_t e;
	luc_diag_result_t dr;
	int c;

	/* 1. Ereignisse haben Vorrang, solange Platz ist */
	while (luc_port_out_space() >= OUT_MAX && luc_lin_event_pop(&e))
		out_frame(&e);

	if (luc_port_out_space() >= OUT_MAX && luc_lin_diag_pop(&dr))
		out_diag(&dr);

	if (luc_stats.overruns != last_overruns && luc_port_out_space() >= OUT_MAX) {
		o_begin('!');
		o_str("E OVR ");
		o_dec(luc_stats.overruns - last_overruns);
		o_end();
		last_overruns = luc_stats.overruns;
	}

	/* 2. laufende mehrzeilige Ausgabe fortsetzen */
	if (job != JOB_NONE) {
		if (luc_port_out_space() >= OUT_RESERVE)
			job_step();
		return;
	}

	/* 3. Befehlszeile einlesen */
	while (!line_ready && (c = luc_port_in_getc()) >= 0) {
		if (c == '\r' || c == '\n') {
			if (line_len || line_overflow)
				line_ready = 1u;
		} else if (line_len < LINE_MAX - 1u) {
			line[line_len++] = (char)c;
		} else {
			line_overflow = 1u;
		}
	}

	/* 4. ausfuehren, wenn die Antwort sicher Platz hat */
	if (line_ready && luc_port_out_space() >= OUT_RESERVE) {
		line[line_len] = 0;
		if (line_overflow)
			err(LUC_E_SYNTAX);
		else
			execute();
		line_len = 0;
		line_overflow = 0;
		line_ready = 0;
	}
}
