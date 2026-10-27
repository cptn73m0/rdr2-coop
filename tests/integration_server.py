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


class IntegrationRunLock:
    """Cross-process lock for the whole integration run.

    INVARIANT: only one integration runner may allocate ephemeral ports and
    start servers at a time. _pick_port() closes its probe socket before the
    child server binds; without this lock two parallel runs can race for the
    same port / hit WinError 10054 during JOIN. The OS releases the lock when
    the process exits, so a crashed runner cannot leave a stale lock.
    """

    def __init__(self):
        self.path = Path(tempfile.gettempdir()) / "rdr2coop-integration.lock"
        self._fh = None
        self._locked = False

    def acquire(self):
        self._fh = open(self.path, "a+b")
        self._fh.seek(0, os.SEEK_END)
        if self._fh.tell() == 0:
            self._fh.write(b"\0")
            self._fh.flush()
        self._fh.seek(0)
        waiting = False
        while True:
            try:
                if os.name == "nt":
                    import msvcrt
                    msvcrt.locking(self._fh.fileno(), msvcrt.LK_NBLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(self._fh.fileno(),
                                fcntl.LOCK_EX | fcntl.LOCK_NB)
                self._locked = True
                return self
            except OSError:
                if not waiting:
                    log("integration: another runner is active; waiting for "
                        "the serialized lock")
                    waiting = True
                time.sleep(0.2)

    def release(self):
        if self._fh is None:
            return
        try:
            if self._locked:
                if os.name == "nt":
                    import msvcrt
                    msvcrt.locking(self._fh.fileno(), msvcrt.LK_UNLCK, 1)
                else:
                    import fcntl
                    fcntl.flock(self._fh.fileno(), fcntl.LOCK_UN)
        finally:
            self._fh.close()
            self._fh = None
            self._locked = False


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


def _proto_const_optional(name):
    m = re.search(re.escape(name) + r"\s*=\s*(\d+)\s*;", _PROTO_SRC)
    return int(m.group(1)) if m else None


def _struct_size_optional(name):
    m = re.search(r"static_assert\s*\(\s*sizeof\s*\(\s*" + re.escape(name) +
                  r"\s*\)\s*==\s*(\d+)", _PROTO_SRC)
    return int(m.group(1)) if m else None


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
# Protocol v14 contract: OutfitState can no longer shrink back to the v13 400 B
# layout (model u64 + count u32 + components[96] u32); v14 adds wearable[96]
# u32 for wearable states => 784 B. A deliberate future bump must update this
# pinned value together with net/protocol.h and docs/PROTOCOL.md.
EXPECTED_OUTFIT_SIZE = 784
OUTFIT_SIZE = _struct_size("OutfitState")
require(OUTFIT_SIZE == EXPECTED_OUTFIT_SIZE,
        f"OutfitState expected {EXPECTED_OUTFIT_SIZE} B (protocol v14: "
        f"components[96]+wearable[96]), got {OUTFIT_SIZE}")
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

# --------------------------------------------------------------------------
# Protocol v15 (E0) + v16 (E2c-b) + v17 (E5) + v18 (E6) + v19 (E3d-b/c):
# actor replication, presence and traffic messages 0x18..0x22. The v15 wire
# format is signed off in the net-review E0 task; v16 ActorAppearance / v17
# ActorDamage / v18 PlayerPresence / v19 VehicleOccupants+TrainSnapshotV2
# extensions are signed off in the E2c-b / E5 / E6 / E3d tasks. These tests are
# skipped while net/protocol.h is still v14 (E0 not merged) and become strict
# automatically once the header carries the messages. Expected values below are
# pinned to the spec: spawn 64 B, frame 32 B, max 12 actors, batch sizes
# 776/392 B, despawn/resync 12 B, event 32 B, outfit 792 B, appearance header
# 12 B + entry 28 B (max 12 + 16*28 = 460 B, 16 компонентов), damage 16 B,
# player presence 8 B, vehicle occupants header 8 B + entry 8 B (max 72 B),
# train snapshot v2 48 B.
# --------------------------------------------------------------------------
_ACTOR_MSG_PINS = {
    "ActorSpawnBatch": 0x18,
    "ActorFrames": 0x19,
    "ActorDespawn": 0x1A,
    "ActorEvent": 0x1B,
    "ActorOutfit": 0x1C,
    "ActorResync": 0x1D,
}
_ACTOR_SIZE_PINS = {
    "ActorSpawn": 64,
    "ActorFrame": 32,
}

ACTOR_AVAILABLE = False
TYPE_ACTOR_SPAWN_BATCH = None
TYPE_ACTOR_FRAMES = None
TYPE_ACTOR_DESPAWN = None
TYPE_ACTOR_EVENT = None
TYPE_ACTOR_OUTFIT = None
TYPE_ACTOR_RESYNC = None
ACTOR_SPAWN_SIZE = 0
ACTOR_FRAME_SIZE = 0
MAX_ACTORS = 0
ACTOR_SPAWN_BATCH_MAX = 0
ACTOR_FRAMES_MAX = 0
ACTOR_DESPAWN_SIZE = 0
ACTOR_EVENT_SIZE = 0
ACTOR_OUTFIT_SIZE = 0
ACTOR_RESYNC_SIZE = 0

if PROTO >= 15:
    _missing_actor_msg = [name for name in _ACTOR_MSG_PINS if name not in MSG]
    _missing_actor_size = [name for name in _ACTOR_SIZE_PINS
                           if _struct_size_optional(name) is None]
    _max_actors = _proto_const_optional("kMaxActors")
    if _missing_actor_msg or _missing_actor_size or _max_actors is None:
        raise RuntimeError(
            "protocol v15 is present but E0 actor spec is incomplete: "
            f"missing msgs={_missing_actor_msg}, "
            f"missing sizes={_missing_actor_size}, kMaxActors={_max_actors}")

    TYPE_ACTOR_SPAWN_BATCH = MSG["ActorSpawnBatch"]
    TYPE_ACTOR_FRAMES = MSG["ActorFrames"]
    TYPE_ACTOR_DESPAWN = MSG["ActorDespawn"]
    TYPE_ACTOR_EVENT = MSG["ActorEvent"]
    TYPE_ACTOR_OUTFIT = MSG["ActorOutfit"]
    TYPE_ACTOR_RESYNC = MSG["ActorResync"]
    ACTOR_SPAWN_SIZE = _struct_size("ActorSpawn")
    ACTOR_FRAME_SIZE = _struct_size("ActorFrame")
    MAX_ACTORS = _max_actors
    ACTOR_SPAWN_BATCH_MAX = 8 + MAX_ACTORS * ACTOR_SPAWN_SIZE
    ACTOR_FRAMES_MAX = 8 + MAX_ACTORS * ACTOR_FRAME_SIZE
    # Despawn/Resync: epoch u32 + actorId/lastSeq u32 + reason u8 + pad3.
    ACTOR_DESPAWN_SIZE = 12
    ACTOR_EVENT_SIZE = 32
    ACTOR_OUTFIT_SIZE = 8 + OUTFIT_SIZE
    ACTOR_RESYNC_SIZE = 12
    for _name, _expected_id in _ACTOR_MSG_PINS.items():
        require(MSG.get(_name) == _expected_id,
                f"Msg::{_name} expected 0x{_expected_id:02X}, "
                f"got {MSG.get(_name)} (protocol v15 E0 spec changed)")
    for _name, _expected_size in _ACTOR_SIZE_PINS.items():
        _actual = _struct_size(_name)
        require(_actual == _expected_size,
                f"{_name} expected {_expected_size} B (v15 E0), got {_actual}")
    require(MAX_ACTORS == 12, f"kMaxActors expected 12, got {MAX_ACTORS}")
    require(ACTOR_SPAWN_BATCH_MAX == 776,
            f"ActorSpawnBatch max expected 776 B, got {ACTOR_SPAWN_BATCH_MAX}")
    require(ACTOR_FRAMES_MAX == 392,
            f"ActorFrames max expected 392 B, got {ACTOR_FRAMES_MAX}")
    require(ACTOR_DESPAWN_SIZE == 12, "ActorDespawn expected 12 B")
    require(ACTOR_EVENT_SIZE == 32, "ActorEvent expected 32 B")
    require(ACTOR_OUTFIT_SIZE == 8 + EXPECTED_OUTFIT_SIZE,
            f"ActorOutfit expected {8 + EXPECTED_OUTFIT_SIZE} B, "
            f"got {ACTOR_OUTFIT_SIZE}")
    require(ACTOR_RESYNC_SIZE == 12, "ActorResync expected 12 B")
    ACTOR_AVAILABLE = True

# --------------------------------------------------------------------------
# Protocol v16 (E2c-b): ActorAppearance (0x1E) — точная внешность NPC.
# header 12 B (epoch u32, actorId u32, count u8, pad[3]) + count × 28 B
# (drawable/albedo/normal/material/palette u32, tint0/1/2 i16, reserved u16);
# count 1..kActorAppearanceMaxComponents (16), максимум 12 + 16*28 = 460 B.
# Только host -> guest.
# --------------------------------------------------------------------------
APPEARANCE_AVAILABLE = False
TYPE_ACTOR_APPEARANCE = None
ACTOR_APPEARANCE_ENTRY_SIZE = 0
ACTOR_APPEARANCE_HEADER_SIZE = 0
ACTOR_APPEARANCE_MAX = 0
MAX_ACTOR_APPEARANCE = 0
if PROTO >= 16:
    _missing_appearance = [name for name in ("ActorAppearance",) if name not in MSG]
    for _name in ("ActorAppearanceEntry", "ActorAppearanceHeader"):
        if _struct_size_optional(_name) is None:
            _missing_appearance.append(_name)
    _max_components_appearance = _proto_const_optional(
        "kActorAppearanceMaxComponents")
    if _missing_appearance or _max_components_appearance is None:
        raise RuntimeError(
            "protocol v16 is present but ActorAppearance spec is incomplete: "
            f"missing={_missing_appearance}, "
            f"kActorAppearanceMaxComponents={_max_components_appearance}")
    require(MSG["ActorAppearance"] == 0x1E,
            f"Msg::ActorAppearance expected 0x1E, got {MSG['ActorAppearance']}")
    require(_struct_size("ActorAppearanceEntry") == 28,
            "ActorAppearanceEntry expected 28 B (v16 spec)")
    require(_struct_size("ActorAppearanceHeader") == 12,
            "ActorAppearanceHeader expected 12 B (v16 spec)")
    MAX_ACTOR_APPEARANCE = _max_components_appearance
    require(MAX_ACTOR_APPEARANCE == 16,
            f"kActorAppearanceMaxComponents expected 16, got "
            f"{MAX_ACTOR_APPEARANCE}")
    ACTOR_APPEARANCE_MAX = 12 + MAX_ACTOR_APPEARANCE * 28
    require(ACTOR_APPEARANCE_MAX == 460,
            f"ActorAppearance max expected 460 B, got {ACTOR_APPEARANCE_MAX}")
    TYPE_ACTOR_APPEARANCE = MSG["ActorAppearance"]
    ACTOR_APPEARANCE_ENTRY_SIZE = 28
    ACTOR_APPEARANCE_HEADER_SIZE = 12
    APPEARANCE_AVAILABLE = True

# Names of the conditional v15 actor tests (skipped while E0 is not merged).
ACTOR_TESTS = {
    "test_actor_spawn_host_only",
    "test_actor_frames_host_only",
    "test_actor_despawn_event_outfit",
    "test_actor_resync_direction",
    "test_actor_validation_drops",
    "test_actor_duplicate_actor_ids",
}

# Names of the conditional v16 ActorAppearance tests (skipped while v16 missing).
APPEARANCE_TESTS = {
    "test_actor_appearance_host_only",
    "test_actor_appearance_validation",
}

# --------------------------------------------------------------------------
# Protocol v17 (E5): ActorDamage (0x1F) — запрос урона от гостя хосту.
# payload 16 B: epoch u32, actorId u32, damage u16 (1..1000), pad u16,
# weaponHash u32. Направление только guest -> server -> host.
# --------------------------------------------------------------------------
DAMAGE_AVAILABLE = False
TYPE_ACTOR_DAMAGE = None
ACTOR_DAMAGE_SIZE = 0
MIN_ACTOR_DAMAGE = 0
MAX_ACTOR_DAMAGE = 0
if PROTO >= 17:
    _damage_size = _struct_size_optional("ActorDamagePayload")
    if "ActorDamage" not in MSG or _damage_size is None:
        raise RuntimeError(
            "protocol v17 is present but ActorDamage spec is incomplete: "
            f"msg={'ActorDamage' in MSG}, size={_damage_size}")
    require(MSG["ActorDamage"] == 0x1F,
            f"Msg::ActorDamage expected 0x1F, got {MSG['ActorDamage']}")
    require(_damage_size == 16, "ActorDamagePayload expected 16 B (v17 spec)")
    MIN_ACTOR_DAMAGE = _proto_const("kMinActorDamage")
    MAX_ACTOR_DAMAGE = _proto_const("kMaxActorDamage")
    require(MIN_ACTOR_DAMAGE == 1,
            f"kMinActorDamage expected 1, got {MIN_ACTOR_DAMAGE}")
    require(MAX_ACTOR_DAMAGE == 1000,
            f"kMaxActorDamage expected 1000, got {MAX_ACTOR_DAMAGE}")
    TYPE_ACTOR_DAMAGE = MSG["ActorDamage"]
    ACTOR_DAMAGE_SIZE = 16
    DAMAGE_AVAILABLE = True

# Names of the conditional v17 ActorDamage tests (skipped while v17 missing).
DAMAGE_TESTS = {
    "test_actor_damage_direction",
    "test_actor_damage_validation",
}

# --------------------------------------------------------------------------
# Protocol v18 (E6): PlayerPresence (0x20) — UI-статус игрока.
# payload 8 B: epoch u32, flags u16, pad u16; flags bitmask (1=in_map,
# 2=in_pause_menu, 4=in_satchel, 8=in_shop, 16=in_cutscene, 32=in_menu_other;
# 0 = нет статуса). Направление: любой клиент -> сервер -> ДРУГОМУ клиенту.
# --------------------------------------------------------------------------
PRESENCE_AVAILABLE = False
TYPE_PLAYER_PRESENCE = None
PLAYER_PRESENCE_SIZE = 0
PLAYER_PRESENCE_KNOWN_FLAGS = 0x3F  # pinned: bits 0..5 (v18 spec)
if PROTO >= 18:
    _presence_size = _struct_size_optional("PlayerPresencePayload")
    if "PlayerPresence" not in MSG or _presence_size is None:
        raise RuntimeError(
            "protocol v18 is present but PlayerPresence spec is incomplete: "
            f"msg={'PlayerPresence' in MSG}, size={_presence_size}")
    require(MSG["PlayerPresence"] == 0x20,
            f"Msg::PlayerPresence expected 0x20, got {MSG['PlayerPresence']}")
    require(_presence_size == 8,
            "PlayerPresencePayload expected 8 B (v18 spec)")
    TYPE_PLAYER_PRESENCE = MSG["PlayerPresence"]
    PLAYER_PRESENCE_SIZE = 8
    PRESENCE_AVAILABLE = True

# Names of the conditional v18 PlayerPresence tests (skipped while v18 missing).
PRESENCE_TESTS = {
    "test_player_presence_relay",
    "test_player_presence_validation",
}

# --------------------------------------------------------------------------
# Protocol v19 (E3d-b/E3d-c): VehicleOccupants (0x21) + TrainSnapshotV2 (0x22).
# 0x21: header 8 B {vehicleNetId u32, count u8, pad[3]} + count*8 B entries
# {actorId u32, seat i8, flags u8, carIndex u8, harness u8}; count 0..8,
# max payload 8+8*8 = 72 B; host-only relay, byte-identical.
# 0x22: 48 B {epoch,trainId,configHash u32; flags/stationIndex/carCount/pad u8;
# speed,x,y,z,heading f32; trackIndex u32; pad2[8]}; host-only relay.
# --------------------------------------------------------------------------
TRAFFIC_AVAILABLE = False
TYPE_VEHICLE_OCCUPANTS = None
TYPE_TRAIN_V2 = None
OCCUPANTS_HEADER_SIZE = 0
OCCUPANT_ENTRY_SIZE = 0
OCCUPANTS_MAX = 0
MAX_OCCUPANTS = 0
TRAIN_V2_SIZE = 0
if PROTO >= 19:
    _missing_traffic = [name for name in ("VehicleOccupants", "TrainSnapshotV2")
                        if name not in MSG]
    for _name in ("VehicleOccupantsHeader", "VehicleOccupantEntry",
                  "TrainSnapshotV2Payload"):
        if _struct_size_optional(_name) is None:
            _missing_traffic.append(_name)
    _max_occ = _proto_const_optional("kMaxVehicleOccupants")
    if _missing_traffic or _max_occ is None:
        raise RuntimeError(
            "protocol v19 is present but E3d traffic spec is incomplete: "
            f"missing={_missing_traffic}, kMaxVehicleOccupants={_max_occ}")
    require(MSG["VehicleOccupants"] == 0x21,
            f"Msg::VehicleOccupants expected 0x21, "
            f"got {MSG['VehicleOccupants']}")
    require(MSG["TrainSnapshotV2"] == 0x22,
            f"Msg::TrainSnapshotV2 expected 0x22, "
            f"got {MSG['TrainSnapshotV2']}")
    require(_struct_size("VehicleOccupantsHeader") == 8,
            "VehicleOccupantsHeader expected 8 B (v19 spec)")
    require(_struct_size("VehicleOccupantEntry") == 8,
            "VehicleOccupantEntry expected 8 B (v19 spec)")
    require(_struct_size("TrainSnapshotV2Payload") == 48,
            "TrainSnapshotV2Payload expected 48 B (v19 spec)")
    MAX_OCCUPANTS = _max_occ
    require(MAX_OCCUPANTS == 8,
            f"kMaxVehicleOccupants expected 8, got {MAX_OCCUPANTS}")
    OCCUPANTS_MAX = 8 + MAX_OCCUPANTS * 8
    require(OCCUPANTS_MAX == 72,
            f"VehicleOccupants max expected 72 B, got {OCCUPANTS_MAX}")
    OCCUPANTS_HEADER_SIZE = 8
    OCCUPANT_ENTRY_SIZE = 8
    TRAIN_V2_SIZE = 48
    TYPE_VEHICLE_OCCUPANTS = MSG["VehicleOccupants"]
    TYPE_TRAIN_V2 = MSG["TrainSnapshotV2"]
    TRAFFIC_AVAILABLE = True

# Names of the conditional v19 E3d traffic tests (skipped while v19 missing).
TRAFFIC_TESTS = {
    "test_vehicle_occupants_host_only",
    "test_vehicle_occupants_validation",
    "test_vehicle_occupants_rate_limit",
    "test_train_v2_host_only",
    "test_train_v2_validation",
    "test_train_v2_rate_limit",
}

# --------------------------------------------------------------------------
# Protocol v20 P1: VehicleInteraction (0x23) / Result (0x24) / State (0x26).
# 0x23: 24 B {epoch u32, vehicleNetId u32, targetId u32, seq u16, action u8,
# seat i8, door u8, flags u8, param u16, weaponHash u32}; guest -> host unicast.
# 0x24: 16 B; 0x26: 24 B; host -> guest. 0x25/0x27 reserved (P3).
# --------------------------------------------------------------------------
VEHICLE_AVAILABLE = False
TYPE_VEHICLE_INTERACTION = None
TYPE_VEHICLE_INTERACTION_RESULT = None
TYPE_VEHICLE_STATE = None
VEHICLE_INTERACTION_SIZE = 0
VEHICLE_RESULT_SIZE = 0
VEHICLE_STATE_SIZE = 0
if PROTO >= 20:
    _missing_veh = [name for name in
                    ("VehicleInteraction", "VehicleInteractionResult",
                     "VehicleState", "VehicleControlInput",
                     "VehicleControlState")
                    if name not in MSG]
    for _name in ("VehicleInteractionPayload",
                  "VehicleInteractionResultPayload", "VehicleStatePayload"):
        if _struct_size_optional(_name) is None:
            _missing_veh.append(_name)
    if _missing_veh:
        raise RuntimeError(
            "protocol v20 is present but P1 vehicle spec is incomplete: "
            f"missing={_missing_veh}")
    require(MSG["VehicleInteraction"] == 0x23,
            f"Msg::VehicleInteraction expected 0x23, "
            f"got {MSG['VehicleInteraction']}")
    require(MSG["VehicleInteractionResult"] == 0x24,
            f"Msg::VehicleInteractionResult expected 0x24, "
            f"got {MSG['VehicleInteractionResult']}")
    require(MSG["VehicleControlInput"] == 0x25, "reserved 0x25")
    require(MSG["VehicleState"] == 0x26,
            f"Msg::VehicleState expected 0x26, got {MSG['VehicleState']}")
    require(MSG["VehicleControlState"] == 0x27, "reserved 0x27")
    require(_struct_size("VehicleInteractionPayload") == 24,
            "VehicleInteractionPayload expected 24 B (v20 spec)")
    require(_struct_size("VehicleInteractionResultPayload") == 16,
            "VehicleInteractionResultPayload expected 16 B (v20 spec)")
    require(_struct_size("VehicleStatePayload") == 24,
            "VehicleStatePayload expected 24 B (v20 spec)")
    TYPE_VEHICLE_INTERACTION = MSG["VehicleInteraction"]
    TYPE_VEHICLE_INTERACTION_RESULT = MSG["VehicleInteractionResult"]
    TYPE_VEHICLE_STATE = MSG["VehicleState"]
    VEHICLE_INTERACTION_SIZE = 24
    VEHICLE_RESULT_SIZE = 16
    VEHICLE_STATE_SIZE = 24
    VEHICLE_AVAILABLE = True

# Pinned v20 P1 server rate limits (D3): 8/20/10 per second.
VEHICLE_RATE_INTERACTION = 8
VEHICLE_RATE_RESULT = 20
VEHICLE_RATE_STATE = 10

VEHICLE_TESTS = {
    "test_vehicle_interaction_guest_only_relay",
    "test_vehicle_interaction_validation",
    "test_vehicle_interaction_rate_limit",
    "test_vehicle_result_host_only_relay",
    "test_vehicle_result_validation_rate",
    "test_vehicle_state_host_only_relay",
    "test_vehicle_interaction_no_host_drop",
    "test_vehicle_interaction_after_migration",
}

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


# ---- Protocol v15 E0 actor payload builders ---------------------------------
# Layouts are pinned to the E0 spec (see docs/PROTOCOL.md, section "v15 E0").
# Builders are only called from tests guarded by ACTOR_AVAILABLE.

def actor_spawn(actor_id=1, epoch=1, kind=0, flags=0, model=0xCF7E73BE,
                meta_outfit=0, mount_actor_id=0, weapon_hash=0,
                scenario_hash=0, x=1.0, y=2.0, z=3.0, heading=90.0,
                health=200, move_style=0, phase=0, pad=0,
                vx=0, vy=0, vz=0, pad2=0, reserved=0, reserved2=0):
    require(ACTOR_AVAILABLE, "actor v15 messages are not in net/protocol.h yet")
    rec = struct.pack("<IHBBIIIIIffffBBBBhhhHII",
                      actor_id, epoch, kind, flags,
                      model, meta_outfit, mount_actor_id, weapon_hash,
                      scenario_hash,
                      x, y, z, heading,
                      health, move_style, phase, pad,
                      vx, vy, vz, pad2, reserved, reserved2)
    require(len(rec) == ACTOR_SPAWN_SIZE,
            f"ActorSpawn builder mismatch: {len(rec)} != {ACTOR_SPAWN_SIZE}")
    return rec


def actor_frame(actor_id=1, flags=0, anim_state=0, speed=0,
                x=1.0, y=2.0, z=3.0, heading=90.0,
                target_actor_id=0, seq=1, pad=0):
    require(ACTOR_AVAILABLE, "actor v15 messages are not in net/protocol.h yet")
    rec = struct.pack("<IHBBffffIHH", actor_id, flags, anim_state, speed,
                      x, y, z, heading, target_actor_id, seq, pad)
    require(len(rec) == ACTOR_FRAME_SIZE,
            f"ActorFrame builder mismatch: {len(rec)} != {ACTOR_FRAME_SIZE}")
    return rec


def actor_spawn_batch(epoch=1, records=None):
    require(ACTOR_AVAILABLE, "actor v15 messages are not in net/protocol.h yet")
    records = list(records or [])
    header = struct.pack("<IHH", epoch, len(records), 0)
    payload = header + b"".join(records)
    require(len(header) == 8, "ActorSpawnBatch header must be 8 B")
    return payload


def actor_frames(epoch=1, seq=1, records=None):
    require(ACTOR_AVAILABLE, "actor v15 messages are not in net/protocol.h yet")
    records = list(records or [])
    header = struct.pack("<IHH", epoch, seq, len(records))
    require(len(header) == 8, "ActorFrames header must be 8 B")
    return header + b"".join(records)


def actor_despawn(epoch=1, actor_id=1, reason=0):
    require(ACTOR_AVAILABLE, "actor v15 messages are not in net/protocol.h yet")
    payload = struct.pack("<IIB3x", epoch, actor_id, reason)
    require(len(payload) == ACTOR_DESPAWN_SIZE,
            "ActorDespawn builder mismatch")
    return payload


def actor_event(epoch=1, actor_id=1, ev_type=0, flags=0, pad=0,
                a=0, b=0, x=0, y=0, z=0):
    require(ACTOR_AVAILABLE, "actor v15 messages are not in net/protocol.h yet")
    payload = struct.pack("<IIBBHIIIII", epoch, actor_id, ev_type, flags,
                          pad, a, b, x, y, z)
    require(len(payload) == ACTOR_EVENT_SIZE, "ActorEvent builder mismatch")
    return payload


def actor_outfit(epoch=1, actor_id=1, outfit=None):
    require(ACTOR_AVAILABLE, "actor v15 messages are not in net/protocol.h yet")
    if outfit is None:
        outfit = b"\xA5" * OUTFIT_SIZE
    require(len(outfit) == OUTFIT_SIZE,
            f"OutfitState must be {OUTFIT_SIZE} B, got {len(outfit)}")
    payload = struct.pack("<II", epoch, actor_id) + outfit
    require(len(payload) == ACTOR_OUTFIT_SIZE, "ActorOutfit builder mismatch")
    return payload


def actor_resync(epoch=1, last_seq=0, reason=0):
    require(ACTOR_AVAILABLE, "actor v15 messages are not in net/protocol.h yet")
    payload = struct.pack("<IIB3x", epoch, last_seq, reason)
    require(len(payload) == ACTOR_RESYNC_SIZE, "ActorResync builder mismatch")
    return payload


def actor_appearance_entry(drawable=0xCF7E73BE, albedo=1, normal=2,
                           material=3, palette=4,
                           tint0=0, tint1=0, tint2=0, reserved=0):
    require(APPEARANCE_AVAILABLE,
            "v16 ActorAppearance is not in net/protocol.h yet")
    rec = struct.pack("<IIIIIhhhH", drawable, albedo, normal, material,
                      palette, tint0, tint1, tint2, reserved)
    require(len(rec) == ACTOR_APPEARANCE_ENTRY_SIZE,
            f"ActorAppearanceEntry builder mismatch: "
            f"{len(rec)} != {ACTOR_APPEARANCE_ENTRY_SIZE}")
    return rec


def actor_appearance(epoch=1, actor_id=1, entries=None):
    require(APPEARANCE_AVAILABLE,
            "v16 ActorAppearance is not in net/protocol.h yet")
    entries = list(entries if entries is not None
                   else [actor_appearance_entry()])
    header = struct.pack("<IIB3x", epoch, actor_id, len(entries))
    require(len(header) == ACTOR_APPEARANCE_HEADER_SIZE,
            "ActorAppearance header must be 12 B")
    return header + b"".join(entries)


def actor_damage(epoch=1, actor_id=1, damage=50, pad=0,
                 weapon_hash=0x8B264946):
    require(DAMAGE_AVAILABLE, "v17 ActorDamage is not in net/protocol.h yet")
    payload = struct.pack("<IIHHI", epoch, actor_id, damage, pad, weapon_hash)
    require(len(payload) == ACTOR_DAMAGE_SIZE,
            f"ActorDamage builder mismatch: "
            f"{len(payload)} != {ACTOR_DAMAGE_SIZE}")
    return payload


def player_presence(epoch=1, flags=1, pad=0):
    require(PRESENCE_AVAILABLE, "v18 PlayerPresence is not in net/protocol.h yet")
    payload = struct.pack("<IHH", epoch, flags, pad)
    require(len(payload) == PLAYER_PRESENCE_SIZE,
            f"PlayerPresence builder mismatch: "
            f"{len(payload)} != {PLAYER_PRESENCE_SIZE}")
    return payload


def occupant(actor_id=1, seat=0, flags=1, car_index=0, harness=0):
    require(TRAFFIC_AVAILABLE,
            "v19 VehicleOccupants is not in net/protocol.h yet")
    rec = struct.pack("<IbBBB", actor_id, seat, flags, car_index, harness)
    require(len(rec) == OCCUPANT_ENTRY_SIZE,
            f"VehicleOccupantEntry builder mismatch: "
            f"{len(rec)} != {OCCUPANT_ENTRY_SIZE}")
    return rec


def vehicle_occupants(vehicle_net_id=1, entries=None):
    require(TRAFFIC_AVAILABLE,
            "v19 VehicleOccupants is not in net/protocol.h yet")
    entries = list(entries or [])
    header = struct.pack("<IB3x", vehicle_net_id, len(entries))
    require(len(header) == OCCUPANTS_HEADER_SIZE,
            "VehicleOccupants header must be 8 B")
    return header + b"".join(entries)


def train_v2(epoch=1, train_id=1, config_hash=0x12345678, flags=0,
             station_index=0, car_count=3, speed=5.0,
             x=1.0, y=2.0, z=3.0, heading=90.0, track_index=1,
             pad2=b"\x00" * 8):
    require(TRAFFIC_AVAILABLE,
            "v19 TrainSnapshotV2 is not in net/protocol.h yet")
    require(len(pad2) == 8, "TrainSnapshotV2 pad2 must be 8 B")
    payload = struct.pack("<IIIBBBBfffffI8s",
                          epoch, train_id, config_hash, flags,
                          station_index, car_count, 0,
                          speed, x, y, z, heading, track_index, pad2)
    require(len(payload) == TRAIN_V2_SIZE,
            f"TrainSnapshotV2 builder mismatch: "
            f"{len(payload)} != {TRAIN_V2_SIZE}")
    return payload


def vehicle_interaction(epoch=1, vehicle_net_id=7, target_id=0, seq=1,
                        action=1, seat=127, door=8, flags=0, param=60,
                        weapon_hash=0):
    require(VEHICLE_AVAILABLE, "v20 VehicleInteraction is not available")
    payload = struct.pack("<IIIHBbBBHI", epoch, vehicle_net_id, target_id, seq,
                          action, seat, door, flags, param, weapon_hash)
    require(len(payload) == VEHICLE_INTERACTION_SIZE,
            f"VehicleInteraction builder mismatch: "
            f"{len(payload)} != {VEHICLE_INTERACTION_SIZE}")
    return payload


def vehicle_result(epoch=1, vehicle_net_id=7, seq=1, action=1, result=0,
                   detail=0):
    require(VEHICLE_AVAILABLE, "v20 VehicleInteractionResult is not available")
    payload = struct.pack("<IIHBBI", epoch, vehicle_net_id, seq, action,
                          result, detail)
    require(len(payload) == VEHICLE_RESULT_SIZE,
            f"VehicleInteractionResult builder mismatch: "
            f"{len(payload)} != {VEHICLE_RESULT_SIZE}")
    return payload


def vehicle_state(epoch=1, vehicle_net_id=7, body_health=500, engine_health=500,
                  door_bits=1, seq=1, flags=1, wheels_bits=0, host_seat=127,
                  guest_seat=127, reserved=0):
    require(VEHICLE_AVAILABLE, "v20 VehicleState is not available")
    payload = struct.pack("<IIHhHHBBbbI", epoch, vehicle_net_id, body_health,
                          engine_health, door_bits, seq, flags, wheels_bits,
                          host_seat, guest_seat, reserved)
    require(len(payload) == VEHICLE_STATE_SIZE,
            f"VehicleState builder mismatch: "
            f"{len(payload)} != {VEHICLE_STATE_SIZE}")
    return payload


def _recv_count(sock, type_, duration):
    n = 0
    for t, _p in recv_msgs(sock, duration):
        if t == type_:
            n += 1
    return n


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
    def __init__(self, exe, workdir, port=None, extra_args=None,
                 host_game_ms=15000):
        self.exe = str(exe)
        self.workdir = Path(workdir)
        self._explicit_port = port is not None
        self.port = port or 0
        self.extra_args = list(extra_args or [])
        # 4-й argv сервера — host game-thread timeout (мс). Легаси-тестам с
        # искусственными mock-клиентами (без Frame-тиков, как у реального
        # игрового потока) нужен запас, иначе ранний JOIN-тест может быть
        # дольше 4 с production-default. Тесты, проверяющие сам таймаут,
        # передают host_game_ms явно.
        self.host_game_ms = host_game_ms
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
        # Protected by IntegrationRunLock: parallel integration runs are
        # serialized, so the ephemeral probe cannot be stolen before _launch.
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
        # argv: port, idleSec, exitMs, hostGameTimeoutMs. extra_args задаёт
        # первые позиции (idle/exit); 4-й argv всегда host-game таймаут.
        args = list(self.extra_args)[:3]
        while len(args) < 3:
            args.append("0")
        if self.host_game_ms is not None:
            args[2] = str(self.host_game_ms)
        self._out = open(self.workdir / "server_stdout.log", "ab")
        self.proc = subprocess.Popen(
            [self.exe, str(self.port)] + args,
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
        require(len(outfit) == EXPECTED_OUTFIT_SIZE,
                f"Outfit payload must be {EXPECTED_OUTFIT_SIZE} B (v14), "
                f"got {len(outfit)}")
        host.sendall(frame_bytes(TYPE_OUTFIT, outfit))
        require(recv_type(guest, TYPE_OUTFIT, 2.0) == outfit,
                "Outfit (784 B OutfitState) must relay byte-identical")
        host.sendall(frame_bytes(TYPE_OUTFIT, outfit[:-1]))
        require(recv_type(guest, TYPE_OUTFIT, 0.4) is None,
                "short Outfit must be dropped")
        host.sendall(frame_bytes(TYPE_OUTFIT, outfit + b"\x00"))
        require(recv_type(guest, TYPE_OUTFIT, 0.4) is None,
                "oversized Outfit must be dropped (exact 784 B required)")

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
        require(len(horse) == EXPECTED_OUTFIT_SIZE,
                f"HorseOutfit payload must be {EXPECTED_OUTFIT_SIZE} B (v14), "
                f"got {len(horse)}")
        host.sendall(frame_bytes(TYPE_HORSE_OUTFIT, horse))
        p = recv_type(guest, TYPE_HORSE_OUTFIT, 2.0)
        require(p == horse,
                "HorseOutfit must relay byte-identical (784 B OutfitState)")
        host.sendall(frame_bytes(TYPE_HORSE_OUTFIT, horse[:-1]))
        require(recv_type(guest, TYPE_HORSE_OUTFIT, 0.4) is None,
                "short HorseOutfit must be dropped")
        host.sendall(frame_bytes(TYPE_HORSE_OUTFIT, horse + b"\x00"))
        require(recv_type(guest, TYPE_HORSE_OUTFIT, 0.4) is None,
                "oversized HorseOutfit must be dropped (exact 784 B required)")

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


# --------------------------------------------------------------------------
# Protocol v15 (E0) / v16 (E2c-b) / v17 (E5): actor replication 0x18..0x1F.
# Tests are conditional on net/protocol.h carrying the Actor* messages; while
# E0/E2c-b are not merged they are reported as [skip]. After merge they are
# strict and match the spec in docs/PROTOCOL.md.
# --------------------------------------------------------------------------

def _actor_pair(srv):
    """Join host+guest and drain JOIN_OK/PlayerJoined noise."""
    host = connect(srv.port)
    join(host, "Alice")
    require(recv_type(host, TYPE_JOIN_OK) is not None, "host JOIN_OK missing")
    guest = connect(srv.port)
    join(guest, "Bob")
    require(recv_type(guest, TYPE_JOIN_OK) is not None, "guest JOIN_OK missing")
    recv_msgs(host, 0.3)
    recv_msgs(guest, 0.3)
    return host, guest


def _fast_pair(srv):
    """Детерминированный join-pair для host-game-timeout тестов.

    В отличие от _actor_pair не тратит ~2 с на фиксированные recv-паузы:
    JOIN_OK — строго первый кадр клиенту (P0-инвариант), поэтому читаем его
    сразу через read_one_frame(). Это убирает гонку «timeout во время setup»:
    хост не должен успеть умереть до того, как гость подключится.
    """
    host = connect(srv.port)
    join(host, "Alice")
    require(read_one_frame(host, 2.0) is not None, "host JOIN_OK missing")
    guest = connect(srv.port)
    join(guest, "Bob")
    require(read_one_frame(guest, 2.0) is not None, "guest JOIN_OK missing")
    return host, guest


def _actor_expect_relay(sender, receiver, type_, payload, what):
    sender.sendall(frame_bytes(type_, payload))
    got = recv_type(receiver, type_, 2.0)
    require(got == payload, f"{what} must relay byte-identical")


def _actor_expect_drop(sender, receiver, type_, payload, what):
    sender.sendall(frame_bytes(type_, payload))
    require(recv_type(receiver, type_, 0.5) is None, f"{what} must be dropped")


def test_actor_spawn_host_only(exe, tmp):
    log("[test] v15 actor spawn batch: host-only relay + limits")
    require(ACTOR_AVAILABLE, "v15 Actor* messages missing")
    with Server(exe, tmp / "actor_spawn") as srv:
        host, guest = _actor_pair(srv)

        one = actor_spawn_batch(epoch=1, records=[actor_spawn(actor_id=7, epoch=1)])
        require(len(one) == 8 + ACTOR_SPAWN_SIZE,
                f"spawn batch size {len(one)} != {8 + ACTOR_SPAWN_SIZE}")
        _actor_expect_relay(host, guest, TYPE_ACTOR_SPAWN_BATCH, one,
                            "ActorSpawnBatch(1)")

        max_batch = actor_spawn_batch(
            epoch=2,
            records=[actor_spawn(actor_id=100 + i, epoch=2)
                     for i in range(MAX_ACTORS)])
        require(len(max_batch) == ACTOR_SPAWN_BATCH_MAX,
                f"max spawn batch {len(max_batch)} != {ACTOR_SPAWN_BATCH_MAX}")
        _actor_expect_relay(host, guest, TYPE_ACTOR_SPAWN_BATCH, max_batch,
                            "ActorSpawnBatch(max)")

        empty = actor_spawn_batch(epoch=3, records=[])
        _actor_expect_relay(host, guest, TYPE_ACTOR_SPAWN_BATCH, empty,
                            "ActorSpawnBatch(count=0)")

        too_many = actor_spawn_batch(
            epoch=4,
            records=[actor_spawn(actor_id=200 + i, epoch=4)
                     for i in range(MAX_ACTORS + 1)])
        _actor_expect_drop(host, guest, TYPE_ACTOR_SPAWN_BATCH, too_many,
                           "ActorSpawnBatch(count>max)")

        _actor_expect_drop(host, guest, TYPE_ACTOR_SPAWN_BATCH, one[:-1],
                           "short ActorSpawnBatch")
        _actor_expect_drop(host, guest, TYPE_ACTOR_SPAWN_BATCH, one + b"\x00",
                           "oversized ActorSpawnBatch")

        guest.sendall(frame_bytes(TYPE_ACTOR_SPAWN_BATCH, one))
        require(recv_type(host, TYPE_ACTOR_SPAWN_BATCH, 0.5) is None,
                "guest ActorSpawnBatch must be blocked (host-only)")
        host.close()
        guest.close()


def test_actor_frames_host_only(exe, tmp):
    log("[test] v15 actor frames: host-only relay + limits/finite")
    require(ACTOR_AVAILABLE, "v15 Actor* messages missing")
    with Server(exe, tmp / "actor_frames") as srv:
        host, guest = _actor_pair(srv)

        one = actor_frames(epoch=1, seq=1,
                           records=[actor_frame(actor_id=7, seq=1)])
        require(len(one) == 8 + ACTOR_FRAME_SIZE,
                f"frames size {len(one)} != {8 + ACTOR_FRAME_SIZE}")
        _actor_expect_relay(host, guest, TYPE_ACTOR_FRAMES, one, "ActorFrames(1)")

        max_batch = actor_frames(
            epoch=2, seq=10,
            records=[actor_frame(actor_id=100 + i, seq=10 + i)
                     for i in range(MAX_ACTORS)])
        require(len(max_batch) == ACTOR_FRAMES_MAX,
                f"max frames {len(max_batch)} != {ACTOR_FRAMES_MAX}")
        _actor_expect_relay(host, guest, TYPE_ACTOR_FRAMES, max_batch,
                            "ActorFrames(max)")

        empty = actor_frames(epoch=3, seq=20, records=[])
        _actor_expect_relay(host, guest, TYPE_ACTOR_FRAMES, empty,
                            "ActorFrames(count=0)")

        too_many = actor_frames(
            epoch=4, seq=30,
            records=[actor_frame(actor_id=200 + i, seq=30 + i)
                     for i in range(MAX_ACTORS + 1)])
        _actor_expect_drop(host, guest, TYPE_ACTOR_FRAMES, too_many,
                           "ActorFrames(count>max)")
        _actor_expect_drop(host, guest, TYPE_ACTOR_FRAMES, one[:-1],
                           "short ActorFrames")
        _actor_expect_drop(host, guest, TYPE_ACTOR_FRAMES, one + b"\x00",
                           "oversized ActorFrames")

        for bad in (float("nan"), float("inf"), 1.0e6):
            bad_frames = actor_frames(
                epoch=5, seq=40,
                records=[actor_frame(actor_id=7, seq=40, x=bad)])
            _actor_expect_drop(host, guest, TYPE_ACTOR_FRAMES, bad_frames,
                               f"ActorFrames(x={bad!r})")

        guest.sendall(frame_bytes(TYPE_ACTOR_FRAMES, one))
        require(recv_type(host, TYPE_ACTOR_FRAMES, 0.5) is None,
                "guest ActorFrames must be blocked (host-only)")
        host.close()
        guest.close()


def test_actor_despawn_event_outfit(exe, tmp):
    log("[test] v15 actor despawn/event/outfit: host-only + exact sizes")
    require(ACTOR_AVAILABLE, "v15 Actor* messages missing")
    with Server(exe, tmp / "actor_misc") as srv:
        host, guest = _actor_pair(srv)

        despawn = actor_despawn(epoch=1, actor_id=7, reason=2)
        require(len(despawn) == ACTOR_DESPAWN_SIZE, "despawn size mismatch")
        _actor_expect_relay(host, guest, TYPE_ACTOR_DESPAWN, despawn,
                            "ActorDespawn")
        _actor_expect_drop(host, guest, TYPE_ACTOR_DESPAWN, despawn[:-1],
                           "short ActorDespawn")
        _actor_expect_drop(host, guest, TYPE_ACTOR_DESPAWN, despawn + b"\x00",
                           "oversized ActorDespawn")
        guest.sendall(frame_bytes(TYPE_ACTOR_DESPAWN, despawn))
        require(recv_type(host, TYPE_ACTOR_DESPAWN, 0.5) is None,
                "guest ActorDespawn must be blocked (host-only)")

        event = actor_event(epoch=1, actor_id=7, ev_type=1, flags=1,
                            a=10, b=20, x=1, y=2, z=3)
        require(len(event) == ACTOR_EVENT_SIZE, "event size mismatch")
        _actor_expect_relay(host, guest, TYPE_ACTOR_EVENT, event,
                            "ActorEvent")
        _actor_expect_drop(host, guest, TYPE_ACTOR_EVENT, event[:-1],
                           "short ActorEvent")
        _actor_expect_drop(host, guest, TYPE_ACTOR_EVENT, event + b"\x00",
                           "oversized ActorEvent")
        guest.sendall(frame_bytes(TYPE_ACTOR_EVENT, event))
        require(recv_type(host, TYPE_ACTOR_EVENT, 0.5) is None,
                "guest ActorEvent must be blocked (host-only)")

        outfit = actor_outfit(epoch=1, actor_id=7)
        require(len(outfit) == ACTOR_OUTFIT_SIZE,
                f"outfit size {len(outfit)} != {ACTOR_OUTFIT_SIZE}")
        _actor_expect_relay(host, guest, TYPE_ACTOR_OUTFIT, outfit,
                            "ActorOutfit")
        _actor_expect_drop(host, guest, TYPE_ACTOR_OUTFIT, outfit[:-1],
                           "short ActorOutfit")
        _actor_expect_drop(host, guest, TYPE_ACTOR_OUTFIT, outfit + b"\x00",
                           "oversized ActorOutfit")
        guest.sendall(frame_bytes(TYPE_ACTOR_OUTFIT, outfit))
        require(recv_type(host, TYPE_ACTOR_OUTFIT, 0.5) is None,
                "guest ActorOutfit must be blocked (host-only)")

        host.close()
        guest.close()


def test_actor_resync_direction(exe, tmp):
    log("[test] v15 actor resync: guest -> host only")
    require(ACTOR_AVAILABLE, "v15 Actor* messages missing")
    with Server(exe, tmp / "actor_resync") as srv:
        host, guest = _actor_pair(srv)

        resync = actor_resync(epoch=1, last_seq=123, reason=1)
        require(len(resync) == ACTOR_RESYNC_SIZE, "resync size mismatch")

        guest.sendall(frame_bytes(TYPE_ACTOR_RESYNC, resync))
        got = recv_type(host, TYPE_ACTOR_RESYNC, 2.0)
        require(got == resync,
                "guest ActorResync must relay byte-identical to the host only")

        host.sendall(frame_bytes(TYPE_ACTOR_RESYNC, resync))
        require(recv_type(guest, TYPE_ACTOR_RESYNC, 0.6) is None,
                "host ActorResync must be blocked (guest-only direction)")

        _actor_expect_drop(host, guest, TYPE_ACTOR_RESYNC, resync[:-1],
                           "short ActorResync")
        _actor_expect_drop(host, guest, TYPE_ACTOR_RESYNC, resync + b"\x00",
                           "oversized ActorResync")
        _actor_expect_drop(host, guest, TYPE_ACTOR_RESYNC,
                           actor_resync(epoch=0, last_seq=1, reason=1),
                           "ActorResync(epoch=0)")
        host.close()
        guest.close()


def test_actor_validation_drops(exe, tmp):
    log("[test] v15 actor validation: epoch/actorId/finite/unknown drops")
    require(ACTOR_AVAILABLE, "v15 Actor* messages missing")
    with Server(exe, tmp / "actor_validate") as srv:
        host, guest = _actor_pair(srv)

        # epoch=0 is invalid for every actor message.
        _actor_expect_drop(host, guest, TYPE_ACTOR_SPAWN_BATCH,
                           actor_spawn_batch(epoch=0,
                                             records=[actor_spawn(actor_id=7,
                                                                  epoch=0)]),
                           "ActorSpawnBatch(epoch=0)")
        _actor_expect_drop(host, guest, TYPE_ACTOR_FRAMES,
                           actor_frames(epoch=0, seq=1,
                                        records=[actor_frame(actor_id=7,
                                                             seq=1)]),
                           "ActorFrames(epoch=0)")
        _actor_expect_drop(host, guest, TYPE_ACTOR_DESPAWN,
                           actor_despawn(epoch=0, actor_id=7), "Despawn(epoch=0)")
        _actor_expect_drop(host, guest, TYPE_ACTOR_EVENT,
                           actor_event(epoch=0, actor_id=7), "Event(epoch=0)")
        _actor_expect_drop(host, guest, TYPE_ACTOR_OUTFIT,
                           actor_outfit(epoch=0, actor_id=7), "Outfit(epoch=0)")

        # actorId=0 is invalid where an actorId is present.
        _actor_expect_drop(host, guest, TYPE_ACTOR_SPAWN_BATCH,
                           actor_spawn_batch(epoch=1,
                                             records=[actor_spawn(actor_id=0)]),
                           "ActorSpawnBatch(actorId=0)")
        _actor_expect_drop(host, guest, TYPE_ACTOR_FRAMES,
                           actor_frames(epoch=1, seq=1,
                                        records=[actor_frame(actor_id=0, seq=1)]),
                           "ActorFrames(actorId=0)")
        _actor_expect_drop(host, guest, TYPE_ACTOR_DESPAWN,
                           actor_despawn(epoch=1, actor_id=0), "Despawn(actorId=0)")
        _actor_expect_drop(host, guest, TYPE_ACTOR_OUTFIT,
                           actor_outfit(epoch=1, actor_id=0), "Outfit(actorId=0)")

        # Non-finite / absurd coordinates in spawn and frames.
        for bad in (float("nan"), float("inf"), -float("inf"), 1.0e6, -1.0e6):
            _actor_expect_drop(host, guest, TYPE_ACTOR_SPAWN_BATCH,
                               actor_spawn_batch(epoch=1,
                                                 records=[actor_spawn(x=bad)]),
                               f"ActorSpawnBatch(x={bad!r})")
        for bad in (float("nan"), float("inf"), 1.0e6):
            _actor_expect_drop(host, guest, TYPE_ACTOR_FRAMES,
                               actor_frames(epoch=1, seq=1,
                                            records=[actor_frame(x=bad,
                                                                 seq=1)]),
                               f"ActorFrames(heading/x={bad!r})")

        # Unknown message type must not be relayed and must not crash.
        host.sendall(frame_bytes(0x7E, b"\x00" * 32))
        require(recv_type(guest, 0x7E, 0.5) is None,
                "unknown type must not be relayed")
        host.close()
        guest.close()


def test_actor_duplicate_actor_ids(exe, tmp):
    log("[test] v15 actor duplicate actorId in one batch must be dropped")
    require(ACTOR_AVAILABLE, "v15 Actor* messages missing")
    with Server(exe, tmp / "actor_dup") as srv:
        host, guest = _actor_pair(srv)

        dup_spawn = actor_spawn_batch(
            epoch=1,
            records=[actor_spawn(actor_id=7, epoch=1),
                     actor_spawn(actor_id=7, epoch=1)])
        _actor_expect_drop(host, guest, TYPE_ACTOR_SPAWN_BATCH, dup_spawn,
                           "ActorSpawnBatch(duplicate actorId)")

        dup_frames = actor_frames(
            epoch=1, seq=1,
            records=[actor_frame(actor_id=7, seq=1),
                     actor_frame(actor_id=7, seq=1)])
        _actor_expect_drop(host, guest, TYPE_ACTOR_FRAMES, dup_frames,
                           "ActorFrames(duplicate actorId)")
        host.close()
        guest.close()


def test_actor_appearance_host_only(exe, tmp):
    log("[test] v16 actor appearance: host-only relay + max size")
    require(APPEARANCE_AVAILABLE, "v16 ActorAppearance messages missing")
    with Server(exe, tmp / "actor_appearance") as srv:
        host, guest = _actor_pair(srv)

        one = actor_appearance(epoch=1, actor_id=7,
                               entries=[actor_appearance_entry()])
        require(len(one) == ACTOR_APPEARANCE_HEADER_SIZE +
                           ACTOR_APPEARANCE_ENTRY_SIZE,
                f"appearance size {len(one)} != header+entry")
        _actor_expect_relay(host, guest, TYPE_ACTOR_APPEARANCE, one,
                            "ActorAppearance(1)")

        max_entries = [actor_appearance_entry(drawable=0x1000 + i,
                                              tint0=i, tint1=i, tint2=i)
                       for i in range(MAX_ACTOR_APPEARANCE)]
        max_batch = actor_appearance(epoch=2, actor_id=8, entries=max_entries)
        require(len(max_batch) == ACTOR_APPEARANCE_MAX,
                f"max appearance {len(max_batch)} != {ACTOR_APPEARANCE_MAX}")
        _actor_expect_relay(host, guest, TYPE_ACTOR_APPEARANCE, max_batch,
                            "ActorAppearance(max)")

        guest.sendall(frame_bytes(TYPE_ACTOR_APPEARANCE, one))
        require(recv_type(host, TYPE_ACTOR_APPEARANCE, 0.5) is None,
                "guest ActorAppearance must be blocked (host-only)")
        host.close()
        guest.close()


def test_actor_appearance_validation(exe, tmp):
    log("[test] v16 actor appearance: exact size/count/epoch/actorId/reserved")
    require(APPEARANCE_AVAILABLE, "v16 ActorAppearance messages missing")
    with Server(exe, tmp / "actor_appearance_validate") as srv:
        host, guest = _actor_pair(srv)

        one = actor_appearance(epoch=1, actor_id=7)
        require(len(one) == ACTOR_APPEARANCE_HEADER_SIZE +
                           ACTOR_APPEARANCE_ENTRY_SIZE,
                f"appearance size {len(one)} != header+entry")
        # Точный размер: 12 + count * 28.
        _actor_expect_drop(host, guest, TYPE_ACTOR_APPEARANCE, one[:-1],
                           "short ActorAppearance")
        _actor_expect_drop(host, guest, TYPE_ACTOR_APPEARANCE, one + b"\x00",
                           "oversized ActorAppearance")
        # count=0 невалиден (пустой список не шлётся), count>16 (17) — тоже.
        header_only = struct.pack("<IIB3x", 1, 7, 0)
        require(len(header_only) == ACTOR_APPEARANCE_HEADER_SIZE,
                "header-only appearance size mismatch")
        _actor_expect_drop(host, guest, TYPE_ACTOR_APPEARANCE, header_only,
                           "ActorAppearance(count=0)")
        too_many = actor_appearance(
            epoch=1, actor_id=7,
            entries=[actor_appearance_entry(drawable=0x100 + i)
                     for i in range(MAX_ACTOR_APPEARANCE + 1)])
        require(len(too_many) == ACTOR_APPEARANCE_HEADER_SIZE +
                                (MAX_ACTOR_APPEARANCE + 1) *
                                ACTOR_APPEARANCE_ENTRY_SIZE,
                "count>max appearance size mismatch")
        _actor_expect_drop(host, guest, TYPE_ACTOR_APPEARANCE, too_many,
                           "ActorAppearance(count>max)")
        # epoch=0 / actorId=0 / reserved!=0 невалидны.
        _actor_expect_drop(host, guest, TYPE_ACTOR_APPEARANCE,
                           actor_appearance(epoch=0, actor_id=7),
                           "ActorAppearance(epoch=0)")
        _actor_expect_drop(host, guest, TYPE_ACTOR_APPEARANCE,
                           actor_appearance(epoch=1, actor_id=0),
                           "ActorAppearance(actorId=0)")
        _actor_expect_drop(host, guest, TYPE_ACTOR_APPEARANCE,
                           actor_appearance(
                               epoch=1, actor_id=7,
                               entries=[actor_appearance_entry(reserved=1)]),
                           "ActorAppearance(reserved!=0)")
        # Unknown message type must not be relayed and must not crash.
        host.sendall(frame_bytes(0x7E, b"\x00" * 32))
        require(recv_type(guest, 0x7E, 0.5) is None,
                "unknown type must not be relayed")
        host.close()
        guest.close()


def test_actor_damage_direction(exe, tmp):
    log("[test] v17 actor damage: guest -> host only (16 B relay)")
    require(DAMAGE_AVAILABLE, "v17 ActorDamage messages missing")
    with Server(exe, tmp / "actor_damage") as srv:
        host, guest = _actor_pair(srv)

        dmg = actor_damage(epoch=1, actor_id=7, damage=42,
                           weapon_hash=0x8B264946)
        require(len(dmg) == ACTOR_DAMAGE_SIZE, "damage size mismatch")
        # Гость -> сервер -> хост: байт-в-байт, только хосту.
        guest.sendall(frame_bytes(TYPE_ACTOR_DAMAGE, dmg))
        got = recv_type(host, TYPE_ACTOR_DAMAGE, 2.0)
        require(got == dmg,
                "guest ActorDamage must relay byte-identical to the host")
        # Гостям/отправителю не релеим: эха быть не должно.
        require(recv_type(guest, TYPE_ACTOR_DAMAGE, 0.5) is None,
                "ActorDamage must not be relayed back to the guest")
        # Хост -> сервер: drop (урон применяет сам хост, запросы не шлёт).
        host.sendall(frame_bytes(TYPE_ACTOR_DAMAGE, dmg))
        require(recv_type(guest, TYPE_ACTOR_DAMAGE, 0.5) is None,
                "host ActorDamage must be dropped (guest-only direction)")
        host.close()
        guest.close()


def test_actor_damage_validation(exe, tmp):
    log("[test] v17 actor damage: size/epoch/actorId/damage validation")
    require(DAMAGE_AVAILABLE, "v17 ActorDamage messages missing")
    with Server(exe, tmp / "actor_damage_validate") as srv:
        host, guest = _actor_pair(srv)

        good = actor_damage(epoch=1, actor_id=7, damage=50)
        require(len(good) == ACTOR_DAMAGE_SIZE, "damage size mismatch")
        # Точный размер 16 B.
        _actor_expect_drop(guest, host, TYPE_ACTOR_DAMAGE, good[:-1],
                           "short ActorDamage")
        _actor_expect_drop(guest, host, TYPE_ACTOR_DAMAGE, good + b"\x00",
                           "oversized ActorDamage")
        # epoch/actorId=0 и damage вне 1..1000 невалидны.
        _actor_expect_drop(guest, host, TYPE_ACTOR_DAMAGE,
                           actor_damage(epoch=0, actor_id=7),
                           "ActorDamage(epoch=0)")
        _actor_expect_drop(guest, host, TYPE_ACTOR_DAMAGE,
                           actor_damage(epoch=1, actor_id=0),
                           "ActorDamage(actorId=0)")
        _actor_expect_drop(guest, host, TYPE_ACTOR_DAMAGE,
                           actor_damage(epoch=1, actor_id=7, damage=0),
                           "ActorDamage(damage=0)")
        _actor_expect_drop(guest, host, TYPE_ACTOR_DAMAGE,
                           actor_damage(epoch=1, actor_id=7, damage=1001),
                           "ActorDamage(damage=1001)")
        # Unknown message type must not be relayed and must not crash.
        host.sendall(frame_bytes(0x7E, b"\x00" * 32))
        require(recv_type(guest, 0x7E, 0.5) is None,
                "unknown type must not be relayed")
        host.close()
        guest.close()


def test_player_presence_relay(exe, tmp):
    log("[test] v18 player presence: bidirectional relay, no echo")
    require(PRESENCE_AVAILABLE, "v18 PlayerPresence messages missing")
    with Server(exe, tmp / "player_presence") as srv:
        host, guest = _actor_pair(srv)

        # host -> guest (in_map).
        p_map = player_presence(epoch=1, flags=1)
        host.sendall(frame_bytes(TYPE_PLAYER_PRESENCE, p_map))
        got = recv_type(guest, TYPE_PLAYER_PRESENCE, 2.0)
        require(got == p_map,
                "host PlayerPresence must relay byte-identical to the guest")
        require(recv_type(host, TYPE_PLAYER_PRESENCE, 0.5) is None,
                "PlayerPresence must not echo back to the host sender")

        # guest -> host (in_pause_menu).
        p_pause = player_presence(epoch=1, flags=2)
        guest.sendall(frame_bytes(TYPE_PLAYER_PRESENCE, p_pause))
        got = recv_type(host, TYPE_PLAYER_PRESENCE, 2.0)
        require(got == p_pause,
                "guest PlayerPresence must relay byte-identical to the host")
        require(recv_type(guest, TYPE_PLAYER_PRESENCE, 0.5) is None,
                "PlayerPresence must not echo back to the guest sender")

        # flags=0 («нет статуса») — валиден и релеится.
        p_idle = player_presence(epoch=1, flags=0)
        host.sendall(frame_bytes(TYPE_PLAYER_PRESENCE, p_idle))
        require(recv_type(guest, TYPE_PLAYER_PRESENCE, 2.0) == p_idle,
                "PlayerPresence(flags=0) must relay")
        host.close()
        guest.close()


def test_player_presence_validation(exe, tmp):
    log("[test] v18 player presence: size/epoch/flags validation")
    require(PRESENCE_AVAILABLE, "v18 PlayerPresence messages missing")
    with Server(exe, tmp / "player_presence_validate") as srv:
        host, guest = _actor_pair(srv)

        good = player_presence(epoch=1, flags=4)  # in_satchel
        require(len(good) == PLAYER_PRESENCE_SIZE, "presence size mismatch")
        # Точный размер 8 B.
        _actor_expect_drop(host, guest, TYPE_PLAYER_PRESENCE, good[:-1],
                           "short PlayerPresence")
        _actor_expect_drop(host, guest, TYPE_PLAYER_PRESENCE, good + b"\x00",
                           "oversized PlayerPresence")
        # epoch=0 и неизвестные биты flags невалидны.
        _actor_expect_drop(host, guest, TYPE_PLAYER_PRESENCE,
                           player_presence(epoch=0, flags=1),
                           "PlayerPresence(epoch=0)")
        _actor_expect_drop(host, guest, TYPE_PLAYER_PRESENCE,
                           player_presence(epoch=1, flags=0x40),
                           "PlayerPresence(unknown flag bit)")
        # Unknown message type must not be relayed and must not crash.
        host.sendall(frame_bytes(0x7E, b"\x00" * 32))
        require(recv_type(guest, 0x7E, 0.5) is None,
                "unknown type must not be relayed")
        host.close()
        guest.close()


def test_vehicle_occupants_host_only(exe, tmp):
    log("[test] v19 vehicle occupants: host-only relay + count bounds")
    require(TRAFFIC_AVAILABLE, "v19 VehicleOccupants messages missing")
    with Server(exe, tmp / "vehicle_occupants") as srv:
        host, guest = _actor_pair(srv)

        three = vehicle_occupants(7, [
            occupant(101, 0, 1),                      # водитель, seat 0
            occupant(102, 1, 2),                      # пассажир, seat 1
            occupant(103, -1, 4, car_index=2, harness=3),  # draftHorse
        ])
        require(len(three) == OCCUPANTS_HEADER_SIZE + 3 * OCCUPANT_ENTRY_SIZE,
                "occupants(3) size mismatch")
        _actor_expect_relay(host, guest, TYPE_VEHICLE_OCCUPANTS, three,
                            "VehicleOccupants(3)")

        max_occ = vehicle_occupants(8, [occupant(200 + i, i, 2)
                                        for i in range(MAX_OCCUPANTS)])
        require(len(max_occ) == OCCUPANTS_MAX,
                f"max occupants {len(max_occ)} != {OCCUPANTS_MAX}")
        _actor_expect_relay(host, guest, TYPE_VEHICLE_OCCUPANTS, max_occ,
                            "VehicleOccupants(max)")

        empty = vehicle_occupants(9, [])
        _actor_expect_relay(host, guest, TYPE_VEHICLE_OCCUPANTS, empty,
                            "VehicleOccupants(count=0 clear)")

        guest.sendall(frame_bytes(TYPE_VEHICLE_OCCUPANTS, three))
        require(recv_type(host, TYPE_VEHICLE_OCCUPANTS, 0.5) is None,
                "guest VehicleOccupants must be blocked (host-only)")
        host.close()
        guest.close()


def test_vehicle_occupants_validation(exe, tmp):
    log("[test] v19 vehicle occupants: size/count/vehicleNetId/entry validation")
    require(TRAFFIC_AVAILABLE, "v19 VehicleOccupants messages missing")
    with Server(exe, tmp / "vehicle_occupants_validate") as srv:
        host, guest = _actor_pair(srv)

        good = vehicle_occupants(7, [occupant(101, 0, 1)])
        _actor_expect_drop(host, guest, TYPE_VEHICLE_OCCUPANTS, good[:-1],
                           "short VehicleOccupants")
        _actor_expect_drop(host, guest, TYPE_VEHICLE_OCCUPANTS, good + b"\x00",
                           "oversized VehicleOccupants")
        too_many = vehicle_occupants(7, [occupant(100 + i, i, 2)
                                         for i in range(MAX_OCCUPANTS + 1)])
        _actor_expect_drop(host, guest, TYPE_VEHICLE_OCCUPANTS, too_many,
                           "VehicleOccupants(count>max)")
        _actor_expect_drop(host, guest, TYPE_VEHICLE_OCCUPANTS,
                           vehicle_occupants(0, [occupant(101, 0, 1)]),
                           "VehicleOccupants(vehicleNetId=0)")
        for ent, what in (
            (occupant(0, 0, 1), "actorId=0"),
            (occupant(101, -2, 1), "seat=-2"),
            (occupant(101, 9, 1), "seat=9"),
            (occupant(101, 0, 1, car_index=17), "carIndex=17"),
            (occupant(101, 0, 4, harness=6), "harness=6"),
            (occupant(101, 0, 8), "unknown flag"),
        ):
            _actor_expect_drop(host, guest, TYPE_VEHICLE_OCCUPANTS,
                               vehicle_occupants(7, [ent]),
                               f"VehicleOccupants({what})")
        host.sendall(frame_bytes(0x7E, b"\x00" * 32))
        require(recv_type(guest, 0x7E, 0.5) is None,
                "unknown type must not be relayed")
        host.close()
        guest.close()


def test_train_v2_host_only(exe, tmp):
    log("[test] v19 train snapshot v2: host-only relay, 48 B")
    require(TRAFFIC_AVAILABLE, "v19 TrainSnapshotV2 messages missing")
    with Server(exe, tmp / "train_v2") as srv:
        host, guest = _actor_pair(srv)

        one = train_v2(epoch=1, train_id=42, car_count=1)
        require(len(one) == TRAIN_V2_SIZE, "train v2 size mismatch")
        _actor_expect_relay(host, guest, TYPE_TRAIN_V2, one,
                            "TrainSnapshotV2(1 car)")

        maxc = train_v2(epoch=2, train_id=43, car_count=MAX_TRAIN_CARS,
                        flags=1, station_index=2, speed=12.5, track_index=3)
        require(len(maxc) == TRAIN_V2_SIZE, "train v2 size mismatch")
        _actor_expect_relay(host, guest, TYPE_TRAIN_V2, maxc,
                            "TrainSnapshotV2(max cars)")

        _actor_expect_drop(host, guest, TYPE_TRAIN_V2, one[:-1],
                           "short TrainSnapshotV2")
        _actor_expect_drop(host, guest, TYPE_TRAIN_V2, one + b"\x00",
                           "oversized TrainSnapshotV2")
        guest.sendall(frame_bytes(TYPE_TRAIN_V2, one))
        require(recv_type(host, TYPE_TRAIN_V2, 0.5) is None,
                "guest TrainSnapshotV2 must be blocked (host-only)")
        host.sendall(frame_bytes(0x7E, b"\x00" * 32))
        require(recv_type(guest, 0x7E, 0.5) is None,
                "unknown type must not be relayed")
        host.close()
        guest.close()


def test_train_v2_validation(exe, tmp):
    log("[test] v19 train snapshot v2: epoch/trainId/carCount/speed/finite/flags")
    require(TRAFFIC_AVAILABLE, "v19 TrainSnapshotV2 messages missing")
    with Server(exe, tmp / "train_v2_validate") as srv:
        host, guest = _actor_pair(srv)

        cases = [
            (dict(epoch=0), "epoch=0"),
            (dict(train_id=0), "trainId=0"),
            (dict(car_count=0), "carCount=0"),
            (dict(car_count=MAX_TRAIN_CARS + 1), "carCount>max"),
            (dict(speed=float("nan")), "speed=NaN"),
            (dict(speed=float("inf")), "speed=Inf"),
            (dict(speed=41.0), "speed=41"),
            (dict(speed=-41.0), "speed=-41"),
            (dict(x=float("nan")), "x=NaN"),
            (dict(y=float("inf")), "y=Inf"),
            (dict(z=1.0e6), "z=1e6"),
            (dict(heading=float("nan")), "heading=NaN"),
            (dict(flags=0x10), "unknown flag"),
        ]
        for kwargs, what in cases:
            _actor_expect_drop(host, guest, TYPE_TRAIN_V2,
                               train_v2(**kwargs),
                               f"TrainSnapshotV2({what})")

        # Граничные скорости ±40 допустимы и релеятся байт-в-байт.
        for sp in (-40.0, 40.0):
            _actor_expect_relay(host, guest, TYPE_TRAIN_V2,
                                train_v2(speed=sp, car_count=1),
                                f"TrainSnapshotV2(speed={sp})")
        host.close()
        guest.close()


def test_vehicle_occupants_rate_limit(exe, tmp):
    log("[test] v19 vehicle occupants: per-client rate-limit ~20/s")
    require(TRAFFIC_AVAILABLE, "v19 VehicleOccupants messages missing")
    # Pinned server limits (см. server/server.cpp: 0x21 = 20/с, 0x22 = 4/с).
    limit = 20
    burst = limit * 2
    with Server(exe, tmp / "vehicle_occupants_rate") as srv:
        host, guest = _actor_pair(srv)

        payload = vehicle_occupants(7, [occupant(101, 0, 1)])
        # Пачка быстрее одной секунды: в окно 1 с проходит ровно limit.
        for _ in range(burst):
            host.sendall(frame_bytes(TYPE_VEHICLE_OCCUPANTS, payload))
        got = 0
        for t, _p in recv_msgs(guest, 1.2):
            if t == TYPE_VEHICLE_OCCUPANTS:
                got += 1
        require(got == limit,
                f"VehicleOccupants rate-limit: expected {limit} relays in 1 s "
                f"window, got {got} (burst={burst})")
        # Окно сбрасывается: после паузы сообщения снова проходят.
        time.sleep(1.2)
        for _ in range(3):
            host.sendall(frame_bytes(TYPE_VEHICLE_OCCUPANTS, payload))
        got2 = 0
        for t, _p in recv_msgs(guest, 1.0):
            if t == TYPE_VEHICLE_OCCUPANTS:
                got2 += 1
        require(got2 == 3,
                f"VehicleOccupants rate window must reset: got {got2}/3")
        host.close()
        guest.close()


def test_train_v2_rate_limit(exe, tmp):
    log("[test] v19 train snapshot v2: per-client rate-limit ~4/s")
    require(TRAFFIC_AVAILABLE, "v19 TrainSnapshotV2 messages missing")
    limit = 4
    burst = limit * 3
    with Server(exe, tmp / "train_v2_rate") as srv:
        host, guest = _actor_pair(srv)

        payload = train_v2(car_count=2)
        for _ in range(burst):
            host.sendall(frame_bytes(TYPE_TRAIN_V2, payload))
        got = 0
        for t, _p in recv_msgs(guest, 1.2):
            if t == TYPE_TRAIN_V2:
                got += 1
        require(got == limit,
                f"TrainSnapshotV2 rate-limit: expected {limit} relays in 1 s "
                f"window, got {got} (burst={burst})")
        time.sleep(1.2)
        for _ in range(2):
            host.sendall(frame_bytes(TYPE_TRAIN_V2, payload))
        got2 = 0
        for t, _p in recv_msgs(guest, 1.0):
            if t == TYPE_TRAIN_V2:
                got2 += 1
        require(got2 == 2,
                f"TrainSnapshotV2 rate window must reset: got {got2}/2")
        host.close()
        guest.close()


def test_vehicle_interaction_guest_only_relay(exe, tmp):
    log("[test] v20 vehicle interaction: guest->host relay, no echo")
    require(VEHICLE_AVAILABLE, "v20 vehicle messages missing")
    with Server(exe, tmp / "vehicle_interaction") as srv:
        host, guest = _fast_pair(srv)
        intent = vehicle_interaction(epoch=3, vehicle_net_id=42, seq=5,
                                     action=1, seat=127, door=8, param=60,
                                     weapon_hash=0xAABBCCDD)
        guest.sendall(frame_bytes(TYPE_VEHICLE_INTERACTION, intent))
        require(recv_type(host, TYPE_VEHICLE_INTERACTION, 2.0) == intent,
                "VehicleInteraction must relay guest->host byte-identical")
        require(recv_type(guest, TYPE_VEHICLE_INTERACTION, 0.4) is None,
                "VehicleInteraction must not echo back to guest")
        host.close()
        guest.close()


def test_vehicle_interaction_validation(exe, tmp):
    log("[test] v20 vehicle interaction: size/action/seat/flags validation")
    require(VEHICLE_AVAILABLE, "v20 vehicle messages missing")
    with Server(exe, tmp / "vehicle_interaction_validate") as srv:
        host, guest = _fast_pair(srv)
        good = vehicle_interaction()

        def expect_drop(payload, what):
            guest.sendall(frame_bytes(TYPE_VEHICLE_INTERACTION, payload))
            require(recv_type(host, TYPE_VEHICLE_INTERACTION, 0.35) is None,
                    f"VehicleInteraction({what}) must be dropped")

        expect_drop(good[:-1], "short 23 B")
        expect_drop(good + b"\x00", "long 25 B")
        bad = []
        bad.append((vehicle_interaction(epoch=0), "epoch=0"))
        bad.append((vehicle_interaction(vehicle_net_id=0), "netId=0"))
        bad.append((vehicle_interaction(seq=0), "seq=0"))
        bad.append((vehicle_interaction(action=0), "action None"))
        bad.append((vehicle_interaction(action=13), "action 13"))
        bad.append((vehicle_interaction(seat=9), "seat 9"))
        bad.append((vehicle_interaction(door=12), "door part 12"))
        bad.append((vehicle_interaction(param=0), "param 0"))
        bad.append((vehicle_interaction(flags=0x40), "unknown flag"))
        bad.append((vehicle_interaction(flags=1 | 2), "actor+entity flags"))
        for payload, what in bad:
            expect_drop(payload, what)
        # 0x25/0x27 reserved: drop, server must stay alive.
        guest.sendall(frame_bytes(0x25, b"\x00" * 24))
        guest.sendall(frame_bytes(0x27, b"\x00" * 24))
        require(recv_type(host, 0x25, 0.3) is None, "0x25 must not relay")
        require(recv_type(host, 0x27, 0.3) is None, "0x27 must not relay")
        # Server alive: valid frame still relays after invalid burst.
        f = frame_bytes(TYPE_FRAME, make_frame_payload())
        guest.sendall(f)
        require(recv_type(host, TYPE_FRAME, 1.5) == make_frame_payload(),
                "server must stay alive after invalid vehicle intents")
        host.close()
        guest.close()


def test_vehicle_interaction_rate_limit(exe, tmp):
    log("[test] v20 vehicle interaction: rate-limit 8/s (guest->host)")
    require(VEHICLE_AVAILABLE, "v20 vehicle messages missing")
    limit = VEHICLE_RATE_INTERACTION
    burst = limit * 2
    with Server(exe, tmp / "vehicle_interaction_rate") as srv:
        host, guest = _fast_pair(srv)
        payload = vehicle_interaction(seq=1)
        for _ in range(burst):
            guest.sendall(frame_bytes(TYPE_VEHICLE_INTERACTION, payload))
        got = _recv_count(host, TYPE_VEHICLE_INTERACTION, 1.2)
        require(got == limit,
                f"VehicleInteraction rate-limit: expected {limit}/s, got {got}")
        time.sleep(1.2)
        for _ in range(2):
            guest.sendall(frame_bytes(TYPE_VEHICLE_INTERACTION, payload))
        got2 = _recv_count(host, TYPE_VEHICLE_INTERACTION, 1.0)
        require(got2 == 2,
                f"VehicleInteraction rate window must reset: got {got2}/2")
        host.close()
        guest.close()


def test_vehicle_result_host_only_relay(exe, tmp):
    log("[test] v20 vehicle result: host->guest relay, guest sender dropped")
    require(VEHICLE_AVAILABLE, "v20 vehicle messages missing")
    with Server(exe, tmp / "vehicle_result") as srv:
        host, guest = _fast_pair(srv)
        payload = vehicle_result(epoch=3, vehicle_net_id=42, seq=5, action=1,
                                 result=0, detail=0)
        host.sendall(frame_bytes(TYPE_VEHICLE_INTERACTION_RESULT, payload))
        require(recv_type(guest, TYPE_VEHICLE_INTERACTION_RESULT, 2.0) == payload,
                "VehicleInteractionResult must relay host->guest")
        require(recv_type(host, TYPE_VEHICLE_INTERACTION_RESULT, 0.4) is None,
                "VehicleInteractionResult must not echo back to host")
        guest.sendall(frame_bytes(TYPE_VEHICLE_INTERACTION_RESULT, payload))
        require(recv_type(host, TYPE_VEHICLE_INTERACTION_RESULT, 0.4) is None,
                "guest result must be dropped (host-only)")
        srv.wait_log_contains("VehicleInteractionResult от не-хоста", 2.0)
        host.close()
        guest.close()


def test_vehicle_result_validation_rate(exe, tmp):
    log("[test] v20 vehicle result: validation + rate-limit 20/s")
    require(VEHICLE_AVAILABLE, "v20 vehicle messages missing")
    limit = VEHICLE_RATE_RESULT
    with Server(exe, tmp / "vehicle_result_validate") as srv:
        host, guest = _fast_pair(srv)

        def expect_drop(payload, what):
            host.sendall(frame_bytes(TYPE_VEHICLE_INTERACTION_RESULT, payload))
            require(recv_type(guest, TYPE_VEHICLE_INTERACTION_RESULT, 0.35) is None,
                    f"VehicleInteractionResult({what}) must be dropped")

        good = vehicle_result()
        expect_drop(good[:-1], "short 15 B")
        expect_drop(vehicle_result(epoch=0), "epoch=0")
        expect_drop(vehicle_result(vehicle_net_id=0), "netId=0")
        expect_drop(vehicle_result(seq=0), "seq=0")
        expect_drop(vehicle_result(action=0), "action 0")
        expect_drop(vehicle_result(action=13), "action 13")
        expect_drop(vehicle_result(result=10), "result 10")
        for _ in range(limit * 2):
            host.sendall(frame_bytes(TYPE_VEHICLE_INTERACTION_RESULT, good))
        got = _recv_count(guest, TYPE_VEHICLE_INTERACTION_RESULT, 1.2)
        require(got == limit,
                f"VehicleInteractionResult rate-limit: expected {limit}/s, got {got}")
        host.close()
        guest.close()


def test_vehicle_state_host_only_relay(exe, tmp):
    log("[test] v20 vehicle state: host->guest relay + validation + rate 10/s")
    require(VEHICLE_AVAILABLE, "v20 vehicle messages missing")
    limit = VEHICLE_RATE_STATE
    with Server(exe, tmp / "vehicle_state") as srv:
        host, guest = _fast_pair(srv)
        payload = vehicle_state(epoch=3, vehicle_net_id=42, body_health=750,
                                engine_health=800, door_bits=0x0005, seq=7,
                                flags=3, wheels_bits=2, host_seat=-1,
                                guest_seat=0)
        host.sendall(frame_bytes(TYPE_VEHICLE_STATE, payload))
        require(recv_type(guest, TYPE_VEHICLE_STATE, 2.0) == payload,
                "VehicleState must relay host->guest byte-identical")
        guest.sendall(frame_bytes(TYPE_VEHICLE_STATE, payload))
        require(recv_type(host, TYPE_VEHICLE_STATE, 0.4) is None,
                "guest VehicleState must be dropped (host-only)")
        srv.wait_log_contains("VehicleState от не-хоста", 2.0)

        def expect_drop(p, what):
            host.sendall(frame_bytes(TYPE_VEHICLE_STATE, p))
            require(recv_type(guest, TYPE_VEHICLE_STATE, 0.35) is None,
                    f"VehicleState({what}) must be dropped")

        expect_drop(payload[:-1], "short 23 B")
        expect_drop(vehicle_state(epoch=0), "epoch=0")
        expect_drop(vehicle_state(vehicle_net_id=0), "netId=0")
        expect_drop(vehicle_state(body_health=1001), "bodyHealth 1001")
        expect_drop(vehicle_state(engine_health=-4001), "engineHealth -4001")
        expect_drop(vehicle_state(host_seat=9), "hostSeat 9")
        for _ in range(limit * 2):
            host.sendall(frame_bytes(TYPE_VEHICLE_STATE, payload))
        got = _recv_count(guest, TYPE_VEHICLE_STATE, 1.2)
        require(got == limit,
                f"VehicleState rate-limit: expected {limit}/s, got {got}")
        host.close()
        guest.close()


def test_vehicle_interaction_no_host_drop(exe, tmp):
    log("[test] v20 vehicle interaction: host-sender dropped, server survives")
    require(VEHICLE_AVAILABLE, "v20 vehicle messages missing")
    with Server(exe, tmp / "vehicle_no_host") as srv:
        lone = connect(srv.port)
        join(lone, "Lone")
        require(recv_type(lone, TYPE_JOIN_OK, 2.0) is not None,
                "lone host JOIN_OK")
        # Единственный клиент назначен хостом: 0x23 от хоста — role violation,
        # молча drop + warn; сервер жив и принимает второго игрока.
        intent = vehicle_interaction()
        lone.sendall(frame_bytes(TYPE_VEHICLE_INTERACTION, intent))
        require(recv_type(lone, TYPE_VEHICLE_INTERACTION_RESULT, 0.4) is None,
                "host intent must be dropped (guest-only)")
        srv.wait_log_contains("VehicleInteraction от хоста", 2.0)
        second = connect(srv.port)
        join(second, "Second")
        require(recv_type(second, TYPE_JOIN_OK, 2.0) is not None,
                "server must accept a second JOIN after dropped host intent")
        second.sendall(frame_bytes(TYPE_VEHICLE_INTERACTION, intent))
        require(recv_type(lone, TYPE_VEHICLE_INTERACTION, 2.0) == intent,
                "guest intent must relay to host after recovery")
        lone.close()
        second.close()


def test_vehicle_interaction_after_migration(exe, tmp):
    log("[test] v20 vehicle interaction: relay to promoted host after migration")
    require(VEHICLE_AVAILABLE, "v20 vehicle messages missing")
    with Server(exe, tmp / "vehicle_migration") as srv:
        host, guest = _fast_pair(srv)
        intent = vehicle_interaction(seq=1)
        guest.sendall(frame_bytes(TYPE_VEHICLE_INTERACTION, intent))
        require(recv_type(host, TYPE_VEHICLE_INTERACTION, 2.0) == intent,
                "intent must relay to original host")
        host.sendall(frame_bytes(TYPE_LEAVE, b""))
        # Гость повышен до хоста; старые результаты/интенты не ломают сервер.
        promoted = None
        deadline = time.time() + 3.0
        while time.time() < deadline and promoted is None:
            for t, p in recv_msgs(guest, 0.2):
                if t == TYPE_HOST_CHANGED and p and p[0] == 1:
                    promoted = p
        require(promoted is not None, "guest must be promoted after host Leave")
        new_peer = connect(srv.port)
        join(new_peer, "NewGuest")
        require(recv_type(new_peer, TYPE_JOIN_OK, 2.0) is not None,
                "new guest JOIN_OK after migration")
        intent2 = vehicle_interaction(seq=2)
        new_peer.sendall(frame_bytes(TYPE_VEHICLE_INTERACTION, intent2))
        require(recv_type(guest, TYPE_VEHICLE_INTERACTION, 2.0) == intent2,
                "intent must relay to promoted host")
        # Старый (уже гость) результат: drop, no crash.
        new_peer.sendall(frame_bytes(TYPE_VEHICLE_INTERACTION_RESULT,
                                     vehicle_result()))
        require(recv_type(guest, TYPE_VEHICLE_INTERACTION_RESULT, 0.4) is None,
                "guest result must be dropped after migration")
        guest.close()
        new_peer.close()


def test_actor_gating_no_actor_log(exe, tmp):
    log("[test] v15 gating: no 'actor:' log lines in a normal session")
    # Server-side gating only: mock clients never send actor messages, so this
    # test is independent of the client-side npc_actor flag and remains valid
    # for E1. Client-side gating (npc_actor=0 -> 0 actor:/probe: lines in
    # rdr2-coop.log) is verified in-game by docs/NPC_E1_TESTING.md (S0/S9).
    with Server(exe, tmp / "actor_gating") as srv:
        host, guest = _actor_pair(srv)
        host.sendall(frame_bytes(TYPE_FRAME, make_frame_payload()))
        require(recv_type(guest, TYPE_FRAME, 2.0) is not None,
                "normal Frame relay broken")
        host.close()
        guest.close()
        srv.wait_log_contains("отключился", 4.0)
        text = srv.log_text()
        actor_lines = [line for line in text.splitlines() if "actor:" in line]
        require(not actor_lines,
                "server logged actor: lines in a session without actor "
                f"messages: {actor_lines[:5]}")


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


def test_host_game_timeout_ping_only(exe, tmp):
    log("[test] host game-timeout: soft migration (demote, no socket close)")
    # NEW SEMANTICS (task-175): a host that stops sending game-thread messages is
    # demoted and the other player is promoted. The server keeps the old host's
    # socket open (no reconnect storm); only idle/connection cleanup may close it.
    with Server(exe, tmp / "host_game_timeout", host_game_ms=2000) as srv:
        host, guest = _fast_pair(srv)
        # One game-thread message switches the host from join-grace to the normal
        # timeout window. After this the host sends only Ping (network thread),
        # which must NOT keep host liveness alive.
        host.sendall(frame_bytes(TYPE_PLAYER_PRESENCE,
                                 player_presence(epoch=1, flags=0)))

        promoted = None
        demoted = None
        got_left = False
        host_closed = False
        deadline = time.time() + 10.0
        while time.time() < deadline and promoted is None:
            if not host_closed:
                try:
                    host.sendall(frame_bytes(TYPE_PING, struct.pack("<Q", 123)))
                except OSError:
                    host_closed = True
            for t, p in recv_msgs(guest, 0.2):
                if t == TYPE_PLAYER_LEFT:
                    got_left = True
                elif t == TYPE_HOST_CHANGED:
                    promoted = p
            for t, p in recv_msgs(host, 0.05):
                if t == TYPE_HOST_CHANGED:
                    demoted = p

        require(not host_closed,
                "soft migration must not close the old host socket")
        require(not got_left,
                "soft migration must NOT remove the old host (no PlayerLeft)")
        require(promoted is not None and promoted[0] == 1,
                f"guest must be promoted after host game-timeout, got {promoted}")
        require(demoted is not None and demoted[0] == 0,
                f"old host must receive HostChanged(isHost=0), got {demoted}")
        text = srv.wait_log_contains("host game-timeout", 3.0)
        require("host game-timeout" in text,
                "server log must contain 'host game-timeout'")
        require("promoting remaining" in text,
                "server log must contain 'promoting remaining'")
        # Old host stays a connected regular client: Ping/Pong still works.
        host.sendall(frame_bytes(TYPE_PING, struct.pack("<Q", 777)))
        pong = recv_type(host, TYPE_PONG, 2.0)
        require(pong == struct.pack("<Q", 777),
                "demoted host must stay connected and answer Pings")
        host.close()
        guest.close()

    # Presence is game-thread liveness: a host that sends no Frame but keeps
    # PlayerPresence heartbeats must not be demoted.
    with Server(exe, tmp / "host_game_presence", host_game_ms=2000) as srv:
        host, guest = _fast_pair(srv)
        end = time.time() + 5.0
        changed = None
        while time.time() < end and changed is None:
            host.sendall(frame_bytes(TYPE_PLAYER_PRESENCE,
                                     player_presence(epoch=1, flags=0)))
            for t, p in recv_msgs(guest, 0.2):
                if t == TYPE_HOST_CHANGED:
                    changed = p
        require(changed is None,
                f"presence-only host must stay host, got HostChanged {changed}")
        require("host game-timeout" not in srv.log_text(),
                "presence-only host must not trigger host game-timeout")
        host.close()
        guest.close()


def test_host_game_join_grace(exe, tmp):
    log("[test] host game-timeout: join grace (no game-thread msg yet)")
    # task-175 invariant: until the host sends its FIRST game-thread message it
    # gets kHostGameGraceMs (15 s) from JOIN/promotion. The normal game-timeout
    # (here 2000 ms) must NOT demote a still-loading host inside that grace.
    # Host is realistic during load: the network thread keeps Pinging (Pong
    # proves the socket is alive), the game thread has not started yet.
    with Server(exe, tmp / "host_game_grace", host_game_ms=2000) as srv:
        host, guest = _fast_pair(srv)
        end = time.time() + 3.0  # > game-timeout 2 s, well inside 15 s grace
        changed = None
        host_closed = False
        while time.time() < end and changed is None:
            if not host_closed:
                try:
                    host.sendall(frame_bytes(TYPE_PING, struct.pack("<Q", 55)))
                except OSError:
                    host_closed = True
            for t, p in recv_msgs(guest, 0.2):
                if t == TYPE_HOST_CHANGED:
                    changed = p
            recv_msgs(host, 0.05)  # drain Pong
        require(not host_closed,
                "host socket must stay open inside the join grace")
        require(changed is None,
                f"host inside join grace must NOT be demoted, got HostChanged {changed}")
        require("host game-timeout" not in srv.log_text(),
                "server must not apply game-timeout inside the join grace")
        host.close()
        guest.close()


def test_host_game_timeout_frame_activity(exe, tmp):
    log("[test] host game-timeout: Frame activity keeps host alive")
    # host_game timeout 2500 мс; Frame каждые ~0.2 с продлевает хоста.
    with Server(exe, tmp / "host_game_keepalive", host_game_ms=2500) as srv:
        host, guest = _fast_pair(srv)

        end = time.time() + 2.0
        while time.time() < end:
            # Frame приходит из игрового потока каждый tick — продлевает хоста.
            host.sendall(frame_bytes(TYPE_FRAME, make_frame_payload()))
            msgs = recv_msgs(guest, 0.2)
            require(not any(t == TYPE_PLAYER_LEFT for t, _ in msgs),
                    "active host must NOT be removed by host game-timeout")
            require(not any(t == TYPE_HOST_CHANGED for t, _ in msgs),
                    "guest must NOT be promoted while host sends Frames")
        require("host game-timeout" not in srv.log_text(),
                "server must not log host game-timeout for an active host")
        host.close()
        guest.close()


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
                    ln = rng.choice([0, 8, 24, 88, OUTFIT_SIZE, 2000])
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
                    ln = rng.choice([0, 1, 8, 12, 16, 24, 32, 88, OUTFIT_SIZE, 1500])
                    try:
                        sock.sendall(struct.pack("<I", ln) + bytes([t]) +
                                     bytes([rng.randrange(256) for _ in range(ln)]))
                    except OSError:
                        break
                    time.sleep(0.003)
            except OSError:
                pass

        def drain_worker(sock):
            # Server closes a socket whose outgoing queue exceeds 512 KB
            # (anti-OOM for a non-reading client). A real client keeps reading
            # relayed traffic, so the test must drain host/guest during the
            # storm; otherwise the queue cap legitimately closes the socket.
            sock.settimeout(0.1)
            while not stop.is_set():
                try:
                    if not sock.recv(65536):
                        return
                except socket.timeout:
                    continue
                except OSError:
                    return

        threads = [threading.Thread(target=rogue_worker, args=(100 + i,),
                                    daemon=True)
                   for i in range(3)]
        threads += [threading.Thread(target=sender_worker, args=(host, 200),
                                     daemon=True),
                    threading.Thread(target=sender_worker, args=(guest, 201),
                                     daemon=True),
                    threading.Thread(target=drain_worker, args=(host,),
                                     daemon=True),
                    threading.Thread(target=drain_worker, args=(guest,),
                                     daemon=True)]
        for th in threads:
            th.start()
        try:
            time.sleep(3.0)
        finally:
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


def _burst_connections(port, count, timeout=1.0):
    socks = []
    for _ in range(count):
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
            s.settimeout(0.5)
            socks.append(s)
        except OSError:
            pass
    return socks


def test_per_ip_rate_limit(exe, tmp):
    log("[test] per-IP rate limit (real assert on rejected connections)")
    with Server(exe, tmp / "rate") as srv:
        # Burst must fit into one server rate window to be deterministic.
        # The server counts connections per fixed 1 s window and rejects above
        # CAP_RATE/s; retry with a fresh window if the machine is slow.
        total = CAP_RATE * 2
        min_events = max(4, CAP_RATE // 2)
        rate_events = 0
        closed = 0
        still_open = 0
        text = ""
        for _attempt in range(3):
            time.sleep(1.05)  # fresh rate window
            socks = _burst_connections(srv.port, total)
            time.sleep(0.4)
            text = srv.wait_log_contains("per-IP rate limit", timeout=2.0)
            rate_events = text.count("per-IP rate limit")
            closed, still_open = classify_connections(socks)
            for s in socks:
                try:
                    s.close()
                except OSError:
                    pass
            if rate_events >= min_events and closed >= min_events:
                break
            time.sleep(0.2)

        require(rate_events >= min_events,
                f"per-IP rate limit rejected too few connections: "
                f"{rate_events} log entries (need >= {min_events}); "
                f"opened={total}, rate={CAP_RATE}/s. log tail:\n{tail(text)}")
        require(closed >= min_events,
                f"server closed only {closed} sockets, "
                f"expected >= {min_events}")
        require(still_open <= CAP_RATE + 2,
                f"too many connections survived rate limit: {still_open}")
        require("лимит соединений" not in text,
                "active connection cap hit before the per-IP rate limit")

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
    run_lock = IntegrationRunLock()
    run_lock.acquire()
    failures = 0
    known_fails = 0
    passed = 0
    skipped = 0
    tests = [
        test_join_rejections,
        test_join_name_validation,
        test_roles_and_full,
        test_prejoin_and_ping,
        test_world_host_only,
        test_payload_sizes_and_host_only_messages,
        test_relay_misc_messages,
        test_worldstate_host_only,
        # Protocol v15 (E0) actor tests + v16 (E2c-b) Appearance + v17 (E5) Damage.
        test_actor_spawn_host_only,
        test_actor_frames_host_only,
        test_actor_despawn_event_outfit,
        test_actor_resync_direction,
        test_actor_validation_drops,
        test_actor_duplicate_actor_ids,
        test_actor_appearance_host_only,
        test_actor_appearance_validation,
        test_actor_damage_direction,
        test_actor_damage_validation,
        test_player_presence_relay,
        test_player_presence_validation,
        test_vehicle_occupants_host_only,
        test_vehicle_occupants_validation,
        test_vehicle_occupants_rate_limit,
        test_train_v2_host_only,
        test_train_v2_validation,
        test_train_v2_rate_limit,
        test_vehicle_interaction_guest_only_relay,
        test_vehicle_interaction_validation,
        test_vehicle_interaction_rate_limit,
        test_vehicle_result_host_only_relay,
        test_vehicle_result_validation_rate,
        test_vehicle_state_host_only_relay,
        test_vehicle_interaction_no_host_drop,
        test_vehicle_interaction_after_migration,
        test_actor_gating_no_actor_log,
        test_oversized_and_truncated,
        test_host_migration,
        test_host_game_timeout_ping_only,
        test_host_game_join_grace,
        test_host_game_timeout_frame_activity,
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
            if not ACTOR_AVAILABLE and test.__name__ in ACTOR_TESTS:
                skipped += 1
                log(f"[skip] {test.__name__}: protocol v15 Actor* messages are "
                    "not in net/protocol.h yet (E0 merge pending)")
                continue
            if test.__name__ in APPEARANCE_TESTS and not APPEARANCE_AVAILABLE:
                skipped += 1
                log(f"[skip] {test.__name__}: protocol v16 ActorAppearance is "
                    "not in net/protocol.h yet (E2c-b merge pending)")
                continue
            if test.__name__ in DAMAGE_TESTS and not DAMAGE_AVAILABLE:
                skipped += 1
                log(f"[skip] {test.__name__}: protocol v17 ActorDamage is "
                    "not in net/protocol.h yet (E5 merge pending)")
                continue
            if test.__name__ in PRESENCE_TESTS and not PRESENCE_AVAILABLE:
                skipped += 1
                log(f"[skip] {test.__name__}: protocol v18 PlayerPresence is "
                    "not in net/protocol.h yet (E6 merge pending)")
                continue
            if test.__name__ in TRAFFIC_TESTS and not TRAFFIC_AVAILABLE:
                skipped += 1
                log(f"[skip] {test.__name__}: protocol v19 E3d traffic is "
                    "not in net/protocol.h yet (E3d merge pending)")
                continue
            if test.__name__ in VEHICLE_TESTS and not VEHICLE_AVAILABLE:
                skipped += 1
                log(f"[skip] {test.__name__}: protocol v20 P1 vehicle is "
                    "not in net/protocol.h yet (U1 merge pending)")
                continue
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
        skipped_part = f", {skipped} skipped (E0 pending)" if skipped else ""
        appearance_status = ("pending" if not APPEARANCE_AVAILABLE else
                             f"on({MAX_ACTOR_APPEARANCE}comp/"
                             f"{ACTOR_APPEARANCE_MAX}B)")
        damage_status = ("pending" if not DAMAGE_AVAILABLE else
                         f"on({ACTOR_DAMAGE_SIZE}B)")
        presence_status = ("pending" if not PRESENCE_AVAILABLE else
                           f"on({PLAYER_PRESENCE_SIZE}B)")
        traffic_status = ("pending" if not TRAFFIC_AVAILABLE else
                          f"on(occ={OCCUPANTS_MAX}B,train={TRAIN_V2_SIZE}B)")
        vehicle_status = ("pending" if not VEHICLE_AVAILABLE else
                          f"on(0x23={VEHICLE_INTERACTION_SIZE}B,"
                          f"0x24={VEHICLE_RESULT_SIZE}B,"
                          f"0x26={VEHICLE_STATE_SIZE}B)")

        log(f"{passed} passed, {failures} failed, {known_fails} known-fail"
            f"{skipped_part} "
            f"(protocol v{PROTO}, outfit={OUTFIT_SIZE}B, "
            f"actors={'on' if ACTOR_AVAILABLE else 'pending'}, "
            f"appearance={appearance_status}, "
            f"damage={damage_status}, "
            f"presence={presence_status}, "
            f"traffic={traffic_status}, "
            f"vehicle={vehicle_status}, "
            f"cap={CAP_ACTIVE}/rate={CAP_RATE}/s, "
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
        run_lock.release()


if __name__ == "__main__":
    sys.exit(main())
