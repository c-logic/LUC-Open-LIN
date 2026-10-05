# LUC-Open-LIN — Inkonsistenz-Review

Projekt: `E:\philippi\LUC-Open-LIN` (STM32F042G6Ux, LIN-USB-Adapter: Master / Slave / Monitor, SLCAN-artiges Kommandoprotokoll über USB-CDC)
Geprüft: `Inc/`, `Src/`, `Src/open-LIN-c/`, `.cproject`, `LUCEmbedded.ioc`, `STM32F042G6UX_FLASH.ld`
Nicht geprüft: ST-HAL, CMSIS, USB-Device-Library (Fremdcode), `Src/open-LIN-c/implementation/{x86,pic16,STM32F1,unit_test}` (im `.cproject` vom Build ausgeschlossen — korrekt, keine Doppeldefinitionen)

Alle Befunde wurden gegen den Code gegengeprüft; nicht haltbare Verdachtsfälle stehen unten unter „Geprüft und verworfen".

---

## A — Kritisch: Absturz / dauerhafter Stillstand

### A1. Schreibzugriff über NULL-Pointer → HardFault (Monitor-Modus)
`Src/lin_slcan.c:212-227`

`open_lin_data_layer_frame` ist file-static, `data_ptr` startet also als `NULL`; `lin_slcan_reset()` (Z. 137-142) setzt ihn nie. Zugewiesen wird er **nur** im Erfolgszweig von `PID_RX` (Z. 215). Wegen A2 (fehlendes `else`) landet ein PID mit Paritätsfehler trotzdem in `DATA_RX` — und Z. 227 schreibt dann `data_ptr[0] = rx_byte`, also auf Adresse 0. Auf dem STM32F0 ist das gespiegelter Boot-Flash → Bus Fault → `HardFault_Handler` (`Src/stm32f0xx_it.c:93`) dreht endlos.

Auslöser: ein einziges Störbyte nach dem Break, bevor je ein gültiger PID empfangen wurde.

### A2. Fehlendes `else`: Reset wird sofort wieder überschrieben
`Src/lin_slcan.c:212-219`

```c
if (open_lin_data_layer_parity(rx_byte) == rx_byte) { ...pid/data_ptr setzen... }
else { lin_slcan_reset(); }
slcan_lin_slave_state = OPEN_LIN_SLAVE_DATA_RX;   // <-- unbedingt!
```
Der Reset bleibt wirkungslos. Ein Frame mit kaputtem PID wird mit der **PID des Vorgänger-Frames** gemeldet (Z. 170-173). Ursache von A1.

Gleiches Muster im Stack: `Src/open-LIN-c/open_lin_slave_data_layer.c:96-102` — nach `open_lin_slave_reset()` läuft die Funktion in `open_lin_slave_set_lin_frame()` mit veraltetem PID weiter (kann DATA_TX/DATA_RX neu starten).

### A3. Blockierendes `HAL_UART_Transmit` im SysTick-ISR — Timeout kann nie ablaufen
`Src/stm32f0xx_it.c:136` → `Src/slcan.c:80` → `open_lin_master_data_layer.c:74,84,87` → `open_lin_hw_st32.c:62,76`

Der Master-Handler sendet Break, Sync, PID, Daten und Checksumme mit `HAL_UART_Transmit(..., 1000)` **aus dem SysTick-Handler heraus**. `LUCEmbedded.ioc:38-40` gibt SysTick, USART1 und USB alle auf Priorität 0:0 — auf Cortex-M0 gibt es bei gleicher Priorität keine Verdrängung. Damit läuft `HAL_IncTick()` nicht, während wir im SysTick stecken, `HAL_GetTick()` steht → die 1000-ms-Timeout-Bedingung in `UART_WaitOnFlagUntilTimeout` wird **niemals** wahr. Bleibt TXE/TC aus, hängt das Gerät dauerhaft.

Im Normalfall blockiert es „nur" ~0,5 ms pro Byte (19200 Bd) den Tick **und** den USART-RX-Interrupt → provoziert Overruns, siehe A4.

### A4. Overrun stoppt den Empfang endgültig (leerer ErrorCallback)
`Src/main.c:182-185`

`HAL_UART_ErrorCallback()` ist leer. Im HAL (FW_F0 V1.11.6) ist ORE ein *blocking error*: `HAL_UART_IRQHandler` ruft `UART_EndRxTransfer()` (löscht RXNEIE/PEIE/EIE, `RxState = READY`, `RxISR = NULL`) und danach den ErrorCallback — `HAL_UART_RxCpltCallback` kommt **nicht**, das Nach-Armieren in `Src/main.c:179` läuft also nie. Erholung nur über `open_lin_hw_reset()`:
- Master: passiert bei jedem RX-Frame (`open_lin_master_data_layer.c:143`) → erholt sich
- Monitor: nur wenn der Host selbst sendet (`Src/lin_slcan.c:182`)
- Slave: nie

→ Monitor und Slave bleiben nach einem Overrun **dauerhaft stumm**.

### A5. `slcanOutputFlush()` — Deadlock statt Busy-Wait
`Src/slcan.c:68-73`

```c
while (((USBD_CDC_HandleTypeDef*)hUsbDeviceFS.pClassData)->TxState){;}
while (CDC_Transmit_FS(sl_frame, sl_frame_len) != USBD_OK);
```
Diese Funktion wird auch aus Interrupt-Kontext erreicht (USART1-IRQ über `slcanReciveCanFrame`, SysTick über `lin_slcan_rx_timeout_handler`). Da USB_IRQn dieselbe Priorität 0 hat, kann `USB_IRQHandler` `TxState` nicht mehr löschen, während wir hier warten → **harter, nicht auflösbarer Deadlock**, kein bloßes Warten. Zusätzlich fehlt ein NULL-Check auf `pClassData` (unkritisch, solange nur nach einem Host-Kommando geflusht wird, aber nicht sauber bei Disconnect).

---

## B — Protokoll- und Logikfehler

### B1. ID-Offset in `transmitStd()` widerspricht `addLinMasterRow()`
`Src/slcan.c:148-151` vs. `Src/lin_slcan.c:46-47,68`

`transmitStd()` liest die ID bei `line[2 + offset]`, `addLinMasterRow()` bei `line[2]` (ohne Offset). Für Großbuchstaben-Kommandos (`'T'`,`'R'` < `'Z'` → `offset = 5`) liest `transmitStd()` damit `line[7..8]` — also Teile des Timeout-Feldes statt der ID. Länge (`line[4+offset]`) und Daten (`line[5+offset+2i]`) stimmen dagegen überein; **nur die ID ist falsch**. Fix: `parseHex(&line[2], 2, &temp)`.

### B2. Classic-Checksum-Erkennung passt nicht zu B1
`Src/slcan.c:293-299`

Die Erkennung von `0x8…/0x9…/0xA…/0xB…` (Classic-CS laut README) prüft und überschreibt immer `line[2]`. Das passt zu `addLinMasterRow`, widerspricht aber `transmitStd`: bei `T`/`R` im Monitor-Modus wird das Flag aus einem Byte entfernt, das dort gar nicht die ID ist.

### B3. `checksumtype` wird nie gesetzt — Classic-CS geht im Monitor-Modus verloren
`Src/lin_slcan.c:134,170,181-188`

`open_lin_data_layer_frame.checksumtype` wird an keiner Stelle zugewiesen, bleibt also permanent `0` = `OPEN_LIN_CHECKSUM_TYPE_EXTENDED`. `lin_slcan_skip_header_reception()` nimmt nur den PID und verwirft die in `transmitStd()` (`Src/slcan.c:147`) bereits berechnete `slot.checksum_type`. Ein `t0a…`-Kommando (Classic) wird dadurch mit dem Extended-Algorithmus geprüft → gültige Antworten werden verworfen.

### B4. Slave-Modus: fehlendes `break` → doppelte Verarbeitung
`Src/main.c:172-176`

```c
case LIN_SLAVE:
    open_lin_slave_rx_header(rbyte);
default: /* Monitor */
    lin_slcan_monitor_rx(rbyte);
```
Im Slave-Modus laufen **beide** Statemachines über jedes Byte. Schlimmer: `open_lin_slave_rx_header()` ruft `open_lin_hw_check_for_break()`, das LBDF **löscht** (`open_lin_hw_st32.c:29`) — die Monitor-Statemachine (`Src/lin_slcan.c:191`) sieht den Break also nie und kann sich im Slave-Modus nie synchronisieren.

### B5. Uppercase `T`/`R` im Monitor-Modus sendet keinen Header — wird aber quittiert
`Src/slcan.c:167-178`

`if (offset == 0) open_lin_master_data_tx_header(&slot);` — bei `offset == 5` geht also nie Break/Sync/PID auf den Bus. Bei `R` ist zusätzlich `lin_data == false`, es wird **gar nichts** gesendet, die Funktion gibt trotzdem `1` zurück, der Host bekommt `'Z'` und es wird ein 3-ms-Empfangsfenster geöffnet (Z. 176).

### B6. Fehlerhafte Kommandos werden im Master/Slave-Modus positiv quittiert
`Src/slcan.c:305-311`

`result = terminator;` steht **außerhalb** des `if (addLinMasterRow(...) == 1)`. Im Monitor-Zweig (Z. 313-323) ist es korrekt innerhalb. Dieselbe Falscheingabe wird also je nach Modus mit BELL (NAK) oder CR (ACK) beantwortet.

### B7. `command[]` wird nicht null-terminiert
`Src/slcan.c:96-97`

```c
line[linepos] = 0;
memcpy(command, line, linepos);   // die 0 wird nicht mitkopiert
```
`command` ist static; nach `slCanCheckCommand()` wird nur `command[0]` gelöscht (Z. 336). Die Bytes `[linepos..61]` bleiben also als Reste des vorherigen Kommandos stehen. Konkret: nach `t0163112233` lässt ein kurzes `t063` `parseHex` (`Src/slcan.c:162`, `Src/lin_slcan.c:118`) auf Altdaten erfolgreich laufen statt `0` zurückzugeben. Fix: `memcpy(command, line, linepos + 1)`.

### B8. Fester 3-ms-Timeout unabhängig von der Baudrate
`Src/stm32f0xx_it.c:138`

`HAL_GetTick() - slcan_lin_timeout_counter > 3` ist hart verdrahtet, während `lin_baund_rate` per `'S'`-Kommando zwischen 19200 und 9600 umgeschaltet wird (`Src/slcan.c:211-219`). Bei 9600 Bd ist ein Byte ~1,04 ms — 3 ms sind dann nur noch ~2,9 Bytezeiten Reserve. Der Zähler wird pro Byte nachgezogen (`Src/lin_slcan.c:221,225`), aber Header→Response-Lücke und Frame-Ende-Erkennung skalieren nicht mit.

### B9. `'C'` (Close) setzt einen anderen Default als der Boot-Zustand
`Src/slcan.c:281-285` vs. `Src/slcan.c:26`

Power-on: `lin_type = LIN_MONITOR`. Nach `'C'`: `lin_type = LIN_MASTER`. „Close-dann-`t…`" verhält sich also anders als „Boot-dann-`t…`". (Dass `'o'/'O'` — anders als `'l'`/`'L'` — den Kanal nicht öffnet, ist dagegen beabsichtigt: das macht das `t1`/`T1`-Subkommando, `Src/lin_slcan.c:54-64`.)

### B10. `master_table_index` wird bei Neukonfiguration nicht zurückgesetzt
`open_lin_master_data_layer.c:31-50`

`open_lin_master_dl_init()` → `open_lin_master_goto_idle(l_false)` setzt `master_rx_count`, State und Zeit zurück, aber **nicht** `master_table_index`. Nach dem Verkleinern der Tabelle wird einmal eine veraltete Zeile gesendet, bevor Z. 60 den Index auf 0 klemmt. Kein Out-of-Bounds (die Tabelle ist immer das feste 64er-Array), aber ein Geisterframe.

---

## C — Nebenläufigkeit (durchgängig ungeschützt)

### C1. `sl_frame` / `sl_frame_len` — Race zwischen Mainloop, USART1-ISR und SysTick
`Src/slcan.c:31-32`

Nicht `volatile`, keine Critical Sections. Schreiber:
- Mainloop: `Src/main.c:110` → `slCanCheckCommand` → `Src/slcan.c:337-338`
- USART1-ISR: `stm32f0xx_it.c:160` → `main.c:175` → `lin_slcan.c:150/243` → `slcanReciveCanFrame`, sowie `main.c:170` → `open_lin_hw_st32.c:90`
- SysTick: `stm32f0xx_it.c:136,140` → `lin_slcan.c:162,167,173`

Ein LIN-Frame, das während des Zusammenbaus einer Kommandoantwort eintrifft, mischt sich in denselben Puffer. (Im gesamten Applikationscode kommt `volatile` nicht vor.)

### C2. `command[]` — Race USB-ISR gegen Mainloop
`Src/slcan.c:89-105` vs. `195-336`

`slCanProccesInput()` läuft im USB-Interrupt (`Src/usbd_cdc_if.c:265` in `CDC_Receive_FS`) und `memcpy`t in das nicht-`volatile` `command`, während die Mainloop es liest **und verändert** (Z. 293-298 schreibt `line[2]` in-place, Z. 336 löscht `line[0]`). Ein zweites Kommando mitten in der Ausführung überschreibt das laufende. Empfehlung: Doppelpuffer + `volatile` Flag.

### C3. Vollständige UART-Reinitialisierung mitten im Betrieb, aus dem SysTick
`open_lin_hw_st32.c:49-58`

`open_lin_set_rx_enabled(true)` ruft `open_lin_hw_reset()` → `HAL_LIN_Init()`, also ein komplettes `HAL_UART_Init` (USART1 aus, umprogrammieren, an) — und das bei **jedem** Master-RX-Frame (`open_lin_master_data_layer.c:143`). Der `false`-Pfad (Z. 56) umgeht den HAL komplett und löscht direkt `RXNEIE|PEIE`, lässt `huart1.RxState` also auf `BUSY_RX` stehen; er läuft in jedem Idle-Tick (`open_lin_master_data_layer.c:159`). Bytes, die während der Reinit ankommen, sind verloren.

### C4. Blockierende und rechenintensive Arbeit im ISR
Checksummenberechnung (`Src/lin_slcan.c:170` — im Code selbst als `/* TODO remove from interrupt */` markiert), USB-Flush (A5) und UART-Transmit (A3) laufen alle in Interrupt-Kontext, bei durchgängig gleicher NVIC-Priorität 0.

---

## D — Deklarations-, Typ- und Build-Inkonsistenzen

| # | Ort | Befund |
|---|---|---|
| D1 | `Inc/lin_slcan.h:11` vs. `Src/lin_slcan.c:16` | Prototyp `uint8_t* out_index` vs. Definition `int8_t*`. Fällt nur nicht auf, weil `lin_slcan.c` sein eigenes Header nie einbindet. |
| D2 | `Inc/lin_slcan.h:11-15` | Header ist nicht selbstständig: nutzt `t_master_frame_table_item`, `open_lin_pid_t`, `l_u8` ohne ein einziges `#include`. Kompiliert nur *nach* `slcan.h`. Strukturelle Ursache von D1. |
| D3 | `Src/slcan.c:22` vs. `Src/main.c:63` | `extern int32_t serialNumber;` vs. `uint32_t serialNumber = 0;` — Typkonflikt über TU-Grenzen. Zudem wird `serialNumber` nie aus `uid` gefüllt → `'N'` liefert immer `N00000000`; `uid` (`main.c:62`) ist komplett unbenutzt. |
| D4 | `open_lin_hw_st32.c:35` vs. `open_lin_hw.h:42` | Definition `open_lin_frame_set_auto_baud()` vs. Deklaration/Aufrufe `open_lin_hw_set_auto_baud()` (`lin_slcan.c:195`, `open_lin_slave_data_layer.c:72`). Latenter Linkfehler, sobald `OPEN_LIN_AUTO_BAUND` aktiviert wird. Alle vier Backends heißen falsch — das Header ist der Ausreißer. |
| D5 | `open_lin_transport_layer.c:11-14` | `open_lin_NAD`, `open_lin_supplier_id`, `open_lin_function_id` sind nur in der **ausgeschlossenen** pic16-Datei definiert; `open_lin_sid_callback` nirgends. Die Datei wird gebaut (`OPEN_LIN_TRANSPORT_LAYER` ist in `open_lin_cfg.h:24` aktiv) und linkt nur, weil `open_lin_transport_layer_handle()` keinen Aufrufer hat und die Symbole weggeräumt werden. `.cproject` serialisiert `-ffunction-sections`/`--gc-sections` nicht — das Projekt hängt hier an CubeIDE-Defaults. |
| D6 | `open_lin_transport_layer.c:35-41` | Beide Zweige von `#ifdef PLATFORM_IS_BIG_ENDIAN` sind byteidentisch — die Byte-Order-Behandlung ist eine Attrappe. |
| D7 | `Inc/slcan.h:28,30` | `slcanClose()` und `slcanReciveCanFrameWithCS()` deklariert, nirgends definiert oder benutzt. Ebenso `RebootToBootloader()` (`Src/slcan.c:24`, Aufruf nur auskommentiert Z. 333). |
| D8 | `open_lin_hw.h:29` / `open_lin_slave_data_layer.h:21` | `open_lin_hw_init()` im Build nicht definiert (nur im ausgeschlossenen x86-Backend); `open_lin_slave_init()` definiert, aber nie aufgerufen. |
| D9 | `open_lin_slave_data_layer.c:77-89` | Bei ungültigem Sync-Byte fällt der `SYNC_RX`-Case in `PID_RX` durch (das `break` steht im `else`). Aktuell tot, weil `OPEN_LIN_HW_BREAK_IS_SYNCH_BYTE` gesetzt ist und `SYNC_RX` damit nie erreicht wird — bleibt aber eine Falle, wenn die Option umgestellt wird. Dieselbe Option ist in `lin_slcan.c:190-196` hart auf „Break = Sync" verdrahtet, ohne `#ifdef`. |
| D10 | `Src/lin_slcan.c:18-25` | `uint8_t i = -1;` ist tot (wird vom `for`-Init überschrieben), und `return &master_frame_table[i]` hat keinen Bounds-Check. Aktuell **nicht** erreichbar, weil `temp &= 0x3f` (Z. 71) nur 64 IDs zulässt und `MAX_SLAVES_COUNT == 64` ist — die Invariante ist aber nirgends dokumentiert und bricht, sobald `MAX_SLAVES_COUNT` verkleinert wird. |
| D11 | `Src/slcan.c:229-230` | `VERSION_HARDWARE_MAJOR` ist definiert, die Ausgabe aber auskommentiert → `'V'` liefert nur ein Byte, `'v'` zwei. Inkonsistente Versionsantwort. |
| D12 | `.cproject` (Include-Pfade) | `../Src/open-LIN-c` steht in den Compiler-Optionen, fehlt aber in der von CubeMX gepflegten Pfadliste → ein Projekt-Regen aus dem `.ioc` kann den Include-Pfad verlieren. |

---

## E — Kosmetik / Hygiene

- **`*.bak`-Dateien im Repo** (`main.c.bak`, `stm32f0xx_it.c.bak`, `usart.c.bak`, `usbd_cdc_if.c.bak`, `open_lin_network_layer.c.bak`): das sind CubeMX-Backups *ohne* den User-Code. Sie gehören in `.gitignore` (das aktuell nur `/Debug` enthält) — Verwechslungsgefahr beim Zurückkopieren.
- `Uart2RxFifo` (`Src/main.c:64`) ist ein einzelnes `uint8_t` an USART**1** — Name führt doppelt in die Irre.
- `MAX_SLAVES_COUNT` (`Inc/slcan.h:20`) ist die Größe der *Frame*-Tabelle, nicht die Slave-Anzahl. Kostet 512 B Tabelle + 512 B `lin_master_data` von 6 KB RAM (plus 1 KB Stack, 512 B Heap, 512 B USB-Puffer) — passt, ist aber knapp.
- `LIN_SLAVE_SIGNALS` (`Inc/slcan.h:21`) unbenutzt.
- `open_lin_net_init()` wird in `Src/lin_slcan.c:37-38` lokal erneut deklariert, obwohl `slcan.h` das Header schon einbindet.
- `Src/slcan.c:291` `// shame on you that you put this code here ...` — der Kommentar hat recht: die Classic-CS-Vorverarbeitung gehört in `transmitStd`/`addLinMasterRow`, nicht in den Dispatcher (siehe B2).
- README nennt als Testhardware einen **STM32L**042G6GQ, das Projekt ist auf **STM32F**042G6Ux gebaut (`.ioc`, Linkerskript, Startup) — vermutlich Tippfehler.
- `open_lin_frame_slot_t` (`open_lin_network_layer.h:15-21`) nutzt 1-Bit-Enum-Bitfelder. Mit GCC/ARM funktioniert das (Enums ohne negative Werte werden unsigned), ist aber implementierungsabhängig.
- `open_lin_net_init()` bekommt einen `open_lin_frame_slot_t*`, in den in Wahrheit ein `t_master_frame_table_item*` gecastet wird, und castet intern zurück (`open_lin_network_layer.c:55-56`, `Src/lin_slcan.c:56`). Funktioniert, ist aber typunsicher — besser eine eigene Signatur.

---

## Geprüft und verworfen (keine echten Befunde)

- **Deklaration direkt nach `case 't':`** (`Src/slcan.c:287-292`) — formal eine C11-Verletzung, GCC akzeptiert das aber in jedem nicht-pedantischen Modus; `.cproject` setzt kein `-std` und kein `-pedantic`. Kompiliert.
- **Doppelte Definitionen der HW-Backends** — `.cproject` schließt `x86`, `pic16`, `STM32F1`, `unit_test` korrekt aus.
- **Out-of-Bounds in `slcan_get_master_table_row`** — durch `temp &= 0x3f` + `MAX_SLAVES_COUNT == 64` aktuell nicht erreichbar (siehe D10).
- **Out-of-Bounds über `master_table_index`** — durch die Klemmung in `data_layer_next_item` und das feste 64er-Array nicht erreichbar (siehe B10).
- **Monitor-Modus verwechselt Sync-Byte mit PID** — geprüft: `parity(0x55) == 0x55`, wäre also ein gültiger PID (ID 0x15). Passiert aber nicht, weil `OPEN_LIN_HW_BREAK_IS_SYNCH_BYTE` gesetzt ist und LBDF erst beim Sync-Byte ausgewertet wird, das dabei verworfen wird. Konsistent.
- **Länge 9 im Monitor-Modus** (8 Datenbytes + Checksumme) passt zum README-Beispiel und zu `data_length:6` sowie zur Nibble-Ausgabe.

---

## Vorschlag für die Reihenfolge

1. **A2 + A1** — zwei Zeilen (`else { ...; return; }` bzw. `break`), verhindern einen HardFault.
2. **B4** — fehlendes `break` in `Src/main.c`.
3. **A5 + C1/C2** — Ausgabe entkoppeln: LIN-Frames aus dem ISR in einen Ringpuffer legen, Flush ausschließlich in der Mainloop; `command` doppelpuffern.
4. **A3 + C3** — LIN-TX aus dem SysTick herausziehen (Flag setzen, in der Mainloop senden bzw. `HAL_UART_Transmit_IT`), NVIC-Prioritäten staffeln (USART1 > SysTick > USB).
5. **A4** — `HAL_UART_ErrorCallback()` implementieren: Fehlerflags löschen und `HAL_UART_Receive_IT` neu armieren.
6. **B7, B6, B1/B2, B3** — Parser- und Quittungsfehler.
7. **D1-D12** — Deklarationen aufräumen, `.bak` aus dem Repo.
