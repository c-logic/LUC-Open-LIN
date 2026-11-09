"""LIN-Hilfsfunktionen: PID, Checksumme, Framezeit."""


def lin_pid(frame_id: int) -> int:
    """Protected Identifier (ID + Paritaetsbits P0/P1)."""
    i = frame_id & 0x3F
    b = [(i >> n) & 1 for n in range(6)]
    p0 = b[0] ^ b[1] ^ b[2] ^ b[4]
    p1 = 1 - (b[1] ^ b[3] ^ b[4] ^ b[5])
    return i | (p0 << 6) | (p1 << 7)


def lin_checksum(frame_id: int, data: bytes, classic: bool = False) -> int:
    """LIN-Checksumme (enhanced inkl. PID, classic nur Daten; 3C/3D immer classic)."""
    s = 0 if (classic or (frame_id & 0x3F) >= 0x3C) else lin_pid(frame_id)
    for d in data:
        s += d
        if s > 0xFF:
            s -= 0xFF
    return (~s) & 0xFF


def frame_time_ms(length: int, baud: int = 19200) -> float:
    """Maximale Framezeit nach LIN-Spezifikation (1.4 x nominal)."""
    bits = 34 + 10 * (length + 1)
    return bits * 1.4 * 1000.0 / baud
