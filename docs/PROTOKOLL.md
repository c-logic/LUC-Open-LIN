# LUC-LIN Host-Protokoll, Version 1

Firmware ab 4.0. Ersetzt das bisherige SLCAN-artige Protokoll vollständig.

## 1. Grundlagen

- Transport: USB-CDC (virtueller COM-Port), Baudrate des COM-Ports beliebig.
- Zeilenbasiertes ASCII. Host-Zeilen enden mit `CR`, `LF` oder `CRLF`, max. 120 Zeichen.
- Schlüsselwörter sind case-insensitiv. Felder werden durch Leerzeichen getrennt.
- LIN-IDs, NAD und Datenbytes in **Hex** ohne Präfix (`21`, `3C`, `510005DC`),
  Zeiten und Baudraten **dezimal**.
- Geräte-Zeilen enden mit `CRLF`. Das erste Zeichen bestimmt den Typ:

| Zeichen | Bedeutung |
|---|---|
| `+` | positive Antwort auf den letzten Befehl (`+OK …`) |
| `-` | negative Antwort (`-ERR <code> <text>`) |
| `=` | Infozeile, gehört zur nächsten `+`-Antwort (mehrzeilige Ausgaben) |
| `!` | asynchrones Ereignis (Frames, Diagnose, Warnungen) |

Jeder Befehl erzeugt **genau eine** `+`- oder `-`-Zeile, in Befehlsreihenfolge.
`!`-Zeilen können jederzeit dazwischen kommen.

### Fehlercodes

| Code | Text | Bedeutung |
|---|---|---|
| 1 | SYNTAX | Befehl falsch aufgebaut |
| 2 | RANGE | Wert außerhalb des Bereichs |
| 3 | MODE | im aktuellen Modus nicht erlaubt |
| 4 | FULL | Tabelle/Puffer voll |
| 5 | UNDEF | ID nicht definiert |
| 6 | BUSY | vorherige Aktion läuft noch |
| 7 | UNKNOWN | unbekannter Befehl |

## 2. Betriebsmodi

| Modus | Verhalten |
|---|---|
| `OFF` | Bus passiv, nichts wird gesendet oder gemeldet (Default nach Reset) |
| `MASTER` | sendet Header nach Schedule, beantwortet eigene PUB-Frames, meldet Frames |
| `SLAVE` | beantwortet Header fremder Master für PUB-Frames, meldet Frames |
| `MONITOR` | hört nur mit und meldet alle Frames; `SEND`/`WAKE` erlaubt |
| `DIAG` | Master ohne Schedule, nur Diagnose (`DREQ`) und `SEND` |

Die **Frame-Tabelle** beschreibt, was *dieses Gerät* zu einer ID tut – unabhängig vom Modus:

- `PUB` – Gerät liefert die Antwort (Slave-Task), immer wenn der Header kommt
- `ONCE` – wie `PUB`, aber nur einmal nach jedem `DATA` (z. B. Diagnose-Antwort 3D)
- `SUB` – Gerät erwartet die Antwort von einem anderen Knoten (Länge/Checksumme bekannt)

Im `MONITOR` antwortet das Gerät nie; die Tabelle dient dort nur der Längenerkennung.
Daten-Updates (`DATA`) werden atomar übernommen – ein Frame wird nie aus alten und neuen
Bytes gemischt.

## 3. Befehle

| Befehl | Antwort | Beschreibung |
|---|---|---|
| `VER` | `+OK LUC-LIN <fw> P1` | Firmware- und Protokollversion |
| `SN` | `+OK <8 hex>` | Seriennummer (UID) |
| `RESET` | `+OK` | Modus OFF, Tabellen leer, Optionen und Baudrate auf Default |
| `BAUD [n]` | `+OK <n>` | LIN-Baudrate 1000…115200 (Default 19200); ohne Argument abfragen |
| `MODE [m]` | `+OK <m>` | `OFF` `MASTER` `SLAVE` `MONITOR` `DIAG`; ohne Argument abfragen |
| `FRAME <id> <dir> <len> [E\|C] [data]` | `+OK` | Frame definieren/ersetzen. `dir` = `PUB` `ONCE` `SUB`, `len` 1…8, Checksumme `E` (enhanced, Default) oder `C` (classic; 3C/3D immer classic). Fehlende Datenbytes = 00 |
| `DATA <id> <data>` | `+OK` | Daten eines PUB/ONCE-Frames atomar ersetzen (genau `len` Bytes). Bei ONCE: scharf schalten |
| `DEL <id>` / `DEL *` | `+OK` | Frame bzw. alle Frames löschen |
| `LIST` | `=FRAME …` je Frame, `+OK <anzahl>` | Tabelle auslesen (`=FRAME 21 PUB 8 E 510005DC00140400`). ONCE-Frames, deren Antwort bereitliegt und noch nicht abgeholt wurde, tragen am Ende `ARMED` |
| `SCHED <id>[/ms] …` | `+OK <anzahl>` | Schedule ersetzen (max. 32 Einträge). Slotzeit in ms, Default = max. Framezeit + 1 ms |
| `SCHED ?` | `=SCHED 21/20 …`, `+OK` | Schedule auslesen |
| `SCHED -` | `+OK 0` | Schedule leeren |
| `SEND <id> [E\|C] [data]` | `+OK` | Einmal-Frame (MASTER, MONITOR, DIAG). Ohne Daten nur Header – die Antwort kommt vom Bus (oder aus der eigenen Tabelle). Mit Daten wird der ganze Frame gesendet. Ergebnis als `!F` |
| `SLEEP` | `+OK` | Go-to-Sleep senden (`3C` classic `00 FF FF FF FF FF FF FF`) |
| `WAKE` | `+OK` | Wakeup-Puls (Break, ≥ 250 µs dominant) |
| `DREQ <nad> <data>` | `+OK` | Diagnose-Request (MASTER, DIAG). `data` = SID + Parameter, 1…40 Bytes. Ergebnis als `!D` |
| `OPT` | `+OK ECHO=1 ALL=1 NR=1 STRICT=0 TIMING=0 DSLOT=10 P2=1000` | Optionen anzeigen |
| `OPT <name> <wert>` | `+OK` | Option setzen (siehe unten) |
| `STAT` | `+OK FR=<n> ERR=<n> OVR=<n>` | Zähler: gemeldete Frames, Busfehler, verlorene Ereignisse |
| `HELP [befehl]` | `=…`, `+OK` | Kurzübersicht bzw. Details zu einem Befehl (`HELP FRAME`, `HELP OPT`); `HELP F` / `HELP D` erklären die Ereignisse |

### Optionen

| Name | Default | Bedeutung |
|---|---|---|
| `ECHO` | 1 | selbst gesendete Antworten (src `T`) melden |
| `ALL` | 1 | auch Frames melden, deren ID nicht in der Tabelle steht (MASTER/SLAVE) |
| `NR` | 1 | Header ohne Antwort melden (st `NR`) |
| `STRICT` | 0 | Checksummenart muss zur Frame-Tabelle passen; sonst st `CS` und Fehlerzähler (siehe 4.1) |
| `TIMING` | 0 | Zeitmessung an `!F` anhängen (siehe 4.2) |
| `DSLOT` | 10 | Abstand der Diagnose-Frames (3C/3D-Polling) in ms |
| `P2` | 1000 | Diagnose-Timeout in ms (bei NRC 0x78 automatisch 5000 ms) |

## 4. Ereignisse

### Frame `!F`

```
!F <t_us> <id> <src> <len> <data> <cs> <st>
!F 1234567 21 R 8 510005DC00140400 6B E
```

| Feld | Inhalt |
|---|---|
| `t_us` | Zeitstempel des Breaks in µs (32 bit, läuft über) |
| `id` | Frame-ID 00…3F (hex) |
| `src` | `T` = Antwort vom Gerät gesendet, `R` = Antwort von anderem Knoten, `-` = keine Antwort |
| `len` | Anzahl Datenbytes (dezimal) |
| `data` | Datenbytes hex, `-` wenn keine |
| `cs` | empfangene Checksumme hex, `--` wenn keine |
| `st` | `E` gültig/enhanced, `C` gültig/classic, `CS` Checksummenfehler, `NR` keine Antwort, `TO` unvollständig, `BIT` Bitfehler beim eigenen Senden (Echo ≠ gesendet) |

### 4.1 Checksummenart

Beim Senden gilt die Art aus der Frame-Tabelle (`FRAME … E|C`) bzw. die bei `SEND`
angegebene; ohne Angabe und ohne Tabelleneintrag enhanced. `3C`/`3D` sind immer classic.

Beim Empfang wird jeder Frame gegen beide Arten geprüft und als `E` oder `C` gemeldet
(beide können nie gleichzeitig passen). Ohne `STRICT` gilt jede gültige Art als OK –
gemischte LIN-1.x/2.x-Busse werden so ohne Konfiguration korrekt angezeigt. Mit
`OPT STRICT 1` wird ein Frame, dessen ID in der Tabelle steht und dessen Art von der
Definition abweicht, als `CS` gemeldet und als Fehler gezählt. Ausgenommen sind `3C`/`3D`
und `SEND` mit eigenen Daten.

### 4.2 Zeitmessung (OPT TIMING 1)

```
!F <t_us> <id> <src> <len> <data> <cs> <st> <H> <R> <I> <F>
!F 661603 21 R 8 510005DC00140400 53 E 1040 1525 228 8949
```

| Feld | Bedeutung (µs) |
|---|---|
| `H` | Header: Break-Erkennung bis Ende PID (nominal 20 Bitzeiten + Break-Rest) |
| `R` | **Antwortpause**: Stoppbit der PID bis Startbit des ersten Datenbytes (Master→Slave-Gap) |
| `I` | größte Pause zwischen zwei Antwortbytes (Inter-Byte-Space) |
| `F` | Break-Erkennung bis Ende des letzten Bytes |

`-` = nicht vorhanden (z. B. `R`/`I` bei `NR`, `I` bei nur einem Byte). Grundlage sind die
Empfangszeitpunkte (Ende Stoppbit) mit µs-Auflösung; die Pausen sind daraus abzüglich einer
Zeichenzeit berechnet. Die Genauigkeit liegt bei wenigen µs (Interrupt-Latenz). Die
Break-Erkennung erfolgt nach 11 dominanten Bits, `H` und `F` beginnen daher nicht am
Break-Anfang. Die Messung funktioniert in allen Modi, im `MONITOR` also auch für fremde
Master und Slaves.

### Diagnose `!D`

```
!D <nad> OK <data>      Antwort (RSID + Daten), z. B.  !D 0A OK F2001122
!D <nad> NRC <sid> <nrc> negative Antwort
!D <nad> TO             keine (vollständige) Antwort innerhalb P2
```

### Warnung `!E`

```
!E OVR <n>     n Ereignisse gingen verloren (USB zu langsam)
```

## 5. Diagnose-Transport (DREQ)

ISO 17987-2 / LIN 2.x: Request auf 3C (`NAD PCI SID D…`, Single Frame bis 5 Datenbytes,
sonst First Frame + Consecutive Frames), danach Polling von 3D im `DSLOT`-Abstand bis zur
vollständigen Antwort (SF oder FF+CF). `7F <sid> 78` (response pending) verlängert den
Timeout auf 5 s. Im MASTER-Modus werden die Diagnose-Frames zwischen die Schedule-Slots
geschoben, im DIAG-Modus läuft nur Diagnose.

Als **Slave** kann das Gerät Diagnose beantworten, indem der Host `3C` als `SUB` und `3D`
als `ONCE` definiert und nach jedem empfangenen Request die Antwort per `DATA 3D …` ablegt.

## 6. Beispiele

Master mit VAG-Batterie (Antworten kommen von der Batterie):

```
> BAUD 19200            < +OK 19200
> FRAME 21 SUB 8        < +OK
> FRAME 2F SUB 8        < +OK
> SCHED 21/20 2F/20     < +OK 2
> MODE MASTER           < +OK MASTER
                        < !F 1000523 21 R 8 510005DC00140400 2E E
```

Batterie simulieren (Slave):

```
> FRAME 21 PUB 8 E 510005DC00140400
> MODE SLAVE
> DATA 21 810105DC00140400        (zyklisch, atomar)
```

Monitor mit Sendeoption:

```
> MODE MONITOR
> SEND 2F                 nur Header, Antwort kommt als !F
> SEND 20 C EF01000000    kompletter Frame, classic
```
