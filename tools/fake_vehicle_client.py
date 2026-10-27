#!/usr/bin/env python3
"""Headless fake player for v20 P1 vehicle interactions (protocol v20).

The tool speaks only the wire protocol of the relay server; it does not need
the game. Three modes (Python 3 standard library only):

    # 1) full single-PC wire scenario (opens BOTH sockets in one process):
    python tools\\fake_vehicle_client.py --mode scenario --port 7777

    # 2) fake HOST (run it BEFORE launching the game as a guest):
    python tools\\fake_vehicle_client.py --mode host --name V20Host --duration 120

    # 3) fake GUEST (run it AFTER the game is already connected as host):
    python tools\\fake_vehicle_client.py --mode guest --name V20Guest --duration 30

v20 P1 messages (see net/protocol.h):
    0x23 VehicleInteraction        24 B  guest -> server -> host   (intent)
    0x24 VehicleInteractionResult  16 B  host  -> server -> guest  (answer)
    0x26 VehicleState              24 B  host  -> server -> guest  (state)
    0x25/0x27                      RESERVED (P3): never sent here.

The scenario mode is the regression/smoke harness for U2 (server relay) and a
ready-to-use probe for U3/U4 (host apply / guest capture): valid/invalid intents,
door/damage/enter actions, result/state relay, role violations, rate limits
(8/20/10 per second) and host migration.

Protocol IDs/sizes/rates are read from the repository (`net/protocol.h`,
`server/server.cpp`) so the tool never lags behind the code.

Exit codes (same style as tools/fake_actor_client.py):
    0 = all checks passed / expected traffic observed
    1 = usage/environment error (server unreachable, JOIN rejected, ...)
    2 = protocol/assertion failure (relay missing, wrong payload, rate leak)
"""

from __future__ import annotations

import argparse
import json
import re
import socket
import struct
import sys
import time
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
PROTOCOL_H = REPO_ROOT / "net" / "protocol.h"
SERVER_CPP = REPO_ROOT / "server" / "server.cpp"
DEFAULT_LOG = Path(__file__).resolve().with_name("fake_vehicle_client.log")

K_MAX_PAYLOAD = 64 * 1024

MSG_JOIN = 0x01
MSG_JOIN_OK = 0x02
MSG_JOIN_REJECTED = 0x0F
MSG_FRAME = 0x04
MSG_LEAVE = 0x05
MSG_PING = 0x0C
MSG_PONG = 0x0D
MSG_HOST_CHANGED = 0x16
MSG_VEHICLE_INTERACTION = 0x23
MSG_VEHICLE_RESULT = 0x24
MSG_VEHICLE_STATE = 0x26

INTERACTION_SIZE = 24
RESULT_SIZE = 16
STATE_SIZE = 24

# Server-side caps (D3): 0x23 8/s, 0x24 20/s, 0x26 10/s per client.
RATE_INTERACTION = 8
RATE_RESULT = 20
RATE_STATE = 10

DEFAULT_MODEL = 0x54D8BE8E  # a_m_m_rancher_01
JOIN_REJECT_NAMES = {1: "VersionMismatch", 2: "ServerFull", 3: "NameTaken",
                     4: "BadName"}
FRAME_PAYLOAD = bytes(88)  # FramePayload size; used as host-liveness keepalive

ACTIONS = {
    "damage": 1, "door_open": 2, "door_close": 3, "door_lock": 4,
    "door_unlock": 5, "door_break": 6, "enter": 7, "exit": 8,
    "hijack": 9, "loot": 10, "hitch": 11, "unhitch": 12,
}
ACTION_NAMES = {value: name for name, value in ACTIONS.items()}
RESULT_NAMES = {
    0: "applied", 1: "not_allowed", 2: "stale_epoch", 3: "not_found",
    4: "out_of_range", 5: "seat_occupied", 6: "locked", 7: "rate_limited",
    8: "timeout", 9: "correction",
}
REASONS = {
    1: "config_off", 2: "mission_protected", 3: "host_vehicle",
    4: "bad_param", 5: "dead_guest", 6: "bad_role", 7: "policy",
}
SEAT_NONE = 127
SEAT_DRIVER = -1
DOOR_ALL = 255
DOOR_NONE = 255
# Damage parts (`door` field for action=Damage).
PARTS = {
    "door0": 0, "door1": 1, "door2": 2, "door3": 3, "door4": 4,
    "door5": 5, "door6": 6, "door7": 7,
    "wheel_fl": 8, "wheel_fr": 9, "wheel_rl": 10, "wheel_rr": 11,
    "engine": 254, "body": 255,
}

# VehicleState flags (net/protocol.h).
STATE_FLAG_DOOR = 1 << 0
STATE_FLAG_DAMAGE = 1 << 1
STATE_FLAG_LOCKED = 1 << 2
STATE_FLAG_MISSION = 1 << 3
STATE_FLAG_HOST_SEAT = 1 << 4
STATE_FLAG_GUEST_SEAT = 1 << 5
STATE_FLAG_HIJACKED = 1 << 6
STATE_FLAG_DEAD = 1 << 7
STATE_KNOWN_FLAGS = 0xFF
BODY_UNKNOWN = 0xFFFF
ENGINE_UNKNOWN = -32768


# ---------------------------------------------------------------------------
# Protocol facts: read from the repository so the tool follows the code.
# ---------------------------------------------------------------------------
def _read_text(path: Path) -> str:
    try:
        return path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return ""


_PROTO_SRC = _read_text(PROTOCOL_H)
_SERVER_SRC = _read_text(SERVER_CPP)


def _proto_version() -> int:
    match = re.search(r"kProtocolVersion\s*=\s*(\d+)\s*;", _PROTO_SRC)
    return int(match.group(1)) if match else 20


def _server_rate(name: str, default: int) -> int:
    match = re.search(re.escape(name) + r"\s*=\s*(\d+)\s*;", _SERVER_SRC)
    return int(match.group(1)) if match else default


PROTOCOL_VERSION = _proto_version()
RATE_INTERACTION = _server_rate("kMaxVehicleInteractionPerSecond", 8)
RATE_RESULT = _server_rate("kMaxVehicleInteractionResultPerSecond", 20)
RATE_STATE = _server_rate("kMaxVehicleStatePerSecond", 10)


# ---------------------------------------------------------------------------
# Logging: stdout + append to a log file (mirrors fake_actor_client.py).
# ---------------------------------------------------------------------------
class Log:
    def __init__(self, path: Path):
        self._file = None
        if path:
            try:
                path.parent.mkdir(parents=True, exist_ok=True)
                self._file = open(path, "a", encoding="utf-8", errors="replace")
            except OSError as exc:  # pragma: no cover - filesystem issue
                print(f"warning: cannot open log file {path}: {exc}", flush=True)
                self._file = None

    def write(self, msg: str) -> None:
        stamp = time.strftime("%H:%M:%S", time.localtime())
        ms = int((time.time() % 1) * 1000)
        line = f"[{stamp}.{ms:03d}] {msg}"
        print(line, flush=True)
        if self._file is not None:
            try:
                self._file.write(line + "\n")
                self._file.flush()
            except OSError:
                pass

    def close(self) -> None:
        if self._file is not None:
            try:
                self._file.close()
            except OSError:
                pass
            self._file = None


# ---------------------------------------------------------------------------
# Wire helpers.
# ---------------------------------------------------------------------------
class ProtocolError(Exception):
    """Malformed/oversized incoming frame or stream closed."""


def make_frame(msg_type: int, payload: bytes = b"") -> bytes:
    if len(payload) > K_MAX_PAYLOAD:
        raise ValueError(f"payload too large: {len(payload)}")
    return struct.pack("<I", len(payload)) + bytes([msg_type]) + payload


class FrameReader:
    """Incremental stream framer: never assumes a whole frame arrives at once."""

    def __init__(self, max_payload: int = K_MAX_PAYLOAD):
        self._buf = bytearray()
        self._max_payload = max_payload

    def feed(self, data: bytes) -> None:
        self._buf.extend(data)

    def pop_frames(self):
        out = []
        while True:
            if len(self._buf) < 5:
                return out
            length = struct.unpack_from("<I", self._buf, 0)[0]
            if length > self._max_payload:
                raise ProtocolError(
                    f"incoming frame length {length} > kMaxPayload "
                    f"({self._max_payload}); stream desync")
            total = 5 + length
            if len(self._buf) < total:
                return out
            msg_type = self._buf[4]
            payload = bytes(self._buf[5:total])
            del self._buf[:total]
            out.append((msg_type, payload))


def connect(host: str, port: int, timeout: float = 5.0) -> socket.socket:
    sock = socket.create_connection((host, port), timeout=timeout)
    try:
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    except OSError:
        pass
    return sock


def recv_available(sock: socket.socket, reader: FrameReader, wait: float = 0.05):
    sock.settimeout(wait)
    try:
        data = sock.recv(65536)
    except socket.timeout:
        return []
    except (BlockingIOError, InterruptedError):
        return []
    if not data:
        raise ProtocolError("connection closed by peer")
    reader.feed(data)
    return reader.pop_frames()


def send_join(sock: socket.socket, name: str, model: int = DEFAULT_MODEL) -> None:
    name_bytes = name.encode("utf-8")
    if not name_bytes or len(name_bytes) > 32:
        raise ValueError(f"name must be 1..32 UTF-8 bytes, got {len(name_bytes)}")
    payload = bytes([len(name_bytes)]) + name_bytes + \
        struct.pack("<Q", model & 0xFFFFFFFFFFFFFFFF) + \
        struct.pack("<H", PROTOCOL_VERSION)
    sock.sendall(make_frame(MSG_JOIN, payload))


def parse_join_ok(payload: bytes):
    if len(payload) < 10:
        raise ProtocolError(f"JoinOk too short: {len(payload)} bytes")
    sid = struct.unpack_from("<Q", payload, 0)[0]
    has_saved = payload[8] != 0
    off = 9
    coords = None
    if has_saved:
        if len(payload) < off + 16 + 1:
            raise ProtocolError("JoinOk saved coords truncated")
        coords = struct.unpack_from("<ffff", payload, off)
        off += 16
    if len(payload) <= off:
        raise ProtocolError("JoinOk without is_host byte")
    is_host = payload[off] != 0
    return sid, has_saved, coords, is_host


def send_frame(sock: socket.socket, msg_type: int, payload: bytes = b"") -> None:
    sock.sendall(make_frame(msg_type, payload))


def send_ping(sock: socket.socket) -> None:
    try:
        send_frame(sock, MSG_PING, struct.pack("<Q", int(time.time() * 1000)))
    except OSError:
        pass


def send_leave(sock: socket.socket) -> None:
    try:
        send_frame(sock, MSG_LEAVE, b"")
    except OSError:
        pass


def join_session(sock: socket.socket, name: str, log: Log, tag: str,
                 timeout: float = 10.0):
    """JOIN and wait for JoinOk. Returns (sid, coords, is_host)."""
    reader = FrameReader()
    send_join(sock, name)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        for msg_type, payload in recv_available(sock, reader, wait=0.1):
            if msg_type == MSG_JOIN_OK:
                sid, has_saved, coords, is_host = parse_join_ok(payload)
                tail = ""
                if has_saved and coords:
                    tail = (f" saved=({coords[0]:.1f},{coords[1]:.1f},"
                            f"{coords[2]:.1f})")
                log.write(f"{tag}: join ok sid={sid} is_host={int(is_host)}"
                          f"{tail}")
                return sid, coords, is_host, reader
            if msg_type == MSG_JOIN_REJECTED:
                reason = payload[0] if payload else 0
                raise RuntimeError(
                    f"JOIN rejected: {reason} "
                    f"({JOIN_REJECT_NAMES.get(reason, 'unknown')})")
    raise TimeoutError("no JoinOk in time; is the server running and v20?")


def recv_frames(sock: socket.socket, reader: FrameReader, duration: float):
    """Collect all frames arriving within `duration` seconds."""
    end = time.monotonic() + duration
    frames = []
    while time.monotonic() < end:
        frames.extend(recv_available(sock, reader, wait=0.02))
    return frames


def recv_type(sock: socket.socket, reader: FrameReader, msg_type: int,
              timeout: float):
    """Return the first payload of `msg_type` within timeout (or None)."""
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        for got_type, payload in recv_available(sock, reader, wait=0.02):
            if got_type == msg_type:
                return payload
    return None


def count_type(sock: socket.socket, reader: FrameReader, msg_type: int,
               duration: float) -> int:
    return sum(1 for got, _p in recv_frames(sock, reader, duration)
               if got == msg_type)


# ---------------------------------------------------------------------------
# v20 P1 payload builders / parsers / validators (net/protocol.h layouts).
# ---------------------------------------------------------------------------
def _intent_defaults(action_id: int) -> dict:
    """Safe per-action defaults so every built intent is wire-valid."""
    name = ACTION_NAMES.get(action_id, "damage")
    if name == "damage":
        return dict(seat=SEAT_NONE, door=PARTS["body"], param=60,
                    weapon_hash=0xAABBCCDD)
    if name in ("door_open", "door_close", "door_break"):
        return dict(seat=SEAT_NONE, door=0, param=0, weapon_hash=0)
    if name in ("door_lock", "door_unlock"):
        return dict(seat=SEAT_NONE, door=DOOR_ALL, param=0, weapon_hash=0)
    if name == "enter":
        return dict(seat=0, door=DOOR_NONE, param=0, weapon_hash=0)
    if name == "exit":
        return dict(seat=SEAT_NONE, door=DOOR_NONE, param=0, weapon_hash=0)
    if name == "hijack":
        return dict(seat=SEAT_DRIVER, door=DOOR_NONE, param=0,
                    weapon_hash=0)
    if name == "loot":
        return dict(seat=SEAT_NONE, door=DOOR_NONE, param=0, weapon_hash=0)
    if name in ("hitch", "unhitch"):
        return dict(seat=SEAT_NONE, door=0, param=0, weapon_hash=0)
    return dict(seat=SEAT_NONE, door=PARTS["body"], param=60,
                weapon_hash=0xAABBCCDD)


def build_intent(epoch=7, net_id=42, target_id=0, seq=1, action="damage",
                 seat=None, door=None, flags=0, param=None, weapon_hash=None):
    action_id = ACTIONS[action] if isinstance(action, str) else int(action)
    defaults = _intent_defaults(action_id)
    if seat is None:
        seat = defaults["seat"]
    if door is None:
        door = defaults["door"]
    if param is None:
        param = defaults["param"]
    if weapon_hash is None:
        weapon_hash = defaults["weapon_hash"]
    payload = struct.pack("<IIIHBbBBHI", epoch, net_id, target_id, seq,
                          action_id, seat, door, flags, param, weapon_hash)
    if len(payload) != INTERACTION_SIZE:
        raise ValueError(f"intent size {len(payload)} != {INTERACTION_SIZE}")
    return payload


def build_result(epoch=7, net_id=42, seq=1, action="damage", result=0, detail=0):
    action_id = ACTIONS[action] if isinstance(action, str) else int(action)
    payload = struct.pack("<IIHBBI", epoch, net_id, seq, action_id, result,
                          detail)
    if len(payload) != RESULT_SIZE:
        raise ValueError(f"result size {len(payload)} != {RESULT_SIZE}")
    return payload


def build_state(epoch=7, net_id=42, body_health=500, engine_health=500,
                door_bits=1, seq=1, flags=STATE_FLAG_DOOR,
                wheels_bits=0, host_seat=SEAT_NONE, guest_seat=SEAT_NONE,
                reserved=0):
    payload = struct.pack("<IIHhHHBBbbI", epoch, net_id, body_health,
                          engine_health, door_bits, seq, flags, wheels_bits,
                          host_seat, guest_seat, reserved)
    if len(payload) != STATE_SIZE:
        raise ValueError(f"state size {len(payload)} != {STATE_SIZE}")
    return payload


def parse_intent(payload: bytes) -> dict:
    if len(payload) != INTERACTION_SIZE:
        raise ProtocolError(f"intent size {len(payload)} != {INTERACTION_SIZE}")
    (epoch, net_id, target_id, seq, action, seat, door, flags, param,
     weapon_hash) = struct.unpack("<IIIHBbBBHI", payload)
    return {
        "epoch": epoch, "net_id": net_id, "target_id": target_id, "seq": seq,
        "action": action, "action_name": ACTION_NAMES.get(action, f"?{action}"),
        "seat": seat, "door": door, "flags": flags, "param": param,
        "weapon_hash": weapon_hash,
    }


def parse_result(payload: bytes) -> dict:
    if len(payload) != RESULT_SIZE:
        raise ProtocolError(f"result size {len(payload)} != {RESULT_SIZE}")
    epoch, net_id, seq, action, result, detail = struct.unpack(
        "<IIHBBI", payload)
    reason = (detail >> 8) & 0xFF
    return {
        "epoch": epoch, "net_id": net_id, "seq": seq, "action": action,
        "action_name": ACTION_NAMES.get(action, f"?{action}"),
        "result": result, "result_name": RESULT_NAMES.get(result, f"?{result}"),
        "detail": detail, "reason": RESULT_NAMES.get(reason, ""),
    }


def parse_state(payload: bytes) -> dict:
    if len(payload) != STATE_SIZE:
        raise ProtocolError(f"state size {len(payload)} != {STATE_SIZE}")
    (epoch, net_id, body_health, engine_health, door_bits, seq, flags,
     wheels_bits, host_seat, guest_seat, reserved) = struct.unpack(
        "<IIHhHHBBbbI", payload)
    return {
        "epoch": epoch, "net_id": net_id, "body_health": body_health,
        "engine_health": engine_health, "door_bits": door_bits, "seq": seq,
        "flags": flags, "wheels_bits": wheels_bits, "host_seat": host_seat,
        "guest_seat": guest_seat, "reserved": reserved,
    }


def _valid_seat(value: int) -> bool:
    return value in (SEAT_DRIVER, SEAT_NONE) or 0 <= value <= 8


def validate_intent(payload: bytes):
    """Client-side mirror of net::IsValidVehicleInteraction (for assertions)."""
    if len(payload) != INTERACTION_SIZE:
        return False, f"size {len(payload)} != {INTERACTION_SIZE}"
    m = parse_intent(payload)
    if m["epoch"] == 0 or m["net_id"] == 0 or m["seq"] == 0:
        return False, "epoch/netId/seq == 0"
    if not 1 <= m["action"] <= 12:
        return False, f"action {m['action']} outside 1..12"
    if m["flags"] & ~0x3F:
        return False, f"unknown flags 0x{m['flags']:02X}"
    if (m["flags"] & 1) and (m["flags"] & 2):
        return False, "target actor+entity flags together"
    if not _valid_seat(m["seat"]):
        return False, f"seat {m['seat']} invalid"
    action = m["action"]
    if action != ACTIONS["damage"] and m["weapon_hash"] != 0:
        return False, "weaponHash on non-damage action"
    target = m["target_id"]
    door = m["door"]
    param = m["param"]
    if action == ACTIONS["damage"]:
        if not (door <= 11 or door in (254, 255)):
            return False, f"damage part {door} invalid"
        if not 1 <= param <= 1000:
            return False, f"damage param {param} outside 1..1000"
        return True, "ok"
    if action in (ACTIONS["door_open"], ACTIONS["door_close"],
                  ACTIONS["door_break"]):
        if not (door <= 7 and param == 0):
            return False, "door action requires door 0..7, param 0"
        return True, "ok"
    if action in (ACTIONS["door_lock"], ACTIONS["door_unlock"]):
        if not (door <= 7 or door == DOOR_ALL) or param != 0:
            return False, "lock action requires door 0..7/255, param 0"
        return True, "ok"
    if action == ACTIONS["enter"]:
        if door != DOOR_NONE or target != 0 or param != 0:
            return False, "enter requires door/target/param = 0/0/255"
        return True, "ok"
    if action == ACTIONS["exit"]:
        if m["seat"] != SEAT_NONE or door != DOOR_NONE or param != 0:
            return False, "exit requires seat 127, door 255, param 0"
        return True, "ok"
    if action == ACTIONS["hijack"]:
        if m["seat"] not in (SEAT_DRIVER, SEAT_NONE) or door != DOOR_NONE \
                or param != 0 or (target != 0 and not (m["flags"] & 1)):
            return False, "hijack seat/door/param/target invalid"
        return True, "ok"
    if action == ACTIONS["loot"]:
        if m["seat"] != SEAT_NONE or param != 0:
            return False, "loot requires seat 127, param 0"
        if (m["flags"] & 2) and target == 0:
            return False, "loot entity flag without targetId"
        if not (m["flags"] & 2) and target != 0:
            return False, "loot targetId without entity flag"
        return True, "ok"
    if action in (ACTIONS["hitch"], ACTIONS["unhitch"]):
        if door > 5 or param != 0 or (target != 0 and not (m["flags"] & 2)):
            return False, "hitch/unhitch harness/target invalid"
        return True, "ok"
    return False, "unknown action"


def validate_result(payload: bytes):
    if len(payload) != RESULT_SIZE:
        return False, f"size {len(payload)} != {RESULT_SIZE}"
    m = parse_result(payload)
    if m["epoch"] == 0 or m["net_id"] == 0 or m["seq"] == 0:
        return False, "epoch/netId/seq == 0"
    if not 1 <= m["action"] <= 12:
        return False, "action outside 1..12"
    if not 0 <= m["result"] <= 9:
        return False, f"result {m['result']} outside 0..9"
    return True, "ok"


def validate_state(payload: bytes):
    if len(payload) != STATE_SIZE:
        return False, f"size {len(payload)} != {STATE_SIZE}"
    m = parse_state(payload)
    if m["epoch"] == 0 or m["net_id"] == 0:
        return False, "epoch/netId == 0"
    if m["flags"] & ~STATE_KNOWN_FLAGS:
        return False, f"unknown state flags 0x{m['flags']:02X}"
    body = m["body_health"]
    if not (body <= 1000 or body == BODY_UNKNOWN):
        return False, f"bodyHealth {body} invalid"
    engine = m["engine_health"]
    if not (-4000 <= engine <= 1000 or engine == ENGINE_UNKNOWN):
        return False, f"engineHealth {engine} invalid"
    if not _valid_seat(m["host_seat"]) or not _valid_seat(m["guest_seat"]):
        return False, "host/guest seat invalid"
    return True, "ok"


# ---------------------------------------------------------------------------
# Scenario checks (single PC: both fake players against one relay server).
# ---------------------------------------------------------------------------
class Scenario:
    """Full single-PC v20 P1 wire scenario against a running relay server."""

    def __init__(self, args, log: Log):
        self.args = args
        self.log = log
        self.passed = 0
        self.failed = 0
        self.failures = []

    # -- assertions ------------------------------------------------------
    def check(self, cond, what: str):
        if cond:
            self.passed += 1
            self.log.write(f"  PASS {what}")
        else:
            self.failed += 1
            self.failures.append(what)
            self.log.write(f"  FAIL {what}")
        return cond

    def _pair(self):
        host = connect(self.args.host, self.args.port)
        host_sid, _coords, is_host, host_reader = join_session(
            host, self.args.host_name, self.log, "fakehost")
        guest = connect(self.args.host, self.args.port)
        guest_sid, _coords2, guest_is_host, guest_reader = join_session(
            guest, self.args.guest_name, self.log, "fakeguest")
        self.check(host_sid != guest_sid, "host/guest got distinct session ids")
        self.check(is_host and not guest_is_host,
                   "first JOIN is host, second JOIN is guest")
        recv_frames(host, host_reader, 0.2)   # PlayerJoined / noise
        recv_frames(guest, guest_reader, 0.2)
        return host, host_reader, guest, guest_reader

    # -- host liveness ---------------------------------------------------
    # 0x24/0x26 are game-thread messages, but the scenario also has seconds
    # with no host traffic. A zeroed 88 B Frame is a valid game-thread message:
    # without it the server would apply the 4 s host game-timeout mid-scenario.
    @staticmethod
    def _host_frame(host):
        send_frame(host, MSG_FRAME, FRAME_PAYLOAD)

    def _host_alive(self, host, seconds):
        end = time.monotonic() + max(0.0, seconds)
        while True:
            self._host_frame(host)
            left = end - time.monotonic()
            if left <= 0:
                return
            time.sleep(min(0.9, left))

    # -- phases ----------------------------------------------------------
    def phase_lone_host(self):
        """Lone host sending 0x23 = role violation; server survives; then pairs."""
        self.log.write("[phase] role violation (lone host sends guest-only 0x23)")
        host = connect(self.args.host, self.args.port)
        _sid, _c, is_host, reader = join_session(host, self.args.host_name,
                                                 self.log, "fakehost")
        self.check(is_host, "lone client became host")
        self._host_frame(host)
        intent = build_intent(seq=1)
        send_frame(host, MSG_VEHICLE_INTERACTION, intent)
        leaked = count_type(host, reader, MSG_VEHICLE_INTERACTION, 0.4)
        self.check(leaked == 0, "host 0x23 is not relayed anywhere (role drop)")
        send_ping(host)
        self.check(recv_type(host, reader, MSG_PONG, 2.0) is not None,
                   "server alive after host role violation (Pong)")
        guest = connect(self.args.host, self.args.port)
        _gsid, _gc, guest_host, guest_reader = join_session(
            guest, self.args.guest_name, self.log, "fakeguest")
        self.check(not guest_host, "second player is guest")
        recv_frames(host, reader, 0.2)
        # Recovery: a guest intent now relays to the host.
        send_frame(guest, MSG_VEHICLE_INTERACTION, intent)
        got = recv_type(host, reader, MSG_VEHICLE_INTERACTION, 2.0)
        self.check(got == intent,
                   "guest 0x23 relays to host after role-violation recovery")
        recv_frames(guest, guest_reader, 0.2)
        return host, reader, guest, guest_reader

    def phase_valid_relay(self, host, host_reader, guest, guest_reader):
        self.log.write("[phase] valid guest->host 0x23 relay, no echo")
        self._host_frame(host)
        intent = build_intent(epoch=9, net_id=77, seq=100, action="damage",
                              door=PARTS["wheel_fl"], param=60,
                              weapon_hash=0xAABBCCDD)
        send_frame(guest, MSG_VEHICLE_INTERACTION, intent)
        got = recv_type(host, host_reader, MSG_VEHICLE_INTERACTION, 2.0)
        self.check(got == intent,
                   "0x23 relayed guest->host byte-identical")
        if got is not None:
            m = parse_intent(got)
            self.check(m["action_name"] == "damage" and m["net_id"] == 77,
                       "relayed 0x23 fields decode (action/netId)")
        echo = recv_type(guest, guest_reader, MSG_VEHICLE_INTERACTION, 0.4)
        self.check(echo is None, "0x23 is not echoed back to the guest")

    def phase_action_matrix(self, host, host_reader, guest, guest_reader):
        self.log.write("[phase] valid action matrix (Damage/doors/enter/...)")
        cases = [
            ("damage door1", dict(action="damage", door=PARTS["door1"],
                                  param=80)),
            ("damage engine", dict(action="damage", door=PARTS["engine"],
                                   param=120)),
            ("damage body", dict(action="damage", door=PARTS["body"],
                                 param=100)),
            ("door_open", dict(action="door_open", door=2)),
            ("door_close", dict(action="door_close", door=2)),
            ("door_lock all", dict(action="door_lock", door=DOOR_ALL)),
            ("door_unlock", dict(action="door_unlock", door=3)),
            ("door_break", dict(action="door_break", door=4)),
            ("enter seat 0", dict(action="enter", seat=0, door=DOOR_NONE,
                                   weapon_hash=0)),
            ("enter driver", dict(action="enter", seat=SEAT_DRIVER,
                                  door=DOOR_NONE, weapon_hash=0)),
            ("exit", dict(action="exit", seat=SEAT_NONE, door=DOOR_NONE,
                          weapon_hash=0)),
            ("hijack", dict(action="hijack", seat=SEAT_DRIVER,
                            door=DOOR_NONE, weapon_hash=0)),
            ("loot", dict(action="loot", seat=SEAT_NONE, door=DOOR_NONE,
                          weapon_hash=0, target_id=0)),
            ("hitch harness 1", dict(action="hitch", door=1,
                                     target_id=0, weapon_hash=0)),
            ("unhitch harness 1", dict(action="unhitch", door=1,
                                       target_id=0, weapon_hash=0)),
        ]
        total_ok = 0
        # Respect the server 8/s cap: at most 6 intents per window.
        for chunk_start in range(0, len(cases), 6):
            chunk = cases[chunk_start:chunk_start + 6]
            self._host_frame(host)
            sent = []
            for index, (label, kwargs) in enumerate(chunk):
                seq = 200 + chunk_start + index
                payload = build_intent(epoch=9, net_id=77, seq=seq, **kwargs)
                ok, why = validate_intent(payload)
                self.check(ok, f"action {label}: local validator accepts ({why})")
                send_frame(guest, MSG_VEHICLE_INTERACTION, payload)
                sent.append((label, payload))
            received = {}
            end = time.monotonic() + 1.2
            while time.monotonic() < end and len(received) < len(sent):
                for got_type, payload in recv_available(host, host_reader,
                                                         wait=0.02):
                    if got_type == MSG_VEHICLE_INTERACTION:
                        received[payload] = True
            for label, payload in sent:
                total_ok += int(received.get(payload, False))
                self.check(received.get(payload, False),
                           f"action {label}: relayed byte-identical")
            if chunk_start + 6 < len(cases):
                recv_frames(guest, guest_reader, 0.0)
                self._host_alive(host, 1.1)  # next rate window + liveness
        self.log.write(f"  action matrix relayed {total_ok}/{len(cases)}")

    def phase_invalid_drops(self, host, host_reader, guest, guest_reader):
        self.log.write("[phase] invalid 0x23 payloads are dropped")
        self._host_frame(host)
        good = build_intent(action="damage", door=PARTS["body"], param=60,
                            seq=300)
        invalid = [
            ("short 23 B", good[:-1]),
            ("long 25 B", good + b"\x00"),
            ("epoch=0", build_intent(epoch=0, seq=301)),
            ("netId=0", build_intent(net_id=0, seq=302)),
            ("seq=0", build_intent(seq=0)),
            ("action None", build_intent(action=0, weapon_hash=0, seq=303)),
            ("action 13", build_intent(action=13, weapon_hash=0, seq=304)),
            ("seat 9", build_intent(seat=9, seq=305)),
            ("door part 12", build_intent(door=12, seq=306)),
            ("damage param 0", build_intent(param=0, seq=307)),
            ("unknown flag 0x40", build_intent(flags=0x40, seq=308)),
            ("actor+entity flags", build_intent(flags=1 | 2, seq=309)),
            ("weapon on non-damage", build_intent(
                action="door_open", door=1, weapon_hash=0x1234, seq=310)),
        ]
        for index, (label, payload) in enumerate(invalid):
            if len(payload) == INTERACTION_SIZE:
                ok, why = validate_intent(payload)
                self.check(not ok, f"invalid {label}: local validator rejects "
                                   f"({why})")
            send_frame(guest, MSG_VEHICLE_INTERACTION, payload)
        leaked = count_type(host, host_reader, MSG_VEHICLE_INTERACTION, 0.6)
        self.check(leaked == 0,
                   "invalid 0x23 batch dropped (host received none)")
        # Server alive: a valid frame still relays after the invalid burst.
        send_frame(guest, MSG_VEHICLE_INTERACTION,
                   build_intent(seq=311))
        self.check(recv_type(host, host_reader, MSG_VEHICLE_INTERACTION,
                             2.0) is not None,
                   "server alive and relays after invalid 0x23 burst")

    def phase_host_only_relay(self, host, host_reader, guest, guest_reader):
        self.log.write("[phase] host->guest 0x24/0x26 relay; guest senders "
                       "dropped")
        self._host_frame(host)
        result = build_result(epoch=9, net_id=77, seq=100, action="damage",
                              result=0, detail=0)
        send_frame(host, MSG_VEHICLE_RESULT, result)
        self.check(recv_type(guest, guest_reader, MSG_VEHICLE_RESULT, 2.0)
                   == result, "0x24 relayed host->guest byte-identical")
        self.check(recv_type(host, host_reader, MSG_VEHICLE_RESULT, 0.3) is None,
                   "0x24 is not echoed back to the host")

        state = build_state(epoch=9, net_id=77, body_health=750,
                            engine_health=800, door_bits=0x0005, seq=7,
                            flags=STATE_FLAG_DOOR | STATE_FLAG_DAMAGE,
                            wheels_bits=2, host_seat=SEAT_DRIVER,
                            guest_seat=0)
        send_frame(host, MSG_VEHICLE_STATE, state)
        self.check(recv_type(guest, guest_reader, MSG_VEHICLE_STATE, 2.0)
                   == state, "0x26 relayed host->guest byte-identical")

        # Role violations: host-only messages from the guest must be dropped.
        send_frame(guest, MSG_VEHICLE_RESULT, result)
        send_frame(guest, MSG_VEHICLE_STATE, state)
        self.check(recv_type(host, host_reader, MSG_VEHICLE_RESULT, 0.4) is None,
                   "guest 0x24 dropped (host-only)")
        self.check(recv_type(host, host_reader, MSG_VEHICLE_STATE, 0.4) is None,
                   "guest 0x26 dropped (host-only)")

    def phase_invalid_host_messages(self, host, host_reader, guest,
                                    guest_reader):
        self.log.write("[phase] invalid 0x24/0x26 payloads are dropped")
        self._host_frame(host)
        results = [
            ("short 15 B", build_result(seq=1)[:-1]),
            ("epoch=0", build_result(epoch=0, seq=2)),
            ("netId=0", build_result(net_id=0, seq=3)),
            ("seq=0", build_result(seq=0)),
            ("action 0", build_result(action=0, seq=4)),
            ("result 10", build_result(result=10, seq=5)),
        ]
        for label, payload in results:
            if len(payload) == RESULT_SIZE:
                ok, why = validate_result(payload)
                self.check(not ok, f"invalid result {label}: rejected ({why})")
            send_frame(host, MSG_VEHICLE_RESULT, payload)
        states = [
            ("short 23 B", build_state(seq=1)[:-1]),
            ("epoch=0", build_state(epoch=0)),
            ("netId=0", build_state(net_id=0)),
            ("body 1001", build_state(body_health=1001)),
            ("engine -4001", build_state(engine_health=-4001)),
            ("hostSeat 9", build_state(host_seat=9)),
        ]
        for label, payload in states:
            if len(payload) == STATE_SIZE:
                ok, why = validate_state(payload)
                self.check(not ok, f"invalid state {label}: rejected ({why})")
            send_frame(host, MSG_VEHICLE_STATE, payload)
        self.check(count_type(guest, guest_reader, MSG_VEHICLE_RESULT, 0.5) == 0,
                   "invalid 0x24 batch dropped")
        self.check(count_type(guest, guest_reader, MSG_VEHICLE_STATE, 0.5) == 0,
                   "invalid 0x26 batch dropped")

    def phase_rate_limits(self, host, host_reader, guest, guest_reader):
        self.log.write("[phase] server rate limits (0x23 8/s, 0x24 20/s, "
                       "0x26 10/s)")
        # Let the previous 1 s server rate windows expire before measuring;
        # keep the host alive while idling (4 s host game-timeout).
        self._host_alive(host, 1.1)
        self._host_frame(host)
        # 0x23: guest -> host, cap 8/s.
        for _ in range(RATE_INTERACTION * 2):
            send_frame(guest, MSG_VEHICLE_INTERACTION, build_intent(seq=500))
        got = count_type(host, host_reader, MSG_VEHICLE_INTERACTION, 1.2)
        self.check(got == RATE_INTERACTION,
                   f"0x23 rate limit {RATE_INTERACTION}/s (got {got})")
        self._host_alive(host, 1.2)
        send_frame(guest, MSG_VEHICLE_INTERACTION, build_intent(seq=501))
        send_frame(guest, MSG_VEHICLE_INTERACTION, build_intent(seq=502))
        got = count_type(host, host_reader, MSG_VEHICLE_INTERACTION, 1.0)
        self.check(got == 2, f"0x23 window resets (got {got}/2)")
        # 0x24: host -> guest, cap 20/s.
        self._host_alive(host, 1.1)
        for _ in range(RATE_RESULT * 2):
            send_frame(host, MSG_VEHICLE_RESULT, build_result(seq=600))
        got = count_type(guest, guest_reader, MSG_VEHICLE_RESULT, 1.2)
        self.check(got == RATE_RESULT,
                   f"0x24 rate limit {RATE_RESULT}/s (got {got})")
        # 0x26: host -> guest, cap 10/s.
        self._host_alive(host, 1.1)
        for _ in range(RATE_STATE * 2):
            send_frame(host, MSG_VEHICLE_STATE, build_state(seq=700))
        got = count_type(guest, guest_reader, MSG_VEHICLE_STATE, 1.2)
        self.check(got == RATE_STATE,
                   f"0x26 rate limit {RATE_STATE}/s (got {got})")

    def phase_migration(self, host, host_reader, guest, guest_reader):
        self.log.write("[phase] host migration: relay goes to the promoted host")
        # Host leaves; the guest must be promoted.
        send_leave(host)
        host.close()
        changed = None
        end = time.monotonic() + 4.0
        while time.monotonic() < end and changed is None:
            for msg_type, payload in recv_available(guest, guest_reader,
                                                    wait=0.05):
                if msg_type == MSG_HOST_CHANGED and payload and payload[0] == 1:
                    changed = payload
        self.check(changed is not None, "guest promoted to HOST after Leave")
        send_ping(guest)  # promoted host answers Pong
        self.check(recv_type(guest, guest_reader, MSG_PONG, 2.0) is not None,
                   "promoted host stays connected (Pong)")
        # A new guest joins and its intent relays to the promoted host.
        new_guest = connect(self.args.host, self.args.port)
        _sid, _c, is_host, new_reader = join_session(
            new_guest, self.args.new_guest_name, self.log, "fakenewguest")
        self.check(not is_host, "new peer joined as guest after migration")
        recv_frames(guest, guest_reader, 0.2)
        intent = build_intent(epoch=10, net_id=88, seq=900)
        send_frame(new_guest, MSG_VEHICLE_INTERACTION, intent)
        got = recv_type(guest, guest_reader, MSG_VEHICLE_INTERACTION, 2.0)
        self.check(got == intent,
                   "0x23 relays to the promoted host after migration")
        result = build_result(epoch=10, net_id=88, seq=900, result=0)
        state = build_state(epoch=10, net_id=88, seq=1)
        send_frame(guest, MSG_VEHICLE_RESULT, result)
        send_frame(guest, MSG_VEHICLE_STATE, state)
        self.check(recv_type(new_guest, new_reader, MSG_VEHICLE_RESULT, 2.0)
                   == result, "0x24 relays from promoted host to new guest")
        self.check(recv_type(new_guest, new_reader, MSG_VEHICLE_STATE, 2.0)
                   == state, "0x26 relays from promoted host to new guest")
        new_guest.close()

    # -- driver ----------------------------------------------------------
    def run(self) -> int:
        self.log.write("=" * 70)
        self.log.write(f"fake_vehicle_client: mode=scenario "
                       f"server={self.args.host}:{self.args.port} "
                       f"protocol=v{PROTOCOL_VERSION}")
        host = host_reader = guest = guest_reader = None
        try:
            host, host_reader, guest, guest_reader = self.phase_lone_host()
            self.phase_valid_relay(host, host_reader, guest, guest_reader)
            self.phase_action_matrix(host, host_reader, guest, guest_reader)
            self.phase_invalid_drops(host, host_reader, guest, guest_reader)
            self.phase_host_only_relay(host, host_reader, guest, guest_reader)
            self.phase_invalid_host_messages(host, host_reader, guest,
                                            guest_reader)
            self.phase_rate_limits(host, host_reader, guest, guest_reader)
            self.phase_migration(host, host_reader, guest, guest_reader)
        except (OSError, ProtocolError, TimeoutError, RuntimeError) as exc:
            self.failed += 1
            self.failures.append(f"exception: {exc}")
            self.log.write(f"  FAIL exception: {exc}")
        finally:
            for sock in (host, guest):
                if sock is not None:
                    try:
                        send_leave(sock)
                        sock.close()
                    except OSError:
                        pass
        self.log.write(f"fakevehicle scenario: {self.passed} passed, "
                       f"{self.failed} failed")
        if self.failures:
            self.log.write("failures:")
            for failure in self.failures:
                self.log.write(f"  - {failure}")
            return 2
        self.log.write("fakevehicle scenario: PASS")
        return 0


# ---------------------------------------------------------------------------
# Manual single-PC modes (real game on the other side).
# ---------------------------------------------------------------------------
class FakeHost:
    """Fake host: accept 0x23 intents, answer 0x24 Applied + 0x26 state."""

    def __init__(self, args, log: Log):
        self.args = args
        self.log = log
        self.intents = 0
        self.invalid = 0
        self.replies = 0
        self.rate_window_start = 0.0
        self.rate_window_count = 0
        self.rate_violations = 0
        self.vehicles = set()
        self.last_state_sent = 0.0
        self.last_summary = time.monotonic()

    def _reply(self, sock, payload: bytes, result: int = 0, detail: int = 0):
        m = parse_intent(payload)
        send_frame(sock, MSG_VEHICLE_RESULT,
                   build_result(epoch=m["epoch"], net_id=m["net_id"],
                                seq=m["seq"], action=m["action"],
                                result=result, detail=detail))
        state = build_state(
            epoch=m["epoch"], net_id=m["net_id"], body_health=500,
            engine_health=500, door_bits=0x0001, seq=1,
            flags=STATE_FLAG_DOOR | STATE_FLAG_DAMAGE,
            host_seat=SEAT_NONE, guest_seat=SEAT_NONE)
        send_frame(sock, MSG_VEHICLE_STATE, state)
        self.replies += 1
        self.log.write(f"fakehost: intent action={m['action_name']} "
                       f"net={m['net_id']} seq={m['seq']} -> result=applied")

    def _send_heartbeat(self, sock):
        # 0x24/0x26 are host game-thread messages: a 1 Hz VehicleState
        # heartbeat keeps the server from applying the host game-timeout.
        for net_id in sorted(self.vehicles):
            send_frame(sock, MSG_VEHICLE_STATE, build_state(
                epoch=self.args.epoch, net_id=net_id, body_health=500,
                engine_health=500, door_bits=0x0001, seq=1,
                flags=STATE_FLAG_DOOR, host_seat=SEAT_NONE,
                guest_seat=SEAT_NONE))

    def run(self) -> int:
        self.log.write("=" * 70)
        self.log.write(f"fakehost: mode=host name={self.args.host_name} "
                       f"server={self.args.host}:{self.args.port} "
                       f"duration={self.args.duration}s protocol=v{PROTOCOL_VERSION}")
        sock = connect(self.args.host, self.args.port)
        _sid, _c, is_host, reader = join_session(sock, self.args.host_name,
                                                 self.log, "fakehost")
        if not is_host:
            self.log.write("fakehost: FAIL first JOIN is not host "
                           "(guest/name conflict?)")
            return 1
        start = time.monotonic()
        end = start + self.args.duration
        try:
            while time.monotonic() < end:
                for msg_type, payload in recv_available(sock, reader, wait=0.05):
                    if msg_type == MSG_VEHICLE_INTERACTION:
                        ok, why = validate_intent(payload)
                        now = time.monotonic()
                        if now - self.rate_window_start >= 1.0:
                            self.rate_window_start = now
                            self.rate_window_count = 0
                        self.rate_window_count += 1
                        if self.rate_window_count > RATE_INTERACTION:
                            self.rate_violations += 1
                        if not ok:
                            self.invalid += 1
                            self.log.write(f"fakehost: invalid intent ({why})")
                            continue
                        self.intents += 1
                        m = parse_intent(payload)
                        self.vehicles.add(m["net_id"])
                        self._reply(sock, payload)
                    elif msg_type == MSG_PING:
                        send_frame(sock, MSG_PONG, payload)
                    elif msg_type == MSG_HOST_CHANGED:
                        self.log.write("fakehost: WARN host changed "
                                       "(lost host role)")
                        return 2
                now = time.monotonic()
                if now - self.last_state_sent >= 1.0:
                    self.last_state_sent = now
                    self._send_heartbeat(sock)
                if now - self.last_summary >= 5.0:
                    self.last_summary = now
                    self.log.write(f"fakehost: intents={self.intents} "
                                   f"invalid={self.invalid} "
                                   f"replies={self.replies}")
        except (OSError, ProtocolError) as exc:
            self.log.write(f"fakehost: connection error: {exc}")
            return 1
        finally:
            try:
                send_leave(sock)
                sock.close()
            except OSError:
                pass
        self.log.write(f"fakehost: summary intents={self.intents} "
                       f"invalid={self.invalid} replies={self.replies} "
                       f"rate_violations={self.rate_violations}")
        if self.invalid or self.rate_violations:
            self.log.write("fakehost: FAIL invalid/rate-violating intents "
                           "reached the host (server relay regression?)")
            return 2
        if self.intents < 1:
            self.log.write("fakehost: FAIL no valid 0x23 intent received")
            return 2
        self.log.write("fakehost: PASS")
        return 0


class FakeGuest:
    """Fake guest: send 0x23 intents, validate 0x24/0x26 from the host."""

    def __init__(self, args, log: Log):
        self.args = args
        self.log = log
        self.results = 0
        self.states = 0
        self.bad_results = 0
        self.bad_states = 0
        self.sent = 0
        self.last_summary = time.monotonic()

    def _send_sequence(self, sock):
        seq = 1
        cases = [
            dict(action="damage", door=PARTS["wheel_fl"], param=60),
            dict(action="damage", door=PARTS["body"], param=100),
            dict(action="door_open", door=1),
            dict(action="door_close", door=1),
            dict(action="enter", seat=0, door=DOOR_NONE, weapon_hash=0),
            dict(action="exit", seat=SEAT_NONE, door=DOOR_NONE,
                 weapon_hash=0),
        ]
        for kwargs in cases:
            payload = build_intent(epoch=self.args.epoch,
                                   net_id=self.args.vehicle_net_id, seq=seq,
                                   **kwargs)
            send_frame(sock, MSG_VEHICLE_INTERACTION, payload)
            self.sent += 1
            action_name = ACTION_NAMES.get(ACTIONS[kwargs["action"]],
                                           kwargs["action"])
            self.log.write(f"fakeguest: sent action={action_name} "
                           f"net={self.args.vehicle_net_id} seq={seq}")
            seq += 1
            time.sleep(0.2)

    def run(self) -> int:
        self.log.write("=" * 70)
        self.log.write(f"fakeguest: mode=guest name={self.args.guest_name} "
                       f"server={self.args.host}:{self.args.port} "
                       f"duration={self.args.duration}s "
                       f"vehicle_net_id={self.args.vehicle_net_id} "
                       f"protocol=v{PROTOCOL_VERSION}")
        sock = connect(self.args.host, self.args.port)
        _sid, _c, is_host, reader = join_session(sock, self.args.guest_name,
                                                 self.log, "fakeguest")
        if is_host:
            self.log.write("fakeguest: FAIL joined as host (join the host "
                           "first / wait for a real host)")
            return 1
        start = time.monotonic()
        end = start + self.args.duration
        sent_sequence = False
        try:
            while time.monotonic() < end:
                if not sent_sequence and time.monotonic() - start >= 1.0:
                    sent_sequence = True
                    self._send_sequence(sock)
                for msg_type, payload in recv_available(sock, reader, wait=0.05):
                    if msg_type == MSG_VEHICLE_RESULT:
                        ok, why = validate_result(payload)
                        if not ok:
                            self.bad_results += 1
                            self.log.write(f"fakeguest: bad result: {why}")
                            continue
                        m = parse_result(payload)
                        self.results += 1
                        self.log.write(f"fakeguest: result seq={m['seq']} "
                                       f"action={m['action_name']} "
                                       f"result={m['result_name']}")
                    elif msg_type == MSG_VEHICLE_STATE:
                        ok, why = validate_state(payload)
                        if not ok:
                            self.bad_states += 1
                            self.log.write(f"fakeguest: bad state: {why}")
                            continue
                        self.states += 1
                        if self.states <= 3:
                            m = parse_state(payload)
                            self.log.write(f"fakeguest: state net={m['net_id']}"
                                           f" body={m['body_health']} "
                                           f"engine={m['engine_health']} "
                                           f"doors=0x{m['door_bits']:04X}")
                    elif msg_type == MSG_PING:
                        send_frame(sock, MSG_PONG, payload)
                now = time.monotonic()
                if now - self.last_summary >= 5.0:
                    self.last_summary = now
                    self.log.write(f"fakeguest: sent={self.sent} "
                                   f"results={self.results} "
                                   f"states={self.states}")
        except (OSError, ProtocolError) as exc:
            self.log.write(f"fakeguest: connection error: {exc}")
            return 1
        finally:
            try:
                send_leave(sock)
                sock.close()
            except OSError:
                pass
        self.log.write(f"fakeguest: summary sent={self.sent} "
                       f"results={self.results} states={self.states} "
                       f"bad_results={self.bad_results} "
                       f"bad_states={self.bad_states}")
        if self.bad_results or self.bad_states:
            self.log.write("fakeguest: FAIL host sent malformed 0x24/0x26")
            return 2
        if self.results < 1 or self.states < 1:
            self.log.write("fakeguest: FAIL no 0x24/0x26 observed from host "
                           "(is host apply U3 enabled?)")
            return 2
        self.log.write("fakeguest: PASS")
        return 0


# ---------------------------------------------------------------------------
# CLI / main.
# ---------------------------------------------------------------------------
def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        description="Fake v20 P1 vehicle-interaction player (0x23/0x24/0x26).",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter)
    ap.add_argument("--mode", choices=("scenario", "host", "guest"),
                    default="scenario",
                    help="scenario = both fake players + assertions; host/guest "
                         "= manual single-PC with the real game")
    ap.add_argument("--host", default="127.0.0.1", help="relay server address")
    ap.add_argument("--port", type=int, default=7777, help="relay server port")
    ap.add_argument("--host-name", default="V20Host", help="host player name")
    ap.add_argument("--guest-name", default="V20Guest",
                    help="guest player name")
    ap.add_argument("--new-guest-name", default="V20NewGuest",
                    help="new guest name after migration (scenario)")
    ap.add_argument("--duration", type=int, default=120,
                    help="host/guest mode duration, seconds")
    ap.add_argument("--vehicle-net-id", type=int, default=42,
                    help="guest mode: vehicleNetId sent in 0x23")
    ap.add_argument("--epoch", type=int, default=7,
                    help="host/guest mode: epoch used in payloads")
    ap.add_argument("--json", action="store_true",
                    help="print a JSON summary line at the end")
    ap.add_argument("--log-file", default=str(DEFAULT_LOG),
                    help="append log file (empty string disables)")
    return ap


def main(argv=None) -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, OSError):
        pass
    args = build_parser().parse_args(argv)
    log = Log(Path(args.log_file)) if args.log_file else Log(None)
    try:
        if args.mode == "scenario":
            rc = Scenario(args, log).run()
        elif args.mode == "host":
            rc = FakeHost(args, log).run()
        else:
            rc = FakeGuest(args, log).run()
        if args.json:
            summary = {"mode": args.mode, "exit_code": rc,
                       "protocol": PROTOCOL_VERSION,
                       "rate_limits": {"interaction": RATE_INTERACTION,
                                       "result": RATE_RESULT,
                                       "state": RATE_STATE}}
            print(json.dumps(summary, ensure_ascii=False), flush=True)
        return rc
    except KeyboardInterrupt:
        log.write(f"fakevehicle {args.mode}: Ctrl+C")
        return 0
    except (OSError, RuntimeError, TimeoutError) as exc:
        log.write(f"fakevehicle: error: {exc}")
        return 1
    finally:
        log.close()


if __name__ == "__main__":
    sys.exit(main())
