#!/usr/bin/env python3
"""
Beispiel: VAG-LIN-Batterie (BDM/BZE) als Slave simulieren.

Der Adapter beantwortet die Header des Steuergeraets selbst; das Skript
aktualisiert nur zyklisch die Daten (atomar per DATA). 0x21 und 0x25 tragen
einen AUTOSAR-E2E-Profile-2-Schutz (CRC8H2F + Alive-Counter).

    python vag_batterie_slave.py COM6
"""
import sys
import time

sys.path.insert(0, __file__.rsplit("examples", 1)[0])
from luclin import LucLin  # noqa: E402

E2E_DATAID = {
    0x21: [0x54, 0xDC, 0x41, 0x83, 0x59, 0x46, 0x5C, 0xFB, 0xD3, 0x5E, 0xEA, 0x58, 0x1F, 0xD5, 0x6A, 0xB3],
    0x25: [0x41, 0xD9, 0x73, 0xB3, 0xC9, 0x09, 0x35, 0xA6, 0xA4, 0x77, 0xE7, 0x68, 0xB9, 0xC5, 0xCD, 0x85],
}


def crc8h2f(data: bytes) -> int:
    c = 0xFF
    for b in data:
        c ^= b
        for _ in range(8):
            c = ((c << 1) ^ 0x2F) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c ^ 0xFF


def e2e(fid: int, data: bytearray, counter: int) -> bytes:
    data[1] = (data[1] & 0xF0) | (counter & 0x0F)
    data[0] = crc8h2f(bytes(data[1:]) + bytes([E2E_DATAID[fid][counter & 0x0F]]))
    return bytes(data)


FRAMES = {                      # Startwerte aus dem Trace
    0x21: "510005DC00140400",
    0x25: "A73DC8B82B010E40",
    0x2E: "FFFF0100800008C3",
    0x2F: "772495803F1CC1C3",
    0x30: "0E80405C7202CC0E",
    0x31: "FEFEFEFFFC0000FF",
    0x34: "5624003E2600280D",
}


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
    with LucLin(port) as lin:
        lin.reset()
        lin.baud = 19200
        for fid, hx in FRAMES.items():
            lin.frame(fid, "PUB", 8, bytes.fromhex(hx))
        lin.options(echo=0)                     # eigene Antworten nicht melden
        lin.on_frame(lambda f: print("RX", f) if f.src == "R" else None)
        lin.mode("SLAVE")
        print("Batterie laeuft als Slave, Strg+C beendet.")
        counter = 0
        d21 = bytearray.fromhex(FRAMES[0x21])
        d25 = bytearray.fromhex(FRAMES[0x25])
        try:
            while True:
                counter = (counter + 1) & 0x0F
                lin.data(0x21, e2e(0x21, d21, counter))
                lin.data(0x25, e2e(0x25, d25, counter))
                time.sleep(0.1)
        except KeyboardInterrupt:
            pass


if __name__ == "__main__":
    main()
