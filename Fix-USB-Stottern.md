# Fix: stockende USB-Ausgabe im Masterbetrieb

Ziel: Interrupt-Kontext und Mainloop entkoppeln. Vorher liefen die USB-Ausgabe
und die blockierenden LIN-Sendevorgaenge in Interrupt-Handlern, die sich bei
durchgaengiger NVIC-Prioritaet 0 gegenseitig blockiert haben.

Gebaut und geprueft mit arm-none-eabi-gcc 13.2 (`-Os`), zusaetzlich mit
`-Wall -Wextra -Wshadow -Wundef -Wcast-align -Wstrict-prototypes` gegengelesen.

| | vorher | nachher |
|---|---|---|
| Flash | 20544 B | 21220 B (+676) von 32768 |
| RAM (data+bss inkl. 1536 B Heap/Stack) | 6008 B | 5896 B (-112) |
| RAM frei | 136 B | 248 B |

## 1. USB-Ausgabe raus aus dem Interrupt

`slcan.c`: `slcanOutputFlush()` mit seinen zwei Warteschleifen ist weg.

- Erzeuger (Mainloop **und** ISRs) bauen ihre Zeile in einem lokalen Puffer
  zusammen (`sl_line_t`) und haengen sie mit `sl_commit()` atomar (PRIMASK) an
  `tx_ring` (256 B). Passt eine Zeile nicht mehr, wird sie komplett verworfen -
  es entstehen keine halben Zeilen.
- Gesendet wird ausschliesslich in `slCanOutputPump()` aus der Mainloop, ohne
  Blockieren, zero-copy direkt aus dem Ring (`USB_WritePMA` kopiert innerhalb
  von `CDC_Transmit_FS` synchron ins PMA, geprueft in `stm32f0xx_ll_usb.c`).
- Die globalen `sl_frame` und `command` sind entfallen.

## 2. USB-Empfang raus aus dem Interrupt

`slCanProccesInput()` (USB-ISR) legt Bytes nur noch in `rx_ring` (128 B).
Zeilenaufbau und Ausfuehrung passieren in `slCanCheckCommand()` in der Mainloop.
Damit ist der Wettlauf auf `command[]` weg - vorher hat der USB-ISR den Puffer
ueberschrieben, waehrend die Mainloop ihn ausgewertet und in-place veraendert
hat. Der fehlende Null-Terminator (`memcpy(command,line,linepos)`) erledigt sich
mit.

## 3. LIN-Master-Schedule raus aus dem SysTick

`SysTick_Handler` ruft nur noch `slCanTickIsr()` (zaehlt `lin_ms_pending`).
`slCanServiceLin()` in der Mainloop arbeitet die angesammelten Millisekunden ab.
Vorher blockierte `HAL_UART_Transmit(...,1000)` den SysTick 1,7 ms (Header) bis
6,4 ms (Header + 8 Datenbytes) bei 19200 Bd. In dieser Zeit gingen SysTick-Ticks
verloren, weil das Pending-Bit nur einen Interrupt haelt - der Schedule lief
dadurch langsamer und je Frametyp unterschiedlich stark daneben.

## 4. NVIC-Prioritaeten gestaffelt

USART1 = 0, USB = 1, SysTick = 3 (`TICK_INT_PRIORITY`). Auch im `.ioc`
nachgezogen, damit ein CubeMX-Regen das nicht zurueckdreht.

## 5. Empfang wird nicht mehr pro Frame neu initialisiert

`open_lin_set_rx_enabled()` machte im `true`-Zweig ein volles `HAL_LIN_Init`
(USART aus, umkonfigurieren, an, Warten auf TEACK/REACK, ~100-200 us) - und das
**nach** dem Header, also genau wenn der Slave zu antworten anfaengt. Die ersten
Antwortbytes gingen verloren, das Frame lief in den Timeout und wurde still
verworfen.

Jetzt: Flags und ein stehengebliebenes Byte verwerfen, RXNEIE/EIE schalten, bei
abgebrochenem HAL-Empfang neu armieren. Aufgerufen wird es in
`open_lin_master_dl_handler()` gezielt vor dem Header (aus) und direkt danach
(an) statt nur danach - und nicht mehr in jedem Millisekunden-Tick.
Der `false`-Zweig legt jetzt auch `CR3.EIE` still und verwirft das Echo der
eigenen Sendebytes, damit kein ORE auflaeuft. Dasselbe in
`lin_slcan_skip_header_reception()` fuer den Monitor-Sendepfad.

## 6. Overrun beendet den Empfang nicht mehr dauerhaft

`HAL_UART_ErrorCallback()` war leer. Die HAL behandelt ORE als *blocking error*
und bricht dabei `HAL_UART_Receive_IT` ab (`UART_EndRxTransfer` setzt
`RxISR = NULL`); `HAL_UART_RxCpltCallback` wird nicht aufgerufen, das
Neuarmieren in `main.c` lief also nie. Monitor- und Slavebetrieb blieben danach
dauerhaft stumm. Jetzt: Flags quittieren, `ErrorCode` loeschen, neu armieren.

## 7. Absturz- und Logikfehler

- `main.c`: fehlendes `break` bei `case LIN_SLAVE` - im Slavebetrieb liefen
  beide Statemachines ueber jedes Byte, und weil `open_lin_slave_rx_header()`
  das LBDF-Flag loescht, hat der Monitor-Pfad den Break nie gesehen.
- `lin_slcan.c`, `OPEN_LIN_SLAVE_PID_RX`: bei Paritaetsfehler wurde
  `lin_slcan_reset()` von der folgenden unbedingten Zuweisung sofort wieder
  ueberschrieben. Folge war ein Frame mit der Id des Vorgaengers - und beim
  allerersten Frame nach dem Reset ein Schreibzugriff ueber `data_ptr == NULL`,
  also HardFault. Jetzt `break`, und `lin_slcan_reset()` setzt `data_ptr`
  grundsaetzlich.
- `open_lin_slave_data_layer.c`: dasselbe Muster im Slave-Stack.
- `open_lin_master_dl_init()` setzt `master_table_index` zurueck.
- Master-Empfangstimeout prueft den Zustand erneut, nachdem der Empfang
  stillgelegt ist - sonst konnte `open_lin_master_goto_idle()` zweimal laufen
  (einmal aus der ISR, einmal aus der Mainloop) und einen Tabelleneintrag
  ueberspringen.
- `open_lin_on_rx_frame()` prueft auf `OPEN_LIN_NET_SLOT_EMPTY`.

## 8. Frametabelle: 64 Eintraege, jetzt mit Bereichspruefung

`slcan_get_master_table_row()` hatte keine Grenzpruefung und lieferte bei voller
Tabelle einen Zeiger hinter das Array. Erreichbar war das nur nicht, weil
`id &= 0x3f` genau 64 Werte zulaesst und `MAX_SLAVES_COUNT == 64` ist - eine
nirgends dokumentierte Kopplung. `MAX_SLAVES_COUNT` bleibt bei 64, die Funktion
liefert jetzt bei voller Tabelle `0` und `addLinMasterRow()` antwortet mit BELL.
Der Prototyp in `lin_slcan.h` stand ausserdem auf `uint8_t*` statt `int8_t*`;
das Header ist jetzt selbst-enthaltend und wird von `lin_slcan.c` eingebunden,
damit so ein Fehler auffaellt.

## 9. Timing baudratenabhaengig

- Empfangstimeout (vorher fest `> 3` ms): jetzt ~4 Zeichenzeiten,
  19200 Bd -> 3 ms, 9600 Bd -> 5 ms.
- `response_wait_ms` (vorher fest fuer ~19200 Bd gerechnet): jetzt aus
  `lin_baund_rate`. Bei 19200 Bd praktisch unveraendert, bei 9600 Bd vorher
  nur halb so lang wie eine Antwort dauert.
- `checksumtype` wird ueber `lin_slcan_skip_header_reception()` durchgereicht
  statt uninitialisiert zu bleiben.

## 10. RAM freigeraeumt

`APP_RX_DATA_SIZE`/`APP_TX_DATA_SIZE` von 256 auf 64 (=
`CDC_DATA_FS_MAX_PACKET_SIZE`). `UserTxBufferFS` wird ohnehin nie als Puffer
benutzt, `CDC_Transmit_FS` setzt den Zeiger jedes Mal selbst. Auch im `.ioc`
nachgezogen.

## Bewusst nicht angefasst

- `transmitStd()` liest die Id bei `line[2 + offset]`, `addLinMasterRow()` bei
  `line[2]`. Fuer Grossbuchstaben-Kommandos (`T`/`R`) liest `transmitStd()`
  damit das Timeout-Feld als Id. Fix waere `line[2]`, aendert aber die
  Wire-Semantik von `T`/`R` im Monitorbetrieb.
- `T`/`R` im Monitorbetrieb senden wegen `if (offset == 0)` keinen Header;
  `R` sendet gar nichts und wird trotzdem mit `Z` quittiert.
- Fehlerhafte Kommandos werden im Master/Slave-Betrieb mit CR quittiert statt
  mit BELL (`result = terminator` steht ausserhalb des `if`).
- `'C'` setzt `lin_type = LIN_MASTER`, der Einschaltzustand ist `LIN_MONITOR`.
- `serialNumber` wird nie aus `uid` gefuellt, `'N'` liefert immer 0.
- `'F'` antwortet weiter nur mit dem Terminator. Die Ringpuffer-Ueberlaeufe
  werden intern in `slcan_overrun` mitgezaehlt; drei auskommentierte Zeilen im
  `'F'`-Zweig machen sie sichtbar (slcan-konform Bit 3 = data overrun).

## Was beim Test zu sehen sein sollte

1. Der Frameabstand im Masterbetrieb ist gleichmaessig und entspricht
   `offset_ms + Framedauer`, statt je Frametyp um 1-6 ms daneben zu liegen.
2. Keine Luecken mehr durch verschluckte Antwortbytes.
3. Monitor- und Slavebetrieb erholen sich nach einem Overrun selbst.

Beachte: `offset_ms` ist die Pause **vor** einem Frame, angewandt nachdem das
vorherige fertig ist. Die Zykluszeit ist Summe(offset_ms + Framedauer) - mit dem
Default von 15 ms fuer Kleinbuchstaben-Kommandos also ~22 ms pro Tabelleneintrag.
Mit `T`/`R` laesst sich der Offset explizit setzen (0-255 ms).
