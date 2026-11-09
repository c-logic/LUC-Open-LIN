#!/usr/bin/env python3
"""
Beispiel: als Master eine LIN-Batterie abfragen und die Frames ausgeben,
anschliessend die Produktkennung per Diagnose lesen.

    python master_abfrage.py COM6
"""
import sys
import time

sys.path.insert(0, __file__.rsplit("examples", 1)[0])
from luclin import LucLin  # noqa: E402


def main():
    port = sys.argv[1] if len(sys.argv) > 1 else "COM6"
    with LucLin(port) as lin:
        print(lin.version(), "SN", lin.serial_number())
        lin.reset()
        lin.baud = 19200
        for fid in (0x21, 0x2E, 0x2F, 0x30, 0x34):
            lin.frame(fid, "SUB", 8)
        lin.schedule([(0x21, 20), (0x2E, 20), (0x2F, 20), (0x30, 20), (0x34, 20)])
        lin.on_frame(print)
        lin.mode("MASTER")
        time.sleep(2.0)

        r = lin.read_by_id(0x7F)                 # Broadcast-NAD
        print("Read by Identifier:", r.status, r.data.hex(" ").upper())
        print("Statistik:", lin.stats())


if __name__ == "__main__":
    main()
