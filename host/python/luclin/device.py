"""Geraeteklasse fuer den LUC-LIN-Adapter (ASCII-Protokoll Version 1)."""
from __future__ import annotations

import queue
import threading
import time
from collections import deque
from dataclasses import dataclass, field
from typing import Callable, Iterable, Optional

import serial
import serial.tools.list_ports

MODES = ("OFF", "MASTER", "SLAVE", "MONITOR", "DIAG")
DIRS = ("PUB", "ONCE", "SUB")
STATUS_TEXT = {
    "E": "ok (enhanced)",
    "C": "ok (classic)",
    "CS": "Checksummenfehler",
    "NR": "keine Antwort",
    "TO": "unvollstaendig",
    "BIT": "Bitfehler",
}


class LucError(Exception):
    """Negative Antwort des Adapters (-ERR) oder Kommunikationsfehler."""

    def __init__(self, code: int, text: str, command: str = ""):
        super().__init__(f"{command!r}: ERR {code} {text}" if command else f"ERR {code} {text}")
        self.code = code
        self.text = text
        self.command = command


@dataclass
class Frame:
    """Ein Frame vom Bus (Ereignis !F)."""
    t_us: int                 # Zeitstempel (entpackt, monoton steigend)
    id: int
    src: str                  # 'T' gesendet, 'R' empfangen, '-' keine Antwort
    data: bytes
    checksum: Optional[int]
    status: str               # E, C, CS, NR, TO, BIT
    host_time: float = field(default_factory=time.time)
    # Zeitmessung (nur mit options(timing=1)), Werte in us, sonst None
    header_us: Optional[int] = None      # Break-Erkennung bis Ende PID
    gap_us: Optional[int] = None         # Antwortpause: Stoppbit PID -> Startbit 1. Datenbyte
    interbyte_us: Optional[int] = None   # groesste Pause zwischen Antwortbytes
    frame_us: Optional[int] = None       # Break-Erkennung bis Ende letztes Byte

    @classmethod
    def from_fields(cls, p: list[str], t_us: int | None = None) -> "Frame":
        """Aus den Feldern einer !F-Zeile (ohne '!F') bauen."""
        def num(s):
            return None if s == "-" else int(s)
        f = cls(int(p[0]) if t_us is None else t_us, int(p[1], 16), p[2],
                bytes.fromhex(p[4]) if p[4] != "-" else b"",
                int(p[5], 16) if p[5] != "--" else None, p[6])
        if len(p) >= 11:
            f.header_us, f.gap_us, f.interbyte_us, f.frame_us = (num(x) for x in p[7:11])
        return f

    def to_line(self) -> str:
        """Als !F-Zeile (Trace-Datei)."""
        d = self.data.hex().upper() if self.data else "-"
        cs = "--" if self.checksum is None else f"{self.checksum:02X}"
        s = f"!F {self.t_us} {self.id:02X} {self.src} {len(self.data)} {d} {cs} {self.status}"
        if self.header_us is not None or self.gap_us is not None:
            s += " " + " ".join("-" if v is None else str(v) for v in
                                (self.header_us, self.gap_us, self.interbyte_us, self.frame_us))
        return s

    @property
    def ok(self) -> bool:
        return self.status in ("E", "C")

    @property
    def classic(self) -> bool:
        return self.status == "C"

    @property
    def t(self) -> float:
        """Zeitstempel in Sekunden."""
        return self.t_us / 1e6

    def __str__(self) -> str:
        d = self.data.hex(" ").upper() if self.data else "-"
        cs = f"{self.checksum:02X}" if self.checksum is not None else "--"
        return f"{self.t:12.6f}  {self.id:02X}  {self.src}  {len(self.data)}  {d:<24}  {cs}  {self.status}"


@dataclass
class DiagResult:
    """Ergebnis eines Diagnose-Requests (Ereignis !D)."""
    nad: int
    status: str               # OK, NRC, TO
    data: bytes = b""         # Antwort inkl. RSID (bei OK)
    sid: int = 0              # bei NRC: abgelehnter Service
    nrc: int = 0              # bei NRC: Negative Response Code

    @property
    def ok(self) -> bool:
        return self.status == "OK"


def list_ports() -> list[str]:
    """Verfuegbare serielle Ports."""
    return [p.device for p in serial.tools.list_ports.comports()]


def _hex(data) -> str:
    return bytes(data).hex().upper()


class LucLin:
    """
    LUC-LIN-Adapter.

    Alle Befehle sind thread-sicher und blockieren bis zur Antwort des
    Adapters. Frames und Diagnose-Ergebnisse kommen asynchron und werden an
    registrierte Callbacks (aus dem Lesethread!) sowie an eine interne Queue
    geliefert (wait_frame, frames).
    """

    def __init__(self, port: str | None = None, timeout: float = 1.0, *,
                 serial_instance=None, keep_frames: int = 10000):
        self.port = port
        self.timeout = timeout
        self._ser = serial_instance
        self._rx_thread: Optional[threading.Thread] = None
        self._stop = threading.Event()
        self._cmd_lock = threading.Lock()
        self._resp: "queue.Queue[str]" = queue.Queue()
        self._frame_cb: list[Callable[[Frame], None]] = []
        self._diag_cb: list[Callable[[DiagResult], None]] = []
        self._event_cb: list[Callable[[str], None]] = []
        self._line_cb: list[Callable[[str, str], None]] = []
        self._frames: deque[Frame] = deque(maxlen=keep_frames)
        self._frame_cond = threading.Condition()
        self._diag_q: "queue.Queue[DiagResult]" = queue.Queue()
        self._t_last = 0
        self._t_wrap = 0
        self.lost_events = 0
        self._p2 = 1.0

    # ------------------------------------------------------------------ #
    # Verbindung
    # ------------------------------------------------------------------ #
    def open(self) -> "LucLin":
        if self._ser is None:
            if not self.port:
                raise ValueError("kein Port angegeben")
            self._ser = serial.Serial(self.port, 115200, timeout=0.05)
        self._stop.clear()
        self._rx_thread = threading.Thread(target=self._reader, name="luclin-rx", daemon=True)
        self._rx_thread.start()
        # evtl. halbe Zeile im Adapter verwerfen, dann Version pruefen
        self._ser.write(b"\r")
        time.sleep(0.05)
        self._drain_responses()
        ver = self.version()
        if not ver.startswith("LUC-LIN"):
            raise LucError(0, f"unerwartete Kennung {ver!r}")
        return self

    def close(self):
        self._stop.set()
        if self._rx_thread:
            self._rx_thread.join(timeout=1)
        if self._ser is not None:
            try:
                self._ser.close()
            except Exception:
                pass
        self._ser = None

    @property
    def is_open(self) -> bool:
        return self._ser is not None and self._ser.is_open

    def __enter__(self):
        return self.open()

    def __exit__(self, *exc):
        try:
            if self.is_open:
                self.mode("OFF")
        except Exception:
            pass
        self.close()

    # ------------------------------------------------------------------ #
    # Callbacks / Empfang
    # ------------------------------------------------------------------ #
    def on_frame(self, cb: Callable[[Frame], None]):
        """Callback fuer jeden Frame (laeuft im Lesethread)."""
        self._frame_cb.append(cb)
        return cb

    def on_diag(self, cb: Callable[[DiagResult], None]):
        self._diag_cb.append(cb)
        return cb

    def on_event(self, cb: Callable[[str], None]):
        """Callback fuer sonstige Ereignisse (!E ...)."""
        self._event_cb.append(cb)
        return cb

    def on_line(self, cb: Callable[[str, str], None]):
        """Rohzeilen-Monitor: cb(richtung '>'/'<', zeile)."""
        self._line_cb.append(cb)
        return cb

    def remove_callback(self, cb):
        for lst in (self._frame_cb, self._diag_cb, self._event_cb, self._line_cb):
            if cb in lst:
                lst.remove(cb)

    def _emit_line(self, d, line):
        for cb in list(self._line_cb):
            try:
                cb(d, line)
            except Exception:
                pass

    def _reader(self):
        buf = b""
        while not self._stop.is_set():
            try:
                chunk = self._ser.read(4096)
            except Exception:
                break
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.decode("ascii", "replace").strip("\r ")
                if line:
                    self._handle(line)

    def _unwrap(self, t: int) -> int:
        if t < self._t_last and self._t_last - t > 0x80000000:
            self._t_wrap += 1 << 32
        self._t_last = t
        return t + self._t_wrap

    def _handle(self, line: str):
        self._emit_line("<", line)
        c = line[0]
        if c in "+-=":
            self._resp.put(line)
            return
        if c != "!":
            return
        parts = line[1:].split()
        if not parts:
            return
        kind = parts[0]
        try:
            if kind == "F" and len(parts) >= 8:
                f = Frame.from_fields(parts[1:], self._unwrap(int(parts[1])))
                with self._frame_cond:
                    self._frames.append(f)
                    self._frame_cond.notify_all()
                for cb in list(self._frame_cb):
                    try:
                        cb(f)
                    except Exception:
                        pass
            elif kind == "D" and len(parts) >= 3:
                nad = int(parts[1], 16)
                st = parts[2]
                if st == "OK":
                    r = DiagResult(nad, "OK", bytes.fromhex(parts[3]) if len(parts) > 3 else b"")
                elif st == "NRC":
                    r = DiagResult(nad, "NRC", b"", int(parts[3], 16), int(parts[4], 16))
                else:
                    r = DiagResult(nad, st)
                self._diag_q.put(r)
                for cb in list(self._diag_cb):
                    try:
                        cb(r)
                    except Exception:
                        pass
            else:
                if kind == "E" and len(parts) >= 3 and parts[1] == "OVR":
                    self.lost_events += int(parts[2])
                for cb in list(self._event_cb):
                    try:
                        cb(line)
                    except Exception:
                        pass
        except ValueError:
            pass

    def _drain_responses(self):
        while True:
            try:
                self._resp.get_nowait()
            except queue.Empty:
                return

    # ------------------------------------------------------------------ #
    # Befehle
    # ------------------------------------------------------------------ #
    def command_lines(self, cmd: str, timeout: float | None = None) -> tuple[list[str], str]:
        """Befehl senden; liefert (Infozeilen ohne '=', Nutzdaten nach '+OK')."""
        if not self.is_open:
            raise LucError(0, "nicht verbunden", cmd)
        timeout = self.timeout if timeout is None else timeout
        info: list[str] = []
        with self._cmd_lock:
            self._drain_responses()
            self._emit_line(">", cmd)
            self._ser.write((cmd + "\r").encode("ascii"))
            deadline = time.monotonic() + timeout
            while True:
                rest = deadline - time.monotonic()
                if rest <= 0:
                    raise LucError(0, "keine Antwort (Timeout)", cmd)
                try:
                    line = self._resp.get(timeout=rest)
                except queue.Empty:
                    continue
                if line.startswith("="):
                    info.append(line[1:])
                    deadline = time.monotonic() + timeout
                elif line.startswith("+"):
                    payload = line[3:].strip() if line.startswith("+OK") else line[1:]
                    return info, payload
                else:
                    p = line[1:].split(None, 2)
                    code = int(p[1]) if len(p) > 1 and p[1].isdigit() else 0
                    text = p[2] if len(p) > 2 else line
                    raise LucError(code, text, cmd)

    def command(self, cmd: str, timeout: float | None = None) -> str:
        """Befehl senden und Nutzdaten der +OK-Antwort liefern."""
        return self.command_lines(cmd, timeout)[1]

    # Grundbefehle ------------------------------------------------------
    def version(self) -> str:
        return self.command("VER")

    def serial_number(self) -> str:
        return self.command("SN")

    def reset(self):
        self.command("RESET")

    def help(self, topic: str | None = None) -> list[str]:
        """Hilfetext des Adapters (Uebersicht oder zu einem Befehl, z.B. 'FRAME', 'F')."""
        return self.command_lines("HELP" + (f" {topic}" if topic else ""))[0]

    @property
    def baud(self) -> int:
        return int(self.command("BAUD"))

    @baud.setter
    def baud(self, value: int):
        self.command(f"BAUD {int(value)}")

    def mode(self, m: str | None = None) -> str:
        """Modus setzen/abfragen: OFF, MASTER, SLAVE, MONITOR, DIAG."""
        if m is None:
            return self.command("MODE")
        m = m.upper()
        if m not in MODES:
            raise ValueError(f"Modus {m!r} unbekannt")
        return self.command(f"MODE {m}")

    # Frame-Tabelle -----------------------------------------------------
    def frame(self, frame_id: int, direction: str, length: int,
              data: bytes | Iterable[int] | None = None, classic: bool = False):
        """Frame definieren. direction: 'PUB', 'ONCE' oder 'SUB'."""
        direction = direction.upper()
        if direction not in DIRS:
            raise ValueError("direction muss PUB, ONCE oder SUB sein")
        cmd = f"FRAME {frame_id:02X} {direction} {int(length)} {'C' if classic else 'E'}"
        if data is not None and len(bytes(data)):
            cmd += " " + _hex(data)
        self.command(cmd)

    def data(self, frame_id: int, data: bytes | Iterable[int]):
        """Daten eines PUB/ONCE-Frames atomar ersetzen."""
        self.command(f"DATA {frame_id:02X} {_hex(data)}")

    def delete(self, frame_id: int | str = "*"):
        self.command("DEL *" if frame_id == "*" else f"DEL {int(frame_id):02X}")

    def frames_table(self) -> list[dict]:
        info, _ = self.command_lines("LIST")
        res = []
        for line in info:
            p = line.split()
            if p and p[0] == "FRAME":
                res.append(dict(id=int(p[1], 16), dir=p[2], len=int(p[3]),
                                classic=p[4] == "C", data=bytes.fromhex(p[5]),
                                armed=len(p) > 6 and p[6] == "ARMED"))
        return res

    # Schedule ----------------------------------------------------------
    def schedule(self, entries: Iterable):
        """Schedule setzen: [(id, ms), ...] oder [id, ...] (ms = Default)."""
        toks = []
        for e in entries:
            if isinstance(e, (tuple, list)):
                fid, ms = e[0], (e[1] if len(e) > 1 else None)
            else:
                fid, ms = e, None
            toks.append(f"{int(fid):02X}" + (f"/{int(ms)}" if ms else ""))
        self.command("SCHED " + (" ".join(toks) if toks else "-"))

    def get_schedule(self) -> list[tuple[int, int]]:
        info, _ = self.command_lines("SCHED ?")
        res = []
        for line in info:
            for tok in line.split()[1:]:
                fid, ms = tok.split("/")
                res.append((int(fid, 16), int(ms)))
        return res

    def clear_schedule(self):
        self.command("SCHED -")

    # Senden ------------------------------------------------------------
    def send(self, frame_id: int, data: bytes | Iterable[int] | None = None,
             classic: bool | None = None):
        """Einmal-Frame: ohne data nur Header, mit data kompletter Frame."""
        cmd = f"SEND {frame_id:02X}"
        if classic is not None:
            cmd += " C" if classic else " E"
        if data is not None and len(bytes(data)):
            cmd += " " + _hex(data)
        self.command(cmd)

    def request(self, frame_id: int, data=None, classic=None, timeout: float = 1.0) -> Frame:
        """Einmal-Frame senden und auf das Ergebnis warten."""
        t0 = time.time()
        self.send(frame_id, data, classic)
        f = self.wait_frame(frame_id, timeout=timeout, since=t0)
        if f is None:
            raise LucError(0, "kein Frame-Ergebnis", f"SEND {frame_id:02X}")
        return f

    def wake(self):
        self.command("WAKE")

    def sleep(self):
        self.command("SLEEP")

    # Optionen / Statistik ----------------------------------------------
    def options(self, **kw) -> dict:
        """Optionen setzen (echo=, all=, nr=, strict=, timing=, dslot=, p2=) und alle liefern."""
        for k, v in kw.items():
            self.command(f"OPT {k.upper()} {int(v)}")
        res = {}
        for tok in self.command("OPT").split():
            k, v = tok.split("=")
            res[k.lower()] = int(v)
        self._p2 = res.get("p2", 1000) / 1000.0
        return res

    def stats(self) -> dict:
        res = {}
        for tok in self.command("STAT").split():
            k, v = tok.split("=")
            res[k.lower()] = int(v)
        return res

    # Frames abholen ----------------------------------------------------
    def wait_frame(self, frame_id: int | None = None, timeout: float = 1.0,
                   since: float | None = None, ok_only: bool = False) -> Optional[Frame]:
        """Auf den naechsten passenden Frame warten (seit 'since', Host-Zeit)."""
        since = time.time() if since is None else since
        deadline = time.monotonic() + timeout
        with self._frame_cond:
            while True:
                for f in self._frames:
                    if f.host_time >= since and (frame_id is None or f.id == frame_id) \
                            and (not ok_only or f.ok):
                        return f
                rest = deadline - time.monotonic()
                if rest <= 0:
                    return None
                self._frame_cond.wait(rest)

    def frames(self, clear: bool = True) -> list[Frame]:
        """Alle bisher empfangenen Frames (und Puffer leeren)."""
        with self._frame_cond:
            res = list(self._frames)
            if clear:
                self._frames.clear()
        return res

    # Diagnose ----------------------------------------------------------
    def diag(self, nad: int, payload: bytes | Iterable[int], timeout: float | None = None) -> DiagResult:
        """Diagnose-Request (SID + Daten) senden und auf die Antwort warten."""
        while not self._diag_q.empty():
            self._diag_q.get_nowait()
        self.command(f"DREQ {nad:02X} {_hex(payload)}")
        if timeout is None:
            timeout = self._p2 + 6.0       # P2 + Verlaengerung durch "pending"
        try:
            return self._diag_q.get(timeout=timeout)
        except queue.Empty:
            raise LucError(0, "keine Diagnose-Antwort", "DREQ")

    def read_by_id(self, nad: int, identifier: int = 0, supplier: int = 0x7FFF,
                   function: int = 0xFFFF) -> DiagResult:
        """LIN Read by Identifier (SID B2). identifier 0 = Produktkennung."""
        return self.diag(nad, bytes([0xB2, identifier, supplier & 0xFF, supplier >> 8,
                                     function & 0xFF, function >> 8]))

    def assign_nad(self, nad: int, new_nad: int, supplier: int = 0x7FFF,
                   function: int = 0xFFFF) -> DiagResult:
        """LIN Assign NAD (SID B0)."""
        return self.diag(nad, bytes([0xB0, supplier & 0xFF, supplier >> 8,
                                     function & 0xFF, function >> 8, new_nad]))

    def save_configuration(self, nad: int) -> DiagResult:
        """LIN Save Configuration (SID B6)."""
        return self.diag(nad, bytes([0xB6]))

    def assign_frame_id_range(self, nad: int, start_index: int, pids: Iterable[int]) -> DiagResult:
        """LIN Assign Frame ID Range (SID B7), bis zu 4 PIDs (0xFF = unveraendert)."""
        p = list(pids)[:4] + [0xFF] * (4 - len(list(pids)[:4]))
        return self.diag(nad, bytes([0xB7, start_index] + p))

    def read_data_by_identifier(self, nad: int, did: int) -> DiagResult:
        """UDS ReadDataByIdentifier (SID 22) ueber LIN-Transportschicht."""
        return self.diag(nad, bytes([0x22, did >> 8, did & 0xFF]))
