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

HUMAN_PREFIXES = ("A_M_M_", "A_M_O_", "G_M_M_", "S_M_M_")
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
                     meta_outfit: int = 0) -> bytes:
    return struct.pack(
        "<IHBBIIIIIffffBBBBhhhHII",
        actor_id, epoch & 0xFFFF, 0, 0,          # actorId, epoch, kind, flags
        model & 0xFFFFFFFF, meta_outfit & 0xFFFFFFFF, 0, 0, 0,  # model, meta, ...
        float(x), float(y), float(z), float(heading),
        health & 0xFF, move_style & 0xFF, 0, 0,  # health, moveStyle, phase, pad
        0, 0, 0, 0,                              # vx, vy, vz, pad2
        0, 0,                                    # reserved, reserved2
    )


def pack_spawn_batch(epoch: int, records) -> bytes:
    return struct.pack("<IHH", epoch, len(records), 0) + b"".join(records)


def pack_actor_frame(actor_id: int, x: float, y: float, z: float,
                     heading: float, seq: int, anim_state: int = 1,
                     speed: int = 64) -> bytes:
    return struct.pack("<IHBBffffIHH", actor_id, 0, anim_state & 0xFF,
                       speed & 0xFF, float(x), float(y), float(z),
                       float(heading), 0, seq & 0xFFFF, 0)


def pack_frames_batch(epoch: int, seq: int, records) -> bytes:
    return struct.pack("<IHH", epoch, seq & 0xFFFF, len(records)) + b"".join(records)


def pack_actor_despawn(epoch: int, actor_id: int, reason: int) -> bytes:
    return struct.pack("<IIB3x", epoch, actor_id, reason & 0xFF)


def is_finite_coord(v: float) -> bool:
    return math.isfinite(v) and abs(v) < 1.0e5


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
        (actor_id, _epoch16, _kind, _flags, model, meta, _mount, _weapon,
         _scenario, x, y, z, heading, health, move_style, _phase, _pad,
         _vx, _vy, _vz, _pad2, _res1, _res2) = struct.unpack_from(
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
        (actor_id, _flags, anim_state, speed, x, y, z, heading,
         _target, rec_seq, _pad) = struct.unpack_from("<IHBBffffIHH", rec, 0)
        if actor_id == 0:
            raise ValueError(f"frame #{i}: actorId=0")
        if actor_id in seen:
            raise ValueError(f"frames batch duplicate actorId={actor_id}")
        seen.add(actor_id)
        if not all(is_finite_coord(v) for v in (x, y, z, heading)):
            raise ValueError(f"frame actorId={actor_id}: non-finite coords")
        records.append({"actorId": actor_id, "animState": anim_state,
                        "speed": speed, "seq": rec_seq})
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
                 direction: float = 0.0):
        self.actor_id = actor_id
        self.model = model
        self.meta_outfit = meta_outfit
        self.center = center  # (x, y, z)
        self.phase = phase
        self.spawn_time = spawn_time
        self.motion = motion  # circle | line | idle
        self.direction = direction


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
        self.next_frame_at = None
        self.next_log_at = None
        self.next_ping_at = 0.0
        self.lost = False
        self.rc = 1
        self._outfit_logged = False

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
        angle = math.radians(72.0 * index)
        dist = 3.0 + 0.75 * (index % 4)  # 3.0 .. 5.25 m around the guest
        cx = center[0] + dist * math.cos(angle)
        cy = center[1] + dist * math.sin(angle)
        model, meta = self._model_and_meta(index)
        motion = self._motion_for(index, self.args.pattern)
        return Actor(actor_id, model, meta, (cx, cy, center[2]), angle,
                     spawn_time, motion, angle)

    def _make_actors(self, center):
        now = time.monotonic()
        return [self._new_actor(i + 1, center, now)
                for i in range(self.args.num)]

    # -- payload helpers ----------------------------------------------------
    def _spawn_records(self, actors):
        return [pack_actor_spawn(a.actor_id, self.epoch, a.model,
                                 a.center[0], a.center[1], a.center[2],
                                 math.degrees(a.phase) % 360.0,
                                 move_style=0 if a.motion == "idle" else 1,
                                 meta_outfit=a.meta_outfit)
                for a in actors]

    def _send_spawn(self, actors, why: str = "") -> None:
        if not actors:
            return
        if not self._outfit_logged:
            self._outfit_logged = True
            self.log.write(
                f"fakehost: outfits pairs={len(getattr(self.args, 'outfit_pairs', []))} "
                f"pattern={self.args.pattern}")
        payload = pack_spawn_batch(self.epoch, self._spawn_records(actors))
        send_frame(self.sock, MSG_ACTOR_SPAWN_BATCH, payload)
        self.spawns_sent += len(actors)
        self.log.write(f"fakehost: spawn epoch={self.epoch} n={len(actors)}"
                       + (f" ({why})" if why else ""))

    def _frames_payload(self):
        now = time.monotonic()
        records = []
        for a in self.actors:
            t = now - a.spawn_time
            if a.motion == "idle":
                x, y = a.center[0], a.center[1]
                heading = math.degrees(a.phase) % 360.0
                anim_state, speed = 0, 0
            elif a.motion == "line":
                # Walk 4 m forward and back along a.direction (2 m each side).
                cycle = (t * self.args.speed) % 8.0
                if cycle < 4.0:
                    offset = -2.0 + cycle
                    forward = True
                else:
                    offset = 2.0 - (cycle - 4.0)
                    forward = False
                x = a.center[0] + offset * math.cos(a.direction)
                y = a.center[1] + offset * math.sin(a.direction)
                heading = (math.degrees(a.direction) +
                           (0.0 if forward else 180.0)) % 360.0
                anim_state, speed = 1, 64
            else:
                omega = self.args.speed / max(0.1, self.args.circle_radius)
                theta = a.phase + t * omega
                x = a.center[0] + self.args.circle_radius * math.cos(theta)
                y = a.center[1] + self.args.circle_radius * math.sin(theta)
                heading = (math.degrees(theta) + 90.0) % 360.0
                anim_state, speed = 1, 64
            records.append(pack_actor_frame(a.actor_id, x, y, a.center[2],
                                            heading, self.frame_seq,
                                            anim_state=anim_state, speed=speed))
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
        self.next_despawn = now + self.args.despawn_after
        self.next_frame_at = now
        self.next_log_at = now + 1.0
        self.frames_sent = 0
        self.log.write("fakehost: guest frame at x=%.2f y=%.2f z=%.2f" %
                       self.guest_pos)
        self._send_spawn(self.actors, "initial")

    def _update_timers(self, now: float) -> None:
        a = self.args
        # Despawn actor 5 once: it disappears from subsequent frame batches.
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
    host.add_argument("--outfits-from-log", default=str(DEFAULT_PROBE_LOG),
                      help="parse `probe: model=... metaOutfit=...` pairs from "
                           "the game log and send them in ActorSpawn.metaOutfit "
                           "(pass empty string to disable)")
    host.add_argument("--circle-radius", type=float, default=3.0,
                      help="circle radius around the spawn point, meters")
    host.add_argument("--speed", type=float, default=1.0,
                      help="actor walk speed on the circle, m/s")
    host.add_argument("--despawn-after", type=float, default=30.0,
                      help="seconds from spawn to ActorDespawn id=5")
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
