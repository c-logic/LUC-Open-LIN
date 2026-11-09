#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
LUC-LIN Studio - grafische Oberflaeche fuer den LUC-LIN-Adapter

Funktionen: Trace (chronologisch / je ID), Frame-Tabelle mit Live-Daten,
Schedule, Einmal-Senden mit Wiederholung, Diagnose, Optionen, Rohkonsole,
Projekte (JSON) und Trace-Dateien.

Start:  python luclin_gui.py [PORT]
Benoetigt: pyserial
"""
from __future__ import annotations

import json
import os
import queue
import sys
import threading
import time
import tkinter as tk
from tkinter import ttk, filedialog, messagebox
from tkinter.scrolledtext import ScrolledText

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from luclin import LucLin, LucError, Frame, list_ports  # noqa: E402
from luclin.device import STATUS_TEXT  # noqa: E402

APP = "LUC-LIN Studio"
MODES = ("OFF", "MASTER", "SLAVE", "MONITOR", "DIAG")
MODE_HINT = {
    "OFF": "Bus passiv",
    "MASTER": "Schedule senden, Antworten empfangen",
    "SLAVE": "auf Header antworten (PUB/ONCE)",
    "MONITOR": "nur mithoeren, Senden moeglich",
    "DIAG": "nur Diagnose (3C/3D) und Senden",
}
MAX_TRACE_ROWS = 20000

C = dict(
    bg="#eef1f4", panel="#ffffff", line="#d3dae1", text="#17212b", muted="#5b6b79",
    accent="#0b7c9c", accent_dk="#085e77", ok="#2f8a4d", warn="#a0700f", err="#bd4133",
    tx="#0b5fa8", nr="#8a97a3", sel="#d6ecf3",
)


def parse_hex(s: str) -> bytes:
    s = s.replace(" ", "").replace(",", "").replace("0x", "").replace("0X", "")
    if len(s) % 2:
        raise ValueError("ungerade Anzahl Hexziffern")
    return bytes.fromhex(s)


def parse_id(s: str) -> int:
    v = int(s.strip().replace("0x", ""), 16)
    if not 0 <= v <= 0x3F:
        raise ValueError("ID muss 00..3F sein")
    return v


def hexs(b: bytes) -> str:
    return b.hex(" ").upper()


# ---------------------------------------------------------------------- #
# Gestaltung
# ---------------------------------------------------------------------- #
def setup_style(root: tk.Tk):
    st = ttk.Style(root)
    try:
        st.theme_use("clam")
    except tk.TclError:
        pass
    base = ("Segoe UI", 10) if sys.platform == "win32" else ("DejaVu Sans", 10)
    mono = ("Consolas", 10) if sys.platform == "win32" else ("DejaVu Sans Mono", 10)
    root.option_add("*Font", base)
    root.configure(bg=C["bg"])
    st.configure(".", background=C["bg"], foreground=C["text"], font=base,
                 bordercolor=C["line"], lightcolor=C["panel"], darkcolor=C["line"])
    st.configure("TFrame", background=C["bg"])
    st.configure("Card.TFrame", background=C["panel"], relief="flat")
    st.configure("TLabel", background=C["bg"])
    st.configure("Card.TLabel", background=C["panel"])
    st.configure("Muted.TLabel", foreground=C["muted"])
    st.configure("CardMuted.TLabel", background=C["panel"], foreground=C["muted"])
    st.configure("Title.TLabel", font=(base[0], 13, "bold"))
    st.configure("H.TLabel", background=C["panel"], font=(base[0], 10, "bold"))
    st.configure("TLabelframe", background=C["panel"], bordercolor=C["line"])
    st.configure("TLabelframe.Label", background=C["panel"], foreground=C["muted"],
                 font=(base[0], 9, "bold"))
    st.configure("TNotebook", background=C["bg"], borderwidth=0)
    st.configure("TNotebook.Tab", padding=(14, 6), background=C["bg"], foreground=C["muted"])
    st.map("TNotebook.Tab", background=[("selected", C["panel"])],
           foreground=[("selected", C["text"])])
    st.configure("TButton", padding=(10, 4), background=C["panel"])
    st.map("TButton", background=[("active", C["sel"])])
    st.configure("Accent.TButton", background=C["accent"], foreground="white")
    st.map("Accent.TButton", background=[("active", C["accent_dk"]), ("disabled", C["line"])])
    st.configure("Mode.Toolbutton", padding=(12, 5), background=C["panel"], foreground=C["muted"])
    st.map("Mode.Toolbutton", background=[("selected", C["accent"]), ("active", C["sel"])],
           foreground=[("selected", "white")])
    st.configure("Treeview", background=C["panel"], fieldbackground=C["panel"],
                 rowheight=22, font=mono, bordercolor=C["line"])
    st.configure("Treeview.Heading", background=C["bg"], foreground=C["muted"],
                 font=(base[0], 9, "bold"), relief="flat")
    st.map("Treeview", background=[("selected", C["sel"])], foreground=[("selected", C["text"])])
    st.configure("TCheckbutton", background=C["panel"])
    st.configure("TRadiobutton", background=C["panel"])
    st.configure("Bar.TCheckbutton", background=C["bg"])
    st.configure("Status.TLabel", background=C["panel"], foreground=C["muted"], padding=(8, 3))
    return mono


class Card(ttk.Frame):
    """weisser Bereich mit Ueberschrift"""

    def __init__(self, master, title=None, **kw):
        super().__init__(master, style="Card.TFrame", padding=12, **kw)
        if title:
            ttk.Label(self, text=title, style="H.TLabel").pack(anchor="w", pady=(0, 8))


# ---------------------------------------------------------------------- #
# Hauptfenster
# ---------------------------------------------------------------------- #
class Studio(tk.Tk):
    def __init__(self, port=None):
        super().__init__()
        self.title(APP)
        self.geometry("1280x820")
        self.minsize(1000, 640)
        self.mono = setup_style(self)

        self.dev: LucLin | None = None
        self.rxq: "queue.Queue" = queue.Queue()
        self.project_path: str | None = None

        # Modell (gilt auch ohne Geraet)
        self.frames: dict[int, dict] = {}
        self.sched: list[list[int]] = []          # [id, ms]
        self.opts = dict(echo=1, all=1, nr=1, dslot=10, p2=1000)

        # Trace
        self.paused = tk.BooleanVar(value=False)
        self.autoscroll = tk.BooleanVar(value=True)
        self.per_id = tk.BooleanVar(value=False)
        self.trace_filter = tk.StringVar()
        self.trace_rows: list[Frame] = []
        self.id_rows: dict[int, str] = {}
        self.id_last: dict[int, Frame] = {}
        self.id_count: dict[int, int] = {}
        self.last_t_by_id: dict[int, int] = {}
        self.rate_cnt = 0
        self.rate = 0.0
        self.err_cnt = 0

        self.repeat_job = None
        self._build()
        self._refresh_ports()
        if port:
            self.port_var.set(port)
        self.after(100, self._pump)
        self.after(1000, self._tick_rate)
        self.protocol("WM_DELETE_WINDOW", self._quit)

    # ------------------------------------------------------------------ #
    # Aufbau
    # ------------------------------------------------------------------ #
    def _build(self):
        self._build_menu()

        top = ttk.Frame(self, padding=(12, 10, 12, 6))
        top.pack(fill="x")
        ttk.Label(top, text="LUC-LIN", style="Title.TLabel").pack(side="left")
        ttk.Label(top, text=" Studio", style="Muted.TLabel").pack(side="left", padx=(0, 18))

        ttk.Label(top, text="Port").pack(side="left")
        self.port_var = tk.StringVar()
        self.port_cb = ttk.Combobox(top, textvariable=self.port_var, width=14)
        self.port_cb.pack(side="left", padx=(4, 2))
        ttk.Button(top, text="⟳", width=3, command=self._refresh_ports).pack(side="left")
        self.btn_conn = ttk.Button(top, text="Verbinden", style="Accent.TButton",
                                   command=self._toggle_connect)
        self.btn_conn.pack(side="left", padx=(6, 16))

        ttk.Label(top, text="Baud").pack(side="left")
        self.baud_var = tk.StringVar(value="19200")
        cb = ttk.Combobox(top, textvariable=self.baud_var, width=7,
                          values=["2400", "9600", "10417", "19200", "20000"])
        cb.pack(side="left", padx=(4, 16))
        cb.bind("<<ComboboxSelected>>", lambda e: self._set_baud())
        cb.bind("<Return>", lambda e: self._set_baud())

        self.mode_var = tk.StringVar(value="OFF")
        mb = ttk.Frame(top)
        mb.pack(side="left")
        for m in MODES:
            ttk.Radiobutton(mb, text=m.capitalize() if m != "DIAG" else "Diag", value=m,
                            variable=self.mode_var, style="Mode.Toolbutton",
                            command=self._set_mode).pack(side="left", padx=1)
        self.mode_hint = ttk.Label(top, text=MODE_HINT["OFF"], style="Muted.TLabel")
        self.mode_hint.pack(side="left", padx=12)

        self.nb = ttk.Notebook(self)
        self.nb.pack(fill="both", expand=True, padx=12, pady=(0, 6))
        self._tab_trace()
        self._tab_frames()
        self._tab_sched()
        self._tab_send()
        self._tab_diag()
        self._tab_opts()
        self._tab_console()

        sb = ttk.Frame(self, style="Card.TFrame")
        sb.pack(fill="x", side="bottom")
        self.status = tk.StringVar(value="nicht verbunden")
        self.stat_r = tk.StringVar(value="")
        ttk.Label(sb, textvariable=self.status, style="Status.TLabel").pack(side="left")
        ttk.Label(sb, textvariable=self.stat_r, style="Status.TLabel").pack(side="right")

    def _build_menu(self):
        m = tk.Menu(self)
        f = tk.Menu(m, tearoff=0)
        f.add_command(label="Projekt öffnen…", command=self._project_open, accelerator="Ctrl+O")
        f.add_command(label="Projekt speichern", command=self._project_save, accelerator="Ctrl+S")
        f.add_command(label="Projekt speichern unter…", command=lambda: self._project_save(True))
        f.add_separator()
        f.add_command(label="Trace speichern…", command=self._trace_save)
        f.add_command(label="Trace laden…", command=self._trace_load)
        f.add_separator()
        f.add_command(label="Beenden", command=self._quit)
        m.add_cascade(label="Datei", menu=f)
        g = tk.Menu(m, tearoff=0)
        g.add_command(label="Projekt aufs Gerät übertragen", command=self._push_all)
        g.add_command(label="Konfiguration vom Gerät lesen", command=self._pull_all)
        g.add_separator()
        g.add_command(label="Gerät zurücksetzen (RESET)", command=self._dev_reset)
        m.add_cascade(label="Gerät", menu=g)
        h = tk.Menu(m, tearoff=0)
        h.add_command(label="Über…", command=lambda: messagebox.showinfo(
            APP, "LUC-LIN Studio\nOberfläche für den LUC-LIN-Adapter\nProtokoll Version 1"))
        m.add_cascade(label="Hilfe", menu=h)
        self.config(menu=m)
        self.bind_all("<Control-o>", lambda e: self._project_open())
        self.bind_all("<Control-s>", lambda e: self._project_save())

    # --- Trace --------------------------------------------------------- #
    def _tab_trace(self):
        tab = ttk.Frame(self.nb, padding=(0, 8, 0, 0))
        self.nb.add(tab, text="Trace")
        bar = ttk.Frame(tab)
        bar.pack(fill="x", pady=(0, 6))
        ttk.Checkbutton(bar, text="Pause", variable=self.paused, style="Bar.TCheckbutton").pack(side="left")
        ttk.Checkbutton(bar, text="Auto-Scroll", variable=self.autoscroll,
                        style="Bar.TCheckbutton").pack(side="left", padx=8)
        ttk.Checkbutton(bar, text="Je ID (feste Ansicht)", variable=self.per_id,
                        style="Bar.TCheckbutton", command=self._trace_view_changed).pack(side="left", padx=8)
        ttk.Label(bar, text="Filter").pack(side="left", padx=(16, 4))
        e = ttk.Entry(bar, textvariable=self.trace_filter, width=22, font=self.mono)
        e.pack(side="left")
        e.bind("<Return>", lambda ev: self._trace_rebuild())
        ttk.Label(bar, text="z.B. 21 2F  oder  !3C !3D", style="Muted.TLabel").pack(side="left", padx=6)
        ttk.Button(bar, text="Leeren", command=self._trace_clear).pack(side="right")
        ttk.Button(bar, text="Speichern…", command=self._trace_save).pack(side="right", padx=6)

        wrap = ttk.Frame(tab, style="Card.TFrame")
        wrap.pack(fill="both", expand=True)
        self.tr_cols = ("t", "dt", "id", "src", "len", "data", "cs", "st", "gap", "ib")
        self.tr = ttk.Treeview(wrap, columns=self.tr_cols, show="headings", selectmode="browse")
        heads = dict(t="Zeit [s]", dt="Δt ID [ms]", id="ID", src="Quelle", len="Len",
                     data="Daten", cs="CS", st="Status", gap="Pause [µs]", ib="Byte-P. [µs]")
        widths = dict(t=130, dt=80, id=40, src=85, len=35, data=220, cs=35, st=195, gap=80, ib=90)
        for c in self.tr_cols:
            self.tr.heading(c, text=heads[c], anchor="w")
            self.tr.column(c, width=widths[c], stretch=(c == "data"), anchor="w")
        vs = ttk.Scrollbar(wrap, orient="vertical", command=self.tr.yview)
        self.tr.configure(yscrollcommand=vs.set)
        self.tr.pack(side="left", fill="both", expand=True)
        vs.pack(side="right", fill="y")
        self.tr.tag_configure("T", foreground=C["tx"])
        self.tr.tag_configure("NR", foreground=C["nr"])
        self.tr.tag_configure("ERR", foreground=C["err"])
        self.tr.tag_configure("chg", background="#fff4dc")

    # --- Frames -------------------------------------------------------- #
    def _tab_frames(self):
        tab = ttk.Frame(self.nb, padding=(0, 8, 0, 0))
        self.nb.add(tab, text="Frames")
        left = Card(tab, "Frame-Tabelle des Adapters")
        left.pack(side="left", fill="both", expand=True)
        cols = ("id", "dir", "len", "cs", "data")
        self.ft = ttk.Treeview(left, columns=cols, show="headings", height=14, selectmode="browse")
        for c, h, w in (("id", "ID", 60), ("dir", "Richtung", 90), ("len", "Len", 50),
                        ("cs", "Checksumme", 100), ("data", "Daten", 300)):
            self.ft.heading(c, text=h, anchor="w")
            self.ft.column(c, width=w, anchor="w", stretch=(c == "data"))
        self.ft.pack(fill="both", expand=True)
        self.ft.bind("<<TreeviewSelect>>", self._frame_selected)
        ttk.Label(left, style="CardMuted.TLabel", wraplength=560, justify="left", text=(
            "PUB = Adapter liefert die Antwort · ONCE = einmal je Datenupdate (z.B. 3D) · "
            "SUB = Antwort kommt von einem anderen Knoten (nur Länge/Checksumme bekannt)")
                  ).pack(anchor="w", pady=(8, 0))

        right = Card(tab, "Frame bearbeiten")
        right.pack(side="left", fill="y", padx=(10, 0))
        g = ttk.Frame(right, style="Card.TFrame")
        g.pack(fill="x")
        self.fe_id = tk.StringVar(value="21")
        self.fe_dir = tk.StringVar(value="PUB")
        self.fe_len = tk.IntVar(value=8)
        self.fe_cs = tk.StringVar(value="E")
        r = 0
        ttk.Label(g, text="ID (hex)", style="Card.TLabel").grid(row=r, column=0, sticky="w", pady=3)
        ttk.Entry(g, textvariable=self.fe_id, width=6, font=self.mono).grid(row=r, column=1, sticky="w")
        r += 1
        ttk.Label(g, text="Richtung", style="Card.TLabel").grid(row=r, column=0, sticky="w", pady=3)
        ttk.Combobox(g, textvariable=self.fe_dir, values=("PUB", "ONCE", "SUB"), width=7,
                     state="readonly").grid(row=r, column=1, sticky="w")
        r += 1
        ttk.Label(g, text="Länge", style="Card.TLabel").grid(row=r, column=0, sticky="w", pady=3)
        ttk.Spinbox(g, from_=1, to=8, textvariable=self.fe_len, width=5,
                    command=self._fe_len_changed).grid(row=r, column=1, sticky="w")
        r += 1
        ttk.Label(g, text="Checksumme", style="Card.TLabel").grid(row=r, column=0, sticky="w", pady=3)
        cf = ttk.Frame(g, style="Card.TFrame")
        cf.grid(row=r, column=1, sticky="w")
        ttk.Radiobutton(cf, text="enhanced", value="E", variable=self.fe_cs).pack(side="left")
        ttk.Radiobutton(cf, text="classic", value="C", variable=self.fe_cs).pack(side="left")

        ttk.Label(right, text="Datenbytes", style="Card.TLabel").pack(anchor="w", pady=(10, 2))
        bf = ttk.Frame(right, style="Card.TFrame")
        bf.pack(anchor="w")
        self.fe_bytes = []
        for i in range(8):
            v = tk.StringVar(value="00")
            ent = ttk.Entry(bf, textvariable=v, width=3, font=self.mono, justify="center")
            ent.grid(row=0, column=i, padx=1)
            ttk.Label(bf, text=str(i + 1), style="CardMuted.TLabel").grid(row=1, column=i)
            v.trace_add("write", lambda *a: self._fe_live())
            self.fe_bytes.append((v, ent))
        self.fe_live = tk.BooleanVar(value=False)
        ttk.Checkbutton(right, text="Live: Datenänderung sofort senden (DATA)",
                        variable=self.fe_live).pack(anchor="w", pady=(8, 0))

        bb = ttk.Frame(right, style="Card.TFrame")
        bb.pack(fill="x", pady=(12, 0))
        ttk.Button(bb, text="Übernehmen", style="Accent.TButton", command=self._frame_apply).pack(fill="x")
        ttk.Button(bb, text="Nur Daten senden (DATA)", command=self._frame_data).pack(fill="x", pady=4)
        ttk.Button(bb, text="Löschen", command=self._frame_delete).pack(fill="x")
        ttk.Button(bb, text="Alle löschen", command=self._frame_delete_all).pack(fill="x", pady=4)
        ttk.Button(bb, text="Vom Gerät lesen", command=self._frames_pull).pack(fill="x")
        self._fe_len_changed()

    # --- Schedule ------------------------------------------------------ #
    def _tab_sched(self):
        tab = ttk.Frame(self.nb, padding=(0, 8, 0, 0))
        self.nb.add(tab, text="Schedule")
        left = Card(tab, "Schedule (Master)")
        left.pack(side="left", fill="both", expand=True)
        cols = ("n", "id", "ms", "info")
        self.st = ttk.Treeview(left, columns=cols, show="headings", selectmode="browse")
        for c, h, w in (("n", "#", 40), ("id", "ID", 60), ("ms", "Slot [ms]", 90), ("info", "Frame", 300)):
            self.st.heading(c, text=h, anchor="w")
            self.st.column(c, width=w, anchor="w", stretch=(c == "info"))
        self.st.pack(fill="both", expand=True)
        self.sched_sum = tk.StringVar(value="")
        ttk.Label(left, textvariable=self.sched_sum, style="CardMuted.TLabel").pack(anchor="w", pady=(6, 0))

        right = Card(tab, "Eintrag")
        right.pack(side="left", fill="y", padx=(10, 0))
        self.se_id = tk.StringVar(value="21")
        self.se_ms = tk.StringVar(value="")
        g = ttk.Frame(right, style="Card.TFrame")
        g.pack(fill="x")
        ttk.Label(g, text="ID (hex)", style="Card.TLabel").grid(row=0, column=0, sticky="w", pady=3)
        ttk.Entry(g, textvariable=self.se_id, width=6, font=self.mono).grid(row=0, column=1, sticky="w")
        ttk.Label(g, text="Slot [ms]", style="Card.TLabel").grid(row=1, column=0, sticky="w", pady=3)
        ttk.Entry(g, textvariable=self.se_ms, width=6, font=self.mono).grid(row=1, column=1, sticky="w")
        ttk.Label(right, text="leer = Framezeit + Reserve", style="CardMuted.TLabel").pack(anchor="w")
        for txt, cmd in (("Hinzufügen", self._sched_add), ("Entfernen", self._sched_del),
                         ("▲ nach oben", lambda: self._sched_move(-1)),
                         ("▼ nach unten", lambda: self._sched_move(1)),
                         ("Alle Frames übernehmen", self._sched_from_frames),
                         ("Leeren", self._sched_clear)):
            ttk.Button(right, text=txt, command=cmd).pack(fill="x", pady=2)
        ttk.Button(right, text="Aufs Gerät übertragen", style="Accent.TButton",
                   command=self._sched_push).pack(fill="x", pady=(12, 2))
        ttk.Button(right, text="Vom Gerät lesen", command=self._sched_pull).pack(fill="x")

    # --- Senden -------------------------------------------------------- #
    def _tab_send(self):
        tab = ttk.Frame(self.nb, padding=(0, 8, 0, 0))
        self.nb.add(tab, text="Senden")
        c = Card(tab, "Einmal-Frame (Master, Monitor, Diag)")
        c.pack(fill="x")
        g = ttk.Frame(c, style="Card.TFrame")
        g.pack(fill="x")
        self.sd_id = tk.StringVar(value="21")
        self.sd_cs = tk.StringVar(value="auto")
        self.sd_data = tk.StringVar(value="")
        self.sd_rep = tk.BooleanVar(value=False)
        self.sd_ms = tk.StringVar(value="100")
        ttk.Label(g, text="ID (hex)", style="Card.TLabel").grid(row=0, column=0, sticky="w")
        ttk.Entry(g, textvariable=self.sd_id, width=6, font=self.mono).grid(row=0, column=1, sticky="w", padx=6)
        ttk.Label(g, text="Checksumme", style="Card.TLabel").grid(row=0, column=2, sticky="w", padx=(12, 0))
        ttk.Combobox(g, textvariable=self.sd_cs, values=("auto", "enhanced", "classic"),
                     state="readonly", width=9).grid(row=0, column=3, sticky="w", padx=6)
        ttk.Label(g, text="Daten (hex, leer = nur Header)", style="Card.TLabel").grid(row=1, column=0, columnspan=2,
                                                                                    sticky="w", pady=(8, 0))
        ttk.Entry(g, textvariable=self.sd_data, width=40, font=self.mono).grid(row=1, column=2, columnspan=3,
                                                                              sticky="w", padx=6, pady=(8, 0))
        bf = ttk.Frame(c, style="Card.TFrame")
        bf.pack(fill="x", pady=(10, 0))
        ttk.Button(bf, text="Senden", style="Accent.TButton", command=self._send_once).pack(side="left")
        ttk.Checkbutton(bf, text="wiederholen alle", variable=self.sd_rep,
                        command=self._send_repeat_changed).pack(side="left", padx=(16, 4))
        ttk.Entry(bf, textvariable=self.sd_ms, width=6, font=self.mono).pack(side="left")
        ttk.Label(bf, text="ms", style="Card.TLabel").pack(side="left", padx=4)
        self.sd_result = tk.StringVar(value="")
        ttk.Label(c, textvariable=self.sd_result, style="Card.TLabel", font=self.mono).pack(anchor="w", pady=(10, 0))

        c2 = Card(tab, "Bus-Steuerung")
        c2.pack(fill="x", pady=(10, 0))
        bf2 = ttk.Frame(c2, style="Card.TFrame")
        bf2.pack(fill="x")
        ttk.Button(bf2, text="Wakeup senden", command=lambda: self._cmd(lambda d: d.wake())).pack(side="left")
        ttk.Button(bf2, text="Go-to-Sleep (3C)", command=lambda: self._cmd(lambda d: d.sleep())).pack(
            side="left", padx=8)

    # --- Diagnose ------------------------------------------------------ #
    def _tab_diag(self):
        tab = ttk.Frame(self.nb, padding=(0, 8, 0, 0))
        self.nb.add(tab, text="Diagnose")
        left = Card(tab, "Request")
        left.pack(side="left", fill="y")
        self.dg_nad = tk.StringVar(value="7F")
        self.dg_req = tk.StringVar(value="B2 00 FF 7F FF FF")
        self.dg_did = tk.StringVar(value="F190")
        self.dg_newnad = tk.StringVar(value="0A")
        g = ttk.Frame(left, style="Card.TFrame")
        g.pack(fill="x")
        ttk.Label(g, text="NAD (hex)", style="Card.TLabel").grid(row=0, column=0, sticky="w", pady=3)
        ttk.Entry(g, textvariable=self.dg_nad, width=6, font=self.mono).grid(row=0, column=1, sticky="w")
        ttk.Label(g, text="7F = Broadcast", style="CardMuted.TLabel").grid(row=0, column=2, sticky="w", padx=6)
        ttk.Label(g, text="SID + Daten", style="Card.TLabel").grid(row=1, column=0, sticky="w", pady=3)
        ttk.Entry(g, textvariable=self.dg_req, width=28, font=self.mono).grid(row=1, column=1, columnspan=2,
                                                                             sticky="w")
        ttk.Button(left, text="Request senden", style="Accent.TButton",
                   command=lambda: self._diag_run(lambda d, nad: d.diag(nad, parse_hex(self.dg_req.get())),
                                                  f"Request {self.dg_req.get()}")).pack(fill="x", pady=(10, 14))

        ttk.Label(left, text="Standarddienste", style="H.TLabel").pack(anchor="w")
        ttk.Button(left, text="Read by Identifier (B2, ID 0)", command=lambda: self._diag_run(
            lambda d, nad: d.read_by_id(nad, 0), "Read by Identifier")).pack(fill="x", pady=2)
        ttk.Button(left, text="Save Configuration (B6)", command=lambda: self._diag_run(
            lambda d, nad: d.save_configuration(nad), "Save Configuration")).pack(fill="x", pady=2)
        f = ttk.Frame(left, style="Card.TFrame")
        f.pack(fill="x", pady=2)
        ttk.Button(f, text="Assign NAD (B0) →", command=lambda: self._diag_run(
            lambda d, nad: d.assign_nad(nad, int(self.dg_newnad.get(), 16)), "Assign NAD")).pack(side="left",
                                                                                              fill="x", expand=True)
        ttk.Entry(f, textvariable=self.dg_newnad, width=5, font=self.mono).pack(side="left", padx=4)
        f = ttk.Frame(left, style="Card.TFrame")
        f.pack(fill="x", pady=2)
        ttk.Button(f, text="ReadDataByIdentifier (22) →", command=lambda: self._diag_run(
            lambda d, nad: d.read_data_by_identifier(nad, int(self.dg_did.get(), 16)), "RDBI")).pack(
            side="left", fill="x", expand=True)
        ttk.Entry(f, textvariable=self.dg_did, width=6, font=self.mono).pack(side="left", padx=4)
        ttk.Label(left, style="CardMuted.TLabel", wraplength=300, justify="left", text=(
            "Im Master-Modus laufen die Diagnose-Frames zwischen den Schedule-Slots, "
            "im Diag-Modus ausschließlich. Abstand/Timeout unter Optionen.")).pack(anchor="w", pady=(12, 0))

        right = Card(tab, "Antworten")
        right.pack(side="left", fill="both", expand=True, padx=(10, 0))
        self.dg_log = ScrolledText(right, font=self.mono, height=10, relief="flat",
                                   background=C["panel"], borderwidth=0)
        self.dg_log.pack(fill="both", expand=True)
        self.dg_log.tag_configure("ok", foreground=C["ok"])
        self.dg_log.tag_configure("err", foreground=C["err"])
        self.dg_log.tag_configure("muted", foreground=C["muted"])

    # --- Optionen ------------------------------------------------------ #
    def _tab_opts(self):
        tab = ttk.Frame(self.nb, padding=(0, 8, 0, 0))
        self.nb.add(tab, text="Optionen")
        c = Card(tab, "Meldungen und Diagnose-Timing")
        c.pack(fill="x")
        self.op_vars = {
            "echo": tk.BooleanVar(value=True), "all": tk.BooleanVar(value=True),
            "nr": tk.BooleanVar(value=True),
            "strict": tk.BooleanVar(value=False), "timing": tk.BooleanVar(value=False),
            "dslot": tk.StringVar(value="10"), "p2": tk.StringVar(value="1000")}
        ttk.Checkbutton(c, text="Selbst gesendete Antworten melden (ECHO)",
                        variable=self.op_vars["echo"]).pack(anchor="w")
        ttk.Checkbutton(c, text="Frames nicht definierter IDs melden (ALL)",
                        variable=self.op_vars["all"]).pack(anchor="w")
        ttk.Checkbutton(c, text="Header ohne Antwort melden (NR)",
                        variable=self.op_vars["nr"]).pack(anchor="w")
        ttk.Checkbutton(c, text="Checksummenart streng prüfen – Abweichung von der Frame-Tabelle = Fehler (STRICT)",
                        variable=self.op_vars["strict"]).pack(anchor="w")
        ttk.Checkbutton(c, text="Zeitmessung: Antwortpause und Byte-Pausen im Trace (TIMING)",
                        variable=self.op_vars["timing"]).pack(anchor="w")
        g = ttk.Frame(c, style="Card.TFrame")
        g.pack(anchor="w", pady=(8, 0))
        ttk.Label(g, text="Diagnose-Frameabstand [ms]", style="Card.TLabel").grid(row=0, column=0, sticky="w")
        ttk.Entry(g, textvariable=self.op_vars["dslot"], width=6, font=self.mono).grid(row=0, column=1, padx=6)
        ttk.Label(g, text="Diagnose-Timeout P2 [ms]", style="Card.TLabel").grid(row=1, column=0, sticky="w",
                                                                              pady=4)
        ttk.Entry(g, textvariable=self.op_vars["p2"], width=6, font=self.mono).grid(row=1, column=1, padx=6)
        ttk.Button(c, text="Übernehmen", style="Accent.TButton", command=self._opts_apply).pack(anchor="w",
                                                                                              pady=(10, 0))

        c2 = Card(tab, "Gerät")
        c2.pack(fill="x", pady=(10, 0))
        self.dev_info = tk.StringVar(value="nicht verbunden")
        ttk.Label(c2, textvariable=self.dev_info, style="Card.TLabel", font=self.mono).pack(anchor="w")
        bf = ttk.Frame(c2, style="Card.TFrame")
        bf.pack(anchor="w", pady=(8, 0))
        ttk.Button(bf, text="Statistik lesen", command=self._stats_read).pack(side="left")
        ttk.Button(bf, text="Gerät zurücksetzen", command=self._dev_reset).pack(side="left", padx=8)

    # --- Konsole ------------------------------------------------------- #
    def _tab_console(self):
        tab = ttk.Frame(self.nb, padding=(0, 8, 0, 0))
        self.nb.add(tab, text="Konsole")
        bar = ttk.Frame(tab)
        bar.pack(fill="x", pady=(0, 6))
        self.con_frames = tk.BooleanVar(value=False)
        ttk.Checkbutton(bar, text="Frame-Ereignisse (!F) anzeigen", variable=self.con_frames,
                        style="Bar.TCheckbutton").pack(side="left")
        ttk.Button(bar, text="Leeren", command=lambda: self.con.delete("1.0", "end")).pack(side="right")
        c = ttk.Frame(tab, style="Card.TFrame")
        c.pack(fill="both", expand=True)
        self.con = ScrolledText(c, font=self.mono, relief="flat", background=C["panel"], borderwidth=0)
        self.con.pack(fill="both", expand=True, padx=6, pady=6)
        self.con.tag_configure(">", foreground=C["tx"])
        self.con.tag_configure("-", foreground=C["err"])
        self.con.tag_configure("!", foreground=C["muted"])
        ent_f = ttk.Frame(tab)
        ent_f.pack(fill="x", pady=(6, 0))
        self.con_cmd = tk.StringVar()
        e = ttk.Entry(ent_f, textvariable=self.con_cmd, font=self.mono)
        e.pack(side="left", fill="x", expand=True)
        e.bind("<Return>", lambda ev: self._con_send())
        ttk.Button(ent_f, text="Senden", style="Accent.TButton", command=self._con_send).pack(side="left", padx=6)
        ttk.Label(tab, text="Befehle siehe HELP bzw. docs/PROTOKOLL.md", style="Muted.TLabel").pack(anchor="w")

    # ------------------------------------------------------------------ #
    # Verbindung
    # ------------------------------------------------------------------ #
    def _refresh_ports(self):
        ports = list_ports()
        self.port_cb["values"] = ports
        if not self.port_var.get() and ports:
            self.port_var.set("COM6" if "COM6" in ports else ports[0])

    def _toggle_connect(self):
        if self.dev:
            self._disconnect()
            return
        port = self.port_var.get().strip()
        try:
            dev = LucLin(port, timeout=1.0)
            dev.on_frame(lambda f: self.rxq.put(("F", f)))
            dev.on_diag(lambda r: self.rxq.put(("D", r)))
            dev.on_event(lambda line: self.rxq.put(("E", line)))
            dev.on_line(lambda d, line: self.rxq.put(("L", (d, line))))
            dev.open()
        except Exception as ex:
            messagebox.showerror(APP, f"Verbindung zu {port} fehlgeschlagen:\n{ex}")
            return
        self.dev = dev
        self.btn_conn.config(text="Trennen")
        try:
            ver, sn = dev.version(), dev.serial_number()
            self.dev_info.set(f"{ver}   SN {sn}   Port {port}")
            self.status.set(f"verbunden: {port} · {ver}")
            if self.frames or self.sched:
                self._push_all(quiet=True)
            else:
                self._pull_all(quiet=True)
            self.mode_var.set(dev.mode())
            self.baud_var.set(str(dev.baud))
            self._mode_hint()
        except LucError as ex:
            messagebox.showerror(APP, str(ex))

    def _disconnect(self):
        self._stop_repeat()
        try:
            self.dev.mode("OFF")
        except Exception:
            pass
        self.dev.close()
        self.dev = None
        self.btn_conn.config(text="Verbinden")
        self.status.set("nicht verbunden")
        self.dev_info.set("nicht verbunden")
        self.mode_var.set("OFF")
        self._mode_hint()

    def _cmd(self, fn, show_error=True):
        """Geraetebefehl ausfuehren, Fehler anzeigen.
        Liefert das Ergebnis (True, wenn der Befehl nichts zurueckgibt) oder None bei Fehler."""
        if not self.dev:
            if show_error:
                self.status.set("nicht verbunden – nur im Projekt gespeichert")
            return None
        try:
            r = fn(self.dev)
            return True if r is None else r
        except LucError as ex:
            if show_error:
                messagebox.showerror(APP, str(ex))
            return None
        except Exception as ex:   # noqa: BLE001
            if show_error:
                messagebox.showerror(APP, f"Fehler: {ex}")
            return None

    def _set_baud(self):
        try:
            b = int(self.baud_var.get())
        except ValueError:
            return
        self._cmd(lambda d: setattr(d, "baud", b))

    def _set_mode(self):
        m = self.mode_var.get()
        if self.dev:
            r = self._cmd(lambda d: d.mode(m))
            if r is None:
                self.mode_var.set(self._cmd(lambda d: d.mode(), False) or "OFF")
        else:
            self.mode_var.set("OFF")
            self.status.set("nicht verbunden")
        self._mode_hint()

    def _mode_hint(self):
        self.mode_hint.config(text=MODE_HINT.get(self.mode_var.get(), ""))

    # ------------------------------------------------------------------ #
    # Datenfluss vom Geraet
    # ------------------------------------------------------------------ #
    def _pump(self):
        batch = []
        try:
            for _ in range(5000):
                kind, obj = self.rxq.get_nowait()
                if kind == "F":
                    batch.append(obj)
                elif kind == "D":
                    self._diag_show(obj)
                elif kind == "E":
                    self._con_add("!", obj)
                elif kind == "L":
                    d, line = obj
                    if line.startswith("!F") and not self.con_frames.get():
                        continue
                    tag = ">" if d == ">" else ("-" if line.startswith("-") else
                                                ("!" if line.startswith("!") else ""))
                    self._con_add(tag, ("> " if d == ">" else "") + line)
        except queue.Empty:
            pass
        if batch:
            self._trace_add(batch)
        self.after(100, self._pump)

    def _tick_rate(self):
        self.rate = self.rate_cnt
        self.rate_cnt = 0
        self.stat_r.set(f"{self.rate:.0f} Frames/s · Fehler {self.err_cnt} · "
                        f"{len(self.trace_rows)} im Trace")
        self.after(1000, self._tick_rate)

    # ------------------------------------------------------------------ #
    # Trace
    # ------------------------------------------------------------------ #
    def _filter_fn(self):
        txt = self.trace_filter.get().replace(",", " ").split()
        inc, exc = set(), set()
        for t in txt:
            try:
                if t.startswith("!"):
                    exc.add(int(t[1:], 16))
                else:
                    inc.add(int(t, 16))
            except ValueError:
                pass
        return lambda f: (not inc or f.id in inc) and f.id not in exc

    def _row_values(self, f: Frame, dt):
        st = f"{f.status} · {STATUS_TEXT.get(f.status, '')}"
        src = {"T": "gesendet", "R": "empfangen", "-": "—"}.get(f.src, f.src)
        return (f"{f.t:.6f}", "" if dt is None else f"{dt:.1f}", f"{f.id:02X}", src, len(f.data),
                hexs(f.data) if f.data else "—",
                "--" if f.checksum is None else f"{f.checksum:02X}", st,
                "" if f.gap_us is None else f.gap_us,
                "" if f.interbyte_us is None else f.interbyte_us)

    def _row_tag(self, f: Frame):
        if f.status in ("CS", "TO", "BIT"):
            return ("ERR",)
        if f.status == "NR":
            return ("NR",)
        if f.src == "T":
            return ("T",)
        return ()

    def _trace_add(self, frames: list[Frame]):
        flt = self._filter_fn()
        for f in frames:
            self.rate_cnt += 1
            if f.status in ("CS", "TO", "BIT"):
                self.err_cnt += 1
            dt = None
            if f.id in self.last_t_by_id:
                dt = (f.t_us - self.last_t_by_id[f.id]) / 1000.0
            self.last_t_by_id[f.id] = f.t_us
            f._dt = dt  # type: ignore[attr-defined]
            if self.paused.get():
                continue
            self.trace_rows.append(f)
            if not flt(f):
                continue
            if self.per_id.get():
                self._per_id_update(f)
            else:
                self.tr.insert("", "end", values=self._row_values(f, dt), tags=self._row_tag(f))
        if len(self.trace_rows) > MAX_TRACE_ROWS:
            drop = len(self.trace_rows) - MAX_TRACE_ROWS
            del self.trace_rows[:drop]
            if not self.per_id.get():
                kids = self.tr.get_children()
                if len(kids) > MAX_TRACE_ROWS:
                    self.tr.delete(*kids[:len(kids) - MAX_TRACE_ROWS])
        if self.autoscroll.get() and not self.per_id.get():
            kids = self.tr.get_children()
            if kids:
                self.tr.see(kids[-1])

    def _per_id_update(self, f: Frame):
        self.id_count[f.id] = self.id_count.get(f.id, 0) + 1
        prev = self.id_last.get(f.id)
        self.id_last[f.id] = f
        vals = list(self._row_values(f, getattr(f, "_dt", None)))
        vals[0] = f"{f.t:.3f}  ×{self.id_count[f.id]}"
        tags = list(self._row_tag(f))
        if prev is not None and prev.data != f.data:
            tags.append("chg")
        if f.id in self.id_rows and self.tr.exists(self.id_rows[f.id]):
            self.tr.item(self.id_rows[f.id], values=vals, tags=tags)
        else:
            # sortiert nach ID einfuegen
            pos = "end"
            for i, iid in enumerate(self.tr.get_children()):
                if int(self.tr.set(iid, "id"), 16) > f.id:
                    pos = i
                    break
            self.id_rows[f.id] = self.tr.insert("", pos, values=vals, tags=tags)

    def _trace_view_changed(self):
        self.tr.heading("t", text="Zuletzt [s] / Anzahl" if self.per_id.get() else "Zeit [s]")
        self._trace_rebuild()

    def _trace_rebuild(self):
        self.tr.delete(*self.tr.get_children())
        self.id_rows.clear()
        self.id_last.clear()
        self.id_count.clear()
        flt = self._filter_fn()
        rows = [f for f in self.trace_rows if flt(f)]
        if self.per_id.get():
            for f in rows:
                self._per_id_update(f)
        else:
            for f in rows[-5000:]:
                self.tr.insert("", "end", values=self._row_values(f, getattr(f, "_dt", None)),
                               tags=self._row_tag(f))

    def _trace_clear(self):
        self.trace_rows.clear()
        self.last_t_by_id.clear()
        self.err_cnt = 0
        self._trace_rebuild()

    def _trace_save(self):
        fn = filedialog.asksaveasfilename(defaultextension=".txt", filetypes=[
            ("LUC-LIN Trace", "*.txt"), ("CSV", "*.csv")])
        if not fn:
            return
        csv = fn.lower().endswith(".csv")
        with open(fn, "w", encoding="utf-8") as fh:
            if csv:
                fh.write("t_s;id;src;len;data;cs;status;header_us;gap_us;interbyte_us;frame_us\n")
            else:
                fh.write("; LUC-LIN Trace  t_us id src len data cs status [header gap interbyte frame us]\n")
            for f in self.trace_rows:
                cs = "--" if f.checksum is None else f"{f.checksum:02X}"
                d = f.data.hex().upper() if f.data else "-"
                if csv:
                    tm = ";".join("" if v is None else str(v) for v in
                                  (f.header_us, f.gap_us, f.interbyte_us, f.frame_us))
                    fh.write(f"{f.t:.6f};{f.id:02X};{f.src};{len(f.data)};{d};{cs};{f.status};{tm}\n")
                else:
                    fh.write(f.to_line() + "\n")
        self.status.set(f"Trace gespeichert: {fn}")

    def _trace_load(self):
        fn = filedialog.askopenfilename(filetypes=[("LUC-LIN Trace", "*.txt *.log"), ("Alle", "*.*")])
        if not fn:
            return
        frames = []
        with open(fn, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                p = line.split()
                if len(p) >= 8 and p[0] == "!F":
                    try:
                        frames.append(Frame.from_fields(p[1:]))
                    except ValueError:
                        pass
        self._trace_clear()
        was = self.paused.get()
        self.paused.set(False)
        self._trace_add(frames)
        self.paused.set(was)
        self.status.set(f"{len(frames)} Frames aus {os.path.basename(fn)} geladen")

    # ------------------------------------------------------------------ #
    # Frames
    # ------------------------------------------------------------------ #
    def _fe_len_changed(self):
        n = self.fe_len.get()
        for i, (v, ent) in enumerate(self.fe_bytes):
            ent.state(["!disabled"] if i < n else ["disabled"])

    def _fe_data(self) -> bytes:
        n = self.fe_len.get()
        return bytes(int(v.get() or "0", 16) & 0xFF for v, _ in self.fe_bytes[:n])

    def _fe_set_data(self, data: bytes):
        self._fe_loading = True
        for i, (v, _) in enumerate(self.fe_bytes):
            v.set(f"{data[i]:02X}" if i < len(data) else "00")
        self._fe_loading = False

    def _fe_live(self):
        if getattr(self, "_fe_loading", False) or not self.fe_live.get() or not self.dev:
            return
        try:
            fid = parse_id(self.fe_id.get())
            data = self._fe_data()
        except ValueError:
            return
        fr = self.frames.get(fid)
        if fr and fr["dir"] in ("PUB", "ONCE") and fr["len"] == len(data):
            if self._cmd(lambda d: d.data(fid, data), show_error=False) is not None:
                fr["data"] = data
                self._frames_refresh()

    def _frames_refresh(self):
        sel = self.ft.selection()
        self.ft.delete(*self.ft.get_children())
        for fid in sorted(self.frames):
            f = self.frames[fid]
            self.ft.insert("", "end", iid=str(fid), values=(
                f"{fid:02X}", f["dir"], f["len"], "classic" if f["classic"] else "enhanced",
                hexs(f["data"]) if f["dir"] != "SUB" and f["data"] else "—"))
        if sel and self.ft.exists(sel[0]):
            self.ft.selection_set(sel[0])
        self._sched_refresh()

    def _frame_selected(self, _ev=None):
        sel = self.ft.selection()
        if not sel:
            return
        fid = int(sel[0])
        f = self.frames[fid]
        self.fe_id.set(f"{fid:02X}")
        self.fe_dir.set(f["dir"])
        self.fe_len.set(f["len"])
        self.fe_cs.set("C" if f["classic"] else "E")
        self._fe_len_changed()
        self._fe_set_data(f["data"])

    def _frame_apply(self):
        try:
            fid = parse_id(self.fe_id.get())
            data = self._fe_data()
        except ValueError as ex:
            messagebox.showerror(APP, str(ex))
            return
        f = dict(dir=self.fe_dir.get(), len=self.fe_len.get(),
                 classic=self.fe_cs.get() == "C" or fid >= 0x3C, data=data)
        if self.dev and self._cmd(lambda d: d.frame(fid, f["dir"], f["len"], f["data"], f["classic"])) is None:
            return
        self.frames[fid] = f
        self._frames_refresh()
        self.ft.selection_set(str(fid))

    def _frame_data(self):
        try:
            fid = parse_id(self.fe_id.get())
            data = self._fe_data()
        except ValueError as ex:
            messagebox.showerror(APP, str(ex))
            return
        if fid in self.frames:
            self._cmd(lambda d: d.data(fid, data))
            self.frames[fid]["data"] = data
            self._frames_refresh()

    def _frame_delete(self):
        try:
            fid = parse_id(self.fe_id.get())
        except ValueError:
            return
        self._cmd(lambda d: d.delete(fid), show_error=False)
        self.frames.pop(fid, None)
        self._frames_refresh()

    def _frame_delete_all(self):
        if not messagebox.askyesno(APP, "Alle Frames löschen?"):
            return
        self._cmd(lambda d: d.delete("*"), show_error=False)
        self.frames.clear()
        self._frames_refresh()

    def _frames_pull(self):
        tab = self._cmd(lambda d: d.frames_table())
        if tab is None:
            return
        self.frames = {t["id"]: dict(dir=t["dir"], len=t["len"], classic=t["classic"], data=t["data"])
                       for t in tab}
        self._frames_refresh()

    # ------------------------------------------------------------------ #
    # Schedule
    # ------------------------------------------------------------------ #
    def _slot_default(self, fid):
        n = self.frames.get(fid, {}).get("len", 8)
        try:
            baud = int(self.baud_var.get())
        except ValueError:
            baud = 19200
        return int(((34 + 10 * (n + 1)) * 1.4 * 1e6 / baud) / 1000) + 2

    def _sched_refresh(self):
        self.st.delete(*self.st.get_children())
        total = 0
        for i, (fid, ms) in enumerate(self.sched):
            f = self.frames.get(fid)
            info = (f"{f['dir']} {f['len']} Byte" + (f"  {hexs(f['data'])}" if f["dir"] != "SUB" else "")
                    ) if f else "nicht definiert (Antwort mit beliebiger Länge)"
            self.st.insert("", "end", iid=str(i), values=(i + 1, f"{fid:02X}", ms, info))
            total += ms
        self.sched_sum.set(f"{len(self.sched)} Einträge · Zykluszeit {total} ms" if self.sched else "leer")

    def _sched_sel(self):
        s = self.st.selection()
        return int(s[0]) if s else None

    def _sched_add(self):
        try:
            fid = parse_id(self.se_id.get())
            ms = int(self.se_ms.get()) if self.se_ms.get().strip() else self._slot_default(fid)
        except ValueError as ex:
            messagebox.showerror(APP, str(ex))
            return
        i = self._sched_sel()
        if i is None:
            self.sched.append([fid, ms])
        else:
            self.sched.insert(i + 1, [fid, ms])
        self._sched_refresh()

    def _sched_del(self):
        i = self._sched_sel()
        if i is not None:
            del self.sched[i]
            self._sched_refresh()

    def _sched_move(self, d):
        i = self._sched_sel()
        if i is None or not 0 <= i + d < len(self.sched):
            return
        self.sched[i], self.sched[i + d] = self.sched[i + d], self.sched[i]
        self._sched_refresh()
        self.st.selection_set(str(i + d))

    def _sched_from_frames(self):
        self.sched = [[fid, self._slot_default(fid)] for fid in sorted(self.frames) if fid < 0x3C]
        self._sched_refresh()

    def _sched_clear(self):
        self.sched.clear()
        self._sched_refresh()

    def _sched_push(self):
        self._cmd(lambda d: d.schedule([tuple(e) for e in self.sched]))
        if self.dev:
            self.status.set(f"Schedule übertragen ({len(self.sched)} Einträge)")

    def _sched_pull(self):
        s = self._cmd(lambda d: d.get_schedule())
        if s is not None:
            self.sched = [list(e) for e in s]
            self._sched_refresh()

    # ------------------------------------------------------------------ #
    # Senden
    # ------------------------------------------------------------------ #
    def _send_once(self):
        try:
            fid = parse_id(self.sd_id.get())
            data = parse_hex(self.sd_data.get()) if self.sd_data.get().strip() else None
            if data is not None and not 1 <= len(data) <= 8:
                raise ValueError("1..8 Datenbytes")
        except ValueError as ex:
            messagebox.showerror(APP, str(ex))
            self._stop_repeat()
            return
        cs = {"auto": None, "enhanced": False, "classic": True}[self.sd_cs.get()]
        if not self.dev:
            self.status.set("nicht verbunden")
            self._stop_repeat()
            return

        def worker():
            try:
                f = self.dev.request(fid, data, cs, timeout=1.0)
                txt = f"{f.id:02X}  {f.src}  {hexs(f.data) or '—'}  CS {f.checksum if f.checksum is None else format(f.checksum, '02X')}  {f.status} · {STATUS_TEXT.get(f.status, '')}"
            except LucError as ex:
                txt = f"Fehler: {ex}"
            self.after(0, lambda: self.sd_result.set(txt))
        threading.Thread(target=worker, daemon=True).start()

    def _send_repeat_changed(self):
        if self.sd_rep.get():
            self._repeat()
        else:
            self._stop_repeat()

    def _repeat(self):
        if not self.sd_rep.get():
            return
        self._send_once()
        try:
            ms = max(20, int(self.sd_ms.get()))
        except ValueError:
            ms = 100
        self.repeat_job = self.after(ms, self._repeat)

    def _stop_repeat(self):
        self.sd_rep.set(False)
        if self.repeat_job:
            self.after_cancel(self.repeat_job)
            self.repeat_job = None

    # ------------------------------------------------------------------ #
    # Diagnose
    # ------------------------------------------------------------------ #
    def _diag_run(self, fn, title):
        if not self.dev:
            self.status.set("nicht verbunden")
            return
        try:
            nad = int(self.dg_nad.get(), 16)
        except ValueError:
            messagebox.showerror(APP, "NAD ungültig")
            return
        self.dg_log.insert("end", f"{time.strftime('%H:%M:%S')}  NAD {nad:02X}  {title}\n", "muted")
        self.dg_log.see("end")

        def worker():
            try:
                fn(self.dev, nad)          # Ergebnis kommt ueber on_diag
            except (LucError, ValueError) as ex:
                msg = str(ex)
                self.after(0, lambda: (self.dg_log.insert("end", f"   Fehler: {msg}\n", "err"),
                                       self.dg_log.see("end")))
        threading.Thread(target=worker, daemon=True).start()

    def _diag_show(self, r):
        if r.status == "OK":
            txt = hexs(r.data)
            asc = "".join(chr(b) if 32 <= b < 127 else "." for b in r.data)
            self.dg_log.insert("end", f"   ← NAD {r.nad:02X}  OK   {txt}   |{asc}|\n", "ok")
        elif r.status == "NRC":
            self.dg_log.insert("end", f"   ← NAD {r.nad:02X}  negative Antwort: SID {r.sid:02X} NRC {r.nrc:02X}\n",
                               "err")
        else:
            self.dg_log.insert("end", f"   ← NAD {r.nad:02X}  keine Antwort (Timeout)\n", "err")
        self.dg_log.see("end")

    # ------------------------------------------------------------------ #
    # Optionen
    # ------------------------------------------------------------------ #
    def _opts_from_vars(self):
        v = self.op_vars
        return dict(echo=int(v["echo"].get()), all=int(v["all"].get()), nr=int(v["nr"].get()),
                    strict=int(v["strict"].get()), timing=int(v["timing"].get()),
                    dslot=int(v["dslot"].get()), p2=int(v["p2"].get()))

    def _opts_to_vars(self, o):
        for k in ("echo", "all", "nr"):
            self.op_vars[k].set(bool(o.get(k, 1)))
        for k in ("strict", "timing"):
            self.op_vars[k].set(bool(o.get(k, 0)))
        self.op_vars["dslot"].set(str(o.get("dslot", 10)))
        self.op_vars["p2"].set(str(o.get("p2", 1000)))

    def _opts_apply(self):
        try:
            self.opts = self._opts_from_vars()
        except ValueError:
            messagebox.showerror(APP, "Zahlenwert ungültig")
            return
        self._cmd(lambda d: d.options(**self.opts))

    def _stats_read(self):
        s = self._cmd(lambda d: d.stats())
        if s:
            self.status.set(f"Gerät: {s['fr']} Frames gemeldet · {s['err']} Busfehler · "
                            f"{s['ovr']} verlorene Ereignisse")

    def _dev_reset(self):
        if not self.dev or not messagebox.askyesno(APP, "Gerät zurücksetzen? Tabellen und Schedule im Gerät "
                                                        "werden gelöscht (das Projekt bleibt erhalten)."):
            return
        self._cmd(lambda d: d.reset())
        self.mode_var.set("OFF")
        self.baud_var.set("19200")
        self._mode_hint()

    # ------------------------------------------------------------------ #
    # Konsole
    # ------------------------------------------------------------------ #
    def _con_add(self, tag, line):
        self.con.insert("end", line + "\n", tag)
        n = int(self.con.index("end-1c").split(".")[0])
        if n > 3000:
            self.con.delete("1.0", f"{n - 3000}.0")
        self.con.see("end")

    def _con_send(self):
        c = self.con_cmd.get().strip()
        if not c or not self.dev:
            return
        self.con_cmd.set("")

        def worker():
            try:
                self.dev.command_lines(c, timeout=2.0)
            except LucError:
                pass   # Antwort steht ohnehin in der Konsole
        threading.Thread(target=worker, daemon=True).start()

    # ------------------------------------------------------------------ #
    # Projekt
    # ------------------------------------------------------------------ #
    def _project_dict(self):
        try:
            self.opts = self._opts_from_vars()
        except ValueError:
            pass
        return dict(
            format="luclin-project", version=1,
            baud=int(self.baud_var.get() or 19200),
            frames=[dict(id=f"{fid:02X}", dir=f["dir"], len=f["len"], classic=f["classic"],
                         data=f["data"].hex().upper()) for fid, f in sorted(self.frames.items())],
            schedule=[[f"{fid:02X}", ms] for fid, ms in self.sched],
            options=self.opts,
            send=dict(id=self.sd_id.get(), data=self.sd_data.get(), cs=self.sd_cs.get(), ms=self.sd_ms.get()),
            diag=dict(nad=self.dg_nad.get(), req=self.dg_req.get()),
        )

    def _project_load_dict(self, p):
        self.baud_var.set(str(p.get("baud", 19200)))
        self.frames = {int(f["id"], 16): dict(dir=f["dir"], len=int(f["len"]), classic=bool(f["classic"]),
                                              data=bytes.fromhex(f.get("data", "")))
                       for f in p.get("frames", [])}
        self.sched = [[int(fid, 16), int(ms)] for fid, ms in p.get("schedule", [])]
        self.opts = p.get("options", self.opts)
        self._opts_to_vars(self.opts)
        s = p.get("send", {})
        self.sd_id.set(s.get("id", "21"))
        self.sd_data.set(s.get("data", ""))
        self.sd_cs.set(s.get("cs", "auto"))
        self.sd_ms.set(s.get("ms", "100"))
        d = p.get("diag", {})
        self.dg_nad.set(d.get("nad", "7F"))
        self.dg_req.set(d.get("req", ""))
        self._frames_refresh()

    def _project_open(self):
        fn = filedialog.askopenfilename(filetypes=[("LUC-LIN Projekt", "*.lucproj *.json"), ("Alle", "*.*")])
        if not fn:
            return
        try:
            with open(fn, encoding="utf-8") as fh:
                self._project_load_dict(json.load(fh))
        except (OSError, ValueError, KeyError) as ex:
            messagebox.showerror(APP, f"Projekt nicht lesbar:\n{ex}")
            return
        self.project_path = fn
        self.title(f"{APP} – {os.path.basename(fn)}")
        if self.dev and messagebox.askyesno(APP, "Projekt jetzt aufs Gerät übertragen?"):
            self._push_all()

    def _project_save(self, ask=False):
        fn = self.project_path
        if ask or not fn:
            fn = filedialog.asksaveasfilename(defaultextension=".lucproj",
                                              filetypes=[("LUC-LIN Projekt", "*.lucproj")])
            if not fn:
                return
        with open(fn, "w", encoding="utf-8") as fh:
            json.dump(self._project_dict(), fh, indent=2)
        self.project_path = fn
        self.title(f"{APP} – {os.path.basename(fn)}")
        self.status.set(f"Projekt gespeichert: {fn}")

    def _push_all(self, quiet=False):
        if not self.dev:
            return

        def do(d):
            d.mode("OFF")
            d.baud = int(self.baud_var.get() or 19200)
            d.delete("*")
            for fid, f in sorted(self.frames.items()):
                d.frame(fid, f["dir"], f["len"], f["data"], f["classic"])
            d.schedule([tuple(e) for e in self.sched])
            d.options(**self._opts_from_vars())
            return True
        if self._cmd(do):
            self.mode_var.set("OFF")
            self._mode_hint()
            self.status.set("Projekt aufs Gerät übertragen")
            if not quiet:
                messagebox.showinfo(APP, "Frames, Schedule und Optionen übertragen.")

    def _pull_all(self, quiet=False):
        def do(d):
            return d.frames_table(), d.get_schedule(), d.options(), d.baud
        r = self._cmd(do)
        if r is None:
            return
        tab, sched, opts, baud = r
        self.frames = {t["id"]: dict(dir=t["dir"], len=t["len"], classic=t["classic"], data=t["data"])
                       for t in tab}
        self.sched = [list(e) for e in sched]
        self.opts = opts
        self._opts_to_vars(opts)
        self.baud_var.set(str(baud))
        self._frames_refresh()
        if not quiet:
            self.status.set("Konfiguration vom Gerät gelesen")

    def _quit(self):
        if self.dev:
            self._disconnect()
        self.destroy()


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else None
    Studio(port).mainloop()


if __name__ == "__main__":
    main()
