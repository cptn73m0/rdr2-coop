#!/usr/bin/env python3
"""Headless integration tests for rdr2_coop_server (no game required).

Covers: JOIN validation, roles/limits, pre-JOIN guard, Ping, host-only
messages, exact payload sizes, WorldSnapshot bounds, malformed/oversized
frames, unknown messages and players.txt persistence.

Usage:
    python tests/integration_server.py --exe build-server-v10/Release/rdr2_coop_server.exe

Requires only Python 3.8+ and a built server executable.
"""
import argparse
import os
import random
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

PROTO = 14
TYPE_FRAME = 0x04
TYPE_JOIN = 0x01
TYPE_JOIN_OK = 0x02
TYPE_PLAYER_JOINED = 0x03
TYPE_LEAVE = 0x06
TYPE_PLAYER_LEFT = 0x06
TYPE_PING = 0x0C
TYPE_PONG = 0x0D
TYPE_JOIN_REJECTED = 0x0F
TYPE_WORLD_SNAPSHOT = 0x10
TYPE_ACTION_STATE = 0x11
TYPE_PLAYER_VEHICLE = 0x12
TYPE_MISSION_EVENT = 0x13
TYPE_MISSION_STATE = 0x14
TYPE_MISSION_BARRIER = 0x15
TYPE_HOST_CHANGED = 0x16
TYPE_TRAIN_SNAPSHOT = 0x17
TYPE_NPC_SPAWN = 0x18
TYPE_NPC_FRAME = 0x19
TYPE_OUTFIT = 0x09
TYPE_HORSE_OUTFIT = 0x0A
TYPE_SHOT = 0x0B

REJECT_VERSION, REJECT_FULL, REJECT_NAME_TAKEN, REJECT_BAD_NAME = 1, 2, 3, 4

FRAME_SIZE = 88
OUTFIT_SIZE = 400
ACTION_SIZE = 24
VEHICLE_SIZE = 32
MISSION_EVENT_SIZE = 8
MISSION_STATE_SIZE = 12
SHOT_SIZE = 16
ENTITY_SIZE = 32


def log(msg):
    print(msg)


def make_frame_payload(x=1.0, y=2.0, z=3.0, heading=90.0):
    # 4f + 4B + 5f + 6Q = 88 bytes
    return struct.pack(
        "<ffffBBBBfffffQQQQQQ",
        x, y, z, heading,
        0, 0, 0, 0,          # move/state/flags/action
        1.0, 0.0, 0.0, 0.0, 0.0,  # health + aim
        0, 0, 0, 0, 0, 0,    # weapon, seq, 3 reserved, horseOutfit
    )


def make_world(count):
    header = struct.pack("<HH", count, 0)
    ents = b"".join(
        struct.pack("<QIffffBBBB", 1000 + i, 0xCF7E73BE + i,
                    1.0 + i, 2.0, 3.0, 90.0, 0, 0, 0, 0)
        for i in range(count))
    return header + ents


def make_train(count):
    header = struct.pack("<IIfBBH", 0x1234, 0xCF7E73BE, 12.5, 1, count, 0)
    ents = b"".join(
        struct.pack("<QIffffBBBB", 2000 + i, 0xCF7E73BE + i,
                    5.0 + i, 6.0, 7.0, 30.0, 1, 0, 0, 0)
        for i in range(count))
    return header + ents


def frame_bytes(type_, payload):
    return struct.pack("<I", len(payload)) + bytes([type_]) + payload


class Server:
    def __init__(self, exe, workdir, port=None, extra_args=None,
                 process_group=False, console=False):
        self.exe = str(exe)
        self.workdir = Path(workdir)
        self.port = port or random.randint(21000, 25000)
        self.extra_args = list(extra_args or [])
        self.process_group = process_group
        self.console = console
        self.proc = None

    def start(self):
        self.workdir.mkdir(parents=True, exist_ok=True)
        creationflags = 0
        startupinfo = None
        if os.name == "nt":
            creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
            if self.process_group:
                creationflags |= getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0)
            if self.console:
                # Hidden console so CTRL_BREAK_EVENT can be delivered to the
                # process group in the graceful-shutdown test.
                creationflags &= ~getattr(subprocess, "CREATE_NO_WINDOW", 0)
                creationflags |= getattr(subprocess, "CREATE_NEW_CONSOLE", 0)
                startupinfo = subprocess.STARTUPINFO()
                startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
                startupinfo.wShowWindow = getattr(subprocess, "SW_HIDE", 0)
        self.proc = subprocess.Popen(
            [self.exe, str(self.port)] + self.extra_args,
            cwd=str(self.workdir),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            creationflags=creationflags,
            startupinfo=startupinfo,
        )
        deadline = time.time() + 8.0
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError("server exited during startup")
            try:
                s = socket.create_connection(("127.0.0.1", self.port), timeout=0.3)
                s.close()
                return
            except OSError:
                time.sleep(0.1)
        raise RuntimeError("server did not start listening")

    def send_ctrl_break(self):
        if os.name != "nt" or not self.proc:
            return False
        import signal
        try:
            self.proc.send_signal(signal.CTRL_BREAK_EVENT)
            return True
        except (OSError, ValueError):
            return False

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *args):
        self.stop()


def connect(port):
    s = socket.create_connection(("127.0.0.1", port), timeout=3)
    s.settimeout(2.0)
    return s


def join_payload(name, version=PROTO, model=0xCF7E73BE):
    nb = name.encode("utf-8")
    return bytes([len(nb)]) + nb + struct.pack("<Q", model) + struct.pack("<H", version)


def join(s, name, version=PROTO):
    s.sendall(frame_bytes(TYPE_JOIN, join_payload(name, version)))


def recv_msgs(s, duration=0.8):
    s.settimeout(0.25)
    out, buf = [], b""
    end = time.time() + duration
    while time.time() < end:
        try:
            data = s.recv(65536)
            if not data:
                break
            buf += data
            while len(buf) >= 5:
                ln = struct.unpack("<I", buf[:4])[0]
                if len(buf) < 5 + ln:
                    break
                out.append((buf[4], buf[5:5 + ln]))
                buf = buf[5 + ln:]
        except socket.timeout:
            pass
    return out


def recv_type(s, target, duration=1.5):
    end = time.time() + duration
    while time.time() < end:
        for t, p in recv_msgs(s, duration=0.3):
            if t == target:
                return p
    return None


def parse_join_ok(p):
    session = struct.unpack("<Q", p[:8])[0]
    has = p[8] != 0
    off = 9
    coords = None
    if has:
        coords = struct.unpack("<ffff", p[off:off + 16])
        off += 16
    is_host = p[off] != 0
    return session, has, coords, is_host


def expect_reject(s, reason):
    p = recv_type(s, TYPE_JOIN_REJECTED)
    assert p is not None, "expected JoinRejected"
    assert p and p[0] == reason, f"expected reject {reason}, got {p[0] if p else None}"


def test_join_rejections(exe, tmp):
    log("[test] JOIN validation")
    with Server(exe, tmp / "join") as srv:
        s = connect(srv.port)
        join(s, "OldClient", PROTO - 1)
        expect_reject(s, REJECT_VERSION)
        s.close()

        s = connect(srv.port)
        join(s, "")
        s.settimeout(1.0)
        try:
            assert s.recv(1024) == b"", "empty name should close"
        except socket.timeout:
            pass  # also acceptable: connection dropped without a frame
        s.close()

        s = connect(srv.port)
        join(s, "Bad:Name")
        p = recv_type(s, TYPE_JOIN_REJECTED)
        if p is not None:
            assert p[0] == REJECT_BAD_NAME
        else:
            assert s.recv(1024) == b""
        s.close()

        s = connect(srv.port)
        join(s, "A" * 40)
        s.settimeout(1.0)
        try:
            assert s.recv(1024) == b"", "long name should close"
        except socket.timeout:
            pass
        s.close()

        # server still alive
        s = connect(srv.port)
        join(s, "StillAlive")
        p = recv_type(s, TYPE_JOIN_OK)
        assert p is not None, "server should still accept valid JOIN"
        s.close()


def test_roles_and_full(exe, tmp):
    log("[test] roles / limits / duplicate name")
    with Server(exe, tmp / "roles") as srv:
        host = connect(srv.port)
        join(host, "Alice")
        p = recv_type(host, TYPE_JOIN_OK)
        assert p is not None
        _, has, _, is_host = parse_join_ok(p)
        assert is_host and not has

        dup = connect(srv.port)
        join(dup, "Alice")
        expect_reject(dup, REJECT_NAME_TAKEN)
        dup.close()

        guest = connect(srv.port)
        join(guest, "Bob")
        p = recv_type(guest, TYPE_JOIN_OK)
        assert p is not None
        _, _, _, is_guest_host = parse_join_ok(p)
        assert not is_guest_host
        assert recv_type(host, TYPE_PLAYER_JOINED) is not None

        third = connect(srv.port)
        join(third, "Charlie")
        expect_reject(third, REJECT_FULL)
        third.close()

        host.close()
        guest.close()


def test_prejoin_and_ping(exe, tmp):
    log("[test] pre-JOIN guard + Ping")
    with Server(exe, tmp / "prejoin") as srv:
        s = connect(srv.port)
        s.sendall(frame_bytes(TYPE_FRAME, make_frame_payload()))
        s.settimeout(1.0)
        try:
            assert s.recv(1024) == b"", "pre-JOIN Frame should close connection"
        except socket.timeout:
            pass
        s.close()

        p = connect(srv.port)
        payload = b"ping-payload"
        p.sendall(frame_bytes(TYPE_PING, payload))
        pong = recv_type(p, TYPE_PONG)
        assert pong == payload, "Ping should be echoed before JOIN"
        p.close()


def test_world_host_only(exe, tmp):
    log("[test] WorldSnapshot host-only + bounds")
    with Server(exe, tmp / "world") as srv:
        host = connect(srv.port)
        join(host, "Alice")
        assert recv_type(host, TYPE_JOIN_OK) is not None
        guest = connect(srv.port)
        join(guest, "Bob")
        assert recv_type(guest, TYPE_JOIN_OK) is not None
        recv_msgs(host, 0.3)
        recv_msgs(guest, 0.3)

        valid = make_world(64)
        host.sendall(frame_bytes(TYPE_WORLD_SNAPSHOT, valid))
        got = recv_type(guest, TYPE_WORLD_SNAPSHOT)
        assert got == valid, "host snapshot 64 must relay byte-identical"

        invalid = make_world(65)
        host.sendall(frame_bytes(TYPE_WORLD_SNAPSHOT, invalid))
        assert recv_type(guest, TYPE_WORLD_SNAPSHOT, 0.5) is None, \
            "snapshot with count > max must be dropped"

        guest.sendall(frame_bytes(TYPE_WORLD_SNAPSHOT, valid))
        assert recv_type(host, TYPE_WORLD_SNAPSHOT, 0.5) is None, \
            "guest WorldSnapshot must be blocked"
        host.close()
        guest.close()


def test_payload_sizes_and_host_only_messages(exe, tmp):
    log("[test] exact payload sizes + mission/vehicle relay")
    with Server(exe, tmp / "relay") as srv:
        host = connect(srv.port)
        join(host, "Alice")
        assert recv_type(host, TYPE_JOIN_OK) is not None
        guest = connect(srv.port)
        join(guest, "Bob")
        assert recv_type(guest, TYPE_JOIN_OK) is not None
        recv_msgs(host, 0.3)
        recv_msgs(guest, 0.3)

        frame = make_frame_payload(12.5, -3.25, 40.0, 45.0)
        assert len(frame) == FRAME_SIZE
        host.sendall(frame_bytes(TYPE_FRAME, frame))
        assert recv_type(guest, TYPE_FRAME) == frame

        host.sendall(frame_bytes(TYPE_FRAME, frame[:-1]))
        assert recv_type(guest, TYPE_FRAME, 0.4) is None, "short Frame must be dropped"

        outfit = b"\x11" * OUTFIT_SIZE
        host.sendall(frame_bytes(TYPE_OUTFIT, outfit))
        assert recv_type(guest, TYPE_OUTFIT) == outfit
        host.sendall(frame_bytes(TYPE_OUTFIT, outfit[:-1]))
        assert recv_type(guest, TYPE_OUTFIT, 0.4) is None

        action = b"\x22" * ACTION_SIZE
        vehicle = b"\x33" * VEHICLE_SIZE
        shot = b"\x44" * SHOT_SIZE
        mission_event = struct.pack("<IBBH", 0x12345678, 3, 1, 0)
        mission_state = struct.pack("<IIBBH", 0x12345678, 0x87654321, 1, 0, 0)
        assert len(mission_event) == MISSION_EVENT_SIZE
        assert len(mission_state) == MISSION_STATE_SIZE
        host.sendall(frame_bytes(TYPE_ACTION_STATE, action))
        host.sendall(frame_bytes(TYPE_PLAYER_VEHICLE, vehicle))
        host.sendall(frame_bytes(TYPE_SHOT, shot))
        host.sendall(frame_bytes(TYPE_MISSION_EVENT, mission_event))
        host.sendall(frame_bytes(TYPE_MISSION_STATE, mission_state))
        msgs = recv_msgs(guest, 0.6)
        assert (TYPE_ACTION_STATE, action) in msgs
        assert (TYPE_PLAYER_VEHICLE, vehicle) in msgs
        assert (TYPE_SHOT, shot) in msgs
        assert (TYPE_MISSION_EVENT, mission_event) in msgs
        assert (TYPE_MISSION_STATE, mission_state) in msgs

        # Guest cannot spoof host-only messages.
        guest.sendall(frame_bytes(TYPE_MISSION_EVENT, mission_event))
        guest.sendall(frame_bytes(TYPE_MISSION_STATE, mission_state))
        assert recv_type(host, TYPE_MISSION_EVENT, 0.4) is None
        assert recv_type(host, TYPE_MISSION_STATE, 0.4) is None

        # Mission barrier: host Offer/StartWindow/Abort -> guest; guest Ready/Started -> host.
        offer = struct.pack("<IIBBH", 0x12345678, 7, 1, 0, 0)  # Offer
        ready = struct.pack("<IIBBH", 0x12345678, 7, 2, 1, 0)  # Ready eligible
        started = struct.pack("<IIBBH", 0x12345678, 7, 4, 0, 0)  # Started
        window = struct.pack("<IIBBH", 0x12345678, 7, 3, 0, 0)  # StartWindow
        abort = struct.pack("<IIBBH", 0x12345678, 7, 5, 2, 0)  # Abort timeout
        for msg in (offer, window, abort):
            host.sendall(frame_bytes(TYPE_MISSION_BARRIER, msg))
        msgs = recv_msgs(guest, 0.5)
        assert (TYPE_MISSION_BARRIER, offer) in msgs
        assert (TYPE_MISSION_BARRIER, window) in msgs
        assert (TYPE_MISSION_BARRIER, abort) in msgs

        for msg in (ready, started):
            guest.sendall(frame_bytes(TYPE_MISSION_BARRIER, msg))
        msgs = recv_msgs(host, 0.5)
        assert (TYPE_MISSION_BARRIER, ready) in msgs
        assert (TYPE_MISSION_BARRIER, started) in msgs

        # Wrong role phases are blocked: guest Offer, host Ready.
        guest.sendall(frame_bytes(TYPE_MISSION_BARRIER, offer))
        assert recv_type(host, TYPE_MISSION_BARRIER, 0.4) is None
        host.sendall(frame_bytes(TYPE_MISSION_BARRIER, ready))
        assert recv_type(guest, TYPE_MISSION_BARRIER, 0.4) is None
        # Short barrier payload is dropped.
        host.sendall(frame_bytes(TYPE_MISSION_BARRIER, offer[:-1]))
        assert recv_type(guest, TYPE_MISSION_BARRIER, 0.4) is None

        # R&D TrainSnapshot: host-only, exact size, carCount <= 16.
        train = make_train(2)
        host.sendall(frame_bytes(TYPE_TRAIN_SNAPSHOT, train))
        assert recv_type(guest, TYPE_TRAIN_SNAPSHOT) == train
        guest.sendall(frame_bytes(TYPE_TRAIN_SNAPSHOT, train))
        assert recv_type(host, TYPE_TRAIN_SNAPSHOT, 0.4) is None
        too_many = make_train(17)
        host.sendall(frame_bytes(TYPE_TRAIN_SNAPSHOT, too_many))
        assert recv_type(guest, TYPE_TRAIN_SNAPSHOT, 0.4) is None
        host.sendall(frame_bytes(TYPE_TRAIN_SNAPSHOT, train[:-1]))
        assert recv_type(guest, TYPE_TRAIN_SNAPSHOT, 0.4) is None

        # NpcSpawn/NpcFrame: host-only, exact 24 bytes.
        spawn = struct.pack("<IIIIIBBBB", 1, 0x1234, 0x5678, 0x9ABC, 0xDEF0,
                            1, 0, 0, 0)
        frame = struct.pack("<IffffBBBB", 1, 1.0, 2.0, 3.0, 90.0, 1, 0, 0, 0)
        host.sendall(frame_bytes(TYPE_NPC_SPAWN, spawn))
        assert recv_type(guest, TYPE_NPC_SPAWN) == spawn
        guest.sendall(frame_bytes(TYPE_NPC_SPAWN, spawn))
        assert recv_type(host, TYPE_NPC_SPAWN, 0.4) is None
        host.sendall(frame_bytes(TYPE_NPC_SPAWN, spawn[:-1]))
        assert recv_type(guest, TYPE_NPC_SPAWN, 0.4) is None
        host.sendall(frame_bytes(TYPE_NPC_FRAME, frame))
        assert recv_type(guest, TYPE_NPC_FRAME) == frame
        guest.sendall(frame_bytes(TYPE_NPC_FRAME, frame))
        assert recv_type(host, TYPE_NPC_FRAME, 0.4) is None

        # Unknown message must not crash or relay.
        host.sendall(frame_bytes(0x7E, b"\x00" * 8))
        assert recv_type(guest, 0x7E, 0.4) is None
        host.close()
        guest.close()


def test_oversized_and_truncated(exe, tmp):
    log("[test] oversized / truncated frames")
    with Server(exe, tmp / "oversize") as srv:
        s = connect(srv.port)
        s.sendall(struct.pack("<I", 0x20000) + b"\x04")  # len > kMaxPayload
        time.sleep(0.2)
        s.close()

        # Truncated frame then close.
        s = connect(srv.port)
        s.sendall(struct.pack("<I", FRAME_SIZE) + b"\x04" + b"\x00" * 10)
        s.close()

        # Server still serves a valid client.
        c = connect(srv.port)
        join(c, "Survivor")
        assert recv_type(c, TYPE_JOIN_OK) is not None
        c.close()


def test_host_migration(exe, tmp):
    log("[test] host migration")
    with Server(exe, tmp / "migration") as srv:
        host = connect(srv.port)
        join(host, "Alice")
        assert recv_type(host, TYPE_JOIN_OK) is not None
        guest = connect(srv.port)
        join(guest, "Bob")
        assert recv_type(guest, TYPE_JOIN_OK) is not None
        recv_msgs(host, 0.3)
        recv_msgs(guest, 0.3)

        host.close()
        # Bob must see PlayerLeft(Alice) and HostChanged(isYouHost=1).
        msgs = recv_msgs(guest, 1.2)
        assert any(t == TYPE_PLAYER_LEFT for t, _ in msgs), \
            "guest should get PlayerLeft for departed host"
        changed = [p for t, p in msgs if t == TYPE_HOST_CHANGED]
        assert changed and changed[-1][0] == 1, \
            f"guest should be promoted, got {changed}"
        recv_msgs(guest, 0.3)

        # Bob is now host: his WorldSnapshot must be accepted and relayed
        # to a new guest.
        charlie = connect(srv.port)
        join(charlie, "Charlie")
        assert recv_type(charlie, TYPE_JOIN_OK) is not None
        recv_msgs(charlie, 0.4)
        snapshot = make_world(1)
        guest.sendall(frame_bytes(TYPE_WORLD_SNAPSHOT, snapshot))
        assert recv_type(charlie, TYPE_WORLD_SNAPSHOT) == snapshot, \
            "promoted host snapshot must relay to new guest"

        # Charlie is still a guest: his snapshot is blocked.
        charlie.sendall(frame_bytes(TYPE_WORLD_SNAPSHOT, snapshot))
        assert recv_type(guest, TYPE_WORLD_SNAPSHOT, 0.5) is None

        guest.close()
        charlie.close()


def test_idle_timeout(exe, tmp):
    log("[test] idle timeout")
    # Second argv = idle timeout seconds. Client joins and goes silent.
    with Server(exe, tmp / "idle", extra_args=["2"]) as srv:
        s = connect(srv.port)
        join(s, "Silent")
        assert recv_type(s, TYPE_JOIN_OK) is not None
        deadline = time.time() + 6.0
        closed = False
        while time.time() < deadline:
            try:
                s.settimeout(0.5)
                data = s.recv(1024)
                if data == b"":
                    closed = True
                    break
            except (socket.timeout, OSError):
                pass
        assert closed, "silent client should be disconnected by idle timeout"
        s.close()

        c = connect(srv.port)
        join(c, "AfterIdle")
        assert recv_type(c, TYPE_JOIN_OK) is not None
        c.close()


def test_fuzz_frames(exe, tmp):
    log("[test] fuzz malformed frames")
    with Server(exe, tmp / "fuzz") as srv:
        s = connect(srv.port)
        join(s, "Fuzz")
        assert recv_type(s, TYPE_JOIN_OK) is not None

        # Random non-control message types/sizes; server must ignore/drop
        # without crashing or relaying nonsensical messages.
        rng = random.Random(12345)
        safe_types = [0x7E, 0x7F, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x0B, 0x09]
        for _ in range(300):
            t = rng.choice(safe_types + [0x00, 0x08, 0x0A, 0x0C])
            ln = rng.choice([0, 1, 8, 12, 16, 23, 24, 25, 32, 64, 255,
                             FRAME_SIZE, OUTFIT_SIZE, 2000])
            s.sendall(struct.pack("<I", ln) + bytes([t]) + bytes([rng.randrange(256) for _ in range(ln)]))
        # Oversized length header (internal FrameReader reset, no crash).
        s.sendall(struct.pack("<I", 0x20000) + b"\x04")
        # Drain Pong echoes produced by fuzzed Ping frames.
        recv_msgs(s, 0.4)
        # Fragmented valid Ping.
        ping = frame_bytes(TYPE_PING, b"fuzz-ping")
        s.sendall(ping[:2]); time.sleep(0.02)
        s.sendall(ping[2:5]); time.sleep(0.02)
        s.sendall(ping[5:])
        deadline = time.time() + 2.0
        pong_ok = False
        while time.time() < deadline and not pong_ok:
            for t, p in recv_msgs(s, 0.2):
                if t == TYPE_PONG and p == b"fuzz-ping":
                    pong_ok = True
        assert pong_ok, "server should answer fragmented Ping"

        # A fresh client must still connect and join.
        c = connect(srv.port)
        join(c, "AfterFuzz")
        assert recv_type(c, TYPE_JOIN_OK) is not None
        s.close()
        c.close()


def test_players_txt_recovery(exe, tmp):
    log("[test] players.txt malformed recovery")
    work = tmp / "recovery"
    srv = Server(exe, work)
    srv.start()
    a = connect(srv.port)
    join(a, "Alice")
    assert recv_type(a, TYPE_JOIN_OK) is not None
    a.sendall(frame_bytes(TYPE_FRAME, make_frame_payload(7.5, 8.25, 9.0, 180.0)))
    a.sendall(frame_bytes(TYPE_PING, b"alice-frame"))
    assert recv_type(a, TYPE_PONG) == b"alice-frame", "Alice frame must be processed"
    time.sleep(0.3)
    b = connect(srv.port)
    join(b, "Bob")
    assert recv_type(b, TYPE_JOIN_OK) is not None
    b.sendall(frame_bytes(TYPE_FRAME, make_frame_payload(-1.5, -2.5, 3.5, 45.0)))
    b.sendall(frame_bytes(TYPE_PING, b"bob-frame"))
    assert recv_type(b, TYPE_PONG) == b"bob-frame", "Bob frame must be processed"
    time.sleep(0.3)
    recv_msgs(a, 0.2)
    recv_msgs(b, 0.2)
    b.close()      # close guest first
    time.sleep(1.0)
    a.close()
    time.sleep(1.5)
    srv.stop()
    time.sleep(0.3)

    players = work / "players.txt"
    original = players.read_text(encoding="utf-8") if players.exists() else ""
    assert "Alice" in original and "Bob" in original, "both players should be saved"
    # Add junk lines and a stale .tmp; loader must ignore both.
    players.write_text(
        "garbage-without-colons\n"
        "Broken:not-a-float\n"
        ":empty-name:1:2:3\n"
        "\n"
        + original,
        encoding="utf-8")
    (work / "players.txt.tmp").write_text("Stale:1:2:3:4\n", encoding="utf-8")

    srv2 = Server(exe, work)
    srv2.start()
    try:
        ca = connect(srv2.port)
        join(ca, "Alice")
        pa = recv_type(ca, TYPE_JOIN_OK)
        assert pa is not None
        _, has, coords, _ = parse_join_ok(pa)
        assert has and coords is not None, "Alice position should survive junk lines"
        assert abs(coords[0] - 7.5) < 0.01 and abs(coords[1] - 8.25) < 0.01
        ca.close()

        cb = connect(srv2.port)
        join(cb, "Bob")
        pb = recv_type(cb, TYPE_JOIN_OK)
        assert pb is not None
        _, hasb, coordsb, _ = parse_join_ok(pb)
        assert hasb and coordsb is not None, "Bob position should survive junk lines"
        assert abs(coordsb[0] + 1.5) < 0.01 and abs(coordsb[1] + 2.5) < 0.01
        cb.close()
    finally:
        srv2.stop()


def test_concurrent_fuzz(exe, tmp):
    log("[test] concurrent / long fuzz")
    import threading

    with Server(exe, tmp / "concurrent") as srv:
        host = connect(srv.port)
        join(host, "Alice")
        assert recv_type(host, TYPE_JOIN_OK) is not None
        guest = connect(srv.port)
        join(guest, "Bob")
        assert recv_type(guest, TYPE_JOIN_OK) is not None
        recv_msgs(host, 0.3)
        recv_msgs(guest, 0.3)

        stop = threading.Event()
        errors = []

        def rogue_worker():
            # Pre-JOIN sockets get closed by the server; keep flooding until stopped.
            try:
                r = connect(srv.port)
                rng = random.Random()
                types = [0x04, 0x09, 0x10, 0x11, 0x13, 0x7E]
                while not stop.is_set():
                    t = rng.choice(types)
                    ln = rng.choice([0, 8, 24, 88, 400, 2000])
                    try:
                        r.sendall(struct.pack("<I", ln) + bytes([t]) +
                                  bytes([rng.randrange(256) for _ in range(ln)]))
                    except OSError:
                        break
                    time.sleep(0.005)
                r.close()
            except OSError:
                pass

        def sender_worker(sock):
            try:
                rng = random.Random()
                types = [0x04, 0x0B, 0x11, 0x12, 0x13, 0x14, 0x7E]
                while not stop.is_set():
                    t = rng.choice(types)
                    ln = rng.choice([0, 1, 8, 12, 16, 24, 32, 88, 400, 1500])
                    try:
                        sock.sendall(struct.pack("<I", ln) + bytes([t]) +
                                     bytes([rng.randrange(256) for _ in range(ln)]))
                    except OSError:
                        break
                    time.sleep(0.003)
            except OSError:
                pass

        threads = [threading.Thread(target=rogue_worker) for _ in range(3)]
        threads += [threading.Thread(target=sender_worker, args=(host,)),
                    threading.Thread(target=sender_worker, args=(guest,))]
        for th in threads: th.start()
        time.sleep(3.0)
        stop.set()
        for th in threads: th.join(timeout=3.0)

        # Server must still be alive after the storm.
        recv_msgs(host, 0.3)
        recv_msgs(guest, 0.3)
        host.sendall(frame_bytes(TYPE_PING, b"alive-after-storm"))
        ok = False
        deadline = time.time() + 2.0
        while time.time() < deadline and not ok:
            for t, p in recv_msgs(host, 0.2):
                if t == TYPE_PONG and p == b"alive-after-storm":
                    ok = True
        assert ok, "server should answer Ping after concurrent fuzz"
        host.close()
        guest.close()
        time.sleep(0.5)
        c = connect(srv.port)
        join(c, "Survivor")
        assert recv_type(c, TYPE_JOIN_OK) is not None
        c.close()


def test_connection_cap(exe, tmp):
    log("[test] connection cap / flood")
    with Server(exe, tmp / "cap") as srv:
        sockets = []
        for _ in range(40):
            try:
                s = socket.create_connection(("127.0.0.1", srv.port), timeout=0.3)
                sockets.append(s)
            except OSError:
                pass
        # Server caps active connections; excess must be closed, not crash.
        time.sleep(0.5)
        for s in sockets:
            try:
                s.close()
            except OSError:
                pass
        time.sleep(1.0)

        c = connect(srv.port)
        join(c, "AfterFlood")
        assert recv_type(c, TYPE_JOIN_OK) is not None, \
            "server must still accept after connection flood"
        c.close()


def test_per_ip_rate_limit(exe, tmp):
    log("[test] per-IP rate limit")
    with Server(exe, tmp / "rate") as srv:
        sockets = []
        for _ in range(30):
            try:
                s = socket.create_connection(("127.0.0.1", srv.port), timeout=0.3)
                sockets.append(s)
            except OSError:
                pass
        time.sleep(0.7)
        closed = 0
        for s in sockets:
            s.settimeout(0.2)
            try:
                if s.recv(1) == b"":
                    closed += 1
            except (socket.timeout, OSError):
                pass
        for s in sockets:
            try:
                s.close()
            except OSError:
                pass
        assert closed > 0, "rate limit should close some connections"
        time.sleep(1.2)  # new rate window
        c = connect(srv.port)
        join(c, "AfterRate")
        assert recv_type(c, TYPE_JOIN_OK) is not None
        c.close()


def test_graceful_shutdown(exe, tmp):
    log("[test] graceful shutdown (test hook)")
    work = tmp / "graceful"
    # 3rd argv: self-initiated graceful stop after 1500 ms.
    srv = Server(exe, work, extra_args=["30", "1500"])
    srv.start()
    try:
        a = connect(srv.port)
        join(a, "Shutdown")
        assert recv_type(a, TYPE_JOIN_OK) is not None
        a.sendall(frame_bytes(TYPE_FRAME, make_frame_payload(3.5, 4.5, 5.5, 10.0)))
        a.sendall(frame_bytes(TYPE_PING, b"before-shutdown"))
        assert recv_type(a, TYPE_PONG) == b"before-shutdown"
        time.sleep(0.3)
        a.close()

        deadline = time.time() + 8.0
        while time.time() < deadline and srv.proc.poll() is None:
            time.sleep(0.1)
        assert srv.proc.poll() is not None, "server did not exit on graceful stop"
        assert srv.proc.returncode == 0, f"exit code {srv.proc.returncode}"
        players = work / "players.txt"
        assert players.exists() and "Shutdown" in players.read_text(encoding="utf-8"), \
            "players.txt should be saved on graceful shutdown"
    finally:
        srv.stop()


def test_persistence(exe, tmp):
    log("[test] players.txt persistence across restart")
    work = tmp / "persist"
    srv = Server(exe, work)
    srv.start()
    s = connect(srv.port)
    join(s, "Alice")
    assert recv_type(s, TYPE_JOIN_OK) is not None
    frame = make_frame_payload(12.5, -3.25, 40.0, 90.0)
    s.sendall(frame_bytes(TYPE_FRAME, frame))
    time.sleep(1.0)
    s.close()
    time.sleep(1.0)  # SaveAll happens on disconnect
    srv.stop()
    time.sleep(0.3)

    # Second start, same working directory (new port is fine).
    srv2 = Server(exe, work)
    srv2.start()
    try:
        c = connect(srv2.port)
        join(c, "Alice")
        p = recv_type(c, TYPE_JOIN_OK)
        assert p is not None
        _, has, coords, is_host = parse_join_ok(p)
        assert has, "saved position should be restored"
        assert coords is not None
        assert abs(coords[0] - 12.5) < 0.01
        assert abs(coords[1] - (-3.25)) < 0.01
        assert abs(coords[2] - 40.0) < 0.01
        assert is_host is False or is_host is True  # first player => host
        c.close()
    finally:
        srv2.stop()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()
    exe = Path(args.exe).resolve()
    assert exe.exists(), f"server not found: {exe}"

    tmp = Path(tempfile.mkdtemp(prefix="rdr2coop-tests-"))
    failures = 0
    tests = [
        test_join_rejections,
        test_roles_and_full,
        test_prejoin_and_ping,
        test_world_host_only,
        test_payload_sizes_and_host_only_messages,
        test_oversized_and_truncated,
        test_host_migration,
        test_idle_timeout,
        test_fuzz_frames,
        test_players_txt_recovery,
        test_concurrent_fuzz,
        test_connection_cap,
        test_per_ip_rate_limit,
        test_graceful_shutdown,
        test_persistence,
    ]
    try:
        for test in tests:
            try:
                test(exe, tmp)
                log(f"[ok] {test.__name__}")
            except Exception as exc:
                failures += 1
                log(f"[FAIL] {test.__name__}: {exc}")
        if failures:
            log(f"\n{failures} test(s) failed; workdir: {tmp}")
            return 1
        log(f"\nALL {len(tests)} SERVER INTEGRATION TESTS PASSED")
        return 0
    finally:
        if not args.keep:
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            log(f"kept workdir: {tmp}")


if __name__ == "__main__":
    sys.exit(main())
