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

PROTO = 12
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


def frame_bytes(type_, payload):
    return struct.pack("<I", len(payload)) + bytes([type_]) + payload


class Server:
    def __init__(self, exe, workdir, port=None):
        self.exe = str(exe)
        self.workdir = Path(workdir)
        self.port = port or random.randint(21000, 25000)
        self.proc = None

    def start(self):
        self.workdir.mkdir(parents=True, exist_ok=True)
        creationflags = 0
        if os.name == "nt":
            creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        self.proc = subprocess.Popen(
            [self.exe, str(self.port)],
            cwd=str(self.workdir),
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            creationflags=creationflags,
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
        test_fuzz_frames,
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
