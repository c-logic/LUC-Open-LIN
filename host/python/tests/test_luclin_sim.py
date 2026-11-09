"""
End-to-End-Tests der Bibliothek gegen den Firmware-Simulator (tools/sim/luc_sim).
Laeuft unter Linux/macOS:  python -m pytest host/python/tests
"""
import os
import subprocess
import sys
import time

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, ".."))
from luclin import LucLin, LucError, lin_checksum, lin_pid  # noqa: E402

SIM = os.path.join(HERE, "..", "..", "..", "tools", "sim", "luc_sim")
pytestmark = pytest.mark.skipif(not os.path.exists(SIM), reason="luc_sim nicht gebaut")


@pytest.fixture
def sim(request, tmp_path):
    args = getattr(request, "param", [])
    link = str(tmp_path / "luc")
    p = subprocess.Popen([SIM, "--link", link] + args, stdout=subprocess.PIPE)
    p.stdout.readline()
    lin = LucLin(link).open()
    yield lin
    lin.close()
    p.terminate()
    p.wait()


def test_helpers():
    assert lin_pid(0x21) == 0x61 and lin_pid(0x15) == 0x55
    assert lin_pid(0x3C) == 0x3C
    assert lin_checksum(0x21, bytes.fromhex("510005DC00140400")) == 0x53
    assert lin_checksum(0x3C, bytes.fromhex("0A02B200FFFFFFFF")) == 0x41


def test_basics(sim):
    assert sim.version().startswith("LUC-LIN 4.0")
    assert len(sim.serial_number()) == 8
    sim.baud = 9600
    assert sim.baud == 9600
    with pytest.raises(LucError) as e:
        sim.baud = 100
    assert e.value.code == 2
    with pytest.raises(LucError) as e:
        sim.command("FOO")
    assert e.value.code == 7
    assert sim.mode() == "OFF"
    o = sim.options(nr=0, dslot=5)
    assert o["nr"] == 0 and o["dslot"] == 5


def test_tables(sim):
    sim.frame(0x21, "PUB", 8, bytes.fromhex("510005DC00140400"))
    sim.frame(0x3D, "ONCE", 8)
    sim.frame(0x30, "SUB", 8, classic=True)
    t = {f["id"]: f for f in sim.frames_table()}
    assert t[0x21]["data"].hex() == "510005dc00140400"
    assert t[0x30]["classic"] and t[0x3D]["classic"]
    sim.data(0x21, b"\x01" * 8)
    with pytest.raises(LucError) as e:
        sim.data(0x21, b"\x01" * 4)          # falsche Laenge
    assert e.value.code == 2
    with pytest.raises(LucError) as e:
        sim.data(0x30, b"\x01" * 8)          # SUB hat keine Daten
    assert e.value.code == 5
    sim.schedule([(0x21, 20), 0x30, (0x2F, 15)])
    assert sim.get_schedule() == [(0x21, 20), (0x30, 11), (0x2F, 15)]
    sim.delete(0x21)
    assert 0x21 not in {f["id"] for f in sim.frames_table()}
    sim.delete()
    assert sim.frames_table() == []


@pytest.mark.parametrize("sim", [["--slave", "21=510005DC00140400", "--slave", "2F+=772495803F1CC1C3"]], indirect=True)
def test_master(sim):
    got = []
    sim.on_frame(got.append)
    sim.frame(0x21, "SUB", 8)
    sim.frame(0x10, "PUB", 2, b"\x12\x34")
    sim.schedule([(0x21, 10), (0x2F, 10), (0x10, 10)])
    sim.mode("MASTER")
    time.sleep(0.6)
    sim.mode("OFF")
    f21 = [f for f in got if f.id == 0x21]
    f2f = [f for f in got if f.id == 0x2F]
    f10 = [f for f in got if f.id == 0x10]
    assert len(f21) >= 15 and all(f.ok and f.src == "R" for f in f21)
    assert f21[0].data == bytes.fromhex("510005DC00140400")
    # Zaehler im simulierten Slave laeuft
    assert f2f[1].data[0] == (f2f[0].data[0] + 1) & 0xFF
    assert all(f.src == "T" and f.data == b"\x12\x34" for f in f10)
    # Slotabstand 30 ms je Umlauf
    dt = (f21[-1].t_us - f21[0].t_us) / (len(f21) - 1)
    assert 29000 < dt < 31500
    assert sim.stats()["err"] == 0


@pytest.mark.parametrize("sim", [["--slave", "21=510005DC00140400"]], indirect=True)
def test_send(sim):
    sim.mode("MASTER")
    f = sim.request(0x21)
    assert f.ok and f.data[0] == 0x51
    f = sim.request(0x20, bytes.fromhex("EF01000000"), classic=True)
    assert f.status == "C" and f.src == "T"
    f = sim.request(0x22)
    assert f.status == "NR"
    # Kollision: Slave antwortet gleichzeitig auf 21
    f = sim.request(0x21, b"\x01\x02\x03\x04\x05\x06\x07\x08")
    assert f.status == "BIT"
    sim.mode("SLAVE")
    with pytest.raises(LucError) as e:
        sim.send(0x21)
    assert e.value.code == 3


@pytest.mark.parametrize("sim", [["--master", "21/10,3D/10,30/10", "--slave", "30=0E80405C7202CC0E"]], indirect=True)
def test_slave(sim):
    got = []
    sim.on_frame(got.append)
    sim.frame(0x21, "PUB", 8, bytes.fromhex("510005DC00140400"))
    sim.frame(0x3D, "ONCE", 8)
    sim.options(all=0, nr=0)
    sim.mode("SLAVE")
    time.sleep(0.15)
    sim.data(0x21, bytes.fromhex("810105DC00140400"))
    sim.data(0x3D, bytes.fromhex("0A06F23412785601"))
    time.sleep(0.2)
    sim.mode("OFF")
    f21 = [f for f in got if f.id == 0x21]
    assert {f.data[0] for f in f21} == {0x51, 0x81}
    assert all(f.ok and f.src == "T" for f in f21)
    # Datenwechsel ohne Mischframes
    assert all(f.data in (bytes.fromhex("510005DC00140400"), bytes.fromhex("810105DC00140400")) for f in f21)
    f3d = [f for f in got if f.id == 0x3D]
    assert len(f3d) == 1 and f3d[0].status == "C"
    assert not [f for f in got if f.id == 0x30]       # ALL=0


@pytest.mark.parametrize("sim", [["--master", "21/10,2F/10", "--slave", "21=510005DC00140400"]], indirect=True)
def test_monitor(sim):
    sim.mode("MONITOR")
    time.sleep(0.3)
    fr = sim.frames()
    assert {f.id for f in fr} == {0x21, 0x2F}
    assert all(f.status == "E" for f in fr if f.id == 0x21)
    assert all(f.status == "NR" for f in fr if f.id == 0x2F)
    assert all(f.src == "R" for f in fr if f.id == 0x21)


@pytest.mark.parametrize("sim", [["--diag", "0A", "--slave", "21=510005DC00140400"]], indirect=True)
def test_diag(sim):
    sim.options(nr=0)
    with pytest.raises(LucError) as e:
        sim.diag(0x0A, b"\xB2\x00")
    assert e.value.code == 3                         # Modus OFF
    sim.mode("DIAG")
    r = sim.read_by_id(0x0A)
    assert r.ok and r.data == bytes.fromhex("F23412785601")
    r = sim.read_data_by_identifier(0x0A, 0xF190)     # pending + FF/CF
    assert r.ok and r.data[:3] == bytes.fromhex("62F190") and r.data[3:] == b"ABCDEFGHIJKLMNOPQ"
    r = sim.diag(0x0A, bytes.fromhex("2EF1900102030405060708"))   # langer Request
    assert r.status == "NRC" and r.sid == 0x2E and r.nrc == 0x11
    sim.options(p2=200)
    r = sim.diag(0x0B, b"\xB2\x00")
    assert r.status == "TO"
    # im Master-Betrieb zwischen die Schedule-Slots
    sim.frame(0x21, "SUB", 8)
    sim.schedule([(0x21, 10)])
    sim.mode("MASTER")
    r = sim.assign_nad(0x0A, 0x0B)
    assert r.ok and r.data == b"\xF0"
    time.sleep(0.1)
    assert any(f.id == 0x21 and f.ok for f in sim.frames())


def test_timestamps_and_wake(sim):
    sim.mode("MASTER")
    sim.wake()
    sim.sleep()
    f = sim.wait_frame(0x3C, timeout=1.0)
    assert f is not None and f.status == "C" and f.data == bytes.fromhex("00FFFFFFFFFFFFFF")


@pytest.mark.parametrize("sim", [["--master", "21/20,22/20", "--slave", "21=510005DC00140400",
                                  "--slave", "22=1122,C", "--gap", "1500", "--ibgap", "300"]], indirect=True)
def test_monitor_timing_strict(sim):
    sim.frame(0x22, "SUB", 2)                    # erwartet enhanced, Slave sendet classic
    sim.options(timing=1)
    sim.mode("MONITOR")
    time.sleep(0.25)
    fr = [f for f in sim.frames() if f.id in (0x21, 0x22)]
    assert fr and all(f.ok for f in fr)
    assert {f.status for f in fr if f.id == 0x22} == {"C"}
    for f in fr:
        assert 1450 <= f.gap_us <= 1600          # Antwortpause des Slaves
        assert 250 <= f.interbyte_us <= 400      # Pause zwischen den Bytes
        assert 1000 <= f.header_us <= 1100       # 20 Bit bei 19200 Bd
        assert f.frame_us > f.header_us + f.gap_us
    sim.options(strict=1)
    sim.frames()
    time.sleep(0.15)
    fr = sim.frames()
    assert {f.status for f in fr if f.id == 0x22} == {"CS"}
    assert {f.status for f in fr if f.id == 0x21} == {"E"}
    assert sim.stats()["err"] > 0
    # ohne TIMING keine Zeitfelder
    sim.options(timing=0)
    sim.frames()
    f = sim.wait_frame(0x21, timeout=0.5)
    assert f.gap_us is None


def test_frame_line_roundtrip():
    from luclin import Frame
    line = "!F 661603 21 R 8 510005DC00140400 53 E 1040 1525 228 8949"
    f = Frame.from_fields(line.split()[1:])
    assert f.gap_us == 1525 and f.interbyte_us == 228 and f.to_line() == line
    line = "!F 641601 23 - 0 - -- NR 1040 - - -"
    f = Frame.from_fields(line.split()[1:])
    assert f.gap_us is None and f.header_us == 1040 and f.to_line() == line
