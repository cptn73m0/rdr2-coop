#!/usr/bin/env python3
"""Headless integration tests for rdr2_coop_server (no game required).

Protocol constants (version, message ids, payload sizes, limits and server
capacity/rate-limit values) are derived at runtime from net/protocol.h and
server/server.cpp, so the tests cannot silently drift from the C++ sources.

Covered: JOIN validation (version/name/limits), atomic concurrent JOIN,
pre-JOIN guard, Ping/Pong, host-only messages, exact payload sizes,
WorldSnapshot bounds, malformed/oversized frames, unknown messages,
Leave/PlayerLeft/Died/Respawned/HorseOutfit relay, host migration,
connection cap and per-IP rate limit, players.txt persistence/recovery
(including corrupt lines of the joining player), graceful shutdown.

Usage:
    python tests/integration_server.py --exe build-server-ci/Release/rdr2_coop_server.exe
    python tests/integration_server.py --exe ... --strict   # known-fail => fail

Run with a plain `python` (optimized mode is rejected: checks are explicit
require() calls, and main() refuses -O/PYTHONOPTIMIZE).

Requires only Python 3.8+ and a built server executable.
"""
import argparse
import os
import random
import re
import select
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
PROTOCOL_H = REPO_ROOT / "net" / "protocol.h"
SERVER_CPP = REPO_ROOT / "server" / "server.cpp"


def _read_text(path):
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:  # pragma: no cover - infra failure
        raise RuntimeError(f"cannot read {path}: {exc}") from exc


_PROTO_SRC = _read_text(PROTOCOL_H)
_SERVER_SRC = _read_text(SERVER_CPP)


def require(cond, what):
    """Explicit check: works even under `python -O` (unlike assert)."""
    if not cond:
        raise AssertionError(what)


def _proto_version():
    m = re.search(r"kProtocolVersion\s*=\s*(\d+)\s*;", _PROTO_SRC)
    if not m:
        raise RuntimeError(f"kProtocolVersion not found in {PROTOCOL_H}")
    return int(m.group(1))


def _msg_ids():
    m = re.search(r"enum\s+class\s+Msg\s*:\s*uint8_t\s*\{(.*?)\};",
                  _PROTO_SRC, re.S)
    if not m:
        raise RuntimeError(f"enum class Msg not found in {PROTOCOL_H}")
    ids = {}
    for name, value in re.findall(r"(\w+)\s*=\s*0x([0-9A-Fa-f]+)", m.group(1)):
        ids[name] = int(value, 16)
    return ids


def _reject_reasons():
    m = re.search(r"enum\s+class\s+JoinRejectReason\s*:\s*uint8_t\s*\{(.*?)\};",
                  _PROTO_SRC, re.S)
    if not m:
        raise RuntimeError(f"enum class JoinRejectReason not found in {PROTOCOL_H}")
    out = {}
    for name, value in re.findall(r"(\w+)\s*=\s*(\d+)", m.group(1)):
        out[name] = int(value)
    return out


def _struct_size(name):
    m = re.search(r"static_assert\s*\(\s*sizeof\s*\(\s*" + re.escape(name) +
                  r"\s*\)\s*==\s*(\d+)", _PROTO_SRC)
    if not m:
        raise RuntimeError(f"static_assert(sizeof({name}) == N) not found in protocol.h")
    return int(m.group(1))


def _proto_const(name):
    m = re.search(re.escape(name) + r"\s*=\s*(\d+)\s*;", _PROTO_SRC)
    if not m:
        raise RuntimeError(f"{name} not found in protocol.h")
    return int(m.group(1))


def _server_const(name, fallback):
    m = re.search(re.escape(name) + r"\s*=\s*(\d+)\s*;", _SERVER_SRC)
    return int(m.group(1)) if m else fallback


PROTO = _proto_version()
MSG = _msg_ids()

_REQUIRED_MSG = [
    "Join", "JoinOk", "PlayerJoined", "Frame", "Leave", "PlayerLeft", "Died",
    "WorldState", "Outfit", "Respawned", "Shot", "Ping", "Pong", "HorseOutfit",
    "JoinRejected", "WorldSnapshot", "ActionState", "PlayerVehicle",
    "MissionEvent", "MissionState", "MissionBarrier", "HostChanged",
    "TrainSnapshot",
]
for _required in _REQUIRED_MSG:
    if _required not in MSG:
        raise RuntimeError(f"Msg::{_required} not found in protocol.h")

TYPE_JOIN = MSG["Join"]
TYPE_JOIN_OK = MSG["JoinOk"]
TYPE_PLAYER_JOINED = MSG["PlayerJoined"]
TYPE_FRAME = MSG["Frame"]
TYPE_LEAVE = MSG["Leave"]
TYPE_PLAYER_LEFT = MSG["PlayerLeft"]
TYPE_DIED = MSG["Died"]
TYPE_WORLD_STATE = MSG["WorldState"]
TYPE_OUTFIT = MSG["Outfit"]
TYPE_RESPAWNED = MSG["Respawned"]
TYPE_SHOT = MSG["Shot"]
TYPE_PING = MSG["Ping"]
TYPE_PONG = MSG["Pong"]
TYPE_HORSE_OUTFIT = MSG["HorseOutfit"]
TYPE_JOIN_REJECTED = MSG["JoinRejected"]
TYPE_WORLD_SNAPSHOT = MSG["WorldSnapshot"]
TYPE_ACTION_STATE = MSG["ActionState"]
TYPE_PLAYER_VEHICLE = MSG["PlayerVehicle"]
TYPE_MISSION_EVENT = MSG["MissionEvent"]
TYPE_MISSION_STATE = MSG["MissionState"]
TYPE_MISSION_BARRIER = MSG["MissionBarrier"]
TYPE_HOST_CHANGED = MSG["HostChanged"]
TYPE_TRAIN_SNAPSHOT = MSG["TrainSnapshot"]

REJECT = _reject_reasons()
REJECT_VERSION = REJECT["VersionMismatch"]
REJECT_FULL = REJECT["ServerFull"]
REJECT_NAME_TAKEN = REJECT["NameTaken"]
REJECT_BAD_NAME = REJECT["BadName"]

FRAME_SIZE = _struct_size("FramePayload")
OUTFIT_SIZE = _struct_size("OutfitState")
ACTION_SIZE = _struct_size("ActionStatePayload")
VEHICLE_SIZE = _struct_size("PlayerVehicleState")
MISSION_EVENT_SIZE = _struct_size("MissionEventPayload")
MISSION_STATE_SIZE = _struct_size("MissionStatePayload")
MISSION_BARRIER_SIZE = _struct_size("MissionBarrierPayload")
SHOT_SIZE = _struct_size("ShotPayload")
ENTITY_SIZE = _struct_size("WorldEntity")
WORLD_HEADER_SIZE = _struct_size("WorldSnapshotHeader")
TRAIN_HEADER_SIZE = _struct_size("TrainSnapshotHeader")
HOST_CHANGED_SIZE = _struct_size("HostChangedPayload")
WORLD_STATE_SIZE = _struct_size("WorldState")

MAX_ENTITIES = _proto_const("kMaxWorldEntities")
MAX_TRAIN_CARS = _proto_const("kMaxTrainCars")
MAX_PLAYERS = _proto_const("kMaxPlayers")

# Limits duplicated in server/server.cpp (auto-detected, fallback for renamed code).
MAX_NAME_BYTES = _server_const("kMaxNameLength", 32)
CAP_ACTIVE = _server_const("kMaxActiveConnections", 32)
CAP_RATE = _server_const("kMaxNewConnectionsPerIpPerSecond", 12)
PREJOIN_CAP = _server_const("kMaxPreJoinConnections", 8)

# Pinned security limits (P3): the values auto-read from server.cpp must match
# these. A silent weakening of a limit must fail the suite instead of being
# absorbed by the dynamic parsing above.
_PINNED_LIMITS = {
    "kMaxNameLength": (MAX_NAME_BYTES, 32),
    "kMaxPreJoinConnections": (PREJOIN_CAP, 8),
    "kMaxActiveConnections": (CAP_ACTIVE, 32),
    "kMaxNewConnectionsPerIpPerSecond": (CAP_RATE, 12),
}
for _limit_name, (_actual, _expected) in _PINNED_LIMITS.items():
    require(_actual == _expected,
            f"{_limit_name}={_actual}: expected {_expected}; security limit was "
            f"weakened or changed in server/server.cpp (update the pinned value "
            f"deliberately only together with a security review)")

# Expected-fail until a P0 fix outside tests/ lands. Currently empty: the P0
# server fixes (atomic JOIN, BadName, strict players.txt, WorldState host-only)
# have landed in server/server.cpp, so these tests are strict again.
KNOWN_FAIL_TESTS = set()

# 50-100 rounds of the JOIN race per task; overridable for slow CI.
JOIN_RACE_ITERS = int(os.environ.get("RDR2_JOIN_RACE_ITERS", "50"))


def log(msg):
    print(msg, flush=True)


# --------------------------------------------------------------------------
# Protocol payload builders
# --------------------------------------------------------------------------

def make_frame_payload(x=1.0, y=2.0, z=3.0, heading=90.0):
    # 4f + 4B + 5f + 6Q = 88 bytes (checked against sizeof(FramePayload)).
    payload = struct.pack(
        "<ffffBBBBfffffQQQQQQ",
        x, y, z, heading,
        0, 0, 0, 0,                    # move/state/flags/action
        1.0, 0.0, 0.0, 0.0, 0.0,       # health + aim
        0, 0, 0, 0, 0, 0,              # weapon, seq, 3 reserved, horseOutfit
    )
    require(len(payload) == FRAME_SIZE,
            f"FramePayload builder mismatch: {len(payload)} != {FRAME_SIZE}")
    return payload


def make_world(count):
    header = struct.pack("<HH", count, 0)
    require(len(header) == WORLD_HEADER_SIZE, "WorldSnapshotHeader size mismatch")
    ents = b"".join(
        struct.pack("<QIffffBBBB", 1000 + i, 0xCF7E73BE + i,
                    1.0 + i, 2.0, 3.0, 90.0, 0, 0, 0, 0)
        for i in range(count))
    require(len(ents) == count * ENTITY_SIZE, "WorldEntity size mismatch")
    return header + ents


def make_train(count):
    header = struct.pack("<IIfBBH", 0x1234, 0xCF7E73BE, 12.5, 1, count, 0)
    require(len(header) == TRAIN_HEADER_SIZE, "TrainSnapshotHeader size mismatch")
    ents = b"".join(
        struct.pack("<QIffffBBBB", 2000 + i, 0xCF7E73BE + i,
                    5.0 + i, 6.0, 7.0, 30.0, 1, 0, 0, 0)
        for i in range(count))
    return header + ents


def frame_bytes(type_, payload):
    return struct.pack("<I", len(payload)) + bytes([type_]) + payload


def join_payload(name, version=PROTO, model=0xCF7E73BE):
    nb = name.encode("utf-8")
    return bytes([len(nb)]) + nb + struct.pack("<Q", model) + struct.pack("<H", version)


def join_payload_raw(name_bytes, version=PROTO, model=0xCF7E73BE):
    return bytes([len(name_bytes)]) + name_bytes + struct.pack("<Q", model) + \
        struct.pack("<H", version)


def join(s, name, version=PROTO):
    s.sendall(frame_bytes(TYPE_JOIN, join_payload(name, version)))


def join_raw(s, name_bytes, version=PROTO):
    s.sendall(frame_bytes(TYPE_JOIN, join_payload_raw(name_bytes, version)))


def make_name_payload(name):
    nb = name.encode("utf-8")
    return bytes([len(nb)]) + nb


# --------------------------------------------------------------------------
# Socket helpers
# --------------------------------------------------------------------------

def connect(port, timeout=3.0):
    s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    s.settimeout(2.0)
    return s


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
                if ln > 1024 * 1024:
                    return out
                if len(buf) < 5 + ln:
                    break
                out.append((buf[4], buf[5:5 + ln]))
                buf = buf[5 + ln:]
        except socket.timeout:
            pass
        except OSError:
            break
    return out


def recv_type(s, target, duration=1.5):
    end = time.time() + duration
    while time.time() < end:
        for t, p in recv_msgs(s, duration=0.3):
            if t == target:
                return p
    return None


def read_one_frame(s, duration=2.0):
    """Read exactly one frame; returns (type, payload) or None on EOF/timeout.

    Only used where a single reply is expected (JOIN race classifier).
    """
    end = time.time() + duration
    buf = b""
    s.settimeout(0.25)
    while time.time() < end:
        try:
            data = s.recv(65536)
        except socket.timeout:
            continue
        except OSError:
            return None
        if not data:
            return None
        buf += data
        while len(buf) >= 5:
            ln = struct.unpack("<I", buf[:4])[0]
            if ln > 1024 * 1024:
                return None
            if len(buf) < 5 + ln:
                break
            return (buf[4], buf[5:5 + ln])
    return None


def drain_raw(s, duration=0.15):
    """Discard pending bytes (stale JOIN_OK/PlayerJoined) before relay checks."""
    end = time.time() + duration
    s.settimeout(0.05)
    while time.time() < end:
        try:
            if not s.recv(65536):
                return
        except socket.timeout:
            return
        except OSError:
            return


def parse_join_ok(p):
    require(p is not None and len(p) >= 10,
            f"JoinOk payload too short: {p!r}")
    session = struct.unpack("<Q", p[:8])[0]
    has = p[8] != 0
    off = 9
    coords = None
    if has:
        require(len(p) >= off + 16 + 1, f"JoinOk with coords truncated: {len(p)}")
        coords = struct.unpack("<ffff", p[off:off + 16])
        off += 16
    require(len(p) > off, f"JoinOk without is_host byte: {len(p)}")
    is_host = p[off] != 0
    return session, has, coords, is_host


def expect_reject(s, reason, duration=2.0):
    p = recv_type(s, TYPE_JOIN_REJECTED, duration)
    require(p is not None,
            "expected JoinRejected, got connection close/timeout "
            "(P0: names must be rejected with BadName, not silently closed)")
    require(len(p) >= 1, "JoinRejected payload empty")
    require(p[0] == reason, f"expected reject {reason}, got {p[0]}")


def expect_disconnect(s, duration=2.0):
    end = time.time() + duration
    s.settimeout(0.25)
    while time.time() < end:
        try:
            data = s.recv(4096)
            if data == b"":
                return True
        except socket.timeout:
            continue
        except OSError:
            return True  # RST counts as "connection closed"
    return False


def classify_connections(socks, wait=0.7):
    """Returns (closed_by_server, still_open) among client sockets."""
    live = [s for s in socks if s.fileno() != -1]
    readable = set()
    if live:
        try:
            readable, _, _ = select.select(live, [], [], wait)
            readable = set(readable)
        except OSError:
            readable = set()
    closed = 0
    still_open = 0
    for s in live:
        if s in readable:
            s.settimeout(0.2)
            try:
                if s.recv(1) == b"":
                    closed += 1
                else:
                    still_open += 1
            except OSError:
                closed += 1
        else:
            still_open += 1
    return closed, still_open


def tail(text, n=2500):
    text = text or ""
    return text[-n:]


def wait_file_contains(path, needle, timeout=3.0):
    end = time.time() + timeout
    text = ""
    while time.time() < end:
        try:
            text = Path(path).read_text(encoding="utf-8", errors="replace")
        except OSError:
            text = ""
        if needle in text:
            return text
        time.sleep(0.05)
    return text


# --------------------------------------------------------------------------
# Server process wrapper
# --------------------------------------------------------------------------

class Server:
    def __init__(self, exe, workdir, port=None, extra_args=None):
        self.exe = str(exe)
        self.workdir = Path(workdir)
        self._explicit_port = port is not None
        self.port = port or 0
        self.extra_args = list(extra_args or [])
        self.proc = None
        self._out = None

    # -- diagnostics -------------------------------------------------------
    @property
    def log_path(self):
        return self.workdir / "server_console.log"

    def log_text(self):
        try:
            return self.log_path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            return ""

    def wait_log_contains(self, needle, timeout=5.0):
        end = time.time() + timeout
        while time.time() < end:
            text = self.log_text()
            if needle in text:
                return text
            time.sleep(0.05)
        return self.log_text()

    def stdout_tail(self, n=2000):
        try:
            return (self.workdir / "server_stdout.log").read_text(
                encoding="utf-8", errors="replace")[-n:]
        except OSError:
            return ""

    # -- process lifecycle -------------------------------------------------
    def _pick_port(self):
        s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        try:
            s.bind(("127.0.0.1", 0))
            return s.getsockname()[1]
        finally:
            s.close()

    def _launch(self):
        self.workdir.mkdir(parents=True, exist_ok=True)
        creationflags = 0
        if os.name == "nt":
            creationflags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        self._out = open(self.workdir / "server_stdout.log", "ab")
        self.proc = subprocess.Popen(
            [self.exe, str(self.port)] + self.extra_args,
            cwd=str(self.workdir),
            stdout=self._out,
            stderr=subprocess.STDOUT,
            creationflags=creationflags,
        )
        deadline = time.time() + 8.0
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(
                    f"server exited during startup (rc={self.proc.returncode}): "
                    f"{self.stdout_tail()}")
            try:
                probe = socket.create_connection(("127.0.0.1", self.port), timeout=0.3)
                probe.close()
                return
            except OSError:
                time.sleep(0.05)
        raise RuntimeError(f"server did not start listening on port {self.port}: "
                           f"{self.stdout_tail()}")

    def start(self):
        last_exc = None
        for _ in range(1 if self._explicit_port else 4):
            if not self._explicit_port:
                self.port = self._pick_port()
            try:
                self._launch()
                return
            except Exception as exc:  # noqa: BLE001 - retry with another port
                last_exc = exc
                self.stop()
        raise RuntimeError(f"could not start server: {last_exc}")

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                try:
                    self.proc.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    pass
        if self._out:
            try:
                self._out.close()
            except OSError:
                pass
            self._out = None

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *args):
        self.stop()


def join_with_retry(srv, name, timeout=4.0, model=0xCF7E73BE):
    """Join when server may still be clearing a previous session (capacity race)."""
    end = time.time() + timeout
    last = None
    while time.time() < end:
        s = connect(srv.port)
        join(s, name)
        frame = read_one_frame(s, duration=1.0)
        if frame is not None:
            t, p = frame
            if t == TYPE_JOIN_OK:
                return s, p
            if t == TYPE_JOIN_REJECTED and p and p[0] == REJECT_FULL:
                last = "server full (previous session not cleaned yet)"
                s.close()
                time.sleep(0.03)
                continue
            last = f"unexpected reply type={t:#x} payload={p!r}"
        s.close()
        time.sleep(0.03)
    raise AssertionError(f"could not join {name!r} within {timeout}s: {last}")


# --------------------------------------------------------------------------
# Tests
# --------------------------------------------------------------------------

def test_join_rejections(exe, tmp):
    log("[test] JOIN validation: protocol version / bad chars")
    with Server(exe, tmp / "join") as srv:
        s = connect(srv.port)
        join(s, "OldClient", PROTO - 1)
        expect_reject(s, REJECT_VERSION)
        s.close()

        s = connect(srv.port)
        join(s, "Bad:Name")
        expect_reject(s, REJECT_BAD_NAME)
        s.close()

        # Server still accepts a valid JOIN after rejected ones.
        s = connect(srv.port)
        join(s, "StillAlive")
        require(recv_type(s, TYPE_JOIN_OK, 2.0) is not None,
                "server should still accept valid JOIN after rejects")
        s.close()


def test_join_name_validation(exe, tmp):
    log("[test] JOIN name validation (BadName, not silent close)")
    work = tmp / "names"
    with Server(exe, work) as srv:
        bad_names = [
            b"",                     # empty
            b"A" * (MAX_NAME_BYTES + 1),
            b"A" * 40,
            b"Alice\nInjected",      # control char injection into log/players.txt
            b"Alice\rInjected",
            b"Alice\x01",
            b"Bad:Name",
        ]
        for raw in bad_names:
            s = connect(srv.port)
            join_raw(s, raw)
            expect_reject(s, REJECT_BAD_NAME)
            s.close()

        # Maximum allowed name length must still be accepted.
        s = connect(srv.port)
        join(s, "A" * MAX_NAME_BYTES)
        require(recv_type(s, TYPE_JOIN_OK, 2.0) is not None,
                f"{MAX_NAME_BYTES}-byte name must be accepted")
        # Mark this player as saveable so players.txt gets written on disconnect.
        s.sendall(frame_bytes(TYPE_FRAME, make_frame_payload()))
        s.close()
        srv.wait_log_contains("отключился", 3.0)

        players = work / "players.txt"
        text = wait_file_contains(players, "A" * MAX_NAME_BYTES, timeout=3.0)
        require(srv.proc.poll() is None, "server died during name validation")
        require(players.exists(), "players.txt should exist after a joined player")
        require("Injected" not in text,
                "control-char name was injected into players.txt")


def test_roles_and_full(exe, tmp):
    log("[test] roles / limits / duplicate name")
    with Server(exe, tmp / "roles") as srv:
        host = connect(srv.port)
        join(host, "Alice")
        p = recv_type(host, TYPE_JOIN_OK)
        require(p is not None, "host JOIN_OK missing")
        _, has, _, is_host = parse_join_ok(p)
        require(is_host and not has, "first player must be HOST without saved position")

        dup = connect(srv.port)
        join(dup, "Alice")
        expect_reject(dup, REJECT_NAME_TAKEN)
        dup.close()

        guest = connect(srv.port)
        join(guest, "Bob")
        p = recv_type(guest, TYPE_JOIN_OK)
        require(p is not None, "guest JOIN_OK missing")
        _, _, _, is_guest_host = parse_join_ok(p)
        require(not is_guest_host, "second player must be GUEST")

        pj = recv_type(host, TYPE_PLAYER_JOINED)
        require(pj is not None, "host should receive PlayerJoined for the guest")
        require(len(pj) >= 1, "PlayerJoined payload empty")
        nl = pj[0]
        require(pj[1:1 + nl] == b"Bob", f"PlayerJoined name mismatch: {pj!r}")
        model = int.from_bytes(pj[1 + nl:9 + nl], "little")
        require(model == 0xCF7E73BE, f"PlayerJoined model mismatch: {model:#x}")

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
        require(expect_disconnect(s),
                "pre-JOIN Frame must close the connection (not be silently ignored)")
        s.close()

        # Strict expected behaviour: pre-JOIN Died is not allowed either.
        s = connect(srv.port)
        s.sendall(frame_bytes(TYPE_DIED, b""))
        require(expect_disconnect(s), "pre-JOIN Died must close the connection")
        s.close()

        # Pre-JOIN Ping is allowed and echoed (8-byte timestamp payload).
        p = connect(srv.port)
        payload = struct.pack("<Q", int(time.time() * 1000) & 0xFFFFFFFFFFFFFFFF)
        require(len(payload) == 8, "Ping timestamp must be 8 bytes")
        p.sendall(frame_bytes(TYPE_PING, payload))
        pong = recv_type(p, TYPE_PONG, 2.0)
        require(pong == payload, f"Ping should be echoed before JOIN: {pong!r}")
        p.close()


def test_world_host_only(exe, tmp):
    log("[test] WorldSnapshot host-only + bounds")
    with Server(exe, tmp / "world") as srv:
        host = connect(srv.port)
        join(host, "Alice")
        require(recv_type(host, TYPE_JOIN_OK) is not None, "host JOIN_OK missing")
        guest = connect(srv.port)
        join(guest, "Bob")
        require(recv_type(guest, TYPE_JOIN_OK) is not None, "guest JOIN_OK missing")
        recv_msgs(host, 0.3)
        recv_msgs(guest, 0.3)

        valid = make_world(MAX_ENTITIES)
        host.sendall(frame_bytes(TYPE_WORLD_SNAPSHOT, valid))
        got = recv_type(guest, TYPE_WORLD_SNAPSHOT, 2.0)
        require(got == valid, "host snapshot must relay byte-identical")

        too_many = make_world(MAX_ENTITIES + 1)
        host.sendall(frame_bytes(TYPE_WORLD_SNAPSHOT, too_many))
        require(recv_type(guest, TYPE_WORLD_SNAPSHOT, 0.5) is None,
                "snapshot with count > max must be dropped")

        guest.sendall(frame_bytes(TYPE_WORLD_SNAPSHOT, valid))
        require(recv_type(host, TYPE_WORLD_SNAPSHOT, 0.5) is None,
                "guest WorldSnapshot must be blocked")
        host.close()
        guest.close()


def test_payload_sizes_and_host_only_messages(exe, tmp):
    log("[test] exact payload sizes + mission/vehicle relay")
    with Server(exe, tmp / "relay") as srv:
        host = connect(srv.port)
        join(host, "Alice")
        require(recv_type(host, TYPE_JOIN_OK) is not None, "host JOIN_OK missing")
        guest = connect(srv.port)
        join(guest, "Bob")
        require(recv_type(guest, TYPE_JOIN_OK) is not None, "guest JOIN_OK missing")
        recv_msgs(host, 0.3)
        recv_msgs(guest, 0.3)

        frame = make_frame_payload(12.5, -3.25, 40.0, 45.0)
        require(len(frame) == FRAME_SIZE, "frame size mismatch")
        host.sendall(frame_bytes(TYPE_FRAME, frame))
        require(recv_type(guest, TYPE_FRAME, 2.0) == frame,
                "Frame must relay byte-identical")

        host.sendall(frame_bytes(TYPE_FRAME, frame[:-1]))
        require(recv_type(guest, TYPE_FRAME, 0.4) is None,
                "short Frame must be dropped")
        host.sendall(frame_bytes(TYPE_FRAME, frame + b"\x00"))
        require(recv_type(guest, TYPE_FRAME, 0.4) is None,
                "Frame with extra byte must be dropped")

        outfit = b"\x11" * OUTFIT_SIZE
        host.sendall(frame_bytes(TYPE_OUTFIT, outfit))
        require(recv_type(guest, TYPE_OUTFIT, 2.0) == outfit, "Outfit must relay")
        host.sendall(frame_bytes(TYPE_OUTFIT, outfit[:-1]))
        require(recv_type(guest, TYPE_OUTFIT, 0.4) is None,
                "short Outfit must be dropped")

        action = b"\x22" * ACTION_SIZE
        vehicle = b"\x33" * VEHICLE_SIZE
        shot = b"\x44" * SHOT_SIZE
        mission_event = struct.pack("<IBBH", 0x12345678, 3, 1, 0)
        mission_state = struct.pack("<IIBBH", 0x12345678, 0x87654321, 1, 0, 0)
        require(len(mission_event) == MISSION_EVENT_SIZE, "MissionEvent size mismatch")
        require(len(mission_state) == MISSION_STATE_SIZE, "MissionState size mismatch")
        host.sendall(frame_bytes(TYPE_ACTION_STATE, action))
        host.sendall(frame_bytes(TYPE_PLAYER_VEHICLE, vehicle))
        host.sendall(frame_bytes(TYPE_SHOT, shot))
        host.sendall(frame_bytes(TYPE_MISSION_EVENT, mission_event))
        host.sendall(frame_bytes(TYPE_MISSION_STATE, mission_state))
        msgs = recv_msgs(guest, 0.6)
        require((TYPE_ACTION_STATE, action) in msgs, "ActionState not relayed")
        require((TYPE_PLAYER_VEHICLE, vehicle) in msgs, "PlayerVehicle not relayed")
        require((TYPE_SHOT, shot) in msgs, "Shot not relayed")
        require((TYPE_MISSION_EVENT, mission_event) in msgs,
                "MissionEvent not relayed")
        require((TYPE_MISSION_STATE, mission_state) in msgs,
                "MissionState not relayed")

        # Guest cannot spoof host-only messages.
        guest.sendall(frame_bytes(TYPE_MISSION_EVENT, mission_event))
        guest.sendall(frame_bytes(TYPE_MISSION_STATE, mission_state))
        require(recv_type(host, TYPE_MISSION_EVENT, 0.4) is None,
                "guest MissionEvent must be blocked")
        require(recv_type(host, TYPE_MISSION_STATE, 0.4) is None,
                "guest MissionState must be blocked")

        # Mission barrier: host Offer/StartWindow/Abort -> guest;
        # guest Ready/Started -> host.
        offer = struct.pack("<IIBBH", 0x12345678, 7, 1, 0, 0)
        ready = struct.pack("<IIBBH", 0x12345678, 7, 2, 1, 0)
        started = struct.pack("<IIBBH", 0x12345678, 7, 4, 0, 0)
        window = struct.pack("<IIBBH", 0x12345678, 7, 3, 0, 0)
        abort = struct.pack("<IIBBH", 0x12345678, 7, 5, 2, 0)
        require(len(offer) == MISSION_BARRIER_SIZE, "MissionBarrier size mismatch")
        for msg in (offer, window, abort):
            host.sendall(frame_bytes(TYPE_MISSION_BARRIER, msg))
        msgs = recv_msgs(guest, 0.5)
        require((TYPE_MISSION_BARRIER, offer) in msgs, "barrier Offer not relayed")
        require((TYPE_MISSION_BARRIER, window) in msgs,
                "barrier StartWindow not relayed")
        require((TYPE_MISSION_BARRIER, abort) in msgs, "barrier Abort not relayed")

        for msg in (ready, started):
            guest.sendall(frame_bytes(TYPE_MISSION_BARRIER, msg))
        msgs = recv_msgs(host, 0.5)
        require((TYPE_MISSION_BARRIER, ready) in msgs, "barrier Ready not relayed")
        require((TYPE_MISSION_BARRIER, started) in msgs,
                "barrier Started not relayed")

        guest.sendall(frame_bytes(TYPE_MISSION_BARRIER, offer))
        require(recv_type(host, TYPE_MISSION_BARRIER, 0.4) is None,
                "guest Offer must be blocked")
        host.sendall(frame_bytes(TYPE_MISSION_BARRIER, ready))
        require(recv_type(guest, TYPE_MISSION_BARRIER, 0.4) is None,
                "host Ready must be blocked")
        host.sendall(frame_bytes(TYPE_MISSION_BARRIER, offer[:-1]))
        require(recv_type(guest, TYPE_MISSION_BARRIER, 0.4) is None,
                "short barrier payload must be dropped")

        # R&D TrainSnapshot: host-only, exact size, carCount <= max.
        train = make_train(2)
        host.sendall(frame_bytes(TYPE_TRAIN_SNAPSHOT, train))
        require(recv_type(guest, TYPE_TRAIN_SNAPSHOT, 2.0) == train,
                "TrainSnapshot must relay")
        guest.sendall(frame_bytes(TYPE_TRAIN_SNAPSHOT, train))
        require(recv_type(host, TYPE_TRAIN_SNAPSHOT, 0.4) is None,
                "guest TrainSnapshot must be blocked")
        too_many = make_train(MAX_TRAIN_CARS + 1)
        host.sendall(frame_bytes(TYPE_TRAIN_SNAPSHOT, too_many))
        require(recv_type(guest, TYPE_TRAIN_SNAPSHOT, 0.4) is None,
                "TrainSnapshot with carCount > max must be dropped")
        host.sendall(frame_bytes(TYPE_TRAIN_SNAPSHOT, train[:-1]))
        require(recv_type(guest, TYPE_TRAIN_SNAPSHOT, 0.4) is None,
                "short TrainSnapshot must be dropped")

        # Unknown message must not crash or relay.
        host.sendall(frame_bytes(0x7E, b"\x00" * 8))
        require(recv_type(guest, 0x7E, 0.4) is None, "unknown message relayed")
        host.close()
        guest.close()


def test_relay_misc_messages(exe, tmp):
    log("[test] relay: Died/Respawned/HorseOutfit/Leave/PlayerLeft")
    with Server(exe, tmp / "misc") as srv:
        host = connect(srv.port)
        join(host, "Alice")
        require(recv_type(host, TYPE_JOIN_OK) is not None, "host JOIN_OK missing")
        guest = connect(srv.port)
        join(guest, "Bob")
        require(recv_type(guest, TYPE_JOIN_OK) is not None, "guest JOIN_OK missing")
        recv_msgs(host, 0.3)
        recv_msgs(guest, 0.3)

        host.sendall(frame_bytes(TYPE_DIED, b""))
        p = recv_type(guest, TYPE_DIED, 2.0)
        require(p == make_name_payload("Alice"),
                f"Died must relay with the sender name: {p!r}")

        host.sendall(frame_bytes(TYPE_RESPAWNED, b""))
        p = recv_type(guest, TYPE_RESPAWNED, 2.0)
        require(p == make_name_payload("Alice"),
                f"Respawned must relay with the sender name: {p!r}")

        horse = b"\x5A" * OUTFIT_SIZE
        host.sendall(frame_bytes(TYPE_HORSE_OUTFIT, horse))
        p = recv_type(guest, TYPE_HORSE_OUTFIT, 2.0)
        require(p == horse, "HorseOutfit must relay byte-identical (OutfitState size)")
        host.sendall(frame_bytes(TYPE_HORSE_OUTFIT, horse[:-1]))
        require(recv_type(guest, TYPE_HORSE_OUTFIT, 0.4) is None,
                "short HorseOutfit must be dropped")

        host.sendall(frame_bytes(TYPE_LEAVE, b""))
        p = recv_type(guest, TYPE_PLAYER_LEFT, 2.0)
        require(p == make_name_payload("Alice"),
                f"PlayerLeft must carry the leaving player name: {p!r}")
        require(expect_disconnect(host), "Leave must close the sender connection")
        guest.close()


def test_worldstate_host_only(exe, tmp):
    log("[test] WorldState host-only")
    with Server(exe, tmp / "worldstate") as srv:
        host = connect(srv.port)
        join(host, "Alice")
        require(recv_type(host, TYPE_JOIN_OK) is not None, "host JOIN_OK missing")
        guest = connect(srv.port)
        join(guest, "Bob")
        require(recv_type(guest, TYPE_JOIN_OK) is not None, "guest JOIN_OK missing")
        recv_msgs(host, 0.3)
        recv_msgs(guest, 0.3)

        # 16-byte WorldState payload.
        ws = struct.pack("<BBBBIIf", 12, 0, 0, 0, 0x1234, 0x5678, 0.5)
        require(len(ws) == WORLD_STATE_SIZE, "WorldState size mismatch")
        host.sendall(frame_bytes(TYPE_WORLD_STATE, ws))
        require(recv_type(guest, TYPE_WORLD_STATE, 2.0) == ws,
                "host WorldState must relay")

        guest.sendall(frame_bytes(TYPE_WORLD_STATE, ws))
        require(recv_type(host, TYPE_WORLD_STATE, 0.6) is None,
                "guest WorldState must be blocked (host-only, like 0x10/0x13/0x14)")
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
        c, p = join_with_retry(srv, "Survivor")
        require(p is not None, "JOIN_OK missing after oversized frames")
        c.close()


def test_host_migration(exe, tmp):
    log("[test] host migration")
    with Server(exe, tmp / "migration") as srv:
        host = connect(srv.port)
        join(host, "Alice")
        require(recv_type(host, TYPE_JOIN_OK) is not None, "host JOIN_OK missing")
        guest = connect(srv.port)
        join(guest, "Bob")
        require(recv_type(guest, TYPE_JOIN_OK) is not None, "guest JOIN_OK missing")
        recv_msgs(host, 0.3)
        recv_msgs(guest, 0.3)

        host.close()
        # Bob must see PlayerLeft(Alice) and HostChanged(isYouHost=1).
        msgs = recv_msgs(guest, 1.2)
        require(any(t == TYPE_PLAYER_LEFT for t, _ in msgs),
                "guest should get PlayerLeft for departed host")
        changed = [p for t, p in msgs if t == TYPE_HOST_CHANGED]
        require(changed and changed[-1][0] == 1,
                f"guest should be promoted, got {changed}")
        require(len(changed[-1]) == HOST_CHANGED_SIZE,
                "HostChanged payload size mismatch")
        recv_msgs(guest, 0.3)

        # Bob is now host: his WorldSnapshot must be accepted and relayed
        # to a new guest.
        charlie, _ = join_with_retry(srv, "Charlie")
        recv_msgs(charlie, 0.4)
        snapshot = make_world(1)
        guest.sendall(frame_bytes(TYPE_WORLD_SNAPSHOT, snapshot))
        require(recv_type(charlie, TYPE_WORLD_SNAPSHOT, 2.0) == snapshot,
                "promoted host snapshot must relay to new guest")

        # Charlie is still a guest: his snapshot is blocked.
        charlie.sendall(frame_bytes(TYPE_WORLD_SNAPSHOT, snapshot))
        require(recv_type(guest, TYPE_WORLD_SNAPSHOT, 0.5) is None,
                "guest snapshot must be blocked after migration")

        guest.close()
        charlie.close()


def _join_race_round(srv, index):
    """One atomic-JOIN race: 3 sockets JOIN simultaneously."""
    names = [f"Race{index}A", f"Race{index}B", f"Race{index}C"]
    barrier = threading.Barrier(len(names))
    socks = [None] * len(names)
    errors = [None] * len(names)

    def worker(k):
        try:
            s = connect(srv.port)
            barrier.wait(timeout=10)
            join(s, names[k])
            socks[k] = s
        except Exception as exc:  # noqa: BLE001 - reported via errors[]
            errors[k] = exc

    threads = [threading.Thread(target=worker, args=(k,), daemon=True)
               for k in range(len(names))]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=15)
    require(all(e is None for e in errors),
            f"race {index}: connect/join threads failed: {errors}")
    require(all(s is not None for s in socks),
            f"race {index}: not all sockets connected")

    joined = []    # (socket, name, join_ok_payload)
    rejected = []  # (socket, name, payload)
    unexpected = []  # (name, type, payload)
    for sock, name in zip(socks, names):
        frame = read_one_frame(sock, duration=2.0)
        require(frame is not None,
                f"race {index}: no JOIN reply for {name} (silent close?). "
                f"server log tail:\n{tail(srv.log_text())}")
        t, p = frame
        if t == TYPE_JOIN_OK:
            joined.append((sock, name, p))
        elif t == TYPE_JOIN_REJECTED:
            rejected.append((sock, name, p))
        else:
            unexpected.append((name, t, p))

    if unexpected:
        for s in socks:
            try:
                s.close()
            except OSError:
                pass
        require(False,
                f"race {index}: first frame to a client must be JoinOk/JoinRejected "
                f"(protocol ordering), got {[(n, hex(t)) for n, t, _ in unexpected]}; "
                f"replies={[(n, 'JOIN_OK' if t == TYPE_JOIN_OK else hex(t)) for _, n, _ in joined + rejected]}. "
                f"server log tail:\n{tail(srv.log_text())}")

    require(len(joined) == 2,
            f"race {index}: expected exactly 2 players joined, got {len(joined)} "
            f"(joined={[n for _, n, _ in joined]}, "
            f"rejected={[(n, p[:1]) for _, n, p in rejected]})")
    require(len(rejected) == 1,
            f"race {index}: expected exactly 1 rejected JOIN, got {len(rejected)}")
    require(rejected[0][2] and rejected[0][2][0] == REJECT_FULL,
            f"race {index}: third player must get ServerFull, got {rejected[0][2]!r}")

    host_flags = [parse_join_ok(p)[3] for _, _, p in joined]
    require(sum(1 for flag in host_flags if flag) == 1,
            f"race {index}: expected exactly one is_host, got {host_flags}")

    host_sock, host_name, _ = next((s, n, p) for s, n, p in joined
                                   if parse_join_ok(p)[3])
    guest_sock, guest_name, _ = next((s, n, p) for s, n, p in joined
                                     if not parse_join_ok(p)[3])

    # Both must be properly registered: guest FRAME relays to host.
    drain_raw(host_sock)
    frame_payload = make_frame_payload(10.0 + index, 20.0, 30.0, 40.0)
    guest_sock.sendall(frame_bytes(TYPE_FRAME, frame_payload))
    end = time.time() + 3.0
    relayed = None
    while time.time() < end and relayed is None:
        frame = read_one_frame(host_sock, duration=1.0)
        if frame is None:
            break
        t, p = frame
        if t == TYPE_FRAME:
            relayed = p
    require(relayed == frame_payload,
            f"race {index}: guest FRAME was not relayed to the host")

    for s in socks:
        try:
            s.close()
        except OSError:
            pass

    # Wait until the server has processed both disconnects before next round.
    end = time.time() + 5.0
    while time.time() < end:
        text = srv.log_text()
        if (f"[-] {host_name} отключился" in text and
                f"[-] {guest_name} отключился" in text):
            break
        time.sleep(0.02)
    else:
        raise AssertionError(
            f"race {index}: server did not clean up players "
            f"{host_name}/{guest_name} within 5s (handler stuck before cleanup?); "
            f"stuck player likely stays in g_online and the server becomes FULL. "
            f"server log tail:\n{tail(srv.log_text())}")


def test_join_race(exe, tmp):
    log(f"[test] concurrent JOIN race x{JOIN_RACE_ITERS}")
    with Server(exe, tmp / "joinrace") as srv:
        require(JOIN_RACE_ITERS >= 1, "RDR2_JOIN_RACE_ITERS must be >= 1")
        # The server accepts at most CAP_RATE new TCP connections/sec per IP;
        # 3 sockets per round must be paced to stay under that limit, otherwise
        # a round is killed by the rate limiter before JOIN (silent close).
        # Use ~75% of the limit to absorb scheduling jitter.
        round_period = max(0.10, 3.0 / max(1.0, CAP_RATE * 0.75))
        round_start = 0.0
        for i in range(JOIN_RACE_ITERS):
            wait = round_period - (time.time() - round_start)
            if wait > 0:
                time.sleep(wait)
            round_start = time.time()
            _join_race_round(srv, i)
        require(srv.proc.poll() is None, "server died during JOIN race")
        survivor, _ = join_with_retry(srv, "AfterRace", timeout=6.0)
        survivor.close()


def test_idle_timeout(exe, tmp):
    log("[test] idle timeout")
    # Second argv = idle timeout seconds. Client joins and goes silent.
    with Server(exe, tmp / "idle", extra_args=["2"]) as srv:
        s = connect(srv.port)
        join(s, "Silent")
        require(recv_type(s, TYPE_JOIN_OK) is not None, "JOIN_OK missing")
        deadline = time.time() + 8.0
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
        require(closed, "silent client should be disconnected by idle timeout")
        s.close()

        c, _ = join_with_retry(srv, "AfterIdle")
        c.close()


def test_fuzz_frames(exe, tmp):
    log("[test] fuzz malformed frames")
    with Server(exe, tmp / "fuzz") as srv:
        s = connect(srv.port)
        join(s, "Fuzz")
        require(recv_type(s, TYPE_JOIN_OK) is not None, "JOIN_OK missing")

        # Random non-control message types/sizes; server must ignore/drop
        # without crashing or relaying nonsensical messages.
        rng = random.Random(12345)
        safe_types = [0x7E, 0x7F, TYPE_WORLD_SNAPSHOT, TYPE_ACTION_STATE,
                      TYPE_PLAYER_VEHICLE, TYPE_MISSION_EVENT, TYPE_MISSION_STATE,
                      TYPE_MISSION_BARRIER, TYPE_SHOT, TYPE_OUTFIT]
        for _ in range(300):
            t = rng.choice(safe_types + [0x00, 0x08, 0x0A, TYPE_PING])
            ln = rng.choice([0, 1, 8, 12, 16, 23, 24, 25, 32, 64, 255,
                             FRAME_SIZE, OUTFIT_SIZE, 2000])
            s.sendall(struct.pack("<I", ln) + bytes([t]) +
                      bytes([rng.randrange(256) for _ in range(ln)]))
        # Oversized length header (internal FrameReader reset, no crash).
        s.sendall(struct.pack("<I", 0x20000) + b"\x04")
        # Drain Pong echoes produced by fuzzed Ping frames.
        recv_msgs(s, 0.4)
        # Fragmented valid Ping.
        ping = frame_bytes(TYPE_PING, struct.pack("<Q", 42))
        s.sendall(ping[:2])
        time.sleep(0.02)
        s.sendall(ping[2:5])
        time.sleep(0.02)
        s.sendall(ping[5:])
        deadline = time.time() + 2.0
        pong_ok = False
        while time.time() < deadline and not pong_ok:
            for t, p in recv_msgs(s, 0.2):
                if t == TYPE_PONG and p == struct.pack("<Q", 42):
                    pong_ok = True
        require(pong_ok, "server should answer fragmented Ping")

        # A fresh client must still connect and join.
        c, _ = join_with_retry(srv, "AfterFuzz")
        s.close()
        c.close()


def test_players_txt_recovery(exe, tmp):
    log("[test] players.txt malformed recovery")
    work = tmp / "recovery"
    srv = Server(exe, work)
    srv.start()
    try:
        a = connect(srv.port)
        join(a, "Alice")
        require(recv_type(a, TYPE_JOIN_OK) is not None, "Alice JOIN_OK missing")
        a.sendall(frame_bytes(TYPE_FRAME, make_frame_payload(7.5, 8.25, 9.0, 180.0)))
        a.sendall(frame_bytes(TYPE_PING, struct.pack("<Q", 1)))
        require(recv_type(a, TYPE_PONG) == struct.pack("<Q", 1),
                "Alice frame must be processed")
        time.sleep(0.3)
        b = connect(srv.port)
        join(b, "Bob")
        require(recv_type(b, TYPE_JOIN_OK) is not None, "Bob JOIN_OK missing")
        b.sendall(frame_bytes(TYPE_FRAME, make_frame_payload(-1.5, -2.5, 3.5, 45.0)))
        b.sendall(frame_bytes(TYPE_PING, struct.pack("<Q", 2)))
        require(recv_type(b, TYPE_PONG) == struct.pack("<Q", 2),
                "Bob frame must be processed")
        time.sleep(0.3)
        recv_msgs(a, 0.2)
        recv_msgs(b, 0.2)
        b.close()      # close guest first
        time.sleep(0.3)
        a.close()
        # Wait for both disconnect handlers (and their SaveAll) to complete.
        srv.wait_log_contains("Bob отключился", 3.0)
        srv.wait_log_contains("Alice отключился", 3.0)
        time.sleep(0.25)
        srv.stop()
        time.sleep(0.3)

        players = work / "players.txt"
        original = players.read_text(encoding="utf-8") if players.exists() else ""
        require("Alice" in original and "Bob" in original,
                "both players should be saved")
        # Add junk lines; loader must ignore them.
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
            ca, pa = join_with_retry(srv2, "Alice")
            _, has, coords, _ = parse_join_ok(pa)
            require(has and coords is not None,
                    "Alice position should survive junk lines")
            require(abs(coords[0] - 7.5) < 0.01 and abs(coords[1] - 8.25) < 0.01,
                    f"Alice coords mismatch: {coords}")
            ca.close()
            srv2.wait_log_contains("Alice отключился", 3.0)

            cb, pb = join_with_retry(srv2, "Bob")
            _, hasb, coordsb, _ = parse_join_ok(pb)
            require(hasb and coordsb is not None,
                    "Bob position should survive junk lines")
            require(abs(coordsb[0] + 1.5) < 0.01 and abs(coordsb[1] + 2.5) < 0.01,
                    f"Bob coords mismatch: {coordsb}")
            cb.close()
        finally:
            srv2.stop()
    finally:
        srv.stop()


def test_players_txt_corrupt_joining_player(exe, tmp):
    log("[test] players.txt corrupt line of the JOINING player")
    work = tmp / "corrupt"
    work.mkdir(parents=True, exist_ok=True)
    (work / "players.txt").write_text(
        "Alice:junk:1:2\n"          # non-float values (used to crash std::stof)
        "Alice:\n"                  # no values at all
        "Carol:nan:inf:3:4\n"       # non-finite values
        "Dave:1:2\n"                # too few fields
        "Bob:1:2:3:4\n",            # valid line for another player
        encoding="utf-8")

    srv = Server(exe, work)
    srv.start()
    try:
        # Corrupt Alice lines must be skipped (no crash, no saved position).
        alice, p = join_with_retry(srv, "Alice")
        _, has, _, _ = parse_join_ok(p)
        require(not has,
                "corrupt players.txt lines for the joining player must be skipped")
        alice.close()
        srv.wait_log_contains("Alice отключился", 4.0)
        require(srv.proc.poll() is None,
                "server crashed on corrupt players.txt line of the joining player")

        # NaN/Inf must not be accepted as a position.
        carol, p = join_with_retry(srv, "Carol")
        _, has, _, _ = parse_join_ok(p)
        require(not has, "NaN/Inf position must not be restored")
        carol.close()
        srv.wait_log_contains("Carol отключился", 4.0)

        # A valid saved player still gets the position.
        bob, p = join_with_retry(srv, "Bob")
        _, has, coords, _ = parse_join_ok(p)
        require(has and coords is not None, "valid saved Bob position must be restored")
        require(abs(coords[0] - 1.0) < 0.01 and abs(coords[1] - 2.0) < 0.01,
                f"Bob coords mismatch: {coords}")
        bob.close()

        # Server is alive and accepts new players.
        c, _ = join_with_retry(srv, "AfterCorrupt", timeout=5.0)
        c.close()
    finally:
        srv.stop()


def test_concurrent_fuzz(exe, tmp):
    log("[test] concurrent / long fuzz")
    with Server(exe, tmp / "concurrent") as srv:
        host = connect(srv.port)
        join(host, "Alice")
        require(recv_type(host, TYPE_JOIN_OK) is not None, "host JOIN_OK missing")
        guest = connect(srv.port)
        join(guest, "Bob")
        require(recv_type(guest, TYPE_JOIN_OK) is not None, "guest JOIN_OK missing")
        recv_msgs(host, 0.3)
        recv_msgs(guest, 0.3)

        stop = threading.Event()

        def rogue_worker(seed):
            # Pre-JOIN sockets get closed by the server; keep flooding until stopped.
            try:
                r = connect(srv.port)
                rng = random.Random(seed)
                types = [TYPE_FRAME, TYPE_OUTFIT, TYPE_WORLD_SNAPSHOT,
                         TYPE_ACTION_STATE, TYPE_MISSION_EVENT, 0x7E]
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

        def sender_worker(sock, seed):
            try:
                rng = random.Random(seed)
                types = [TYPE_FRAME, TYPE_SHOT, TYPE_ACTION_STATE,
                         TYPE_PLAYER_VEHICLE, TYPE_MISSION_EVENT,
                         TYPE_MISSION_STATE, 0x7E]
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

        threads = [threading.Thread(target=rogue_worker, args=(100 + i,))
                   for i in range(3)]
        threads += [threading.Thread(target=sender_worker, args=(host, 200)),
                    threading.Thread(target=sender_worker, args=(guest, 201))]
        for th in threads:
            th.start()
        time.sleep(3.0)
        stop.set()
        for th in threads:
            th.join(timeout=3.0)

        # Server must still be alive after the storm.
        recv_msgs(host, 0.3)
        recv_msgs(guest, 0.3)
        host.sendall(frame_bytes(TYPE_PING, struct.pack("<Q", 777)))
        ok = False
        deadline = time.time() + 2.0
        while time.time() < deadline and not ok:
            for t, p in recv_msgs(host, 0.2):
                if t == TYPE_PONG and p == struct.pack("<Q", 777):
                    ok = True
        require(ok, "server should answer Ping after concurrent fuzz")
        host.close()
        guest.close()
        time.sleep(0.5)
        c, _ = join_with_retry(srv, "Survivor")
        c.close()


def test_connection_cap(exe, tmp):
    log("[test] connection flood: pre-JOIN limit + active-cap invariant")
    # kMaxActiveConnections is a defense-in-depth cap; the reachable number of
    # sockets is bounded by pre-JOIN slots + real players. Verify the cap is
    # not below that (otherwise legitimate clients could be rejected first).
    require(CAP_ACTIVE >= PREJOIN_CAP + MAX_PLAYERS,
            f"kMaxActiveConnections={CAP_ACTIVE} must exceed pre-JOIN limit "
            f"{PREJOIN_CAP} + kMaxPlayers={MAX_PLAYERS}")

    with Server(exe, tmp / "cap") as srv:
        socks = []
        total = PREJOIN_CAP + CAP_RATE + 4
        # Wave 1: fill the pre-JOIN slots (<= rate limit, so only the pre-JOIN
        # limit can reject the excess).
        for _ in range(min(CAP_RATE, total)):
            try:
                s = socket.create_connection(("127.0.0.1", srv.port), timeout=1.0)
                s.settimeout(0.5)
                socks.append(s)
            except OSError:
                pass
        time.sleep(1.05)  # roll the per-IP rate window
        # Wave 2: more raw sockets; must be rejected by the pre-JOIN limit.
        while len(socks) < total:
            try:
                s = socket.create_connection(("127.0.0.1", srv.port), timeout=1.0)
                s.settimeout(0.5)
                socks.append(s)
            except OSError:
                pass
            if len(socks) % 4 == 0:
                time.sleep(0.1)
        time.sleep(0.4)

        text = srv.wait_log_contains("лимит pre-JOIN соединений", timeout=3.0)
        prejoin_events = text.count("лимит pre-JOIN соединений")
        require(prejoin_events >= 1,
                "pre-JOIN connection limit was not enforced: no "
                f"'лимит pre-JOIN соединений' log entries. log tail:\n{tail(text)}")
        require("лимит соединений" not in text,
                "active connection cap was hit before the pre-JOIN limit: "
                "limits are applied in the wrong order")

        closed, still_open = classify_connections(socks)
        require(still_open <= PREJOIN_CAP,
                f"accepted {still_open} pre-JOIN sockets, exceeds pre-JOIN limit "
                f"{PREJOIN_CAP}")
        require(closed >= 1,
                f"pre-JOIN flood sockets were not closed (closed={closed}, "
                f"still_open={still_open}); prejoin log entries={prejoin_events}")
        for s in socks:
            try:
                s.close()
            except OSError:
                pass

        c, _ = join_with_retry(srv, "AfterFlood", timeout=8.0)
        c.close()


def test_per_ip_rate_limit(exe, tmp):
    log("[test] per-IP rate limit (real assert on rejected connections)")
    with Server(exe, tmp / "rate") as srv:
        total = CAP_RATE + 18
        socks = []
        for _ in range(total):
            try:
                s = socket.create_connection(("127.0.0.1", srv.port), timeout=1.0)
                s.settimeout(0.5)
                socks.append(s)
            except OSError:
                pass
        time.sleep(0.4)

        text = srv.wait_log_contains("per-IP rate limit", timeout=3.0)
        rate_events = text.count("per-IP rate limit")
        expected_rejected = max(0, len(socks) - CAP_RATE)
        require(rate_events >= max(1, expected_rejected - 3),
                f"per-IP rate limit rejected too few connections: "
                f"{rate_events} log entries, expected ~{expected_rejected} "
                f"(opened={len(socks)}, rate={CAP_RATE}/s). log tail:\n{tail(text)}")

        closed, still_open = classify_connections(socks)
        require(closed >= max(1, expected_rejected - 3),
                f"server closed only {closed} sockets, expected ~{expected_rejected}")
        require(still_open <= CAP_RATE + 2,
                f"too many connections survived rate limit: {still_open}")
        for s in socks:
            try:
                s.close()
            except OSError:
                pass

        time.sleep(1.2)  # new rate window
        c, _ = join_with_retry(srv, "AfterRate", timeout=6.0)
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
        require(recv_type(a, TYPE_JOIN_OK) is not None, "JOIN_OK missing")
        a.sendall(frame_bytes(TYPE_FRAME, make_frame_payload(3.5, 4.5, 5.5, 10.0)))
        a.sendall(frame_bytes(TYPE_PING, struct.pack("<Q", 5)))
        require(recv_type(a, TYPE_PONG) == struct.pack("<Q", 5),
                "before-shutdown Ping failed")
        time.sleep(0.3)
        a.close()

        deadline = time.time() + 8.0
        while time.time() < deadline and srv.proc.poll() is None:
            time.sleep(0.1)
        require(srv.proc.poll() is not None, "server did not exit on graceful stop")
        require(srv.proc.returncode == 0,
                f"graceful stop exit code {srv.proc.returncode}")
        players = work / "players.txt"
        require(players.exists() and "Shutdown" in players.read_text(encoding="utf-8"),
                "players.txt should be saved on graceful shutdown")
    finally:
        srv.stop()


def test_persistence(exe, tmp):
    log("[test] players.txt persistence across restart")
    work = tmp / "persist"
    srv = Server(exe, work)
    srv.start()
    try:
        s = connect(srv.port)
        join(s, "Alice")
        require(recv_type(s, TYPE_JOIN_OK) is not None, "JOIN_OK missing")
        frame = make_frame_payload(12.5, -3.25, 40.0, 90.0)
        s.sendall(frame_bytes(TYPE_FRAME, frame))
        s.close()
        srv.wait_log_contains("Alice отключился", 4.0)
        time.sleep(0.25)  # let the disconnect SaveAll finish before terminating
        srv.stop()
    finally:
        srv.stop()
    time.sleep(0.3)

    # Second start, same working directory (new port is fine).
    srv2 = Server(exe, work)
    srv2.start()
    try:
        c, p = join_with_retry(srv2, "Alice")
        _, has, coords, is_host = parse_join_ok(p)
        require(has, "saved position should be restored")
        require(coords is not None, "saved coords missing")
        require(abs(coords[0] - 12.5) < 0.01, f"x mismatch: {coords[0]}")
        require(abs(coords[1] - (-3.25)) < 0.01, f"y mismatch: {coords[1]}")
        require(abs(coords[2] - 40.0) < 0.01, f"z mismatch: {coords[2]}")
        require(is_host, "first player on a fresh server must be HOST")
        c.close()
    finally:
        srv2.stop()


# --------------------------------------------------------------------------
# Entry point
# --------------------------------------------------------------------------

def main():
    if sys.flags.optimize:
        print("ERROR: integration_server.py must not run with -O/PYTHONOPTIMIZE; "
              "explicit checks are disabled in optimized mode", file=sys.stderr)
        return 2

    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", required=True)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--strict", action="store_true",
                    help="treat known-fail tests as failures")
    args = ap.parse_args()
    exe = Path(args.exe).resolve()
    if not exe.exists():
        print(f"ERROR: server not found: {exe}", file=sys.stderr)
        return 2

    tmp = Path(tempfile.mkdtemp(prefix="rdr2coop-tests-"))
    failures = 0
    known_fails = 0
    passed = 0
    tests = [
        test_join_rejections,
        test_join_name_validation,
        test_roles_and_full,
        test_prejoin_and_ping,
        test_world_host_only,
        test_payload_sizes_and_host_only_messages,
        test_relay_misc_messages,
        test_worldstate_host_only,
        test_oversized_and_truncated,
        test_host_migration,
        test_join_race,
        test_idle_timeout,
        test_fuzz_frames,
        test_players_txt_recovery,
        test_players_txt_corrupt_joining_player,
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
                if test.__name__ in KNOWN_FAIL_TESTS:
                    print(f"[known-fail-passed] {test.__name__} "
                          "(remove from KNOWN_FAIL_TESTS)")
                log(f"[ok] {test.__name__}")
                passed += 1
            except Exception as exc:  # noqa: BLE001 - collect all failures
                if test.__name__ in KNOWN_FAIL_TESTS and not args.strict:
                    known_fails += 1
                    log(f"[known-fail] {test.__name__}: {exc}")
                else:
                    failures += 1
                    log(f"[FAIL] {test.__name__}: {exc}")
        log("")
        log(f"{passed} passed, {failures} failed, {known_fails} known-fail "
            f"(protocol v{PROTO}, cap={CAP_ACTIVE}/rate={CAP_RATE}/s, "
            f"name<={MAX_NAME_BYTES}B)")
        if failures:
            log(f"\n{failures} test(s) failed; workdir: {tmp}")
            return 1
        log("ALL INTEGRATION TESTS PASSED")
        return 0
    finally:
        if not args.keep and failures == 0:
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            log(f"kept workdir: {tmp}")


if __name__ == "__main__":
    sys.exit(main())
