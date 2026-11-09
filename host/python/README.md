# LUC-LIN Host-Software

| Datei | Inhalt |
|---|---|
| `luclin/` | Python-Bibliothek für eigene Skripte |
| `luclin_gui.py` | **LUC-LIN Studio** – GUI für alle Funktionen (tkinter) |
| `examples/` | Beispielskripte (Master-Abfrage, VAG-Batterie als Slave) |
| `tests/` | End-to-End-Tests gegen den Firmware-Simulator |

Voraussetzung: Python ≥ 3.9 und `pip install pyserial`.

## GUI starten

```
python luclin_gui.py COM6
```

Reiter: **Trace** (chronologisch oder je ID, Filter `21 2F` / `!3C`, speichern/laden),
**Frames** (Tabelle mit Byte-Editor und Live-DATA), **Schedule**, **Senden**
(Einmal-Frame, Wiederholung, Wakeup, Go-to-Sleep), **Diagnose** (frei oder
Standarddienste B0/B2/B6/22), **Optionen**, **Konsole** (Rohbefehle).
Projekte (Frames, Schedule, Optionen) werden als `.lucproj` (JSON) gespeichert.

## Bibliothek

```python
from luclin import LucLin

with LucLin("COM6") as lin:
    lin.baud = 19200
    lin.frame(0x21, "SUB", 8)                  # Antwort kommt von der Batterie
    lin.frame(0x10, "PUB", 2, b"\x12\x34")     # Antwort liefert der Adapter
    lin.schedule([(0x21, 20), (0x10, 15)])
    lin.on_frame(print)                        # Callback je Frame
    lin.mode("MASTER")

    f = lin.wait_frame(0x21, timeout=1)        # oder auf einen Frame warten
    lin.data(0x10, b"\x56\x78")                # atomares Datenupdate
    print(lin.request(0x2F))                   # Einmal-Header, Ergebnis-Frame
    print(lin.read_by_id(0x7F))                # Diagnose (B2)
```

Wichtige Methoden: `mode()`, `baud`, `frame()`, `data()`, `delete()`, `frames_table()`,
`schedule()`, `get_schedule()`, `send()`, `request()`, `wake()`, `sleep()`, `options()`,
`stats()`, `diag()`, `read_by_id()`, `assign_nad()`, `save_configuration()`,
`assign_frame_id_range()`, `read_data_by_identifier()`, `command()` (Rohbefehl).

`Frame`: `t_us`, `id`, `src` (`T`/`R`/`-`), `data`, `checksum`, `status`
(`E`, `C`, `CS`, `NR`, `TO`, `BIT`), `ok`.

## Ohne Hardware testen

Der Simulator in `tools/sim` führt den unveränderten Firmware-Kern auf dem PC
aus (Linux/macOS/WSL) und stellt ein Pseudo-Terminal bereit:

```
cd tools/sim && make -f build.mk
./luc_sim --link /tmp/luc --slave 21=510005DC00140400 --master 21/20,30/20 --diag 0A
python luclin_gui.py /tmp/luc
python -m pytest host/python/tests
```
