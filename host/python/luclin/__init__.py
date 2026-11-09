"""
luclin - Python-Bibliothek fuer den LUC-LIN-Adapter (Protokoll Version 1)

Beispiel:

    from luclin import LucLin

    with LucLin("COM6") as lin:
        lin.baud = 19200
        lin.frame(0x21, "SUB", 8)
        lin.schedule([(0x21, 20), (0x2F, 20)])
        lin.on_frame(print)
        lin.mode("MASTER")
        f = lin.wait_frame(0x21, timeout=1.0)
        print(f.data.hex())
"""
from .device import LucLin, LucError, Frame, DiagResult, list_ports
from .lin import lin_pid, lin_checksum

__all__ = ["LucLin", "LucError", "Frame", "DiagResult", "list_ports",
           "lin_pid", "lin_checksum"]
__version__ = "1.0.0"
