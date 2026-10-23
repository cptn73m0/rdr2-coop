#!/usr/bin/env python3
"""Headless fake protocol-v15 player for single-PC NPC replication tests.

Two modes (Python 3 standard library only):

    # 1) fake HOST: run it BEFORE launching the game as a guest
    python tools\\fake_actor_client.py --mode host --name TestHost --duration 300

    # 2) fake GUEST: run it AFTER the game is already connected as host
    python tools\\fake_actor_client.py --mode guest --name TestGuest --duration 120

Host mode sends ActorSpawnBatch/ActorFrames/ActorDespawn to the real game
guest (E1 `npc_actor=1`): 5 human NPCs appear next to the in-game guest.
Guest mode only receives/validates actor messages from the real game host.

Protocol v15, framing: [len:u32 LE][type:u8][payload]. This tool does not
implement the client-side FPS/game logic; it only joins the relay server and
speaks the wire protocol. See tools/README.md for the full instructions.
"""

from __future__ import annotations

import argparse
import json
import math
import re
import socket
import struct
import sys
import time
from pathlib import Path

# ---------------------------------------------------------------------------
# Protocol constants (must match net/protocol.h, protocol v15).
# ---------------------------------------------------------------------------
PROTOCOL_VERSION = 15
MSG_JOIN = 0x01
MSG_JOIN_OK = 0x02
MSG_JOIN_REJECTED = 0x0F
MSG_FRAME = 0x04
MSG_LEAVE = 0x05
MSG_PING = 0x0C
MSG_PONG = 0x0D
MSG_ACTOR_SPAWN_BATCH = 0x18
MSG_ACTOR_FRAMES = 0x19
MSG_ACTOR_DESPAWN = 0x1A
MSG_ACTOR_EVENT = 0x1B
MSG_ACTOR_OUTFIT = 0x1C
MSG_ACTOR_RESYNC = 0x1D

K_MAX_ACTORS = 12
K_MAX_PAYLOAD = 64 * 1024
ACTOR_SPAWN_SIZE = 64
ACTOR_FRAME_SIZE = 32
ACTOR_SPAWN_BATCH_HEADER = 8
ACTOR_FRAMES_HEADER = 8
ACTOR_DESPAWN_SIZE = 12
ACTOR_RESYNC_SIZE = 12
FRAME_MIN_SIZE = 12  # x,y,z,heading; only the first 12 bytes are needed here

DEFAULT_MODEL = 0x54D8BE8E  # a_m_m_rancher_01 (same as tools/fake_client.ps1)
PEDS_JSON = Path(__file__).resolve().parents[1] / "third_party" / "rdr2-hashdb" / "Peds.json"
DEFAULT_LOG = Path(__file__).resolve().with_name("fake_actor_client.log")
DEFAULT_PROBE_LOG = Path(r"E:\Games\Red Dead Redemption 2\scripts\rdr2-coop.log")
PROBE_OUTFIT_RE = re.compile(
    r"probe:\s*model=0x([0-9A-Fa-f]+)\s+metaOutfit=0x([0-9A-Fa-f]+)")
SCENARIO_HASH_RE = re.compile(r"dynScenario=0x([0-9A-Fa-f]+)")

HUMAN_PREFIXES = ("A_M_M_", "A_M_O_", "G_M_M_", "S_M_M_")
HORSE_PREFIX = "A_C_HORSE_"
# E2b fallback horse models (A_C_HORSE_* from Peds.json) if the DB is missing.
DEFAULT_MOUNT_MODELS = [
    2295768478,  # A_C_HORSE_ARABIAN_BLACK
    3844913586,  # A_C_HORSE_AMERICANPAINT_OVERO
    3850356320,  # A_C_HORSE_ANDALUSIAN_DARKBAY
    3266891371,  # A_C_HORSE_APPALOOSA_BLACKSNOWFLAKE
]
K_ACTOR_FLAG_MOUNTED = 1 << 0
K_ACTOR_FLAG_DEAD = 1 << 1
K_ACTOR_FLAG_DRAWN = 1 << 2
K_ACTOR_FLAG_CHANGING = 1 << 3
K_ACTOR_FLAG_COMBAT = 1 << 4
K_ACTOR_FLAG_SCENARIO = 1 << 5
K_ACTOR_FLAG_RELOADING = 1 << 6
K_ACTOR_FLAG_AIMING = 1 << 7
K_MOVE_STYLE_RIDE_HORSE = 3
# E3b: RDR2 weapon hashes are NOT joaat of the weapon name. These are verified
# against third_party/rdr2-hashdb/CatalogItems.json:
#   WEAPON_REVOLVER_CATTLEMAN=0x169F59F7, WEAPON_SHOTGUN_DOUBLEBARREL=0x6DFA071B,
#   WEAPON_REPEATER_CARBINE=0xF5175BA1, WEAPON_PISTOL_M1899=0x5B78B8DD,
#   WEAPON_RIFLE_SPRINGFIELD=0x63F46DE6.
# 0x8B264946 (joaat of the name) is invalid; 0x67DC3FDE is WEAPON_MELEE_TORCH.
DEFAULT_WEAPON_HASH = 0x169F59F7
DEFAULT_WEAPON_LIST = (
    0x169F59F7,  # WEAPON_REVOLVER_CATTLEMAN
    0x6DFA071B,  # WEAPON_SHOTGUN_DOUBLEBARREL
    0xF5175BA1,  # WEAPON_REPEATER_CARBINE
    0x5B78B8DD,  # WEAPON_PISTOL_M1899
    0x63F46DE6,  # WEAPON_RIFLE_SPRINGFIELD
)
# E3a fallback scenario hashes (dynScenario values seen in probe logs).
FALLBACK_SCENARIO_HASHES = [
    0x62A4554D, 0xF6CD006D, 0x4BF802FE, 0x85AE7E5D, 0x189292F9,
]
# Valid RDR2 scenario names for TASK_START_SCENARIO_IN_PLACE_HASH (E3a).
# Defaults are "standing" scenarios that need no props (cleared field test);
# chair/campfire scenarios require a matching prop, pass them via --scenario-names.
# Hashes are joaat(lowercase_name).
DEFAULT_SCENARIO_NAMES = (
    "WORLD_HUMAN_SMOKE_CIGARETTE",
    "WORLD_HUMAN_STAND_IMPATIENT",
)


def joaat(name: str) -> int:
    """RDR2/Jenkins one-at-a-time hash (lowercase), used for scenario hashes."""
    h = 0
    for c in str(name).lower().encode("utf-8"):
        h = (h + c) & 0xFFFFFFFF
        h = (h + (h << 1) + (h << 4) + (h << 7) + (h << 8) + (h << 24)) & 0xFFFFFFFF
    return h & 0xFFFFFFFF
# Prefer ordinary town/ranch humans for a believable test crowd; avoid corpse/
# undead variants that are technically human peds but look broken as NPCs.
MODEL_GOOD_WORDS = ("TOWNFOLK", "RANCHER", "FARMER", "LABOURER", "LIVERY",
                    "CITIZEN", "VAGRANT", "TRAMP", "BEGGAR", "DRIVER")
MODEL_BAD_WORDS = ("CORPSE", "UNDEAD", "ZOMBIE", "SKELETON", "MUTANT",
                   "VAMPIRE", "HANGED", "DECOMPOSED")
JOIN_REJECT_NAMES = {1: "VersionMismatch", 2: "ServerFull", 3: "NameTaken", 4: "BadName"}


# ---------------------------------------------------------------------------
# Logging: stdout + append to a log file (both with monotonic wall time).
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
    """Read up to one recv() chunk and return all complete frames."""
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


def send_join(sock: socket.socket, name: str, model: int, version: int = PROTOCOL_VERSION) -> None:
    name_bytes = name.encode("utf-8")
    if not name_bytes or len(name_bytes) > 32:
        raise ValueError(f"name must be 1..32 UTF-8 bytes, got {len(name_bytes)}")
    payload = bytes([len(name_bytes)]) + name_bytes + \
        struct.pack("<Q", model & 0xFFFFFFFFFFFFFFFF) + \
        struct.pack("<H", version)
    sock.sendall(make_frame(MSG_JOIN, payload))


def parse_join_ok(payload: bytes):
    """Actual server layout: sid:u64, has_saved:u8, [x,y,z,heading:f32], is_host:u8."""
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


def parse_join_rejected(payload: bytes) -> int:
    return payload[0] if payload else 0


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


def join_session(sock: socket.socket, name: str, model: int, log: Log,
                 tag: str = "fake", timeout: float = 10.0):
    """JOIN and wait for JoinOk (or JoinRejected). Returns (sid, coords, is_host)."""
    reader = FrameReader()
    send_join(sock, name, model)
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
                return sid, coords, is_host
            if msg_type == MSG_JOIN_REJECTED:
                reason = parse_join_rejected(payload)
                raise RuntimeError(
                    f"JOIN rejected: {reason} "
                    f"({JOIN_REJECT_NAMES.get(reason, 'unknown')})")
    raise TimeoutError("no JoinOk in time; is the server running and v15?")


# ---------------------------------------------------------------------------
# Actor payload builders / validators (struct layouts from net/protocol.h).
# ---------------------------------------------------------------------------
def pack_actor_spawn(actor_id: int, epoch: int, model: int,
                     x: float, y: float, z: float, heading: float,
                     health: int = 200, move_style: int = 0,
                     meta_outfit: int = 0, flags: int = 0,
                     mount_model: int = 0, scenario_hash: int = 0,
                     weapon_hash: int = 0) -> bytes:
    return struct.pack(
        "<IHBBIIIIIffffBBBBhhhHII",
        actor_id, epoch & 0xFFFF, 0, flags & 0xFF,  # actorId/epoch/kind/flags
        model & 0xFFFFFFFF, meta_outfit & 0xFFFFFFFF,
        0, weapon_hash & 0xFFFFFFFF,              # mountActorId, weaponHash
        scenario_hash & 0xFFFFFFFF,              # scenarioHash
        float(x), float(y), float(z), float(heading),
        health & 0xFF, move_style & 0xFF, 0, 0,  # health, moveStyle, phase, pad
        0, 0, 0, 0,                              # vx, vy, vz, pad2
        mount_model & 0xFFFFFFFF, 0,             # reserved = mount model, reserved2
    )


def pack_spawn_batch(epoch: int, records) -> bytes:
    return struct.pack("<IHH", epoch, len(records), 0) + b"".join(records)


def pack_actor_frame(actor_id: int, x: float, y: float, z: float,
                     heading: float, seq: int, anim_state: int = 1,
                     speed: int = 0, pad: int = 0, flags: int = 0,
                     target_actor_id: int = 0) -> bytes:
    return struct.pack("<IHBBffffIHH", actor_id, flags & 0xFFFF,
                       anim_state & 0xFF, speed & 0xFF,
                       float(x), float(y), float(z),
                       float(heading), target_actor_id & 0xFFFFFFFF,
                       seq & 0xFFFF, pad & 0xFFFF)


def pack_frames_batch(epoch: int, seq: int, records) -> bytes:
    return struct.pack("<IHH", epoch, seq & 0xFFFF, len(records)) + b"".join(records)


def pack_actor_despawn(epoch: int, actor_id: int, reason: int) -> bytes:
    return struct.pack("<IIB3x", epoch, actor_id, reason & 0xFF)


def is_finite_coord(v: float) -> bool:
    return math.isfinite(v) and abs(v) < 1.0e5


# E2a.2: ActorFrame.speed is u8 on the 0..8 m/s scale; ActorFrame.pad is
# GET_PED_DESIRED_MOVE_BLEND_RATIO*1000 (gait: walk=1000, run=2000, ...).
K_SPEED_SCALE_MPS = 8.0
K_GAIT_MAX = 6.5


def encode_speed_u8(speed_m_s: float) -> int:
    value = int(round(max(0.0, speed_m_s) / K_SPEED_SCALE_MPS * 255.0))
    return max(0, min(255, value))


def encode_gait_pad(gait: float) -> int:
    value = int(round(max(0.0, min(K_GAIT_MAX, gait)) * 1000.0))
    return max(0, min(65535, value))


def move_style_for_gait(gait: float) -> int:
    if gait >= 3.0:
        return 3  # sprint
    if gait >= 2.0:
        return 2  # run
    if gait >= 0.3:
        return 1  # walk
    return 0      # idle


def combat_cycle_enabled(args) -> bool:
    """E3b.3: repeating combat cycles are active only for a combat run."""
    cycle = getattr(args, "combat_cycle", 0.0) or 0.0
    if cycle <= 0:
        return False
    if getattr(args, "combat", False) or getattr(args, "drawn", False):
        return True
    return any(getattr(args, name, 0.0) > 0 for name in
               ("aim_after", "shoot_after", "reload_after"))


def validate_spawn_batch(payload: bytes):
    """Return (epoch, records) or raise ValueError with a short reason."""
    if len(payload) < ACTOR_SPAWN_BATCH_HEADER:
        raise ValueError("spawn batch shorter than header")
    epoch, count, _pad = struct.unpack_from("<IHH", payload, 0)
    if epoch == 0:
        raise ValueError("spawn batch epoch=0")
    if count > K_MAX_ACTORS:
        raise ValueError(f"spawn batch count={count} > {K_MAX_ACTORS}")
    need = ACTOR_SPAWN_BATCH_HEADER + count * ACTOR_SPAWN_SIZE
    if len(payload) != need:
        raise ValueError(f"spawn batch size={len(payload)} != {need}")
    records = []
    seen = set()
    for i in range(count):
        rec = payload[ACTOR_SPAWN_BATCH_HEADER + i * ACTOR_SPAWN_SIZE:
                      ACTOR_SPAWN_BATCH_HEADER + (i + 1) * ACTOR_SPAWN_SIZE]
        (actor_id, _epoch16, _kind, flags, model, meta, _mount, weapon,
         scenario, x, y, z, heading, health, move_style, _phase, _pad,
         _vx, _vy, _vz, _pad2, reserved, _res2) = struct.unpack_from(
            "<IHBBIIIIIffffBBBBhhhHII", rec, 0)
        if actor_id == 0:
            raise ValueError(f"spawn record #{i}: actorId=0")
        if actor_id in seen:
            raise ValueError(f"spawn batch duplicate actorId={actor_id}")
        seen.add(actor_id)
        if not all(is_finite_coord(v) for v in (x, y, z, heading)):
            raise ValueError(f"spawn actorId={actor_id}: non-finite coords")
        records.append({
            "actorId": actor_id, "model": model, "metaOutfit": meta,
            "x": x, "y": y, "z": z,
            "heading": heading, "health": health, "moveStyle": move_style,
            "flags": flags, "mountModel": reserved,
            "scenarioHash": scenario, "weaponHash": weapon,
        })
    return epoch, records


def validate_frames(payload: bytes):
    """Return (epoch, seq, records)."""
    if len(payload) < ACTOR_FRAMES_HEADER:
        raise ValueError("frames batch shorter than header")
    epoch, seq, count = struct.unpack_from("<IHH", payload, 0)
    if epoch == 0:
        raise ValueError("frames epoch=0")
    if count > K_MAX_ACTORS:
        raise ValueError(f"frames count={count} > {K_MAX_ACTORS}")
    need = ACTOR_FRAMES_HEADER + count * ACTOR_FRAME_SIZE
    if len(payload) != need:
        raise ValueError(f"frames size={len(payload)} != {need}")
    records = []
    seen = set()
    for i in range(count):
        rec = payload[ACTOR_FRAMES_HEADER + i * ACTOR_FRAME_SIZE:
                      ACTOR_FRAMES_HEADER + (i + 1) * ACTOR_FRAME_SIZE]
        (actor_id, flags, anim_state, speed, x, y, z, heading,
         target, rec_seq, pad) = struct.unpack_from("<IHBBffffIHH", rec, 0)
        if actor_id == 0:
            raise ValueError(f"frame #{i}: actorId=0")
        if actor_id in seen:
            raise ValueError(f"frames batch duplicate actorId={actor_id}")
        seen.add(actor_id)
        if not all(is_finite_coord(v) for v in (x, y, z, heading)):
            raise ValueError(f"frame actorId={actor_id}: non-finite coords")
        records.append({"actorId": actor_id, "flags": flags,
                        "animState": anim_state, "targetActorId": target,
                        "speed": speed, "pad": pad, "seq": rec_seq})
    return epoch, seq, records


def validate_despawn(payload: bytes):
    if len(payload) != ACTOR_DESPAWN_SIZE:
        raise ValueError(f"despawn size={len(payload)} != {ACTOR_DESPAWN_SIZE}")
    epoch, actor_id, reason = struct.unpack_from("<IIB", payload, 0)
    if epoch == 0:
        raise ValueError("despawn epoch=0")
    if actor_id == 0:
        raise ValueError("despawn actorId=0")
    return epoch, actor_id, reason


# ---------------------------------------------------------------------------
# Model hashes from the bundled RDR2 hash DB.
# ---------------------------------------------------------------------------
def load_human_models(num: int, peds_json: Path):
    if not peds_json.is_file():
        raise RuntimeError(f"peds hash DB not found: {peds_json}")
    try:
        data = json.loads(peds_json.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise RuntimeError(f"cannot read {peds_json}: {exc}") from exc
    found = []
    for key, name in data.items():
        name_u = str(name).upper()
        if not any(name_u.startswith(p) for p in HUMAN_PREFIXES):
            continue
        try:
            model = int(key) & 0xFFFFFFFF
        except ValueError:
            continue
        if model != 0:
            score = 0
            if any(word in name_u for word in MODEL_GOOD_WORDS):
                score += 3
            if any(word in name_u for word in MODEL_BAD_WORDS):
                score -= 10
            found.append((name_u, model, score))
    found.sort(key=lambda item: (-item[2], item[0]))
    if len(found) < num:
        raise RuntimeError(
            f"only {len(found)} human peds in {peds_json}, need {num}")
    selected = found[:num]
    return [m for _n, m, _s in selected], [n for n, _m, _s in selected]


def parse_model_list(text: str, num: int):
    models = []
    for part in text.replace(";", ",").split(","):
        part = part.strip()
        if not part:
            continue
        models.append(int(part, 0) & 0xFFFFFFFF)
    if len(models) < num:
        raise ValueError(f"--models needs at least {num} hashes, got {len(models)}")
    return models[:num]


def parse_single_hash(text: str) -> int:
    value = int(str(text), 0) & 0xFFFFFFFF
    if value == 0:
        raise ValueError("hash must be non-zero")
    return value


def load_mount_models(num: int, peds_json: Path):
    """Return up to `num` A_C_HORSE_* model hashes (or the fallback list)."""
    if num <= 0:
        return [], []
    found = []
    if peds_json.is_file():
        try:
            data = json.loads(peds_json.read_text(encoding="utf-8"))
            for key, name in data.items():
                name_u = str(name).upper()
                if not name_u.startswith(HORSE_PREFIX):
                    continue
                try:
                    model = int(key) & 0xFFFFFFFF
                except ValueError:
                    continue
                if model:
                    found.append((name_u, model))
        except (OSError, ValueError):
            found = []
    found.sort(key=lambda item: item[0])
    if not found:
        names = ["A_C_HORSE_FALLBACK_%02d" % (i + 1)
                 for i in range(len(DEFAULT_MOUNT_MODELS))]
        return list(DEFAULT_MOUNT_MODELS), names
    selected = found[:num]
    return [m for _n, m in selected], [n for n, _m in selected]


def load_scenario_hashes(path: Path, wanted: int):
    """Parse unique non-zero `dynScenario=0x...` hashes from a probe log (E3a).

    Falls back to known in-game scenario hashes if the log is missing or has no
    scenario records.
    """
    if wanted <= 0:
        return [], None
    hashes = []
    seen = set()
    if path.is_file():
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as handle:
                for line in handle:
                    match = SCENARIO_HASH_RE.search(line)
                    if not match:
                        continue
                    value = int(match.group(1), 16) & 0xFFFFFFFF
                    if value == 0 or value in seen:
                        continue
                    seen.add(value)
                    hashes.append(value)
        except OSError:
            hashes = []
    if hashes:
        return hashes, None
    fallback = list(FALLBACK_SCENARIO_HASHES)
    warning = (f"no dynScenario hashes in {path}; using fallback "
               f"{len(fallback)} scenario hash(es)")
    return fallback, warning


def load_outfit_pairs(path: Path):
    """Parse `probe: model=0x... metaOutfit=0x...` lines -> [(model, meta), ...].

    Only pairs with metaOutfit != 0 and model != 0 are used (E2a: NPCs whose
    meta-outfit is known and can be sent in ActorSpawn.metaOutfit).
    """
    if not path.is_file():
        return [], f"outfit log not found: {path} (fallback: metaOutfit=0)"
    pairs = []
    seen = set()
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as handle:
            for line in handle:
                match = PROBE_OUTFIT_RE.search(line)
                if not match:
                    continue
                model = int(match.group(1), 16) & 0xFFFFFFFF
                meta = int(match.group(2), 16) & 0xFFFFFFFF
                if model == 0 or meta == 0:
                    continue
                key = (model, meta)
                if key in seen:
                    continue
                seen.add(key)
                pairs.append(key)
    except OSError as exc:
        return [], f"cannot read outfit log {path}: {exc} (fallback: metaOutfit=0)"
    return pairs, None


# ---------------------------------------------------------------------------
# HOST mode: fake host sends 5 NPC actors to the real game guest.
# ---------------------------------------------------------------------------
class Actor:
    def __init__(self, actor_id: int, model: int, meta_outfit: int, center,
                 phase: float, spawn_time: float, motion: str = "circle",
                 direction: float = 0.0, gait: float = 1.0,
                 mounted: bool = False, mount_model: int = 0,
                 scenario: bool = False, scenario_hash: int = 0,
                 scenario_name: str = "", weapon_hash: int = 0):
        self.actor_id = actor_id
        self.model = model
        self.meta_outfit = meta_outfit
        self.center = center  # (x, y, z)
        self.phase = phase
        self.spawn_time = spawn_time
        self.motion = motion  # circle | line | idle
        self.direction = direction
        self.gait = gait  # E2a.2 gait ratio: 0 idle, 1 walk, 2 run
        self.mounted = mounted  # E2b: actor rides a horse
        self.mount_model = mount_model  # E2b: ActorSpawn.reserved
        self.scenario = scenario  # E3a: in scenario (sits/works in place)
        self.scenario_hash = scenario_hash  # E3a: ActorSpawn.scenarioHash
        self.scenario_name = scenario_name  # E3a: human-readable name
        self.weapon_hash = weapon_hash  # E3b: ActorSpawn.weaponHash
        self.dead = False  # E3a: dead flag/health=0 until revive
        self.dead_pos = None  # fixed corpse position
        self.dead_heading = 0.0
        self.combat_hold_key = None  # E3b: static aim/shoot pose cache
        self.combat_hold = None


class FakeHost:
    def __init__(self, args, log: Log):
        self.args = args
        self.log = log
        self.sock = None
        self.epoch = 1
        self.actors = []
        self.frame_seq = 0
        self.frames_sent = 0
        self.spawns_sent = 0
        self.despawns = 0
        self.guest_pos = None
        self.guest_frame_at = None
        self.last_lost_log = 0.0
        self.spawned = False
        self.spawn_time = None
        self.next_despawn = None
        self.next_respawn = None
        self.next_death = None
        self.next_revive = None
        self.next_frame_at = None
        self.next_log_at = None
        self.next_ping_at = 0.0
        self.lost = False
        self.rc = 1
        self._outfit_logged = False
        self._weapon_drawn_logged = False
        self._aim_logged = False
        self._shoot_logged = False
        self._reload_logged = False
        self._reload_state = False
        self._cycle_logged = -1
        self._cycle_phase = None

    # -- actor construction -------------------------------------------------
    @staticmethod
    def _motion_for(index: int, pattern: str) -> str:
        if pattern == "circle":
            return "circle"
        if pattern == "line":
            return "line"
        # mixed: 2 circle, 2 line, 1+ idle (E2a: check standing actors too)
        if index < 2:
            return "circle"
        if index < 4:
            return "line"
        return "idle"

    def _model_and_meta(self, index: int):
        pairs = getattr(self.args, "outfit_pairs", []) or []
        if pairs:
            return pairs[index % len(pairs)]
        return self.args.models[index % max(1, len(self.args.models))], 0

    def _new_actor(self, actor_id: int, center, spawn_time: float) -> Actor:
        index = actor_id - 1
        scenario = actor_id <= self.args.scenario
        if scenario:
            # E3a.4: spread scenario actors on a wide ring so they do not share
            # the same scenario point (the game warps to the nearest one).
            n = max(1, self.args.scenario)
            angle = (2.0 * math.pi * index / n) if n > 1 else math.pi
            dist = self.args.scenario_spacing
        else:
            num = max(1, self.args.num)
            # Distinct spawn ring around the guest: angle 2*pi*i/num, radius 3..6 m.
            # Every actor also moves around its OWN spawn point (a.center).
            angle = (2.0 * math.pi * index / num) if num > 1 else math.pi
            dist = 4.5 if num == 1 else (3.0 + 3.0 * index / (num - 1))
        cx = center[0] + dist * math.cos(angle)
        cy = center[1] + dist * math.sin(angle)
        model, meta = self._model_and_meta(index)
        motion = self._motion_for(index, self.args.pattern)
        gait = 0.0 if motion == "idle" else self.args.gait
        mounted = actor_id <= self.args.mounted
        mount_model = 0
        if mounted and self.args.mount_models:
            mount_model = self.args.mount_models[
                (actor_id - 1) % len(self.args.mount_models)]
        scenario_hash = 0
        scenario_name = ""
        if scenario and self.args.scenario_hashes:
            idx = (actor_id - 1) % len(self.args.scenario_hashes)
            scenario_hash = self.args.scenario_hashes[idx]
            if getattr(self.args, "scenario_names_list", None):
                scenario_name = self.args.scenario_names_list[
                    idx % len(self.args.scenario_names_list)]
        weapon_hash = 0
        if getattr(self.args, "weapon_hashes", None):
            weapon_hash = self.args.weapon_hashes[
                (actor_id - 1) % len(self.args.weapon_hashes)]
        return Actor(actor_id, model, meta, (cx, cy, center[2]), angle,
                     spawn_time, motion, angle, gait, mounted, mount_model,
                     scenario, scenario_hash, scenario_name,
                     weapon_hash=weapon_hash)

    def _make_actors(self, center):
        now = time.monotonic()
        return [self._new_actor(i + 1, center, now)
                for i in range(self.args.num)]

    # -- payload helpers ----------------------------------------------------
    def _spawn_records(self, actors):
        records = []
        for a in actors:
            flags = 0
            if a.mounted:
                flags |= K_ACTOR_FLAG_MOUNTED
            if a.scenario:
                flags |= K_ACTOR_FLAG_SCENARIO
            if a.dead:
                flags |= K_ACTOR_FLAG_DEAD
            if getattr(self.args, "combat", False):
                flags |= K_ACTOR_FLAG_COMBAT
            if a.mounted:
                move_style = K_MOVE_STYLE_RIDE_HORSE
            elif a.scenario or a.dead:
                move_style = 0
            else:
                move_style = move_style_for_gait(a.gait)
            records.append(pack_actor_spawn(
                a.actor_id, self.epoch, a.model,
                a.center[0], a.center[1], a.center[2],
                math.degrees(a.phase) % 360.0,
                health=0 if a.dead else 200,
                move_style=move_style,
                meta_outfit=a.meta_outfit,
                flags=flags,
                mount_model=a.mount_model,
                scenario_hash=a.scenario_hash,
                weapon_hash=a.weapon_hash))
        return records

    def _send_spawn(self, actors, why: str = "") -> None:
        if not actors:
            return
        if not self._outfit_logged:
            self._outfit_logged = True
            self.log.write(
                f"fakehost: outfits pairs={len(getattr(self.args, 'outfit_pairs', []))} "
                f"pattern={self.args.pattern} gait={self.args.gait:.1f} "
                f"mounted={self.args.mounted}/{self.args.num} "
                f"scenario={self.args.scenario}/{self.args.num}")
        payload = pack_spawn_batch(self.epoch, self._spawn_records(actors))
        send_frame(self.sock, MSG_ACTOR_SPAWN_BATCH, payload)
        self.spawns_sent += len(actors)
        self.log.write(f"fakehost: spawn epoch={self.epoch} n={len(actors)}"
                       + (f" ({why})" if why else ""))

    def _motion_pos(self, a: Actor, now: float):
        """Current position/heading of a walking actor (used for death freeze)."""
        t = now - a.spawn_time
        move_speed = (self.args.speed * a.gait) if a.gait > 0 else 0.0
        if a.motion == "idle":
            return (a.center[0], a.center[1],
                    math.degrees(a.phase) % 360.0)
        if a.motion == "line":
            traveled = (t * move_speed) % 8.0
            if traveled < 4.0:
                offset = -2.0 + traveled
                forward = True
            else:
                offset = 2.0 - (traveled - 4.0)
                forward = False
            x = a.center[0] + offset * math.cos(a.direction)
            y = a.center[1] + offset * math.sin(a.direction)
            heading = (math.degrees(a.direction) +
                       (0.0 if forward else 180.0)) % 360.0
            return x, y, heading
        omega = move_speed / max(0.1, self.args.circle_radius)
        theta = a.phase + t * omega
        x = a.center[0] + self.args.circle_radius * math.cos(theta)
        y = a.center[1] + self.args.circle_radius * math.sin(theta)
        return x, y, (math.degrees(theta) + 90.0) % 360.0

    def _combat_state(self, a: Actor, now: float):
        """E3b flags timeline.

        Returns (flags, hold_start|None, targetActorId).
        hold_start is the offset from actor spawn at which x/y/heading must be
        frozen for the whole aim/shoot window (no per-frame heading updates).
        """
        a_args = self.args
        flags = K_ACTOR_FLAG_COMBAT if getattr(a_args, "combat", False) else 0
        if a.scenario or a.dead:
            return flags, None, 0
        elapsed = now - a.spawn_time
        if combat_cycle_enabled(a_args):
            # E3b.3 repeating cycle: +0..2 Drawn, +2..7 Aim, +7..10 Shoot
            # (targetActorId=1), +12..13 Reload. Pose is frozen for +2..10.
            cycle = a_args.combat_cycle
            cycle_idx = int(elapsed // cycle)
            ce = elapsed % cycle
            aim_active = 2.0 <= ce < 7.0
            shoot_active = 7.0 <= ce < 10.0
            reload_active = 12.0 <= ce < 13.0
            if 0.0 <= ce < 10.0:
                flags |= K_ACTOR_FLAG_DRAWN
            if aim_active or shoot_active:
                flags |= K_ACTOR_FLAG_AIMING | K_ACTOR_FLAG_DRAWN
            if reload_active:
                flags |= K_ACTOR_FLAG_RELOADING | K_ACTOR_FLAG_DRAWN
            if getattr(a_args, "drawn", False) and elapsed >= 3.0:
                flags |= K_ACTOR_FLAG_DRAWN
            hold_start = None
            if aim_active or shoot_active:
                hold_start = cycle_idx * cycle + 2.0
            target = 1 if shoot_active else 0
            return flags, hold_start, target
        aim_after = getattr(a_args, "aim_after", 0.0)
        shoot_after = getattr(a_args, "shoot_after", 0.0)
        reload_after = getattr(a_args, "reload_after", 0.0)
        aim_active = aim_after > 0 and aim_after <= elapsed < aim_after + 8.0
        shoot_active = (shoot_after > 0 and
                        shoot_after <= elapsed < shoot_after + 3.0)
        # E3b.1: 1 s reload impulse every 8 s (not every 4 s).
        reload_active = (reload_after > 0 and elapsed >= reload_after and
                         ((elapsed - reload_after) % 8.0) < 1.0)
        if aim_active or shoot_active:
            flags |= K_ACTOR_FLAG_AIMING | K_ACTOR_FLAG_DRAWN
        if reload_active:
            flags |= K_ACTOR_FLAG_RELOADING | K_ACTOR_FLAG_DRAWN
        if getattr(a_args, "drawn", False) and elapsed >= 3.0:
            flags |= K_ACTOR_FLAG_DRAWN
        starts = []
        if aim_active:
            starts.append(aim_after)
        if shoot_active:
            starts.append(shoot_after)
        hold_start = min(starts) if starts else None
        target = 1 if shoot_active else 0
        return flags, hold_start, target

    def _frames_payload(self):
        now = time.monotonic()
        records = []
        for a in self.actors:
            flags = 0
            if a.mounted:
                flags |= K_ACTOR_FLAG_MOUNTED
            if a.scenario:
                flags |= K_ACTOR_FLAG_SCENARIO
            combat_flags, hold_start, target = self._combat_state(a, now)
            flags |= combat_flags
            move_speed = (self.args.speed * a.gait) if a.gait > 0 else 0.0
            speed_u8 = encode_speed_u8(move_speed)
            gait_pad = encode_gait_pad(a.gait)
            if a.dead:
                # E3a: corpse stays fixed, no movement/animation.
                x, y = a.dead_pos[0], a.dead_pos[1]
                heading = a.dead_heading
                anim_state, speed_u8, gait_pad = 0, 0, 0
                flags |= K_ACTOR_FLAG_DEAD
            elif a.scenario:
                # E3a: scenario actors stand/sit in place (idle).
                x, y = a.center[0], a.center[1]
                heading = math.degrees(a.phase) % 360.0
                anim_state, speed_u8, gait_pad = 0, 0, 0
            elif hold_start is not None:
                # E3b.1: freeze x,y AND heading at the window start; cache the
                # pose so heading cannot drift from the movement pattern.
                hold_key = a.spawn_time + hold_start
                if a.combat_hold_key != hold_key or a.combat_hold is None:
                    a.combat_hold_key = hold_key
                    a.combat_hold = self._motion_pos(a, hold_key)
                x, y, heading = a.combat_hold
                anim_state, speed_u8, gait_pad = 0, 0, 0
            else:
                a.combat_hold_key = None
                a.combat_hold = None
                x, y, heading = self._motion_pos(a, now)
                anim_state = (K_MOVE_STYLE_RIDE_HORSE if a.mounted
                              else move_style_for_gait(a.gait))
            records.append(pack_actor_frame(a.actor_id, x, y, a.center[2],
                                            heading, self.frame_seq,
                                            anim_state=anim_state,
                                            speed=speed_u8, pad=gait_pad,
                                            flags=flags,
                                            target_actor_id=target))
        return pack_frames_batch(self.epoch, self.frame_seq, records)

    # -- loop ---------------------------------------------------------------
    def run(self) -> int:
        a = self.args
        self.log.write(f"fakehost: connecting to {a.host}:{a.port} as {a.name}")
        try:
            self.sock = connect(a.host, a.port)
            _sid, _coords, is_host = join_session(self.sock, a.name, a.model,
                                                  self.log, tag="fakehost")
            if not is_host:
                self.log.write(
                    "fakehost: ERROR: this tool is not HOST. Start fake host "
                    "BEFORE launching the game (the first player becomes host).")
                return 3
            if a.guest_name:
                self.log.write(f"fakehost: waiting for guest FRAME "
                               f"(expected guest name: {a.guest_name})")
            else:
                self.log.write("fakehost: waiting for guest FRAME ...")
            self.log.write("fakehost: models: " + ", ".join(
                f"0x{m:08X} ({n})" for m, n in
                zip(a.models, getattr(a, "model_names", []))))
            if a.outfit_warning:
                self.log.write(f"fakehost: WARN {a.outfit_warning}")
            if a.outfit_pairs:
                sample = ", ".join(f"0x{m:08X}:0x{meta:08X}"
                                   for m, meta in a.outfit_pairs[:5])
                self.log.write(f"fakehost: outfits from log: "
                               f"{len(a.outfit_pairs)} pair(s): {sample}")
            else:
                self.log.write("fakehost: outfits: none in log, "
                               "metaOutfit=0 (E1 fallback)")
            if a.mounted:
                self.log.write(
                    f"fakehost: mounted={a.mounted}/{a.num} mount_models=" +
                    ", ".join(f"0x{m:08X} ({n})" for m, n in
                              zip(a.mount_models, a.mount_model_names)))
            else:
                self.log.write("fakehost: mounted=0")
            if a.scenario:
                assigned = a.scenario_hashes[:a.scenario]
                names = getattr(a, "scenario_names_list", []) or []
                pairs = []
                for i, h in enumerate(assigned):
                    name = names[i % len(names)] if names else ""
                    pairs.append(f"0x{h:08X}" + (f" ({name})" if name else ""))
                self.log.write(f"fakehost: scenario={a.scenario}/{a.num}")
                self.log.write("fakehost: scenario hashes=" + ", ".join(pairs))
                if a.scenario_warning:
                    self.log.write(f"fakehost: WARN {a.scenario_warning}")
            else:
                self.log.write("fakehost: scenario=0")
            self.log.write(
                "fakehost: weapons=" + ", ".join(
                    f"0x{h:08X}" for h in getattr(a, "weapon_hashes", [])))
            self.log.write(
                f"fakehost: combat flags drawn={int(a.drawn)} "
                f"aim_after={a.aim_after:.1f} shoot_after={a.shoot_after:.1f} "
                f"reload_after={a.reload_after:.1f} combat={int(a.combat)} "
                f"cycle={a.combat_cycle:.1f} weapon=0x{a.weapon_hash:08X}")
            start = time.monotonic()
            deadline = start + a.duration
            reader = FrameReader()
            self.next_ping_at = start + 1.0
            while True:
                now = time.monotonic()
                if now >= deadline:
                    break
                for msg_type, payload in recv_available(self.sock, reader, wait=0.02):
                    now = time.monotonic()
                    if msg_type == MSG_FRAME and len(payload) >= FRAME_MIN_SIZE:
                        x, y, z = struct.unpack_from("<fff", payload, 0)
                        if is_finite_coord(x) and is_finite_coord(y) and is_finite_coord(z):
                            self.guest_pos = (x, y, z)
                            self.guest_frame_at = now
                            if not self.spawned:
                                self._start_test(now)
                            elif self.lost:
                                self.lost = False
                                self.log.write("fakehost: guest FRAME resumed; "
                                               "resending roster")
                                self._send_spawn(list(self.actors), "resume")
                    # Other messages (PlayerJoined/Pong/...): ignore.

                if not self.spawned:
                    # Wait for the game guest; if it never appears, exit 1 later.
                    self._maybe_ping(now)
                    continue

                # Guest frame heartbeat lost for >5 s: keep last known position.
                if self.guest_frame_at is not None and now - self.guest_frame_at > 5.0:
                    if not self.lost or now - self.last_lost_log > 5.0:
                        self.lost = True
                        self.last_lost_log = now
                        self.log.write("fakehost: guest FRAME lost >5 s; "
                                       "using last known position")

                if now >= self.next_frame_at:
                    if self.actors:
                        try:
                            send_frame(self.sock, MSG_ACTOR_FRAMES,
                                       self._frames_payload())
                            self.frames_sent += 1
                        except OSError as exc:
                            self.log.write(f"fakehost: send frames failed: {exc}")
                            return 1
                    # Fixed 10 Hz cadence: advance from the scheduled time,
                    # not from "now", so scheduling jitter does not slow us down.
                    self.next_frame_at += 0.1
                    if self.next_frame_at < now - 0.5:
                        self.next_frame_at = now + 0.1
                self._update_timers(now)
                self._maybe_ping(now)
                if self.next_log_at <= now:
                    self.next_log_at = now + 1.0
                    self.log.write(f"fakehost: frames sent={self.frames_sent}")

            if not self.spawned:
                self.log.write("fakehost: ERROR: no guest FRAME received; "
                               "start the game guest with npc_actor=1")
                return 1
            self.log.write(f"fakehost: done frames={self.frames_sent} "
                           f"spawns={self.spawns_sent} despawns={self.despawns}")
            return 0
        except (OSError, ProtocolError, RuntimeError, TimeoutError) as exc:
            self.log.write(f"fakehost: ERROR: {exc}")
            return 1
        finally:
            if self.sock is not None:
                self.log.write("fakehost: leaving")
                send_leave(self.sock)
                try:
                    self.sock.close()
                except OSError:
                    pass
                self.sock = None

    def _start_test(self, now: float) -> None:
        self.actors = self._make_actors(self.guest_pos)
        self.spawned = True
        self.spawn_time = now
        if self.args.scenario:
            scen = [a for a in self.actors if a.scenario]
            min_dist = 0.0
            if len(scen) >= 2:
                min_dist = min(
                    math.hypot(a.center[0] - b.center[0],
                               a.center[1] - b.center[1])
                    for i, a in enumerate(scen)
                    for b in scen[i + 1:])
            self.log.write(
                f"fakehost: scenario positions distance_min={min_dist:.2f} "
                f"(n={len(scen)}, spacing={self.args.scenario_spacing:.1f})")
        self.next_despawn = (now + self.args.despawn_after
                             if self.args.despawn_after > 0 else None)
        self.next_death = (now + self.args.dead_after
                           if self.args.dead_after > 0 else None)
        self.next_revive = None
        self.next_frame_at = now
        self.next_log_at = now + 1.0
        self.frames_sent = 0
        self.log.write("fakehost: guest frame at x=%.2f y=%.2f z=%.2f" %
                       self.guest_pos)
        self._send_spawn(self.actors, "initial")

    def _update_timers(self, now: float) -> None:
        a = self.args
        if self.spawned:
            elapsed = now - self.spawn_time
            if a.drawn and elapsed >= 3.0 and not self._weapon_drawn_logged:
                self._weapon_drawn_logged = True
                self.log.write("fakehost: weapon drawn")
            if combat_cycle_enabled(a):
                # E3b.3: repeating aim/shoot/reload windows every combat cycle.
                cycle = max(0.1, a.combat_cycle)
                cycle_idx = int(elapsed // cycle)
                ce = elapsed % cycle
                if cycle_idx != self._cycle_logged:
                    self._cycle_logged = cycle_idx
                    self._cycle_phase = None
                if 2.0 <= ce < 7.0:
                    phase = "aim"
                elif 7.0 <= ce < 10.0:
                    phase = "shoot"
                elif 12.0 <= ce < 13.0:
                    phase = "reload"
                else:
                    phase = None
                if phase and phase != self._cycle_phase:
                    self._cycle_phase = phase
                    self.log.write(
                        f"fakehost: cycle {cycle_idx + 1} {phase}")
            else:
                if (a.aim_after > 0 and elapsed >= a.aim_after and
                        not self._aim_logged):
                    self._aim_logged = True
                    self.log.write("fakehost: aim")
                if (a.shoot_after > 0 and elapsed >= a.shoot_after and
                        not self._shoot_logged):
                    self._shoot_logged = True
                    self.log.write("fakehost: shoot")
                if a.reload_after > 0:
                    reload_active = (elapsed >= a.reload_after and
                                     ((elapsed - a.reload_after) % 8.0) < 1.0)
                    if reload_active != self._reload_state:
                        self._reload_state = reload_active
                        self.log.write("fakehost: reload " +
                                       ("on" if reload_active else "off"))
        # E3a: one actor (last id) dies; dead flag + health=0, frames fixed.
        if self.next_death is not None and now >= self.next_death:
            self.next_death = None
            target = next((x for x in self.actors if x.actor_id == a.num), None)
            if target is not None and not target.dead:
                if target.scenario:
                    pos = (target.center[0], target.center[1],
                           math.degrees(target.phase) % 360.0)
                else:
                    pos = self._motion_pos(target, now)
                target.dead = True
                target.dead_pos = (pos[0], pos[1], target.center[2])
                target.dead_heading = pos[2]
                try:
                    self._send_spawn([target], "dead")
                except OSError as exc:
                    self.log.write(f"fakehost: death spawn failed: {exc}")
                self.log.write(f"fakehost: death id={target.actor_id}")
                if a.revive_after > 0:
                    self.next_revive = now + a.revive_after
        # E3a: revive the same actor: dead=0, health 200, normal frames resume.
        if self.next_revive is not None and now >= self.next_revive:
            self.next_revive = None
            target = next((x for x in self.actors if x.actor_id == a.num), None)
            if target is not None and target.dead:
                target.dead = False
                target.dead_pos = None
                target.spawn_time = now
                try:
                    self._send_spawn([target], "revive")
                except OSError as exc:
                    self.log.write(f"fakehost: revive spawn failed: {exc}")
                self.log.write(f"fakehost: revive id={target.actor_id}")
        # Despawn actor 5 once (0 = disabled): it disappears from frame batches.
        if self.next_despawn is not None and now >= self.next_despawn:
            self.next_despawn = None
            target = next((x for x in self.actors if x.actor_id == 5), None)
            if target is not None:
                try:
                    send_frame(self.sock, MSG_ACTOR_DESPAWN,
                               pack_actor_despawn(self.epoch, target.actor_id, 1))
                    self.despawns += 1
                    self.actors.remove(target)
                    self.log.write(f"fakehost: despawn id={target.actor_id}")
                except OSError as exc:
                    self.log.write(f"fakehost: despawn failed: {exc}")
                if a.respawn_after > 0:
                    self.next_respawn = now + a.respawn_after
        # Optional respawn of the same actorId near the last known guest pos.
        if self.next_respawn is not None and now >= self.next_respawn:
            self.next_respawn = None
            center = self.guest_pos or (0.0, 0.0, 0.0)
            actor = self._new_actor(5, center, now)
            self.actors.append(actor)
            try:
                self._send_spawn([actor], "respawn")
            except OSError as exc:
                self.log.write(f"fakehost: respawn failed: {exc}")
            self.log.write("fakehost: respawn id=5")
        # Keep the 10 Hz frame schedule from drifting after a slow tick.
        if self.next_frame_at is not None and self.next_frame_at < now - 0.5:
            self.next_frame_at = now + 0.1

    def _maybe_ping(self, now: float) -> None:
        if now >= self.next_ping_at:
            self.next_ping_at = now + 1.0
            send_ping(self.sock)


# ---------------------------------------------------------------------------
# GUEST mode: validate actor messages from the real game host.
# ---------------------------------------------------------------------------
class FakeGuest:
    def __init__(self, args, log: Log):
        self.args = args
        self.log = log
        self.sock = None
        self.spawn_batches = 0
        self.spawns = 0
        self.frames_msgs = 0
        self.frame_records = 0
        self.despawns = 0
        self.errors = 0
        self.outfits_nonzero = 0
        self.models_nonzero = 0
        # E2b: mounted actors (flags bit0) and their ActorSpawn.reserved mount
        # model hash. Heartbeat spawn batches repeat actorIds, so use sets.
        self.mounted_ids = set()
        self.mounted_reserved_ok = set()
        self.mounted_reserved_zero = set()
        # E3a: scenario actors (flags bit5 + scenarioHash) and dead frames.
        self.scenario_ids = set()
        self.scenario_zero = set()
        self.scenario_frames = 0
        self.dead_frames = 0
        self.dead_ids = set()
        # E3b: weapon/combat flags.
        self.weapon_ids = set()
        self.drawn_frames = 0
        self.aim_frames = 0
        self.reload_frames = 0
        self.combat_frames = 0
        self.target_frames = 0
        # E2a.2: speed/gait statistics from ActorFrame (speed u8 on 0..8 m/s
        # scale, pad = gait*1000).
        self.speed_min = 255
        self.speed_max = 0
        self.speed_sum = 0
        self.speed_count = 0
        self.pad_zero = 0
        self.gait_min = float("inf")
        self.gait_max = 0.0
        self.gait_sum = 0.0
        self.gait_count = 0
        self.unique_ids = set()
        self.known_ids = set()
        self.current_epoch = 0
        self.last_seq = None
        self.first_actor_at = None
        self.first_spawn_at = None
        self.first_frame_at = None
        self.last_actor_at = None
        self.max_gap = 0.0
        self.next_report = 0.0
        self.next_ping_at = 0.0
        self._error_logs = 0
        self.epochs_seen = set()

    def _error(self, text: str) -> None:
        self.errors += 1
        if self._error_logs < 20:
            self._error_logs += 1
            self.log.write(f"fakeguest: ERROR {text}")

    def _observe_actor_time(self, now: float) -> None:
        if self.first_actor_at is None:
            self.first_actor_at = now
        if self.last_actor_at is not None:
            self.max_gap = max(self.max_gap, now - self.last_actor_at)
        self.last_actor_at = now

    def _handle_spawn(self, payload: bytes, now: float) -> None:
        try:
            epoch, records = validate_spawn_batch(payload)
        except ValueError as exc:
            self._error(f"ActorSpawnBatch: {exc}")
            return
        self._observe_actor_time(now)
        self.spawn_batches += 1
        self.spawns += len(records)
        if self.first_spawn_at is None:
            self.first_spawn_at = now
        if epoch != self.current_epoch:
            if self.current_epoch:
                self.log.write(f"fakeguest: epoch changed {self.current_epoch} -> {epoch}")
            self.current_epoch = epoch
            self.last_seq = None
            self.known_ids.clear()
            self.epochs_seen.add(epoch)
        for rec in records:
            self.unique_ids.add(rec["actorId"])
            self.known_ids.add(rec["actorId"])
            if rec["metaOutfit"] != 0:
                self.outfits_nonzero += 1
            if rec["model"] != 0:
                self.models_nonzero += 1
            if rec["flags"] & K_ACTOR_FLAG_MOUNTED:
                actor_id = rec["actorId"]
                self.mounted_ids.add(actor_id)
                if rec["mountModel"] != 0:
                    self.mounted_reserved_ok.add(actor_id)
                elif actor_id not in self.mounted_reserved_zero:
                    self.mounted_reserved_zero.add(actor_id)
                    self._error(f"ActorSpawnBatch: mounted actorId={actor_id} "
                                f"has reserved(mountModel)=0")
            # E3a: scenarioHash != 0 is what the fake guest reports; a scenario
            # flag without a hash is an invalid spawn.
            if rec["scenarioHash"] != 0:
                self.scenario_ids.add(rec["actorId"])
            elif rec["flags"] & K_ACTOR_FLAG_SCENARIO:
                actor_id = rec["actorId"]
                if actor_id not in self.scenario_zero:
                    self.scenario_zero.add(actor_id)
                    self._error(f"ActorSpawnBatch: scenario actorId={actor_id} "
                                f"has scenarioHash=0")
            if rec["weaponHash"] != 0:
                self.weapon_ids.add(rec["actorId"])
        self.log.write(f"fakeguest: spawn batch epoch={epoch} n={len(records)} "
                       f"ids={[r['actorId'] for r in records]}")

    def _handle_frames(self, payload: bytes, now: float) -> None:
        try:
            epoch, seq, records = validate_frames(payload)
        except ValueError as exc:
            self._error(f"ActorFrames: {exc}")
            return
        self._observe_actor_time(now)
        self.frames_msgs += 1
        self.frame_records += len(records)
        if self.first_frame_at is None:
            self.first_frame_at = now
        if epoch != self.current_epoch:
            if self.current_epoch:
                self.log.write(f"fakeguest: epoch changed {self.current_epoch} -> {epoch}")
            self.current_epoch = epoch
            self.last_seq = None
            self.known_ids.clear()
            self.epochs_seen.add(epoch)
        # Monotonic sequence with a small tolerance for wraparound.
        if self.last_seq is not None:
            if seq == self.last_seq:
                self._error(f"ActorFrames: duplicate seq={seq}")
            elif seq < self.last_seq and not (self.last_seq > 60000 and seq < 5000):
                self._error(f"ActorFrames: seq regressed {self.last_seq} -> {seq}")
        self.last_seq = seq
        for rec in records:
            speed = rec["speed"]
            pad = rec["pad"]
            gait = pad / 1000.0
            self.speed_sum += speed
            self.speed_count += 1
            self.speed_min = min(self.speed_min, speed)
            self.speed_max = max(self.speed_max, speed)
            self.gait_sum += gait
            self.gait_count += 1
            self.gait_min = min(self.gait_min, gait)
            self.gait_max = max(self.gait_max, gait)
            if pad == 0:
                self.pad_zero += 1
            if rec["flags"] & K_ACTOR_FLAG_DEAD:
                self.dead_frames += 1
                self.dead_ids.add(rec["actorId"])
            if rec["flags"] & K_ACTOR_FLAG_SCENARIO:
                self.scenario_frames += 1
            if rec["flags"] & K_ACTOR_FLAG_DRAWN:
                self.drawn_frames += 1
            if rec["flags"] & K_ACTOR_FLAG_AIMING:
                self.aim_frames += 1
            if rec["flags"] & K_ACTOR_FLAG_RELOADING:
                self.reload_frames += 1
            if rec["flags"] & K_ACTOR_FLAG_COMBAT:
                self.combat_frames += 1
            if rec["targetActorId"] != 0:
                self.target_frames += 1
        unknown = [r["actorId"] for r in records
                   if r["actorId"] not in self.known_ids]
        if unknown and self.frames_msgs <= 5:
            self.log.write(f"fakeguest: frames before spawn for ids={unknown[:5]}")

    def _handle_despawn(self, payload: bytes, now: float) -> None:
        try:
            epoch, actor_id, reason = validate_despawn(payload)
        except ValueError as exc:
            self._error(f"ActorDespawn: {exc}")
            return
        self._observe_actor_time(now)
        if actor_id not in self.known_ids:
            self._error(f"ActorDespawn unknown actorId={actor_id}")
        else:
            self.despawns += 1
            self.known_ids.discard(actor_id)
            self.log.write(f"fakeguest: despawn id={actor_id} reason={reason}")

    def _summary(self, now: float) -> str:
        elapsed = max(0.001, now - (self.first_actor_at or now))
        fps = self.frames_msgs / elapsed if self.frames_msgs else 0.0
        return (f"fakeguest: spawns={self.spawns} unique={len(self.unique_ids)} "
                f"frames={self.frames_msgs} fps≈{fps:.1f} "
                f"despawns={self.despawns} errors={self.errors}")

    def run(self) -> int:
        a = self.args
        self.log.write(f"fakeguest: connecting to {a.host}:{a.port} as {a.name}")
        rc = 2
        try:
            self.sock = connect(a.host, a.port)
            _sid, _coords, is_host = join_session(self.sock, a.name, a.model,
                                                  self.log, tag="fakeguest")
            if is_host:
                self.log.write(
                    "fakeguest: ERROR: this tool is HOST. Start the game first "
                    "(it must be the first player/host), then run fake guest.")
                return 3
            start = time.monotonic()
            deadline = start + a.duration
            self.next_report = start + 2.0
            self.next_ping_at = start + 1.0
            reader = FrameReader()
            while time.monotonic() < deadline:
                now = time.monotonic()
                for msg_type, payload in recv_available(self.sock, reader, wait=0.05):
                    now = time.monotonic()
                    if msg_type == MSG_ACTOR_SPAWN_BATCH:
                        self._handle_spawn(payload, now)
                    elif msg_type == MSG_ACTOR_FRAMES:
                        self._handle_frames(payload, now)
                    elif msg_type == MSG_ACTOR_DESPAWN:
                        self._handle_despawn(payload, now)
                    # ActorEvent/ActorOutfit/ActorResync: not needed for E1.
                    # Pong/PlayerJoined/Frame/...: ignore.
                if now >= self.next_report:
                    self.next_report = now + 2.0
                    self.log.write(self._summary(now))
                if now >= self.next_ping_at:
                    self.next_ping_at = now + 1.0
                    send_ping(self.sock)
            now = time.monotonic()
            self.log.write(self._summary(now))
            if self.first_frame_at is not None and self.first_spawn_at is not None:
                self.log.write(
                    f"fakeguest: latency spawn->first frame "
                    f"≈{(self.first_frame_at - self.first_spawn_at) * 1000.0:.0f} ms, "
                    f"max actor gap ≈{self.max_gap * 1000.0:.0f} ms")
            self.log.write(f"fakeguest: outfits nonzero={self.outfits_nonzero}")
            self.log.write(f"fakeguest: models nonzero={self.models_nonzero}")
            self.log.write(f"fakeguest: mounted={len(self.mounted_ids)}")
            self.log.write(
                f"fakeguest: mounted reserved_ok={len(self.mounted_reserved_ok)} "
                f"reserved_zero={len(self.mounted_reserved_zero)}")
            self.log.write(
                f"fakeguest: scenarios={len(self.scenario_ids)} "
                f"dead_frames={self.dead_frames}")
            self.log.write(
                f"fakeguest: scenario_frames={self.scenario_frames}")
            self.log.write(
                f"fakeguest: weapon_spawns={len(self.weapon_ids)} "
                f"drawn_frames={self.drawn_frames} "
                f"aim_frames={self.aim_frames} "
                f"reload_frames={self.reload_frames}")
            self.log.write(
                f"fakeguest: combat_frames={self.combat_frames}")
            self.log.write(
                f"fakeguest: target_frames={self.target_frames}")
            speed_avg = (self.speed_sum / self.speed_count
                         if self.speed_count else 0.0)
            gait_avg = (self.gait_sum / self.gait_count
                        if self.gait_count else 0.0)
            self.log.write(f"fakeguest: speed avg={speed_avg:.1f} "
                           f"gait avg={gait_avg:.3f} "
                           f"pad_zero={self.pad_zero}")
            if self.speed_count:
                self.log.write(
                    f"fakeguest: speed min={self.speed_min} "
                    f"max={self.speed_max} u8 (scale 8 m/s); "
                    f"gait min={self.gait_min:.3f} max={self.gait_max:.3f}")
            if self.spawn_batches >= 1 and self.frames_msgs >= 10:
                rc = 0
                self.log.write("fakeguest: PASS")
            else:
                rc = 2
                self.log.write("fakeguest: FAIL (need >=1 ActorSpawnBatch and "
                               ">=10 ActorFrames; is npc_actor=1 in the game host?)")
            return rc
        except (OSError, ProtocolError, RuntimeError, TimeoutError) as exc:
            self.log.write(f"fakeguest: ERROR: {exc}")
            return 2
        finally:
            if self.sock is not None:
                self.log.write("fakeguest: leaving")
                send_leave(self.sock)
                try:
                    self.sock.close()
                except OSError:
                    pass
                self.sock = None


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(
        description="Headless fake v15 player for single-PC NPC replication tests "
                    "(fake host sends 5 NPC actors to the game guest; fake guest "
                    "validates actor messages from the game host).",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    ap.add_argument("--mode", required=True, choices=("host", "guest"),
                    help="host: run BEFORE the game guest; guest: run AFTER "
                         "the game is connected as host")
    ap.add_argument("--host", default="127.0.0.1", help="relay server address")
    ap.add_argument("--port", type=int, default=7777, help="relay server TCP port")
    ap.add_argument("--name", default=None,
                    help="player name (default: TestHost / TestGuest)")
    ap.add_argument("--duration", type=int, default=None,
                    help="run duration seconds (default: 300 host / 120 guest)")
    ap.add_argument("--model", default=hex(DEFAULT_MODEL),
                    help="JOIN ped model hash (u64)")
    ap.add_argument("--log-file", default=str(DEFAULT_LOG),
                    help="append log file (also mirrored to stdout)")
    ap.add_argument("--guest-name", default=None,
                    help="host mode: expected guest name (informational)")

    host = ap.add_argument_group("host mode")
    host.add_argument("--num", type=int, default=5,
                      help="number of human NPC actors (1..12)")
    host.add_argument("--pattern", choices=("circle", "line", "mixed"),
                      default="circle",
                      help="movement pattern: circle; line (4 m back/forth); "
                           "mixed (2 circle, 2 line, rest idle)")
    host.add_argument("--gait", type=float, choices=(1.0, 2.0), default=1.0,
                      help="E2a.2 gait ratio for moving actors: 1.0 walk, "
                           "2.0 run (sent as ActorFrame.pad=gait*1000; idle=0)")
    host.add_argument("--mounted", type=int, default=0,
                      help="E2b: how many of --num actors ride a horse "
                           "(first N actors; 0..num)")
    host.add_argument("--mount-model", default=None,
                      help="E2b: explicit horse model hash for all mounted "
                           "actors (default: A_C_HORSE_* from Peds.json)")
    host.add_argument("--scenario", type=int, default=0,
                      help="E3a: how many of --num actors are in a scenario "
                           "(first N; idle, scenario flag + scenarioHash)")
    host.add_argument("--scenario-names", default=",".join(DEFAULT_SCENARIO_NAMES),
                      help="E3a: comma-separated valid RDR2 scenario names; "
                           "scenarioHash = joaat(lowercase name); "
                           "'none' = use probe-log hashes instead")
    host.add_argument("--scenario-from-log", default=str(DEFAULT_PROBE_LOG),
                      help="E3a: parse unique dynScenario=0x... hashes from "
                           "this probe log (empty string disables)")
    host.add_argument("--scenario-hash", default=None,
                      help="E3a: explicit scenario hash for all scenario actors")
    host.add_argument("--scenario-spacing", type=float, default=12.0,
                      help="E3a.4: ring radius (m) for scenario actors around "
                           "the guest; they are evenly spaced to avoid stacking "
                           "on one scenario point (default 12)")
    host.add_argument("--dead-after", type=float, default=0.0,
                      help="E3a: seconds after first spawn to mark the last "
                           "actor dead (0 = off)")
    host.add_argument("--revive-after", type=float, default=10.0,
                      help="E3a: seconds after death to revive that actor "
                           "(0 = stay dead)")
    host.add_argument("--weapon", default=hex(DEFAULT_WEAPON_HASH),
                      help="E3b: real RDR2 weapon hash for ActorSpawn.weaponHash "
                           f"(default 0x{DEFAULT_WEAPON_HASH:08X}; joaat of the "
                           "weapon name is NOT valid)")
    host.add_argument("--weapon-list",
                      nargs="?", default=None,
                      const=",".join(f"0x{h:08X}" for h in DEFAULT_WEAPON_LIST),
                      help="E3b: real weapon hashes assigned round-robin; "
                           "with no value uses validated list "
                           "0x169F59F7,0x6DFA071B,0xF5175BA1,0x5B78B8DD,0x63F46DE6")
    host.add_argument("--drawn", action="store_true",
                      help="E3b: keep holstered first 3 s, then set Drawn flag")
    host.add_argument("--aim-after", type=float, default=0.0,
                      help="E3b: seconds after spawn to aim (Aiming, idle) "
                           "for 8 s; 0=off")
    host.add_argument("--shoot-after", type=float, default=0.0,
                      help="E3b: seconds after spawn to shoot (Drawn|Aiming "
                           "for 3 s); 0=off")
    host.add_argument("--reload-after", type=float, default=0.0,
                      help="E3b: seconds after spawn to start Reloading "
                           "impulses (1 s on / 4 s period); 0=off")
    host.add_argument("--combat-cycle", type=float, default=20.0,
                      help="E3b.3: repeat combat windows every SEC seconds: "
                           "+0..2 drawn, +2..7 aim (idle), +7..10 shoot "
                           "(target=1, idle), +12 reload 1 s; 0 = old one-shot "
                           "behavior (requires --combat/--drawn/--*-after)")
    host.add_argument("--combat", action="store_true",
                      help="E3b: set Combat flag (16)")
    host.add_argument("--outfits-from-log", default=str(DEFAULT_PROBE_LOG),
                      help="parse `probe: model=... metaOutfit=...` pairs from "
                           "the game log and send them in ActorSpawn.metaOutfit "
                           "(pass empty string to disable)")
    host.add_argument("--circle-radius", type=float, default=3.0,
                      help="circle radius around the spawn point, meters")
    host.add_argument("--speed", type=float, default=1.0,
                      help="base movement speed for moving actors, m/s "
                           "(actual = speed*gait; converted to ActorFrame.speed "
                           "on the 0..8 m/s scale)")
    host.add_argument("--despawn-after", type=float, default=30.0,
                      help="seconds from spawn to ActorDespawn id=5 "
                           "(0 = disabled)")
    host.add_argument("--respawn-after", type=float, default=10.0,
                      help="seconds after despawn to respawn id=5 (0 = disable)")
    host.add_argument("--peds-json", default=str(PEDS_JSON),
                      help="rdr2-hashdb Peds.json path for model hashes")
    host.add_argument("--models", default=None,
                      help="override model hashes, comma-separated (>= num)")
    return ap


def resolve_args(args):
    if args.mode == "host":
        args.name = args.name or "TestHost"
        args.duration = int(args.duration if args.duration is not None else 300)
    else:
        args.name = args.name or "TestGuest"
        args.duration = int(args.duration if args.duration is not None else 120)
    try:
        args.model = int(str(args.model), 0) & 0xFFFFFFFFFFFFFFFF
    except ValueError:
        raise SystemExit(f"--model must be a hash (hex or decimal): {args.model!r}")
    if args.mode == "host":
        if not 1 <= args.num <= K_MAX_ACTORS:
            raise SystemExit(f"--num must be 1..{K_MAX_ACTORS}")
        if args.circle_radius <= 0 or args.speed <= 0:
            raise SystemExit("--circle-radius/--speed must be > 0")
        if args.despawn_after < 0 or args.respawn_after < 0:
            raise SystemExit("--despawn-after/--respawn-after must be >= 0")
        if args.models:
            try:
                args.models = parse_model_list(args.models, args.num)
            except (ValueError, TypeError) as exc:
                raise SystemExit(str(exc))
        else:
            try:
                args.models, names = load_human_models(args.num,
                                                       Path(args.peds_json))
            except RuntimeError as exc:
                raise SystemExit(str(exc))
            args.model_names = names
        if not hasattr(args, "model_names"):
            args.model_names = []
        # E2a: optional model+metaOutfit pairs from a probe log. If none are
        # found, keep the E1 behaviour (metaOutfit=0).
        args.outfit_pairs = []
        args.outfit_warning = None
        if args.outfits_from_log:
            args.outfit_pairs, args.outfit_warning = load_outfit_pairs(
                Path(args.outfits_from_log))
        # E2b: mounted actor support. Mount model hashes are sent in
        # ActorSpawn.reserved; first --mounted actors get the mounted flag.
        if not 0 <= args.mounted <= args.num:
            raise SystemExit(f"--mounted must be 0..{args.num}")
        args.mount_models = []
        args.mount_model_names = []
        if args.mounted:
            if args.mount_model:
                try:
                    model = parse_single_hash(args.mount_model)
                except (ValueError, TypeError) as exc:
                    raise SystemExit(f"--mount-model: {exc}")
                args.mount_models = [model]
                args.mount_model_names = ["--mount-model"]
            else:
                try:
                    args.mount_models, args.mount_model_names = load_mount_models(
                        args.mounted, Path(args.peds_json))
                except RuntimeError as exc:
                    raise SystemExit(str(exc))
                if not args.mount_models:
                    raise SystemExit("cannot load A_C_HORSE_* mount models")
        # E3a: scenarios + death/revive.
        if not 0 <= args.scenario <= args.num:
            raise SystemExit(f"--scenario must be 0..{args.num}")
        args.scenario_hashes = []
        args.scenario_warning = None
        args.scenario_names_list = []
        if args.scenario:
            names_text = str(getattr(args, "scenario_names", "") or "").strip()
            names = [n.strip() for n in names_text.split(",") if n.strip()] \
                if names_text.lower() not in ("none", "off", "") else []
            if args.scenario_hash:
                try:
                    args.scenario_hashes = [parse_single_hash(args.scenario_hash)]
                except (ValueError, TypeError) as exc:
                    raise SystemExit(f"--scenario-hash: {exc}")
            elif names:
                # E3a: known scenario names -> joaat hashes, assigned by actor.
                args.scenario_names_list = names
                args.scenario_hashes = [joaat(n) for n in names]
            else:
                # 'none' (or no names): fall back to dynScenario probe hashes.
                source = getattr(args, "scenario_from_log", "") or ""
                hashes, warning = load_scenario_hashes(Path(source),
                                                       args.scenario) \
                    if source else ([], None)
                if hashes:
                    args.scenario_hashes = hashes
                    args.scenario_warning = warning
                else:
                    args.scenario_hashes = list(FALLBACK_SCENARIO_HASHES)
                    args.scenario_warning = (
                        f"no scenario log; using {len(args.scenario_hashes)} "
                        f"fallback hash(es)")
        if args.dead_after < 0 or args.revive_after < 0:
            raise SystemExit("--dead-after/--revive-after must be >= 0")
        if args.scenario_spacing <= 0:
            raise SystemExit("--scenario-spacing must be > 0")
        try:
            args.weapon_hash = int(str(args.weapon), 0) & 0xFFFFFFFF
        except ValueError:
            raise SystemExit(f"--weapon must be a hash (hex or decimal): "
                             f"{args.weapon!r}")
        weapon_list_text = (str(args.weapon_list).strip()
                            if args.weapon_list is not None else "")
        if weapon_list_text and weapon_list_text.lower() not in ("none", "off"):
            weapon_hashes = []
            for part in weapon_list_text.replace(";", ",").split(","):
                part = part.strip()
                if not part:
                    continue
                try:
                    weapon_hashes.append(int(part, 0) & 0xFFFFFFFF)
                except ValueError:
                    raise SystemExit(f"--weapon-list: bad hash {part!r}")
            if not weapon_hashes:
                raise SystemExit("--weapon-list is empty")
            args.weapon_hashes = weapon_hashes
        else:
            args.weapon_hashes = [args.weapon_hash]
        if args.aim_after < 0 or args.shoot_after < 0 or args.reload_after < 0:
            raise SystemExit("--aim-after/--shoot-after/--reload-after "
                             "must be >= 0")
        if args.combat_cycle < 0:
            raise SystemExit("--combat-cycle must be >= 0")
    if args.duration < 1:
        raise SystemExit("--duration must be >= 1 second")
    return args


def main(argv=None) -> int:
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, OSError):
        pass
    ap = build_parser()
    args = resolve_args(ap.parse_args(argv))
    log = Log(Path(args.log_file))
    args.log_file = str(args.log_file)
    log.write("=" * 70)
    log.write(f"fake_actor_client: mode={args.mode} name={args.name} "
              f"server={args.host}:{args.port} duration={args.duration}s "
              f"protocol=v{PROTOCOL_VERSION}")
    client = FakeHost(args, log) if args.mode == "host" else FakeGuest(args, log)
    try:
        rc = client.run()
    except KeyboardInterrupt:
        log.write(f"fake_{args.mode}: Ctrl+C")
        if args.mode == "guest":
            try:
                log.write(client._summary(time.monotonic()))
            except Exception:  # noqa: BLE001 - best effort on interrupt
                pass
            rc = 0 if (getattr(client, "spawn_batches", 0) >= 1 and
                       getattr(client, "frames_msgs", 0) >= 10) else 2
        else:
            rc = 0 if getattr(client, "frames_sent", 0) > 0 else 1
    finally:
        log.close()
    return rc


if __name__ == "__main__":
    sys.exit(main())
