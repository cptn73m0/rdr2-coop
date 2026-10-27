# Guest influence on the host world — v20 design (interactivity)

**Status:** design proposal / pre-implementation. Code not changed.
**Target protocol:** v20 (v19 remains shipped: npc-e16).
**Author:** worldsync-architect (task-179), 2026-10-27.
**Related:** `docs/PROTOCOL.md`, `docs/TRAINS.md`, `docs/NATIVE_RESEARCH_INTERACT.md` (native R&D, parallel; see §12), `docs/PERF.md`.

## 1. Summary

Today the host owns the world; the guest receives host vehicles/actors as
visual proxies. Guest vehicle proxies are `mission`, `invincible` and
**non-collidable** (`src/world_sync.cpp:2330-2347`, `SetNoCollision(veh, true)`),
so the guest cannot ram, damage, open, enter, loot, hitch or drive host
vehicles. The host never receives vehicle intent from the guest. The few
existing guest→host paths are actor-specific:

- `ActorDamage` (0x1F): guest→server→host, damage by **actorId** only; host
  applies `APPLY_DAMAGE_TO_PED` (`src/actor_sync.cpp:3089+`). It cannot name a
  vehicle or a vehicle part.
- `PlayerVehicle` (0x12): relayed any direction; used for the remote
  player's visual vehicle proxy (`src/coop.cpp:5727+`), but its `netId` field
  is reserved and the host does not use it to address a real host vehicle.
- `ActorEvent` (0x1B) is host→guest only; it cannot carry guest intents.

Observed failures (lead/user report) match that model:

- host log `world: occupants vehicle=14 n=1 driver=0`; the host's
  `CollectVehicleOccupants` deliberately excludes `localPlayerPed`
  (`src/world_sync.cpp:407-484`), so a **host player driving** is never an
  occupant entry; the guest must learn the host player's seat from
  `PlayerVehicle`, which is gated by `vehicle_sync` (`src/coop.cpp:6829+`)
  and is not a seat-authority channel;
- the guest applied only some draft/seat entries; `SET_PED_INTO_VEHICLE` retries
  exist but depend on actor proxies/occupant entries arriving together;
- guest vehicle proxies have collision disabled, so they feel like ghosts;
- nothing in v19 can damage/break a vehicle or open a door.

**Proposal:** keep the host authoritative and add a small **intent protocol**
for guest interactions. The guest sends what it wants; the host validates and
applies the native to the real world; the host acknowledges the result and
replicates authoritative state (`VehicleState`) and, for driving, control
feedback (`VehicleControlState`). The guest's local proxy is a solid,
predictive approximation; the host state always wins on conflict.

Recommended order:

1. **Phase 0 (v19, no wire change):** fix player-seat/occupant identity and
   make nearby guest vehicle proxies collidable/enterable; do not pretend the
   guest can damage host vehicles yet.
2. **v20 P1:** `VehicleInteraction` intent for damage / doors / enter / exit /
   hijack-seat / loot / hitch-unhitch; host result + `VehicleState`.
3. **P2:** authoritative player seating (host player and guest player),
   identity/de-duplication with `PlayerVehicle`.
4. **P3:** guest driving through a control lease (input/transform + host
   correction + fallback).
5. **P4:** NPC/item interactions, mission-aware policy.

P1 is the largest user-visible win for the smallest risk: the guest can smash
a wheel/door, open/lock a door, and seat/hijack a parked non-mission wagon.
Driving is deliberately deferred.

## 2. Authority model

Definitions:

- **host** — player whose game instance owns world physics, NPC AI, damage,
  doors, draft hitching and mission state.
- **guest** — player whose game instance has a replica/proxy of host entities.
- **vehicleNetId** — stable host netId from `WorldSnapshot` (v19
  `WorldEntity.netId`); the only address used on the wire. Handles never cross
  the wire.
- **intent** — a guest request. It is not an instruction to be executed
  directly; the host decides.
- **lease** — a time-bound host grant allowing a guest to send driving input
  for one vehicle/seat. Invalidated by timeout, revoke, seat change, disconnect
  or host migration.
- **epoch** — host session id, already used by actor messages. All v20 vehicle
  messages carry it; stale epochs are dropped.

Rules:

1. The host is the only writer of real-world vehicle state. A guest never
   sends absolute transform, health, door state or seat state as authority.
2. The guest sends intents and bounded control input. The host validates
   `epoch`, `vehicleNetId`, distance, role, action policy, cooldown and
   mission protection **before** calling any native.
3. The host applies at most a small number of vehicle natives per tick
   (budget; see §10) and returns an idempotent result.
4. The guest's local proxy is allowed to predict for responsiveness, but it is
   reconciled to host state. Any divergence beyond thresholds is corrected by
   the host state (warp/rewind); the guest cannot overwrite host state.
5. The server is a relay and a role/rate/format boundary, not a semantic
   authority. It cannot know host world positions or mission state; therefore
   distance/ownership/mission checks live on the host, and the server only
   limits format, direction, rate and role.
6. Both peers run the same protocol/build. v20 is a hard bump: the server
   rejects v19 joins; there is no mixed-version fallback.

## 3. Why a new message is needed

No existing v19 message can express "damage/open/enter/hijack this host
vehicle":

| existing | direction | can carry vehicle intent? |
|---|---|---|
| `ActorDamage` 0x1F | guest→host | no: `actorId` only; vehicle/part cannot be addressed |
| `ActorEvent` 0x1B | host→guest only | no: wrong direction; server intentionally host-only |
| `PlayerVehicle` 0x12 | any→server→others | only player's model/position/seat; reserved `netId` is not used; no ACK/damage/door |
| `ActionState` 0x11 | any→server→others | player action enum/scenario; no vehicleNetId/target, no ACK |
| `WorldSnapshot` 0x10 | host→guest only | host state only; no guest request |
| `VehicleOccupants` 0x21 | host→guest only | host snapshot of NPC seats; no guest request |

Repurposing `ActorEvent`/`ActorDamage`/`PlayerVehicle` without a version bump
would create a hidden wire-semantics change that older peers cannot detect.
The design therefore takes a v20 bump and one small, explicit intent type.
`PlayerVehicle` and `WorldSnapshot` are reused for the existing visual/state
streams rather than duplicated.

### 3.1 v19-only interim: what is realistic now

| desired effect | possible on v19 only? | mechanism / limits |
|---|---|---|
| guest damages/breaks a **vehicle** | **no** | `ActorDamage` has actorId only, no vehicleNetId/part; `ActorEvent` is host→guest; `ActionState`/`PlayerVehicle` have no vehicle state/ACK; must not abuse semantics |
| guest opens/closes/locks a **door** | **no** | no door/vehicle state message; host→guest `VehicleOccupants` only seats |
| guest enters/hijacks a **seat** | **partial, paired-build only** | `PlayerVehicle` 0x12 is relayed any direction and has a reserved `netId`; a paired v19 hotfix could map it to a host real vehicle and seat the guest ghost. No ACK/anti-grief/state, and the host must not create a duplicate proxy. Semantic use of a reserved field is not a release-quality protocol; recommend v20 P2 instead |
| guest sees **host player driving** | **partial, paired-build only** | host player is excluded from `VehicleOccupants` (`localPlayerPed`); a paired hotfix can send host `PlayerVehicle` with `netId` to the guest and map it to the world proxy (fixes `driver=0` visual). Still not an authority channel |
| guest wagon feels solid/enterable | **yes, local only** | enable collision on nearby guest proxies (`SET_ENTITY_COLLISION`); host remains authoritative; no host-world effect |
| guest damages the **NPC driver / draft horse** | **yes** | already possible via `ActorDamage` 0x1F once the actor is in the guest roster and host validates. Effect is indirect: driver/horse dies/stops, wagon stops/detaches; not vehicle damage/doors |
| guest changes vehicle doors/damage and host sees it | **no** | requires a new host-authoritative intent/result (v20 `VehicleInteraction`) |

**Recommendation for the first iteration:** do the v19 **reliability/collision**
hotfix (host-player seat visibility, occupant application, local proxy
collision) only if it is small and paired; for user-visible guest influence
(damage/doors/hijack) implement **v20 P1** (§5), which is the smallest new
protocol that actually changes the host world. The absolute maximum effect for
one new message pair is v20 P1: damage + doors + enter/hijack seat, no driving.
The v19-only “max effect” is indirect (`ActorDamage` to driver/horse + solid
proxies + host-player seat fix), not comparable to door/vehicle damage.

## 4. v20 messages (proposal)

New IDs start after the v19 `0x22` and preserve all existing layouts. All
structs are little-endian and fixed-size, with `static_assert` in
`net/protocol.h`.

| ID | name | direction | payload | rate cap (per client) |
|---|---|---|---|---|
| `0x23` | `VehicleInteraction` | guest→server→host | 24 B | 8/s |
| `0x24` | `VehicleInteractionResult` | host→server→guest | 16 B | 20/s |
| `0x25` | `VehicleControlInput` | guest→server→host | 16 B | 20/s |
| `0x26` | `VehicleState` | host→server→guest | 24 B | 10/s |
| `0x27` | `VehicleControlState` | host→server→guest | 20 B | 15/s |

Server routing:

- 0x23/0x25: **guest-only to host**; host sender is logged and dropped; no
  echo; if no host, drop.
- 0x24/0x26/0x27: **host-only to guest(s)**; guest sender is logged/dropped.
- All use exact-size validation plus field validation (below); invalid payloads
  are dropped before relay/host.
- Traffic bucket: new `Vehicle` bucket (`server.cpp` `TrafficBucket`), so
  `net:` counters and `perf_report.py` show it separately. This is metrics
  only; wire stays as above.
- `IsGameThreadMessage` (server host liveness) must count host→guest 0x24/
  0x26/0x27 as game activity, and must **not** make guest intents/control count
  as host activity. Ping remains excluded from host timeout.

### 4.1 `VehicleInteraction` (0x23, 24 B)

```cpp
enum class VehicleAction : uint8_t {
    None       = 0,
    Damage     = 1,  // param = damage amount (1..1000), door = part/zone
    DoorOpen   = 2,  // door = doorId 0..7
    DoorClose  = 3,
    DoorLock   = 4,
    DoorUnlock = 5,
    Enter      = 6,  // seat = -1 driver / 0..8 passenger
    Exit       = 7,
    Hijack     = 8,  // targetId = driver actorId if known; host finds driver
    Loot       = 9,  // targetId = container/entity netId
    Hitch      = 10, // door = harness index 0..5
    Unhitch    = 11,
};

// kInteractionFlag* (uint8_t)
constexpr uint8_t kInteractionTargetActor   = 1u << 0; // targetId = actorId
constexpr uint8_t kInteractionTargetEntity  = 1u << 1; // targetId = netId
constexpr uint8_t kInteractionUrgent        = 1u << 2; // allow short cooldown
constexpr uint8_t kInteractionLootContainer = 1u << 3;
constexpr uint8_t kInteractionGuestHorse    = 1u << 4; // hitch/unhitch ownership hint
constexpr uint8_t kInteractionForce         = 1u << 5; // host may ignore
constexpr uint8_t kInteractionKnownFlags =
    kInteractionTargetActor | kInteractionTargetEntity |
    kInteractionUrgent | kInteractionLootContainer |
    kInteractionGuestHorse | kInteractionForce;

struct VehicleInteractionPayload {
    uint32_t epoch = 0;          // host epoch
    uint32_t vehicleNetId = 0;   // host WorldSnapshot netId
    uint32_t targetId = 0;       // actorId or netId depending on flags; 0 = none
    uint16_t seq = 0;            // guest request id; nonzero; idempotency key
    uint8_t  action = 0;         // VehicleAction
    int8_t   seat = 127;         // -1 driver, 0..8 passenger, 127 n/a
    uint8_t  door = 255;         // doorId/part/harness depending on action
    uint8_t  flags = 0;          // kInteractionFlag*
    uint16_t param = 0;          // amount/slot/extra; 0 = default
    uint32_t weaponHash = 0;     // optional, for damage context
};
static_assert(sizeof(VehicleInteractionPayload) == 24, "...");
```

`seq` is the request idempotency key; the host caches the last results per
`(epoch, seq)` for ~2 s and re-sends the result for duplicates without
reapplying the native.

Semantics per action:

- **Damage:** host maps `door` (part/zone) + `param` (amount) to
  `SET_VEHICLE_DAMAGE` offsets/radius and caps amount by part and per-second
  budget. No damage on mission-protected vehicles.
- **DoorOpen/Close/Lock/Unlock:** host maps `door` to door id and calls the
  door natives; lock is a vehicle-wide `SET_VEHICLE_DOORS_LOCKED`/door state
  native. State is returned in `VehicleState.doorBits`.
- **Enter/Exit:** host validates seat, distance and seat free; seats the
  guest's host-side ghost ped into the real vehicle/seat
  (`SET_PED_INTO_VEHICLE`/`TASK_ENTER_VEHICLE`), or removes it
  (`TASK_LEAVE_VEHICLE`). The guest's local player proxy follows the result.
- **Hijack:** host finds the NPC driver (`GET_DRIVER_OF_VEHICLE`), validates
  non-mission/not locked, ejects/kills/leaves the NPC per policy, then seats the
  guest ghost at the driver seat. This is P1 (seat takeover) without driving.
- **Loot:** P1 only for non-mission containers/vehicles; host applies its own
  loot transaction. Item transfer to the guest's inventory is **not** part of
  P1 (separate inventory sync, P4).
- **Hitch/Unhitch:** host validates draft vehicle/horse ownership/policy and
  calls the draft attach/detach native (verified by native R&D before use).

`targetId` is never a raw engine handle.

### 4.2 `VehicleInteractionResult` (0x24, 16 B)

```cpp
enum class VehicleResult : uint8_t {
    Accepted = 0,
    Applied = 1,       // native applied successfully
    Rejected = 2,
    StaleEpoch = 3,
    NotFound = 4,
    OutOfRange = 5,
    SeatOccupied = 6,
    MissionProtected = 7,
    Locked = 8,
    NoAuthority = 9,
    RateLimited = 10,
    Timeout = 11,
    Correction = 12,   // state was corrected; guest must reconcile
};

struct VehicleInteractionResultPayload {
    uint32_t epoch = 0;
    uint32_t vehicleNetId = 0;
    uint16_t seq = 0;       // echoes request seq
    uint8_t  action = 0;    // VehicleAction
    uint8_t  result = 0;    // VehicleResult
    uint32_t detail = 0;    // damage/part/native/diagnostic
};
static_assert(sizeof(VehicleInteractionResultPayload) == 16, "...");
```

Host sends this to the requesting guest (0x24 is host→guest, not broadcast).
The result is final for P1/P2 actions; for P3 it grants/refuses a lease and
`detail` carries the 16-bit `leaseId` (upper bits reserved). A rejected intent
has no world side effect. The guest applies only on `Applied`; on rejection it
rolls back any local prediction.

### 4.3 `VehicleControlInput` (0x25, 16 B)

```cpp
struct VehicleControlInputPayload {
    uint32_t epoch = 0;
    uint32_t vehicleNetId = 0;
    uint16_t leaseId = 0;   // issued by host in result.detail
    uint16_t seq = 0;       // input sequence; host drops old/duplicate
    uint8_t  throttle = 0;  // 0..255
    uint8_t  brake = 0;     // 0..255
    int8_t   steer = 0;     // -127..127
    uint8_t  flags = 0;     // bit0 handbrake, bit1 reverse, bit2 horn (reserved)
};
static_assert(sizeof(VehicleControlInputPayload) == 16, "...");
```

Guest sends this at **10–15 Hz only while a lease is active**. It carries no
absolute position; the guest cannot teleport the vehicle. The host rejects
inputs with wrong epoch/vehicle/lease, stale seq, dead driver, or timeout.

### 4.4 `VehicleState` (0x26, 24 B)

```cpp
// doorBits: 2 bits per door (0 closed, 1 open, 2 broken, 3 latched/locked)
// damageBits: bit0 body, bit1 engine, bit2 wheels, bit3 doors, bit4 draft,
//             bits 5..7 part/zone detail (implementation-defined; the host
//             also reads GET_VEHICLE_*_HEALTH for detail, not just this mask)
constexpr uint8_t kVehicleStateFlagDamageValid     = 1u << 0;
constexpr uint8_t kVehicleStateFlagDoorValid       = 1u << 1;
constexpr uint8_t kVehicleStateFlagLocked          = 1u << 2;
constexpr uint8_t kVehicleStateFlagMissionProtected= 1u << 3;
constexpr uint8_t kVehicleStateFlagLeaseActive     = 1u << 4;
constexpr uint8_t kVehicleStateFlagHostSeat        = 1u << 5;
constexpr uint8_t kVehicleStateFlagGuestSeat       = 1u << 6;
constexpr uint8_t kVehicleStateFlagHijacked        = 1u << 7;

constexpr int8_t kVehicleSeatNone = 127;
constexpr int8_t kVehicleSeatDriver = -1;

struct VehicleStatePayload {
    uint32_t epoch = 0;
    uint32_t vehicleNetId = 0;
    uint32_t damageBits = 0;
    uint16_t doorBits = 0;
    uint8_t  flags = 0;
    int8_t   hostSeat = kVehicleSeatNone;  // host player's seat in this vehicle
    int8_t   guestSeat = kVehicleSeatNone; // guest player's seat (host view)
    uint16_t seq = 0;
    uint32_t leaseId = 0;                  // 0 = no valid lease
};
static_assert(sizeof(VehicleStatePayload) == 24, "...");
```

This is the **player-seat/authority reconciliation** message. It fixes the
`driver=0`/host-player-invisible problem without abusing `ActorDamage`:

- the host sends its own vehicle state (host player's seat in `hostSeat`) even
  though `localPlayerPed` is excluded from NPC occupant entries;
- the host tells the guest which seat the guest's host-side ghost occupies
  (`guestSeat`);
- damage/door/lock state is replicated and self-healing on loss;
- `leaseId != 0` tells both sides which guest (if any) has control.

`VehicleState` is sent on change and as a 1 s heartbeat for vehicles with a
player seat, a lease, or recent interaction; it can be dropped without
desync (next heartbeat corrects). NPC/draft occupants continue to use the
existing `VehicleOccupants` 0x21; it is not modified.

### 4.5 `VehicleControlState` (0x27, 20 B)

```cpp
struct VehicleControlStatePayload {
    uint32_t epoch = 0;
    uint32_t vehicleNetId = 0;
    uint16_t leaseId = 0;
    uint16_t inputSeqAck = 0;  // highest guest input applied
    int16_t  speedCmS = 0;     // signed forward speed of real vehicle
    int8_t   steer = 0;        // host's current steer input/bias
    uint8_t  flags = 0;        // bit0 active, bit1 revoked, bit2 correction,
                               // bit3 fallbackHost, bit4 timeout, bit5 onGround
    uint16_t seq = 0;
    uint16_t pad = 0;          // align to 20 B
};
static_assert(sizeof(VehicleControlStatePayload) == 20, "...");
```

Sent by the host at 10–15 Hz **only while a lease is active or just revoked**.
It is control feedback, not a second transform stream: the authoritative
position/heading continues to arrive through the existing `WorldSnapshot`
0x10 and the existing vehicle interpolation. This avoids duplicating vehicle
bytes in the protocol.

## 5. Phase P1 — events without control transfer

Goal: the guest can affect the host world and both see the result, without
handing over driving.

### 5.1 Actions in P1

- `Damage` (vehicle body/engine/wheels/doors) — host applies
  `SET_VEHICLE_DAMAGE` / `SET_VEHICLE_ENGINE_HEALTH` / door-broken natives;
  `VehicleState.damageBits` updates the guest proxy.
- `DoorOpen`/`DoorClose`/`DoorLock`/`DoorUnlock` — host applies door natives;
  `VehicleState.doorBits`/lock updates both sides.
- `Enter`/`Exit` — seating (P1 visual sync; no driving). This is the bridge to
  P2.
- `Hijack` — seat hijack of a **non-mission parked** vehicle: host ejects NPC
  driver and seats the guest ghost. Guest can sit in the driver seat but does
  not yet drive (P3).
- `Loot` — host-world container/vehicle loot; no guest inventory transfer in
  P1; result/log only.
- `Hitch`/`Unhitch` — host-owned draft horses/vehicles; host applies; both see.
  Use `_ATTACH_DRAFT_VEHICLE_HARNESS_PED` /
  `_DETACH_DRAFT_VEHICLE_HARNESS_PED` for **wagon harness**; `_HITCH_HORSE` /
  `_UNHITCH_HORSE` are **world-point hitches**, not wagon harness (per native
  R&D). Harness enum 0..5: LR, RR, LM, RM, LF, RF.

P1 does **not** include: guest driving, guest control of host NPC AI, mission
item transfer, damage/hijack of mission-critical entities. `Loot` in P1 is
limited to host-side loot tables/coach-robbery-style interactions; there is
**no generic vehicle-inventory native** (R&D), so “loot this wagon into the
guest inventory” is P4/inventory-sync, not P1.

### 5.2 Host apply path

1. Inbox: server relays 0x23 to host; network callback only copies into a
   mutex-protected queue (same pattern as actor messages).
2. Game thread: after vehicle/occupant snapshot update, host drains a bounded
   number (e.g. 2) of intents per tick.
3. Validation (all before any native):
   - `epoch == g_hostEpoch`; `vehicleNetId` maps to a current host vehicle;
   - vehicle exists and is in the host snapshot/replicated set;
   - action known; seat/door/flags/param in range; `seq != 0`;
   - guest ghost/player within distance (P1: doors/enter ≤ 5 m; damage/hijack
     ≤ 12 m; loot/hitch ≤ 6 m; configurable, host world units);
   - seat free, driver exists for hijack, vehicle not dead/locked, guest not
     dead/incapacitated;
   - mission protection: `IsMissionEntity(vehicle)` / mission-owned → reject
     `MissionProtected` for damage/hijack/loot; mission vehicles may allow
     passenger `Enter` only if explicitly allowed (P4);
   - policy/ownership: host player's current vehicle may be protected from
     guest hijack/damage by config; guest cannot target its own host-side
     exceptions incorrectly;
   - per-vehicle action cooldown, per-second damage budget, idempotency cache.
4. Apply exactly one native group; read back state where available
   (`GET_VEHICLE_*_HEALTH`, door lock/state). Update host-side caches and
   `VehicleState`.
5. Send `VehicleInteractionResult`; on change send `VehicleState` (or mark for
   next heartbeat). Log with `vehicle: interaction ...` and result code.

Host apply budget: at most 2 intents/tick and 1 `SET_VEHICLE_DAMAGE`-class
native group per tick; queue over-budget intents for the next tick and return
`RateLimited`/`Accepted` only after actual application. This protects host
`perf: frame host` p95/max (see §10).

### 5.3 Guest side

- Guest vehicle proxy remains host-interpolated (`InterpolateVehicleProxies`),
  but P1 enables **collision on nearby non-warp proxies** (see §9).
- On `Applied`, guest applies the visual effect (damage/door) to its local
  proxy, using the same natives as a cosmetic mirror; no host state is written
  from the guest. On `Rejected`, no local change (or rollback if predicted).
- Guest reads `VehicleState` for damage/door/seat; this is the recovery path
  if an event message is lost.
- **P1/P2 driver seat is visual only.** If the guest sits in a driver seat
  before P3, local driving controls must be suppressed (sit animation allowed,
  engine/controls not authoritative; local proxy remains host-interpolated and
  cannot drive away). The UI/log should make clear that guest driving is not
  enabled until a P3 control lease. This prevents the confusing case where the
  guest locally drives a ghost proxy while the host wagon stays still.
- `PlayerVehicle`/`WorldSnapshot` continue to drive the visual proxy transform;
  no transform is added to P1 intents.

### 5.4 Traffic / rates (P1)

| stream | bytes on wire | typical | max |
|---|---:|---:|---:|
| `VehicleInteraction` 0x23 | 24+5 = 29 | 0.1–0.5/s | 8/s → 0.23 KB/s |
| `VehicleInteractionResult` 0x24 | 16+5 = 21 | same as intents | 20/s → 0.21 KB/s |
| `VehicleState` 0x26 | 24+5 = 29 | on change + 1/s/vehicle | 10/s → 0.29 KB/s |

P1 worst-case is < 0.5 KB/s per direction, negligible against v19 traffic
(host worst-case ~25 KB/s, target G3 ≤15 KB/s). No new heartbeat for every
vehicle; `VehicleState` heartbeat is 1/s only for recently touched/seat/
lease vehicles.

### 5.5 P1 tests

- C++: `static_assert` sizes, enums, `IsValid*` accept/reject tables.
- Server integration: host-only/guest-only directions, non-host drop/log,
  exact sizes, malformed action/seat/door/epoch, per-client rate caps, no echo,
  host migration clears routing.
- Fake client/host: extend `tools/fake_actor_client.py` or add a small
  `tools/fake_vehicle_client.py` to send 0x23 and assert 0x24/0x26; host fake
  applies/logs policy rejects; integration stays 41+.
- In-game: guest damages a wagon wheel/engine, opens/closes/locks a door,
  enters a parked wagon, hijacks a non-mission NPC driver (seat only), tries
  mission wagon (must reject), host sees guest seat, network loss recovery.
- Native R&D gate: do not implement a native action until its semantics,
  mission behavior and host-only constraints are verified in
  `docs/NATIVE_RESEARCH_INTERACT.md`.

## 6. Phase P2 — authoritative seating / player seat coordination

Goal: both players see correct seats for host player, guest player and NPCs;
no duplicate player vehicle/proxy; enter/exit cannot deadlock.

### 6.1 Current gaps

- `CollectVehicleOccupants` excludes `localPlayerPed`
  (`src/world_sync.cpp:417`); host player is never in 0x21.
- `PlayerVehicle` 0x12 is gated by `vehicle_sync`
  (`src/coop.cpp:6829+`), has a reserved `netId` field, and creates a separate
  proxy vehicle on the receiver (`ApplyRemotePlayerVehicle`,
  `src/coop.cpp:5727+`), which can duplicate a world-synced vehicle.
- 0x21 is host→guest only; the guest cannot report its host-side seat except
  as a `PlayerVehicle` visual state.
- Seat attach failures/timeouts are retried, but there is no host-authoritative
  seat ACK for players.

### 6.2 Design

- **Host is the seat authority.** A seat assignment is a host decision; a
  guest may request `Enter`/`Exit`/`Hijack` via 0x23 but cannot force a seat.
- **VehicleState 0x26 carries player seats:** `hostSeat` (host player) and
  `guestSeat` (guest player, host view), with `-1`/0..8/127 values. This is
  the fix for `driver=0` when the host player drives: host player seat is
  explicit, not an NPC occupant entry.
- **0x21 continues to carry NPC/draft occupants only.** No layout change; no
  guest player mixing into `actorId` fields.
- **`PlayerVehicle` 0x12 remains the local player's vehicle/proxy transform
  stream.** v20 activates its reserved `netId`:
  - `netId != 0` identifies the host world vehicle; receiver maps to the
    existing proxy instead of creating a second vehicle;
  - `netId == 0` keeps v19 behavior (model/coords/seat only);
  - receiver must not create a proxy for a `vehicleNetId` it already has from
    0x10;
  - host treats a guest `PlayerVehicle` as visual/seating state, not as damage
    or control authority.
- **Local player seat vs host-side ghost:** guest's local player can enter its
  local proxy for feel; host seats the guest's host-side ghost into the real
  vehicle at the same `vehicleNetId`/seat. Both sides display the seat; the
  host's seat is authoritative. `VehicleState.guestSeat` is the host view.
- **Seat reservation:** result `SeatOccupied` if free-seat check fails; host
  retries only on explicit re-request; no spam loop. Host and guest keep a
  last-known seat; on `Correction`, guest re-seats or exits.
- **Duplicate avoidance:** before creating a `PlayerVehicle` proxy, check
  `vehicleNetId` against the world proxy map; if present, apply seat/transform
  to that proxy. This is the main P2 correctness fix.
- **Enter/exit sequence (P2):**
  1. guest requests Enter (vehicleNetId, seat) via 0x23;
  2. host validates, sends result `Accepted`, seats guest ghost
     (`SET_PED_INTO_VEHICLE`/`TASK_ENTER_VEHICLE`), updates `guestSeat`;
  3. host emits `VehicleState` with `guestSeat` and `lease=0`;
  4. guest enters its local proxy and sets its local seat; on mismatch,
     applies host state (re-seat/warp/exit);
  5. exit reverses: host removes guest ghost, clears `guestSeat`; guest leaves
     local proxy; `VehicleState` confirms.
- **Host player driving:** host sends `VehicleState.hostSeat=-1` for its real
  vehicle; guest seats host ghost in host proxy even if 0x21 has no player
  entry. This is the direct fix for the reported missing host driver.
- **Seat conflict/host migration:** host clears `guestSeat` on leave/reset;
  new host has no old lease; guest re-requests seat after host change; stale
  `VehicleState` epoch dropped.

### 6.3 P2 traffic

P2 adds no new message type: `VehicleState` (P1) carries seats; existing 0x10
and 0x12 carry transform/visual state. `VehicleState` seat changes are
event-driven with 1 s heartbeat; no per-tick seat spam. Optional
`VehicleInteraction` Enter/Exit rate same as P1 (≤8/s).

### 6.4 P2 tests

- Integration: Enter/Exit/seat-occupied/host-player-seat messages; no duplicate
  0x12 proxy when `netId` matches; host migration clears seats.
- In-game: host drives, guest sees host seated; guest enters passenger/driver
  seat; host and guest both see correct seat; exit; reconnect; mission vehicle
  seat policy.

## 7. Phase P3 — guest driving (control lease)

This is the hardest phase. It is intentionally separate from P1/P2.

### 7.1 Lease handshake

1. Guest requests `Enter` driver seat or `Hijack` via 0x23 (P1/P2).
2. Host validates policy (`guest_can_drive`), distance, seat, mission state,
   and **host control arbitration**: there is no native “transfer control”; the
   host player must not be actively driving the same vehicle, or the host must
   explicitly grant it and suppress/handbrake its own input. Host input always
   wins. Creates a **control lease**:
   ```
   leaseId = 16-bit monotonically increasing id (per host epoch)
   holder = guest name/role
   vehicleNetId, seat=-1
   expires = now + 300 ms without input
   ```
   Host sends `VehicleInteractionResult` with `Applied`; `detail` lower 16 bits
   = leaseId, high bits = flags (control granted). Host includes
   `VehicleState.flags |= kVehicleStateFlagLeaseActive` and
   `leaseId`.
3. Guest starts sending 0x25 at 10–15 Hz.
4. Host applies bounded input to the real vehicle; sends `VehicleControlState`
   at 10–15 Hz (active/correction/revoked).
5. Exit/revoke/timeout/disconnect/migration: host clears lease, sends result/
   state, stops applying input; guest stops sending; local proxy returns to
   host interpolation/fallback.

### 7.2 Host application model

- Inputs are **throttle/brake/steer**, not absolute position. Host clamps:
  - throttle/brake to 0..255; steer to -127..127;
  - forward speed ≤ vehicle-specific cap (e.g. `kGuestMaxSpeedMps` 20–30 m/s;
    wagon lower); acceleration/jerk limits;
  - only applies when seat is driver, lease active, actor alive, vehicle
    exists, not mission-protected;
  - input seq must be newer than last; stale/duplicate dropped; rate-limited
    server-side and host-side.
- Host natives (target 1491.50, per `docs/NATIVE_RESEARCH_INTERACT.md`;
  status labels in §12):
  - `SET_VEHICLE_FORWARD_SPEED(0xF9F92AF49F12F6E7)` — **SIGNATURE-ONLY**;
    test on real host vehicle and against host player input/AI;
  - `SET_VEHICLE_STEER_BIAS(0x84DAAE11E9EE4FC3)` — **CONFIRMED** signature/
    comment; call every tick while the lease is active;
  - `SET_VEHICLE_HANDBRAKE(0x91BE51AEC4E99710)` /
    `SET_VEHICLE_ENGINE_ON(0xB64CFA14CB9A2E78)` for stop/start/revoke;
  - `SET_VEHICLE_ON_GROUND_PROPERLY(0x7263332501E07F52)` after spawn/warp;
  - AI fallback for NPC drivers: `TASK_VEHICLE_TEMP_ACTION(0xC429DCEEB339E129)`
    (actions 1/9/23/27; never 16/17/18) or
    `TASK_VEHICLE_DRIVE_TO_COORD(0xE2A2AA2F659D77A7)`;
  - no direct `SET_ENTITY_COORDS` from guest input; host physics owns position.
    If host correction does use `SET_ENTITY_COORDS`, the native has **8 args**
    (current project wrapper has a 7-arg bug — fix it first).
- Host sends `VehicleControlState` with `inputSeqAck`, actual speed/steer,
  flags. If host cannot apply (vehicle dead/stuck/mission), it revokes with
  `fallbackHost`/`NoAuthority`/`MissionProtected` and stops input.
- Host's existing `WorldSnapshot` 0x10 continues to carry the authoritative
  transform at 10 Hz; no second transform stream.

### 7.3 Guest local prediction and correction

- While lease is active, guest may drive its local proxy from its input for
  responsiveness; it is **prediction**, not authority.
- Guest receives host 0x10 and 0x27; computes error between predicted and host
  state.
- Correction thresholds (proposal, tune in game):
  - position error > 1.5 m or heading error > 15°: soft lerp back within
    ~150 ms;
  - error > 4 m or > 35°, or host `Correction` flag: hard rewind/snap of the
    local proxy; clear local velocity; resume interpolation;
  - repeated hard corrections (>3 in 1 s): guest stops prediction, falls back
    to host interpolation and displays/logs “driving fallback”.
- Local collision: enable collision on the local proxy while guest drives so
  crashes feel real; host physics remains authoritative. If the local proxy is
  blocked but host state keeps moving, host correction warps the proxy; if the
  host vehicle is blocked and guest prediction would pass through, the host
  state wins. The guest must never “win” a conflict by local physics.
- Input timeout: if guest misses >300 ms of 0x25, host revokes lease, applies
  brake/stop, sends `Revoked`; guest falls back. This avoids a runaway wagon on
  network loss.
- Guest cannot alter `leaseId`, `vehicleNetId`, speed clamp or seat; any
  mismatch is `NoAuthority` and lease revoked.

### 7.4 P3 traffic

| stream | bytes | rate | max KB/s |
|---|---:|---:|---:|
| 0x25 ControlInput | 16+5 = 21 | 10–15 Hz while driving | 0.32 |
| 0x27 ControlState | 20+5 = 25 | 10–15 Hz while driving | 0.38 |
| existing 0x10 transform | — | already streamed | 0 |
| existing `VehicleState` | 24+5=29 | 1 Hz + changes | 0.03 |

P3 adds < 0.8 KB/s per direction while driving, and stops when not driving.
No extra transform bytes; the main network cost of v19 remains player frames/
actor frames/vehicles. Accepted under G3; verify with `perf: net` counters and
`perf_report.py` during P3 acceptance.

### 7.5 P3 risks/tests

Risks: desync, teleport/warp spam, host frame native spikes, collision
conflicts, latency, input replay, host migration. Tests:

- integration: lease grant/revoke, wrong/old lease, rate limiting, host
  migration clears lease, control state host-only;
- fake client: input burst/lag; server drops over-rate; no transform accepted;
- in-game: guest drives short distance, collision, lag (simulate 200–400 ms),
  input loss fallback, host Alt-F4 → no stale control, guest disconnect mid-
  drive, mission vehicle rejects driving;
- perf: host frame p95/max during input bursts (≤2 ms target), traffic G3.

## 8. Phase P4 — NPC/items/missions (after P3)

Candidate actions, all host-applied after validation:

- push/grapple/rob NPCs, knock NPC from vehicle, loot NPC/vehicle container;
- hitch/unhitch draft horses, lead/tie horses;
- mission items / scripted interactions (strictly host-side; mission state and
  completion remain host-only; guest cannot alter mission progress).

P4 needs an **item/inventory transfer design** for loot (host world loot cannot
silently move to guest inventory). Until then, loot results are visual/event
only or host drops a local pickup; document the limitation.

Mission protection rules:

- mission/script-owned vehicles: no damage/hijack/drive; passenger Enter only
  if explicitly allowed and mission state permits;
- mission NPCs/animals: no damage via vehicle intents (ActorDamage already
  host-authoritative for NPCs, but keep mission policy);
- guest’s local game must not advance/modify host mission state; mission
  barrier/state remains host→guest; guest remains a spectator/participant in
  host world only.

## 9. Local guest proxies: physical and enterable

Current behavior (`src/world_sync.cpp:2330-2347`): guest vehicle proxies are
`SET_ENTITY_AS_MISSION_ENTITY`, `SET_ENTITY_INVINCIBLE(true)` and
`SetNoCollision(veh, true)` (collision off). That is why wagons are ghosts.
Proposal:

1. **Phase 0/P1:** enable collision on vehicle proxies near the guest player
   (`SET_ENTITY_COLLISION(veh, true, true)`); keep them mission/invincible for
   host authority; make them enterable locally (local player can enter proxy
   for feel). Do not make them fully dynamic/authoritative.
2. **Enterable/tangible mode:** only for proxies within interaction radius and
   not in warp/correction; collision toggled off during hard warp/snap to
   avoid pushing the player, then back on.
3. **P3 driving:** local proxy becomes locally controllable for prediction;
   host state remains authoritative; correction/teleport disables collision
   during snap; fallback returns to host-interpolated mode.
4. **Conflict rule:** if local proxy collision/physics disagrees with host
   state, host state wins; guest proxy is warped/interpolated back. Never let
   local penetration or local wagon speed change host’s real vehicle without a
   host intent/lease.
5. **Patch/rejection:** on rejected interaction (doors/damage/seat), local
   proxy returns to last authoritative `VehicleState`/snapshot; no permanent
   local-only state.
6. **Far proxies:** keep collision off to avoid ghost collisions lagging behind
   host snapshots; enabling only near avoids “invisible wall” artifacts.

This is deliberately not “guest owns local physics”; it is a local feel layer
under host authority.

## 10. Anti-grief / validation / rate limits

### 10.1 Server (format/role/rate only)

| message | accepted from | validation | rate |
|---|---|---|---|
| 0x23 | guest | exact 24 B, epoch/vehicle/seq != 0, action 1..11, seat/door/flags ranges, target/param ranges | 8/s |
| 0x24 | host | exact 16 B, epoch/vehicle/seq != 0, action/result known | 20/s |
| 0x25 | guest | exact 16 B, epoch/vehicle/lease/seq != 0, steer -127..127, flags known | 20/s |
| 0x26 | host | exact 24 B, epoch/vehicle != 0, seat values, flags known, seq | 10/s |
| 0x27 | host | exact 20 B, epoch/vehicle/lease != 0, flags known | 15/s |

Server does not validate distance/ownership/mission state (no world view); it
logs and drops role violations, malformed packets and over-rate packets. It
must count host 0x24/0x26/0x27 in host game-thread liveness; guest messages do
not keep a host alive.

### 10.2 Host semantic validation

Before any native, reject with a result code:

- epoch mismatch (stale session);
- `vehicleNetId` not in current host replicated set/handle cache;
- guest dead/incapacitated/not near (distance by action; no line-of-sight
  requirement for v1, optional raycast later);
- action not allowed for vehicle class/mission/ownership; host player's
  current vehicle protected unless `guest_can_hijack_host_vehicle=1`;
- seat/door/part/harness out of range; seat occupied; locked; driver missing;
- cooldown per `(vehicle, action)`; damage budget per second/vehicle/part;
  max number of vehicle interactions per tick/queue;
- P3 only: valid lease/epoch/vehicle/holder/seat; input seq newer; input
  timeout; speed/accel/steer clamps; host not in cutscene/mission barrier.

### 10.3 Anti-grief controls

- Server rate caps and exact-size enforcement stop floods/malformed spam.
- Host action cooldowns and damage caps stop “one-shot delete my wagon”.
- Mission entities are protected by default; host can opt in per mission.
- Host player's vehicle/horse can be protected from guest hijack by config.
- Guest never sends absolute transform, health, door or seat authority; the
  only transform authority is host 0x10.
- No silent application: every intent gets a result; host logs
  `vehicle: interaction action=... result=...`; over-limit logs are rate
  limited.
- Idempotency (`epoch,seq`) prevents replay of old intents; control lease
  prevents stale input.
- Abuse reporting: host can revoke/disable guest interactivity at runtime via
  config/console; UI result shows “denied”.

### 10.4 Proposed config / rollout defaults

These are new INI keys only when the phase is implemented; both peers must
match. Suggested defaults are conservative:

| key | default | meaning |
|---|---:|---|
| `vehicle_interaction` | `1` | P1 guest intents for non-mission vehicles |
| `vehicle_interaction_mission` | `0` | allow guest damage/hijack/enter on mission vehicles |
| `vehicle_interaction_host_vehicle` | `0` | allow guest damage/hijack of the host player's current vehicle |
| `guest_can_drive` | `0` | P3 control leases; host opt-in |
| `guest_drive_max_speed_mps` | `20` | hard cap, clamped by vehicle/type |
| `vehicle_interaction_debug` | `0` | result/queue logs |

Defaults preserve the current “safe host world” for users who do not opt in;
the user-facing acceptance INI enables only the P1 rows after they are
implemented. Do not add these keys to v19; they ship with v20 only.

### 10.5 Host migration

- `HostChanged`: client resets `world_sync`, actor proxies, remote player
  vehicle and vehicle-interaction state (as existing reset does for actor/world
  state).
- All vehicle intents/control leases are cleared; `leaseId` invalid; guest
  stops sending 0x25; old host's `VehicleState`/results with old epoch are
  dropped.
- New host rebuilds vehicle netIds from its own WorldSnapshot; guest re-learns
  identity; guest re-requests seat/enter if desired.
- Server relays only to/from the new host; host-only role validation uses the
  promoted player; old pending packets/results are dropped.
- Local guest proxies reset/despawn; no stale wagon control or duplicate
  proxies.

## 11. Protocol/traffic/ordering summary

- `kProtocolVersion = 20`; new IDs 0x23–0x27; existing structs/IDs unchanged.
- `WorldSnapshot` 0x10 remains the authoritative transform/interpolation
  stream; `VehicleOccupants` 0x21 remains NPC/draft occupants;
  `PlayerVehicle` 0x12 remains the local player's vehicle/proxy stream and
  v20 uses its reserved `netId` for identity/seat mapping.
- New state is small and event/heartbeat based: P1/P2 <0.5 KB/s, P3 <0.8 KB/s
  while driving; no per-vehicle heartbeat, no duplicated transform.
- Ordering: intent seq is the request order; host applies in receive/tick
  order after validation; result seq echoes; control input seq is monotonic
  and old/duplicate dropped; host state seq only for diagnostics/recovery.
- Loss/recovery: heartbeat 0x26 (1 s) repairs damage/door/seat state; host
  result cache + guest re-request repairs intents; `PlayerVehicle`/0x10 repair
  transform; P3 control has 300 ms timeout and fallback.
- Release/rollout: v20 is a paired ASI+server snapshot (`dist\npc-e17+`), not a
  hot-swap. `docs/PROTOCOL.md` must add the five message specs, `docs/PERF.md`
  the new `Vehicle` traffic bucket, and `docs/ACCEPTANCE_2PC.md` a vehicle
  interaction block before deploy. Old v19 peers are rejected by JOIN version
  check; no mixed-version compatibility is promised.

## 12. Native mapping (synced with `docs/NATIVE_RESEARCH_INTERACT.md`)

**Important:** the local `third_party/rdr3-nativedb-data/natives.json`
snapshot is build 1207 and its hashes for several vehicle natives **differ**
from the target RDR2 1491.50 ScriptHook/NativeDecl hashes. Use the table
below (from `docs/NATIVE_RESEARCH_INTERACT.md`, verified against
NativeDecl 1491.50/latest nativedb) and never copy a hash from an untrusted
native DB without the same cross-check. Status is the researcher's label:

- **CONFIRMED** — hash/arity/parameter order present in NativeDecl 1491.50
  and/or latest nativedb and semantics confirmed by code/ScriptHook or
  documentation; still in-game check for visual effects.
- **SIGNATURE-ONLY** — hash/signature known, effect not verified in game.
- **UNCERTAIN** — semantics/parameters undocumented; mandatory in-game A/B.

The intent protocol is independent of exact native choice; these are the
expected mappings, not permission to implement unverified actions.

| purpose | native | hash (1491.50/latest) | status / notes |
|---|---|---|---|
| body/part damage | `SET_VEHICLE_DAMAGE` | `0x1D7678F81452BB41` | CONFIRMED signature/comment; `(veh, xOff,yOff,zOff, damage, radius, BOOL p6)`; p6 test false; may be blocked by invincible/strong/mission |
| body health | `SET_VEHICLE_BODY_HEALTH` | `0x55CCAAE4F28C67A0` | CONFIRMED signature; range test |
| engine health | `SET_VEHICLE_ENGINE_HEALTH` | `0x8BDC5B998B4654EF` | CONFIRMED comment: 1000 max, -4000 destroyed |
| repair | `SET_VEHICLE_FIXED` | `0x79811282A9D1AE56` | CONFIRMED; engine may not be repaired (comment) |
| door open | `SET_VEHICLE_DOOR_OPEN` | `0x550CE392A4672412` | CONFIRMED; works on trains/wagons |
| door shut | `SET_VEHICLE_DOOR_SHUT` | `0x6A3C24B91FD0EA09` | CONFIRMED |
| door broken | `SET_VEHICLE_DOOR_BROKEN` | `0x9666CF20A1C6D780` | CONFIRMED; `deleteDoor` |
| door control/latch | `SET_VEHICLE_DOOR_CONTROL` / `SET_VEHICLE_DOOR_LATCHED` | `0xD57F10EBBA814ECF` / `0x06F8A202EB312A3C` | control CONFIRMED; latch SIGNATURE-ONLY (p2..p4 unknown) |
| door lock | `SET_VEHICLE_DOORS_LOCKED` / `_INDIVIDUAL_` | `0x96F78A6A075D55D9` / `0xA9F1D75195CC40F6` | SIGNATURE-ONLY; lock enum unconfirmed; test |
| lock readback | `GET_VEHICLE_DOOR_LOCK_STATUS` | `0xC867FD144F2469D3` | SIGNATURE-ONLY; enum test |
| break locks | `_BREAK_LOCKS_ON_VEHICLE` | `0x9D12796EF4BF9EA9` | CONFIRMED (coach robbery) |
| window | `REMOVE_VEHICLE_WINDOW` | `0x745F15A215F2DDF1` | CONFIRMED; windowIndex 0 FR,1 FL,2 BR,3 BL |
| enter/exit | `SET_PED_INTO_VEHICLE` / `TASK_ENTER_VEHICLE` / `TASK_LEAVE_VEHICLE` | `0xF75B0D629E1C063D` / `0xC20E50AA46D09CA8` / `0xD3DBCE61A490BE02` | CONFIRMED; enter flags `ECF_JACK_ANYONE (1<<3)`, `ECF_WARP_PED (1<<4)`, `ECF_JUST_PULL_PED_OUT (1<<19)`, `ECF_DONT_WAIT_FOR_VEHICLE_TO_STOP (1<<6)` |
| seat checks | `IS_VEHICLE_SEAT_FREE` / `ARE_ANY_VEHICLE_SEATS_FREE` / `GET_PED_IN_VEHICLE_SEAT` | `0xE052C1B1CAA4ECE4` / `0xA0A424505A1B6429` / `0xBB40DD2270B65366` | CONFIRMED; host validates seat; seat enum in §3.2 |
| driver lookup | `GET_DRIVER_OF_VEHICLE` / `GET_VEHICLE_PED_IS_IN` | `0x2963B5C1637E8A27` / `0x9A9112A0FE9A4713` | CONFIRMED |
| model seat count | `GET_VEHICLE_MODEL_NUMBER_OF_SEATS` / `GET_VEHICLE_MAX_NUMBER_OF_PASSENGERS` | `0x9A578736FF3A17C3` / `0xA9C55F1C15E62E06` | CONFIRMED; seat -1 driver, 0..max-1 passengers |
| hijack/board | `_TASK_BOARD_VEHICLE` / `_TASK_BOARD_VEHICLE_2` / `TASK_BREAK_VEHICLE_DOOR_LOCK` | `0xE53D17AD837CBF7C` / `0xE41A09C8DDFF7AA4` / `0xBB28D1BC9EA8A6A5` | board SIGNATURE-ONLY/UNCERTAIN; break lock CONFIRMED signature; prefer `TASK_ENTER_VEHICLE` + flags after test |
| speed control | `SET_VEHICLE_FORWARD_SPEED` | `0xF9F92AF49F12F6E7` | SIGNATURE-ONLY; host real/NPC only; may conflict with player input/AI; test wheel spin |
| steer | `SET_VEHICLE_STEER_BIAS` | `0x84DAAE11E9EE4FC3` | CONFIRMED comment; call every tick; steering unlocks when not called |
| handbrake/engine | `SET_VEHICLE_HANDBRAKE` / `SET_VEHICLE_ENGINE_ON` | `0x91BE51AEC4E99710` / `0xB64CFA14CB9A2E78` | CONFIRMED |
| ground correction | `SET_VEHICLE_ON_GROUND_PROPERLY` | `0x7263332501E07F52` | CONFIRMED; after transform/warp |
| AI fallback | `TASK_VEHICLE_TEMP_ACTION` / `TASK_VEHICLE_DRIVE_TO_COORD` / `TASK_VEHICLE_DRIVE_WANDER` | `0xC429DCEEB339E129` / `0xE2A2AA2F659D77A7` / `0x480142959D337D00` | temp-action CONFIRMED (actions 1 brake, 9 weak accel, 23 fast accel, 27 brake; **16/17/18 crash/dangerous**); AI tasks need a driver ped |
| speed readback/clamp | `GET_ENTITY_SPEED` / `_GET_VEHICLE_ESTIMATED_MAX_SPEED` | `0xFB6BA510A533DF81` / `0xFE52F34491529F0B` | CONFIRMED |
| wheels | `SET_VEHICLE_WHEELS_CAN_BREAK` / `_BREAK_OFF_VEHICLE_WHEEL` / `_IS_VEHICLE_WHEEL_DESTROYED` | `0x839137C40275FB77` / `0xD4F5EFB55769D272` / `0xCB2CA620C48BC875` | CONFIRMED; no public wheel-rotation native; wheel damage via `SET_VEHICLE_DAMAGE` |
| hitch horse | `_HITCH_HORSE` / `_UNHITCH_HORSE` | `0x06AADE17334F7A40` / `0x0348469DAA17576C` | CONFIRMED signature; world hitch, not wagon harness |
| draft harness | `_ATTACH_DRAFT_VEHICLE_HARNESS_PED` / `_DETACH_DRAFT_VEHICLE_HARNESS_PED` / `_GET_PED_IN_DRAFT_HARNESS` | `0x316CDB5B6E8F4110` / `0xB36D3EC70963BE60` / `0xA8BA0BAE0173457B` | CONFIRMED; harness enum 0..5 LR,RR,LM,RM,LF,RF |
| draft control | `_SET_DRAFT_VEHICLE_DESIRED_SPEED` / `IS_DRAFT_VEHICLE` | `0x0C3F0F7F92CA847C` / `0xEA44E97849E9F3DD` | desired speed SIGNATURE-ONLY; test remote wagon movement |
| draft wheel/straps | `_BREAK_OFF_DRAFT_WHEEL` / `_BREAK_VEHICLE_STRAPS` | `0xC372B6A88F6E4AD8` / `0xD1EFA8D68BF5D63D` | CONFIRMED signature; only if the break mechanic is designed |
| loot | `TASK_LOOT_ENTITY` / `GET_LOOTING_PICKUP_TARGET_ENTITY` / `_SET_FORCE_COACH_ROBBERY_LOOT` | `0x48FAE038401A2888` / `0x14169FA823679E41` / `0xF489F94BFEE12BB0` | CONFIRMED signatures; **no generic vehicle-inventory native**; coach robbery loot table is SIGNATURE-ONLY/UNCERTAIN; P4 only after confirmation |
| transform/velocity | `SET_ENTITY_COORDS` / `SET_ENTITY_ROTATION` / `SET_ENTITY_VELOCITY` / `FREEZE_ENTITY_POSITION` | `0x06843DA7060A026B` / `0x9CC8314DFEDE441E` / `0x1C99BB7B6E96D16F` / `0x7D9EFB7AD6B19754` | CONFIRMED; `SET_ENTITY_COORDS` has **8 args** (current project wrapper has a 7-arg bug — fix before host correction) |
| proxy physics (local) | `SET_ENTITY_DYNAMIC` / `SET_ENTITY_HAS_GRAVITY` / `SET_ENTITY_COLLISION` | `0xFBFC4473F66CE344` / `0x0CEDB728A1083FA7` / (existing wrapper) | CONFIRMED signatures; only for local proxy A/B; host authority remains |
| player-driven conflict flags | `SET_VEHICLE_IS_CONSIDERED_BY_PLAYER` / `SET_VEHICLE_NOT_STEALABLE_AMBIENTLY` / `SET_VEHICLE_IS_STOLEN` | `0x54800D386C5825E5` / `0x09C970AE59ABF6B2` / `0x6C32FC81DFF25C9A` | CONFIRMED signatures; proxy/theft UX; local Player only for some natives |

### 12.1 Wheel-spin finding (user complaint “колёса статичны”)

The native researcher found **no public wheel-rotation native** in 1491.50:
`SET_VEHICLE_WHEEL_ROTATION`/`_SET_VEHICLE_WHEEL_ROTATION` and
`SET_VEHICLE_WHEELS_*` setters (except can-break/destroy) are absent;
`SET_ENTITY_ROTATION` rotates the whole vehicle, not wheels. Guest vehicle
proxies currently get only interpolated `SET_ENTITY_COORDS`
(`src/world_sync.cpp:2367-2415`), so physical speed is zero and wheels do not
spin. Options, in order of risk:

1. **A/B only (recommended first):** keep interpolation and call
   `SET_VEHICLE_FORWARD_SPEED(proxy, hostSpeed)` each tick after interpolation;
   test whether wheels spin without moving the proxy. Do not promise it.
2. **Physical proxy:** `SET_ENTITY_DYNAMIC(proxy,true)`, gravity, collision off
   for hosts (on for local interaction), `SET_VEHICLE_FORWARD_SPEED` +
   `SET_VEHICLE_STEER_BIAS` + `SET_VEHICLE_ON_GROUND_PROPERLY` each tick;
   correct only when error >1–2 m. High risk: drift/rollover/draft losses.
3. **Wagon/draft:** also test `_SET_DRAFT_VEHICLE_DESIRED_SPEED`; forward
   speed may not pull draft horses. Do not break straps/harness unless the
   break mechanic is explicitly designed.

If A/B fails, either accept static wheels in P1/P2 or defer wheel visuals to a
separate visual R&D; the interaction protocol does not depend on wheel spin.
This is an explicit **P3/perf risk**, not a promise of the design.

### 12.2 Host control arbitration (no “transfer control” native)

The research confirms there is **no native to transfer control** from the host
player to a remote guest. Therefore:

- P3 leases only when the host is **not** actively driving that vehicle, or
  when the host explicitly enables guest control and the host player's input is
  suppressed/handbrake-held for the lease. Host input always wins.
- If the host is a passenger and an NPC driver exists, the host may use
  `TASK_VEHICLE_TEMP_ACTION`/AI tasks (driver ped) or direct forward speed;
  some natives override the NPC task.
- If the host player is driving, reject guest control until the host stops and
  grants; do not fight the host's input frame-by-frame.
- Draft wagons may ignore forward speed; fall back to `_SET_DRAFT_VEHICLE_DESIRED_SPEED`
  or re-enable AI/host control; if not stable, P3 for draft wagons is deferred.

### 12.3 Mission/draft/network restrictions (from R&D)

- Mission entities (`IS_ENTITY_A_MISSION_ENTITY`, `SET_ENTITY_INVINCIBLE`,
  `SET_VEHICLE_STRONG`, `SET_VEHICLE_CAN_BREAK`) can block or make damage/
  control undefined; validate and reject by default.
- Guest proxies are `isNetwork=false`, mission/invincible/no-collision;
  native effects on them are local visual only; host authority is mandatory.
- `SET_ENTITY_AS_MISSION_ENTITY` should be cleared before deleting proxies.
- Draft wagons: forward speed may not pull horses; harness attach/detach and
  desired-speed natives must be tested; mission/draft policy is P4.
- No direct remote native invocation; all commands go through host process.

Remaining open native questions (after R&D): target-build loot hash; lock enum;
`_TASK_BOARD_VEHICLE` parameters/flags; whether `SET_VEHICLE_FORWARD_SPEED`
spins wheels without moving proxy; draft desired-speed behavior; exact
`SET_ENTITY_COORDS` wrapper fix; whether damage persists after snapshot/heartbeat.
Do not implement these actions until the researcher marks them CONFIRMED and
the in-game A/B is recorded.

## 13. Testing plan

### 13.1 Protocol/unit (headless)

- `static_assert` sizes 24/16/16/24/20; enum/flags known-mask.
- Validator tables: good/malformed/oversized/trailing/unknown action/seat/
  door/flags/epoch/seq.
- Server tests: exact sizes, direction (guest-only/host-only), rate windows,
  no echo, host migration, `IsGameThreadMessage`.
- C++ target: new `tests/vehicle_interaction_protocol_test.cpp` (or extend
  protocol tests) + server integration additions; keep C++7+.

### 13.2 Integration/fake clients

- `tools/fake_actor_client.py` currently supports actor damage; add optional
  vehicle message packers/senders or a sibling `fake_vehicle_client.py`.
- Tests:
  - host→guest result/state relay; guest→host intent relay; role violations
    drop and log; over-rate drop; malformed payload drop; no echo.
  - fake guest sends Enter/Door/Damage; fake host logs/applies nothing (host
    game thread not in fake) and returns applied/rejected results; assert
    exact bytes/seq.
  - fake P3 input rate/timeout; host result lease grant/revoke; no transform
    fields accepted.
  - host migration clears intents/leases; stale epoch dropped.
- Existing 41 integration tests must stay green; add counts to 41+.

### 13.3 In-game 2-PC scenarios (acceptance additions)

| # | scenario | pass criteria |
|---|---|---|
| I1 | guest damages non-mission wagon wheel/engine | host real vehicle damaged, guest proxy shows damage, result Applied, state heartbeat matches |
| I2 | guest opens/closes/locks door | host + guest door state matches; lock rejects enter; no mission damage |
| I3 | guest enters parked wagon passenger/driver | host seats guest ghost; guest local proxy seats guest; `VehicleState.hostSeat/guestSeat` correct; no duplicate vehicle |
| I4 | host drives, guest looks | guest sees host player at driver seat despite `localPlayerPed` exclusion (`driver=0` bug fixed) |
| I5 | guest hijacks non-mission NPC driver | NPC ejected/leaves, guest takes driver seat; no driving yet; mission wagon rejects |
| I6 | guest tries mission wagon damage/hijack | `MissionProtected`, no host change, log/result |
| I7 | network loss mid-intent | no duplicate application; retry/result idempotent |
| I8 | P3 guest drives short distance | host vehicle moves; guest local proxy corrects; host sees; no teleport/cheat; input timeout fallback |
| I9 | host migration during P2/P3 | lease/seats reset; no stale control; new host seat re-request works |
| I10 | guest collides with wagon locally | local proxy is solid; host state wins; no permanent local-only damage/position |

### 13.4 Perf/traffic gates

- Host frame p95/max (existing `perf: frame host`), guest tick, `perf: net`
  bytes/messages per bucket; P1/P2 <0.5 KB/s, P3 <0.8 KB/s.
- Host native budget: ≤2 intents/tick, ≤1 damage/vehicle group/tick; no
  persistent p95 regression; max spike bounded.
- Server: exact-size/rate tests; no queue growth; host timeout unaffected by
  guest intents.

## 14. Implementation order (ub-mode units)

Each unit: smallest change → failing test first where practical → build/CI →
no wire change in earlier phases → record proof. Suggested order:

0. **V19 Phase 0 (no wire bump):**
   - **Host player seat visibility (P0 bug):** `CollectVehicleOccupants`
     excludes `localPlayerPed` (`src/world_sync.cpp:417`); the host must send
     its own vehicle/seat through `PlayerVehicle` 0x12 (reserved `netId`) even
     when `vehicle_sync=0`, or the guest will always see `driver=0`. This is
     a paired-build semantic hotfix; if strict protocol discipline is required,
     skip it and fix it in v20 P2 via `VehicleState`.
   - **Proxy identity/duplicate prevention:** host/guest must map
     `PlayerVehicle.netId` to the existing world proxy and must not create a
     second vehicle for the same `vehicleNetId` (`ApplyRemotePlayerVehicle`,
     `src/coop.cpp:5727+`). This is required before any guest enter/seat.
   - **Collision/enterable proxy:** replace unconditional
     `SetNoCollision(veh, true)` (`src/world_sync.cpp:2345`) for nearby
     non-warp proxies with collision-enabled local proxy mode (§9); keep
     mission/invincible host authority.
   - **Occupant reliability:** host log `occupants vehicle=… n=… driver=0`;
     ensure host player seat is explicit, occupant actor handles are in the
     host roster before sending, guest applies seat/draft with retries, and
     `npc_occupants=0` rollback still works. This is a bugfix/reliability
     unit, not guest influence.
   - tests: in-game C2/C3/S25 and no `driver=0` when host player drives; no
     protocol change.
1. **v20 protocol constants/structs/validators** (wire + tests; no behavior).
2. **Server routing/rate/role** for 0x23–0x27; integration fake tests.
3. **Host P1 intent apply + guest P1 mirror** (damage/doors/enter/hijack
   seat/loot/hitch) with validation/queue/result; in-game I1–I6.
4. **P2 seat authority** (`VehicleState.hostSeat/guestSeat`, 0x12 netId
   mapping, duplicate proxy prevention, refresh); I3/I4/I7.
5. **P3 control lease** (0x25/0x27, driver input/state/correction/fallback);
   I8/I9; perf.
6. **P4 NPC/items/missions** (policy, inventory/loot design; I10/P4).
7. Docs/acceptance/protocol update; dist tag/deploy pair; rollback plan.

## 15. Open questions for Lead

1. **Scope/priority:** P1 (damage/doors/enter/hijack seat) first, or include P3
   driving in the same milestone? Recommendation: P1+P2 first; P3 second.
2. **Mission policy:** allow guest damage/hijack of mission/script vehicles by
   default? Recommendation: protected by default, config opt-in.
3. **Host player's vehicle:** allow guest hijack/damage by default?
   Recommendation: protect host's current vehicle from hijack/damage unless
   host enables it; passenger enter allowed.
4. **v20 bump:** paired deploy only; v19 clients rejected; acceptable?
5. **Config:** new keys `vehicle_interaction`, `guest_can_drive`,
   `vehicle_interaction_mission`, `vehicle_interaction_host_vehicle`? Defaults
   and acceptance INI.
6. **Loot/items:** P1 loot host-only without guest inventory transfer, or wait
   for inventory sync? Recommendation: host/visual only in P1.
7. **Host migration/peer count:** v20 designed for 2 players; if more later,
   add player id/owner fields (reserve in structs now).
8. **Native R&D gate:** agree no native action is implemented until
   `docs/NATIVE_RESEARCH_INTERACT.md` verifies it; if the R&D file arrives
   after this doc, update §12/§5 before implementation.

## 16. Rejected/low-value alternatives

- **Reuse `ActorDamage` for vehicle damage:** no vehicleNetId/part; would
  require fake actor IDs and host guesswork; not safe.
- **Reuse `ActorEvent`:** wrong direction (host→guest); changing server
  direction would silently change v19 semantics.
- **Reuse `ActionState`:** no vehicle/seat/target fields; no ACK/anti-grief;
  action enum is player-state-specific.
- **Guest sends absolute transform / drives proxy only:** produces ghost world
  divergence, cheating, and host/guest disagreement; rejected.
- **Server authoritative vehicle physics:** server has no world/native access;
  impossible.
- **Guest owns vehicle physics locally:** host world would not change; rejected
  by requirement “guest influences host world”.

## 17. Summary of user-visible gains by phase

- **Phase 0:** wagons feel solid/enterable; host player driver visible; no
  more `driver=0` ghost look; no host-world influence yet.
- **P1:** guest can damage/break doors/vehicle parts, open/lock doors, enter,
  hijack seat (non-mission), hitch/unhitch, loot visual; host world changes
  for real. Maximum effect/effort ratio.
- **P2:** correct multi-seat/enter/exit and player seats; no duplicate
  proxies; robust reconnect/migration.
- **P3:** guest can drive a host wagon/mount with host authority, crashes and
  corrections; fallback safe.
- **P4:** NPC/rob/items/mission-aware interactions; still host-owned world.
