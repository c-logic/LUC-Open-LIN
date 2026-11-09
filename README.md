# LUC-Open-LIN

It is a more comprehensive fork of https://github.com/uCAN-LIN/LinUSBConverter

## Firmware 4.0 – neues Host-Protokoll (LUC-LIN P1)

Ab Firmware 4.0 spricht der Adapter ein eigenes, sauberes ASCII-Protokoll
statt der SLCAN-artigen Syntax. Beschreibung: [docs/PROTOKOLL.md](docs/PROTOKOLL.md)

| Modus | Funktion |
|---|---|
| `MASTER` | Schedule senden, Antworten empfangen, eigene PUB-Frames beantworten |
| `SLAVE` | auf Header antworten (Daten atomar per `DATA` aktualisierbar) |
| `MONITOR` | mithören, Einmal-Frames senden (`SEND`) |
| `DIAG` | Diagnose 3C/3D mit Transportschicht (`DREQ`, SF/FF/CF, response pending) |

Firmware-Aufbau:

| Datei | Inhalt |
|---|---|
| `Src/luc_lin.c` | LIN-Engine: eine Empfangs-Statemachine für alle Modi, echo-gesteuertes Senden (Bitfehler-/Kollisionserkennung), Schedule, Diagnose |
| `Src/luc_proto.c` | ASCII-Protokoll (Parser, Ausgabe ohne printf) |
| `Src/luc_port_stm32.c` | STM32F042: USART1-Interrupt (Break + RXNE), USB-Ringpuffer, µs-Zeitbasis |
| `Inc/luc_port.h` | Hardware-Schnittstelle des Kerns |

Die bisherigen Quellen (`slcan.c`, `lin_slcan.c`, `open-LIN-c/`) sind im
CubeIDE-Projekt vom Build ausgeschlossen.

Werkzeuge:

- `host/python/` – Python-Bibliothek `luclin`, GUI **LUC-LIN Studio** (`luclin_gui.py`), Beispiele, Tests
- `tools/sim/` – Firmware-Kern auf dem PC mit virtuellem LIN-Bus (Pseudo-Terminal statt COM-Port)
- `tools/arm/` – Kommandozeilen-Build (`make -C tools/arm -f build.mk`, arm-none-eabi-gcc)

---

## Bis Firmware 3.x (SLCAN-artiges Protokoll)

## Common Enhancements

Enhanced checksum ID = 0x00 - 0x3f<br>
Classic checksum ID = 0x80 - 0xBF

## open-LIN-c implementations

add a seperate STM32F0 and STM32F1 variant<br>
STM32F1 needs to read the UART RX byte in some cases and the register naming is different

### Monitor mode

Delivers including Checksum
```
l #Open in Monitor-Modus

r0228 #Receive id 0x22 Length 8
z #Accepted
t022912341E0D63023200F6 #Empfang id 0x22 t0 Id:22 Len:9 Data: 12 34 1E 0D 63 02 32 00 Cs:F6

t0a05ef01000000 #Transmit 0x20 with Classic Checksum = 0xA0 Len:5 Data: EF 01 00 00 00
z #Accepted
t0200 #No Response Data
```

Everything else is as usual<br>
see https://ucandevices.github.io/ulc.html

## My testing ground

A bluepill with a T151 for the STM32F1 implementation test

A LIN(Local interconnect network) Usb Converter(LUC) from Ucandevices with a STM32L042G6GQ for the STM32F0 implementation test
