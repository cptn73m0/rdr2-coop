# v20 P1 — guest→host vehicle interactions: implementation-ready spec

**Status:** implementation-ready spec (pre-code). Код мода НЕ менялся; это документ.
**Target protocol:** v20 (`kProtocolVersion = 20`), пара ASI+server, paired deploy.
**Author:** architect-verify2 (task: v20 P1 spec), 2026-10-27.
**Base:** HEAD `1e336e9` (npc-e19, protocol v19), code line refs — `E:\rdr2-coop`.
**Related:** `docs/INTERACTIVITY.md` (design P0–P4), `docs/NATIVE_RESEARCH_INTERACT.md`
(native R&D, 1491.50), `docs/PROTOCOL.md`, `docs/PERF.md`, `docs/ACCEPTANCE_2PC.md`.

> **Решение по объёму:** P1 = событийные интенты гостя к хосту **без передачи
> управления**. В P1 нет вождения (0x25/0x27 остаются зарезервированными под P3),
> нет инвентарного трансфера лута, нет изменения mission state. Хост остаётся
> единственным автором реального мира.

---

## 1. Краткое решение (TL;DR)

1. **Три новых сообщения**, остальные ID — резерв:
   - `0x23 VehicleInteraction` (24 B) — гость → сервер → хост;
   - `0x24 VehicleInteractionResult` (16 B) — хост → сервер → гость (unicast-по-смыслу,
     при `kMaxPlayers=2` = broadcast остальным);
   - `0x26 VehicleState` (24 B) — хост → сервер → гость (change + heartbeat 1/s);
   - `0x25`/`0x27` **зарезервированы** под P3 (`VehicleControlInput/State`), в P1
     не реализуются и не шлются.
2. **`VehicleOccupants` 0x21 не расширяем**: 0x21 остаётся NPC/draft occupants
   host→guest; vehicle health/doors/seats/player-seat идут отдельным 0x26.
   Причины: другой уровень состояния (vehicle, не actorId), другой rate/heartbeat,
   не ломаем layout/валидаторы v19.
3. **Сервер** — формат/роль/rate-boundary, без знания мира: он не может проверить
   `vehicleNetId`, дистанцию, mission/ownership; это делает хост до вызова натива.
4. **Хост** применяет ≤2 интента/тик, с валидацией (epoch/netId/handle/distance/
   mission/cooldown/damage budget/idempotency), возвращает финальный результат и
   authoritative `VehicleState`.
5. **Гость** ловит намерения из своих локальных действий на «осязаемом» прокси
   (выстрел по прокси; посадка/выход/угон; изменение дверей прокси) и зеркалит
   визуал только после `Applied`/`VehicleState`; локального авторитета нет.
6. **v20 — жёсткий bump:** v19-клиенты отклоняются по версии; deploy только парой
   ASI+server; в промежуточных коммитах U1–U4 не деплоить (см. §7).
7. **R&D для v20 P1 выполнен** (`docs/NATIVE_RESEARCH_V20_HEALTH.md`, 2026-10-27):
   health readback — **float** (квантование u16/i16, `DamageValid` только finite);
   `IS_CONTROL_*` **PRESENT** → двери/loot/hitch capture через controls, P1.1 не
   fallback-only; `TASK_LEAVE_VEHICLE` — **4 арг**; door status/closed ABSENT
   (ajar не читается); lock enum и wheelIndex mapping не подтверждены → gated/A-B;
   petrol не поле 0x26. Детали — §2.4/§4.3/§10.

---

## 2. Wire protocol v20 P1

### 2.1. Версия и ID

`net/protocol.h:22` → `constexpr uint16_t kProtocolVersion = 20;`
`net/protocol.h:36-78` (enum `Msg`) — добавить после `0x22 TrainSnapshotV2`:

```cpp
    // v20 P1: события транспорта. 0x23 — только гость -> сервер -> хосту;
    // 0x24/0x26 — только хост -> сервер -> гостю(ям). 0x25/0x27 — P3, не в P1.
    VehicleInteraction       = 0x23, // guest -> server -> host: интент
    VehicleInteractionResult = 0x24, // host -> server -> guest: результат
    VehicleControlInput      = 0x25, // RESERVED (P3): не реализуется в P1
    VehicleState             = 0x26, // host -> server -> guest: состояние
    VehicleControlState      = 0x27, // RESERVED (P3): не реализуется в P1
```

Все новые payload'ы — **fixed-size, little-endian, без float** (только целые),
`static_assert(sizeof(...))` в `net/protocol.h`; раскладка без implicit padding
(все поля выровнены явно).

### 2.2. `VehicleInteraction` 0x23 (24 B)

```cpp
// net/protocol.h, после TrainSnapshotV2Payload (после :659)
// NB: это уточнение enum из docs/INTERACTIVITY.md §4.1 (там не было отдельного
// DoorBreak и нумерация 1..11); здесь — канонические значения v20 P1 (1..12).
enum class VehicleAction : uint8_t {
    None       = 0,
    Damage     = 1,   // door = part*, param = amount 1..1000
    DoorOpen   = 2,   // door = doorId 0..7
    DoorClose  = 3,   // door = doorId 0..7
    DoorLock   = 4,   // door = doorId 0..7 или 255 (все двери)
    DoorUnlock = 5,   // door = doorId 0..7 или 255 (все двери)
    DoorBreak  = 6,   // door = doorId 0..7 (отдельный интент; в capture v1 —
                      //     следствие выстрела, см. §4.5)
    Enter      = 7,   // seat = -1 driver / 0..8 passenger / 127 = first free
    Exit       = 8,   // seat = 127
    Hijack     = 9,   // seat = -1; targetId = actorId водителя (или 0)
    Loot       = 10,  // targetId = netId контейнера (0 = груз vehicle), P1:
                      //     host-side визуал/лог, без передачи инвентаря
    Hitch      = 11,  // door = harness 0..5; targetId = netId лошади (0 = any)
    Unhitch    = 12,  // door = harness 0..5
};

// Части vehicle для Damage (поле door = part):
constexpr uint8_t kVehiclePartDoorBase   = 0;   // 0..7  — doorId (eDoorId 0..7)
constexpr uint8_t kVehiclePartWheelFL    = 8;   // wheels: 8..11 (FL, FR, RL, RR)
constexpr uint8_t kVehiclePartWheelFR    = 9;
constexpr uint8_t kVehiclePartWheelRL    = 10;
constexpr uint8_t kVehiclePartWheelRR    = 11;
constexpr uint8_t kVehiclePartWindowBase = 12;  // 12..15 — P1 RESERVED/reject
constexpr uint8_t kVehiclePartEngine     = 254; // отдельный health-натив
constexpr uint8_t kVehiclePartBody       = 255; // SET_VEHICLE_DAMAGE/body health

// Флаги интента (uint8_t).
constexpr uint8_t kInteractionTargetActor   = 1u << 0; // targetId = actorId
constexpr uint8_t kInteractionTargetEntity  = 1u << 1; // targetId = entity netId
constexpr uint8_t kInteractionUrgent        = 1u << 2; // короткий cooldown
constexpr uint8_t kInteractionLootContainer = 1u << 3;
constexpr uint8_t kInteractionGuestHorse    = 1u << 4; // hitch: лошадь гостя
constexpr uint8_t kInteractionForce         = 1u << 5; // host может игнорировать
constexpr uint8_t kInteractionKnownFlags =
    kInteractionTargetActor | kInteractionTargetEntity |
    kInteractionUrgent | kInteractionLootContainer |
    kInteractionGuestHorse | kInteractionForce;

// Значения seat/door.
constexpr int8_t  kVehicleSeatNone   = 127;
constexpr int8_t  kVehicleSeatDriver = -1;
constexpr uint8_t kVehicleDoorAll    = 255;
constexpr uint8_t kVehicleDoorNone   = 255;

struct VehicleInteractionPayload {
    uint32_t epoch = 0;          // 0  host epoch (actor_sync::HostEpoch/GuestEpoch)
    uint32_t vehicleNetId = 0;   // 4  u32 netId из WorldSnapshot (как 0x21)
    uint32_t targetId = 0;       // 8  actorId/netId по flags; 0 = нет
    uint16_t seq = 0;            // 12 idempotency key; 0 невалиден
    uint8_t  action = 0;         // 14 VehicleAction
    int8_t   seat = kVehicleSeatNone; // 15 -1/0..8/127
    uint8_t  door = 255;         // 16 doorId/part/harness по action
    uint8_t  flags = 0;          // 17 kInteractionFlag*
    uint16_t param = 0;          // 18 Damage amount 1..1000; иначе 0..255
    uint32_t weaponHash = 0;     // 20 контекст Damage (0 = неизвестно)
};
static_assert(sizeof(VehicleInteractionPayload) == 24, "...");
static_assert(offsetof(VehicleInteractionPayload, action) == 14, "...");
static_assert(offsetof(VehicleInteractionPayload, weaponHash) == 20, "...");
```

Семантика по action (кратко; host-детали §4):

| action | обязательные поля | проверки хоста (сверх общих) |
|---|---|---|
| Damage | `door`=part, `param`=1..1000 | part валиден для P1 (0..11, 254,255); не mission; damage budget; wheel-part damage может НЕ менять `_IS_VEHICLE_WHEEL_DESTROYED` (R&D §2.4) |
| DoorOpen/Close | `door`=0..7 | vehicle существует; door не broken |
| DoorLock/Unlock | `door`=0..7 или 255 | lock enum **не подтверждён** (R&D §4.2): default `NotAllowed`/gated, `Locked` — только из host cache; вкл. после A/B |
| DoorBreak | `door`=0..7 | one-way (deleteDoor A/B, restore нет); только при A/B-константе `kVehicleInteractionDoorBreak=true`; в P1 default false (ломание — по порогу damage) |
| Enter/Exit | `seat`=-1/0..8/127 (Enter), 127 (Exit) | seat free, гость рядом, гость жив |
| Hijack | `seat`=-1/127, `targetId`=actorId (optional) | NPC driver существует/не mission; seat -1; host vehicle policy |
| Loot | `targetId`=netId(optional), flags | P1: host-side лог/loot-таблица; без инвентаря |
| Hitch/Unhitch | `door`=harness 0..5, `targetId`=netId(optional) | draft/wagon, harness свободен/занят |

### 2.3. `VehicleInteractionResult` 0x24 (16 B)

```cpp
enum class VehicleResult : uint8_t {
    Applied = 0,       // натив применён, состояние прочитано
    NotAllowed = 1,    // policy/param reject; reason — detail bits 8..15
    StaleEpoch = 2,
    NotFound = 3,      // netId не в host handle map / vehicle исчез
    OutOfRange = 4,
    SeatOccupied = 5,
    Locked = 6,
    RateLimited = 7,   // cooldown/budget/queue
    Timeout = 8,       // не применено за отведённый тик-бюджет
    Correction = 9,    // state поправлен; guest обязан reconcile
};

// detail bits 8..15 для result=NotAllowed (u8 reason = (detail >> 8) & 0xFF):
constexpr uint8_t kResultReasonConfigOff        = 1; // vehicle_interaction=0
constexpr uint8_t kResultReasonMissionProtected = 2; // mission vehicle (default)
constexpr uint8_t kResultReasonHostVehicle      = 3; // host player's current vehicle
constexpr uint8_t kResultReasonBadParam         = 4; // seat/door/part/flags/param
constexpr uint8_t kResultReasonDeadGuest        = 5; // guest ghost мёртв/нет
constexpr uint8_t kResultReasonBadRole          = 6; // не та роль / self-target
constexpr uint8_t kResultReasonPolicy           = 7; // прочая политика (P1 default)

struct VehicleInteractionResultPayload {
    uint32_t epoch = 0;        // 0
    uint32_t vehicleNetId = 0; // 4
    uint16_t seq = 0;          // 8  echo запроса
    uint8_t  action = 0;       // 10 echo
    uint8_t  result = 0;       // 11 VehicleResult
    uint32_t detail = 0;       // 12 action-specific detail (см. таблицу)
};
static_assert(sizeof(VehicleInteractionResultPayload) == 16, "...");
```

`detail` (LE, u32):

| action | bits | смысл |
|---|---|---|
| **NotAllowed (любой action)** | 8..15 reason (`kResultReason*`); 0..7 — action-specific (0) | причина policy-reject: mission/host-vehicle/config/bad-param/dead-guest/role |
| Damage | 0..7 part; 8..9 resultFlags (0 applied,1 mission-blocked native,2 health-read-failed); 16..31 appliedDamage | какой урон реально применён |
| DoorOpen/Close/Break | 0..7 doorId; 8..9 doorState (0 closed/ajar,1 fully open,2 broken,3 unknown) | readback (ajar не читается) |
| DoorLock/Unlock | 0..7 lockStatus raw (enum ABSENT, diagnostic only), 255 unknown | readback; `Locked` — host cache/global flag, не per-door |
| Enter/Exit/Hijack | 0..7 guestSeat (u8: 255=-1 driver, 0..8, 127 none); 8..15 hostSeat | назначенное место |
| Loot | 0..7 lootKind (0 none,1 coach,2 container), 8..15 lootFlags | P1 лог |
| Hitch/Unhitch | 0..7 harness; 8..15 draftCount/resultFlags | readback |

Результат — финальный для P1 (`Applied`/`NotAllowed`/…). Дубликат `(epoch, seq)`
host отвечает из кеша ≤2 с **без повторного натива**.

### 2.4. `VehicleState` 0x26 (24 B)

```cpp
// doorBits: 2 бита на дверь, doorId 0..7 (0..7 → биты 0-1,2-3,...):
//   0 = closed (включая ajar: ajar НЕ читается), 1 = fully open,
//   2 = broken, 3 = unknown (readback failure; НЕ locked).
//   Per-door lock readback ABSENT → locked только глобальный
//   kVehicleStateFlagLocked (из host cache; A/B enum).
// wheelsBits: 2 бита на колесо 0..3:
//   0 = intact, 1 = RESERVED/не читается (damaged/burst getter ABSENT),
//   2 = destroyed/detached (_IS_VEHICLE_WHEEL_DESTROYED), 3 = unknown
//   (mapping/readback mismatch; wheelIndex mapping — A/B).
constexpr uint8_t kVehicleStateFlagDoorValid      = 1u << 0;
constexpr uint8_t kVehicleStateFlagDamageValid    = 1u << 1;
constexpr uint8_t kVehicleStateFlagLocked         = 1u << 2;
constexpr uint8_t kVehicleStateFlagMissionProtected= 1u << 3;
constexpr uint8_t kVehicleStateFlagHostSeatValid  = 1u << 4;
constexpr uint8_t kVehicleStateFlagGuestSeatValid = 1u << 5;
constexpr uint8_t kVehicleStateFlagHijacked       = 1u << 6;
constexpr uint8_t kVehicleStateFlagDead           = 1u << 7;
constexpr uint8_t kVehicleStateKnownFlags =
    kVehicleStateFlagDoorValid | kVehicleStateFlagDamageValid |
    kVehicleStateFlagLocked | kVehicleStateFlagMissionProtected |
    kVehicleStateFlagHostSeatValid | kVehicleStateFlagGuestSeatValid |
    kVehicleStateFlagHijacked | kVehicleStateFlagDead;

// Readback health — float (R&D §2.1); на wire — квантованные целые:
//   body   = round(clamp(GET_VEHICLE_BODY_HEALTH(veh), 0, 1000))    -> u16
//   engine = round(clamp(GET_VEHICLE_ENGINE_HEALTH(veh),-4000,1000))-> i16
// Unknown — sentinels ниже; DamageValid выставлять ТОЛЬКО если vehicle
// valid (DoesEntityExist + IS_ENTITY_A_VEHICLE) и оба float finite.
// NB: GET_VEHICLE_ENGINE_HEALTH на invalid vehicle возвращает 1000 (валидный max).
constexpr uint16_t kVehicleBodyHealthUnknown   = 0xFFFF; // valid: 0..1000
constexpr int16_t  kVehicleEngineHealthUnknown = -32768; // valid: -4000..1000

struct VehicleStatePayload {
    uint32_t epoch = 0;         // 0
    uint32_t vehicleNetId = 0;  // 4
    uint16_t bodyHealth = kVehicleBodyHealthUnknown; // 8  quantized float 0..1000
    int16_t  engineHealth = kVehicleEngineHealthUnknown; // 10 quantized float -4000..1000
    uint16_t doorBits = 0;      // 12
    uint16_t seq = 0;           // 14 монотонный per vehicle
    uint8_t  flags = 0;         // 16 kVehicleStateFlag*
    uint8_t  wheelsBits = 0;    // 17
    int8_t   hostSeat = kVehicleSeatNone;  // 18 host player seat (host view)
    int8_t   guestSeat = kVehicleSeatNone; // 19 guest player seat (host view)
    uint32_t reserved = 0;      // 20 зарезервировано (в P3 здесь leaseId)
};
static_assert(sizeof(VehicleStatePayload) == 24, "...");
```

Readback-правила (host, U4; R&D §6):
- сначала `DoesEntityExist(veh) && IS_ENTITY_A_VEHICLE(veh)` (gate; иначе engine=1000
  можно принять за валидный max);
- `doorBits`: `broken ? 2 : (IS_VEHICLE_DOOR_FULLY_OPEN ? 1 : 0)` для doorId 0..7;
  `3` — только при невозможности readback; bonnet/boot (8..11) в bits не кодируются;
  после `SET_VEHICLE_DOOR_OPEN(openInstantly=false)` readback может быть false 1–2
  тика; для mirror использовать `openInstantly=true`;
- `wheelsBits`: `destroyed ? 2 : 0`; `1` (damaged/burst) из readback НЕ получить
  (getter ABSENT) — только host cache; если `_ARE_ANY_VEHICLE_WHEELS_DESTROYED`
  true, а bits==0 → `0xFF` (unknown); wheelIndex mapping A/B;
- **petrol health не поле 0x26**: `GET_VEHICLE_PETROL_TANK_HEALTH` — только
  диагностика/лог; payload 24 B не расширять;
- `kVehicleStateFlagLocked` — из host cache (lock enum не подтверждён) и/или
  raw `GET_VEHICLE_DOOR_LOCK_STATUS != 0`, пока A/B не подтвердит enum.

Правила отправки 0x26 (хост):
- change-driven: изменение health/дверей/локов/seat → отправить на ближайшем тике;
- heartbeat **1/s** только для «отслеживаемых» vehicle (≤ `kMaxVehicleStateTracked=8`):
  активный guestSeat, недавний интент (<10 с), hostSeat != 127, или lease (P3);
- суммарно ≤ `kMaxVehicleStateRate=10/с` (серверный cap), иначе ставим в dirty-set и
  отправляем позже; при переполнении — round-robin, потеря не критична (heartbeat починит);
- `seq` инкрементируется per vehicle; guest отбрасывает state с чужим epoch или
  `seq` старше последнего для этого vehicle.

Почему не 0x21: 0x21 семантически «кто в каких seats (actorId)», host→guest,
layout 8+count*8 ≤72 B, валидаторы привязаны к actorId/seat/carIndex/harness.
Vehicle health/doors/wheels/player-seat — другой объект и другой ритм; расширение 0x21
дало бы скрытую смену семантики v19-сообщения без bump. 0x26 независим и
forward-compatible с P2 (hostSeat/guestSeat) и P3 (reserved → leaseId).

### 2.5. Кодирование/порядок/idempotency

- Byte order: LE; structs memcpy'ятся как есть (как в v19); `static_assert` размера
  и offset'ов; никаких float в 0x23/0x24/0x26.
- `seq` — u16, монотонный per client/session, wrap: 0 пропускается (после 65535 → 1).
  Host хранит последний ответ на `(epoch, vehicleNetId, seq)` ≤2 с → дубль
  переотправляет тот же `VehicleInteractionResultPayload`, натива не вызывает.
- Гость может повторить тот же seq (retry ≤2, интервал 250 мс) только если ответа
  нет; повтор с новым seq — новое намерение (host cooldown/budget решает).
- Порядок: TCP+реле сервер сохраняет порядок; host применяет в порядке получения
  внутри тика; результат эхо seq; `VehicleState.seq` — только диагностика/recovery.
- Reject никогда не имеет world side effect; host не «частично» применяет группу:
  либо натив-группа применена, либо ничего (кроме mission-block: натив вызван, но
  readback не изменился → `Applied`/`Correction` по факту, detail flags).

### 2.6. Лимиты (wire-константы и правила)

| Константа | Значение | Где |
|---|---:|---|
| `kMaxVehicleInteractionRate` | 8/с на клиента | server.cpp (+pinned test) |
| `kMaxVehicleInteractionResultRate` | 20/с | server.cpp |
| `kMaxVehicleStateRate` | 10/с | server.cpp |
| `kMaxHostVehicleIntentsPerTick` | 2 | world_sync.cpp |
| `kMaxHostVehicleIntentQueue` | 8 (overflow → `RateLimited`) | world_sync.cpp |
| `kVehicleInteractionResultCacheMs` | 2000 | world_sync.cpp |
| `kVehicleInteractionDamageBudgetPerSec` | 200 HP/vehicle/с | world_sync.cpp |
| `kVehicleInteractionCooldownMs` | 250 (Urgent=100) | world_sync.cpp |
| range damage / door+enter / loot+hitch | 12 / 5 / 6 м | world_sync.cpp |
| `kVehicleInteractionProxyRadius` | 20 м (локальная осязаемость) | world_sync.cpp |
| `kMaxVehicleStateTracked` | 8 | world_sync.cpp |

В `net/protocol.h` — только wire-диапазоны (`kVehiclePart*`, seats, flags, размеры);
rate/дистанции — серверные/клиентские константы рядом с использованием.

### 2.7. Версионирование и deploy

- `kProtocolVersion 19 → 20`: сервер отклоняет v19 JOIN (`VersionMismatch`),
  смешанная пара невозможна; fallback'а нет (как v18↔v19).
- Deploy только парой ASI+server из одного снапшота `dist\npc-e20`; оба ПК —
  одинаковые хеши. Откат — `dist\npc-e19` (v19) полной парой.
- Промежуточные unit-коммиты (U1–U4) **не деплоить**: после U2 версия уже v20,
  но guest capture/host apply ещё не полны.

---

## 3. Server relay v20 (без знания мира)

### 3.1. Направления

| ID | направление | роль отправителя | маршрут |
|---|---|---|---|
| 0x23 | guest → server → host | **только guest** (host-sender: warn+drop) | unicast текущему хосту; нет хоста → drop |
| 0x24 | host → server → guest(s) | **только host** | всем не-хост клиентам (при 2 игроках = гость) |
| 0x26 | host → server → guest(s) | **только host** | всем не-хост клиентам |

Сервер **не знает**: существует ли `vehicleNetId`, где vehicle/игроки, mission ли
это, есть ли seat/driver. Он проверяет только формат, роль, направление, rate и
диапазоны полей. Вся семантика — на хосте (§4.2). Это осознанная граница: сервер не
имеет world view, поэтому spoof `vehicleNetId` не опасен сам по себе — хост отбросит
неизвестный netId/дальность/mission.

### 3.2. Валидация

Общие функции — **inline в `net/protocol.h`** (единый источник для server/client/tests,
crapper/dryer): `IsValidVehicleInteraction`, `IsValidVehicleInteractionResult`,
`IsValidVehicleState`. Точные правила:

`IsValidVehicleInteraction(p)`:
1. `epoch != 0`, `vehicleNetId != 0`, `seq != 0`;
2. `action` ∈ 1..12 (`None` и >12 reject);
3. `flags & ~kInteractionKnownFlags == 0`; `TargetActor` и `TargetEntity` не вместе;
4. seat: `-1`/0..8/127; door: 0..7 или 255 (см. п.5);
5. per-action:
   - Damage: door ∈ {0..11, 254, 255}; `param` 1..1000;
   - DoorOpen/Close/Break: door 0..7; param 0;
   - DoorLock/Unlock: door 0..7 или 255; param 0;
   - Enter: seat -1/0..8/127, door 255, target 0; Exit: seat 127;
   - Hijack: seat -1/127; door 255; (targetId 0 или flags&TargetActor);
   - Loot: flags&TargetEntity → targetId != 0; иначе targetId=0; seat 127;
   - Hitch/Unhitch: door 0..5 (harness); targetId 0 или flags&TargetEntity;
   - window parts (12..15) в P1 reject (`NotAllowed`, reason=bad param);
6. `weaponHash` — любой (контекст), для не-Damage должен быть 0 (reject иначе).

`IsValidVehicleInteractionResult(p)`:
`epoch != 0`, `vehicleNetId != 0`, `seq != 0`, `action` 1..12, `result` 0..9.

`IsValidVehicleState(p)`:
`epoch != 0`, `vehicleNetId != 0`, `flags & ~kVehicleStateKnownFlags == 0`;
`bodyHealth` 0..1000 или 0xFFFF; `engineHealth` -4000..1000 или -32768; seats
-1/0..8/127; `seq` любой. Валидатор проверяет **квантованные int** с провода
(float-диапазоны — забота host-сборки состояния, R&D §2.1); `doorBits`/`wheelsBits`
могут содержать `3` (unknown) — это допустимо.

Сервер дополнительно: `ReadExactPayload` (точный размер 24/16/24), направление/роль,
rate. Невалидный payload — молча drop (как v19); role violation — `LogWarn`
`[!] <name>: VehicleInteraction от хоста проигнорирован` (и аналогично 0x24/0x26
от гостя) + счётчик.

### 3.3. Rate / anti-flood

Добавить в `RateWindow`-поля `OnlinePlayer` (`server/server.cpp:59-74`):
`interactionRate` (8/с), `interactionResultRate` (20/с), `vehicleStateRate` (10/с);
константы рядом `server.cpp:193-202`. Реализация — через существующие
`RelayHostOnlyRate` / новый `RelayGuestToHostRate` (вынести общий helper из
существующих inline-блоков ActorDamage/ActorResync — dryer; см. §7 U2).

- over-rate → молча drop (сервер), host при этом может вернуть `RateLimited`, если
  интент всё же дошёл и превысил host-бюджет;
- host game liveness (`IsGameThreadMessage`, `server.cpp:287-316`):
  **0x24 и 0x26 — game-thread (host activity) = true; 0x23 — false**
  (guest-сообщения не продлевают жизнь хоста; хост их не шлёт);
- host migration: маршрут выбирается по `isHost` на момент отправки; сервер не
  хранит очередей; старые epoch/seq у нового хоста → `StaleEpoch`/drop у клиента.

### 3.4. Нет хоста / нет vehicle / fallback

- нет хоста online: 0x23 drop (debug-счётчик `vehicle-intent-no-host`), гость не
  получает result → локальный retry policy клиента (§4.7) → лог и сдача;
- хост не нашёл netId/vehicle: `NotFound` (не relay-ошибка — это ответ хоста);
- vehicle исчез между интентом и тиком: `NotFound`/`OutOfRange`;
- mission/locked/seat busy: `NotAllowed` (reason=mission) / `Locked` / `SeatOccupied`;
- сервер никогда не эмулирует и не отвечает от себя.

### 3.5. Метрики / traffic bucket

`server.cpp:104-148` — новый `TrafficBucket::Vehicle` (после `Occupants`, до `Other`),
mapping для 0x23/0x24/0x26; `PrintStatus` (`:1038-1048`) добавляет `| vehicle=`;
`docs/PERF.md`/`tools/perf_report.py` — новый счётчик (perf_report обновить, если
парсит occupants/damage-поля; см. §8).

---

## 4. Клиент (ASI): host apply + guest capture

### 4.1. Поток данных

```
guest локальное действие
  → capture (coop.cpp Tick)
  → net/client SendVehicleInteraction (0x23)
  → server (format/role/rate) → host
  → host inbox (network thread, mutex, без нативов)
  → host world_sync::Tick: drain ≤2, validate, apply native, readback
  → net/client SendVehicleInteractionResult (0x24) + SendVehicleState (0x26)
  → guest inbox → guest world_sync::Tick: mirror/rollback, log
```

Хост и гость — один и тот же ASI; роль — `Client::IsHost()`/`coop::s_amHost`.
Cross-модульные вызовы — как в v19: `coop::SendXxx(...)` (coop.cpp → g_netClient),
`world_sync::OnXxx(...)` (coop callback → inbox), `actor_sync::HostEpoch()/GuestEpoch()`.

### 4.2. Host apply pipeline (`world_sync::Tick` host-branch)

Новые точки (`world_sync.cpp`):
- `OnVehicleInteraction(const VehicleInteractionPayload&)` — network thread:
  `std::lock_guard(g_vehicleIntentsMutex); if (queue.size() < kMaxHostVehicleIntentQueue)
  push;` (иначе пометить overflow-результат).
- `ApplyVehicleInteractionIntents(unsigned localPlayerPed, unsigned partnerGhost,
  tickCount, partnerX/Y/Z, partnerValid)` — game thread, вызывается из host-branch
  `world_sync::Tick` (`world_sync.cpp:3509-3523`) до/после `SendHostSnapshot`.
- `SendVehicleStateFor(...)` — по dirty-set.

Обработка одного интента (порядок; при reject — сразу result, без натива):

1. `coopcfg::Get().vehicleInteraction` выключен или `partnerGhost == 0`/не существует →
   `NotAllowed` (reason=config_off / dead_guest) при наличии seq; при выключенном
   конфиге — молча drop, чтобы не спамить.
2. `p.epoch == actor_sync::HostEpoch()` и `p.epoch != 0`; иначе `StaleEpoch`.
3. idempotency: `(epoch, vehicleNetId, seq)` в кеше ≤2 с → отправить закешированный
   result, выйти (натив не повторять).
4. `FindHostVehicleHandleByNetId(p.vehicleNetId, handle)` — расширить
   `HostIdState` (`world_sync.cpp:320-347`) полем `uint8_t kind`, заполнять в
   `EnsureHostNetId` из vehicle/actor путей; для P1 — только `kEntityKindVehicle`.
   Нет handle / `!DoesExist` → `NotFound`.
5. policy:
   - `IsMissionEntity(vehicle)` (`IS_ENTITY_A_MISSION_ENTITY`) && !`vehicleInteractionMission`
     → `NotAllowed` (reason=mission_protected);
   - host's own vehicle: `GET_VEHICLE_PED_IS_USING(localPlayerPed) == vehicle` &&
     !`vehicleInteractionHostVehicle` → `NotAllowed` (reason=host_vehicle) для
     Damage/Hijack/DoorBreak; passenger Enter разрешён (Leads decision D2/D4, §9);
   - гость мёртв/оглушён (партнёрский ghost) → `NotAllowed` (reason=dead_guest);
   - distance: coords(vehicle) vs coords(partnerGhost) по action (12/5/6 м) →
     `OutOfRange`; учитывать высоту (dz ≤ 3 м) и invalid partner → `OutOfRange`.
6. action-specific (см. таблицу §2.2, validate helper и R&D §2/§4):
   - lock/unlock: enum `SET_VEHICLE_DOORS_LOCKED` **не подтверждён** — до A/B
     только `NotAllowed` (reason=policy) или `Locked` из host cache; нативы не
     вызывать до A/B (R&D §4.2);
   - door open/close: readback `IS_VEHICLE_DOOR_FULLY_OPEN`/`_IS_VEHICLE_DOOR_BROKEN`
     (ajar не читается; 0 = closed/ajar — R&D §2.3);
   - wheel/destroyed: readback `_IS_VEHICLE_WHEEL_DESTROYED` (0/2/3; damaged/burst
     getter ABSENT, mapping wheelIndex A/B — R&D §2.4);
   - seat free через `IS_VEHICLE_SEAT_FREE`, driver для Hijack `GET_DRIVER_OF_VEHICLE`,
     harness free/occupied через `_GET_PED_IN_DRAFT_HARNESS` + `GET_DRAFT_ANIMAL_COUNT`;
   - vehicle dead/`IS_VEHICLE_WRECKED` → `NotAllowed` (reason=bad_param)/`Locked`.
7. cooldown per `(vehicle, action)` 250 мс (Urgent 100 мс); damage budget 200 HP/с/
   vehicle; per-tick применили ≥2 → остаток обработать в следующем тике (`Timeout`
   не шлём, если остались в очереди; если очередь переполнена → `RateLimited`).
8. apply **одну** натив-группу (см. §4.3), readback состояния, обновить host-кеш,
   пометить vehicle dirty, отправить result; лог throttled:
   `vehicle: interaction action=%s net=%u seq=%u result=%s guest=%s`.

Состояние host-кеша (per vehicle, `std::unordered_map<uint32_t, HostVehicleRuntime>`):
последний `VehicleStatePayload`, `lastDamageMs`, `damageBudget`, `lastActionMs[action]`,
`dirty`, `lastSentStateMs`. Очистка при `Reset()`/`HostChanged`/vehicle despawn.

### 4.3. Native recipes (target 1491.50; R&D **выполнен**: `docs/NATIVE_RESEARCH_V20_HEALTH.md`)

Hashes/arity/семантика — из `docs/NATIVE_RESEARCH_V20_HEALTH.md` (NativeDecl 1491.50 +
latest nativedb; 2026-10-27). Где статус SIGNATURE-ONLY/UNCERTAIN — только A/B, не
выдумывать значения. Все нативы добавляются в `src/natives.h` (новый блок
`v20 P1: vehicle interaction`, перед закрытием namespace ~`:397-402`) и
оборачиваются в `world_sync.cpp` рядом с существующими helper'ами (`:60-216`).

| действие | натив(ы) | hash (arity) | статус |
|---|---|---|---|
| body damage | `SET_VEHICLE_DAMAGE` | `0x1D7678F81452BB41` (7: veh, x,y,z offset, damage, radius, p6) | CONFIRMED sig (ND:14549); p6 **UNCERTAIN** (v1 false); offset — **локальная модель** (не world); mission/invincible/strong могут блокировать (A/B) |
| body health write | `SET_VEHICLE_BODY_HEALTH` | `0x55CCAAE4F28C67A0` (2) | CONFIRMED sig; range SIGNATURE-ONLY (A/B 0..1000) |
| engine health write | `SET_VEHICLE_ENGINE_HEALTH` | `0x8BDC5B998B4654EF` (2) | CONFIRMED sig; range A/B (-4000..1000; 0 — fire) |
| **body/engine readback** | `GET_VEHICLE_BODY_HEALTH` / `GET_VEHICLE_ENGINE_HEALTH` | `0x42113B857E33C16E` (1) / `0x90DBFFAC43B22081` (1) | **CONFIRMED**: return **float** (body 0..1000, engine -4000..1000; invalid entity → 1000!). Квантование в u16/i16; `DamageValid` только finite + valid entity (§2.4) |
| petrol (не поле 0x26) | `GET_VEHICLE_PETROL_TANK_HEALTH` | `0x1E5A9B356D5098BE` (1) | CONFIRMED float 0..1000; только диагностика/лог, payload не расширять |
| wheel damage | `SET_VEHICLE_DAMAGE` (offset зоны колеса, A/B) + `_BREAK_OFF_VEHICLE_WHEEL` (A/B) | `0xCB2CA620C48BC875` (2) / `0xD4F5EFB55769D272` (2) | destroyed readback CONFIRMED, mapping **UNCERTAIN**; wheel-zone damage может не менять destroyed; wheelBits 0/2/3 |
| door open | `SET_VEHICLE_DOOR_OPEN` | `0x550CE392A4672412` (4: veh, doorId, loose, openInstantly) | CONFIRMED (trains/wagons); `openInstantly=true` для deterministic mirror |
| door shut | `SET_VEHICLE_DOOR_SHUT` | `0x6A3C24B91FD0EA09` (3) | CONFIRMED; `closeInstantly=true` |
| door broken | `SET_VEHICLE_DOOR_BROKEN` | `0x9666CF20A1C6D780` (3: deleteDoor) | CONFIRMED; **one-way**, restore нет; `SET_DOOR_ALLOWED_TO_BE_BROKEN_OFF` перед (A/B) |
| door state readback | `IS_VEHICLE_DOOR_FULLY_OPEN` / `_IS_VEHICLE_DOOR_BROKEN` | `0x7AE191143C7A9107` (2) / `0xE979BB5602AD3402` (2) | CONFIRMED; `GET_VEHICLE_DOOR_STATUS`/`IS_VEHICLE_DOOR_CLOSED` **ABSENT**; ajar не читается (false = closed/ajar) |
| locks | `SET_VEHICLE_DOORS_LOCKED` / `GET_VEHICLE_DOOR_LOCK_STATUS` | `0x96F78A6A075D55D9` (2) / `0xC867FD144F2469D3` (1) | SIGNATURE-ONLY; **enum НЕ подтверждён** → lock actions default gated/`NotAllowed`, `Locked` только из host cache; per-door lock readback ABSENT |
| break locks | `_BREAK_LOCKS_ON_VEHICLE` | `0x9D12796EF4BF9EA9` (1) | CONFIRMED sig/comment; **one-way** (pop/doors open, undo нет) — только coach robbery/hijack A/B |
| seat free | `IS_VEHICLE_SEAT_FREE` | `0xE052C1B1CAA4ECE4` (2) | CONFIRMED (seat enum: -1 driver, 0..8; last=maxPass-1); wagon count A/B |
| driver | `GET_DRIVER_OF_VEHICLE` | `0x2963B5C1637E8A27` (1) | CONFIRMED (уже в natives.h) |
| seat warp | `SET_PED_INTO_VEHICLE` | `0xF75B0D629E1C063D` (3) | CONFIRMED sig; **non-local ped — UNCERTAIN** (host ghost A/B); readback `GET_PED_IN_VEHICLE_SEAT` |
| animated enter/leave | `TASK_ENTER_VEHICLE` / `TASK_LEAVE_VEHICLE` | `0xC20E50AA46D09CA8` (7) / `0xD3DBCE61A490BE02` (**4**: ped, veh, flags, unkPed) | CONFIRMED; flags ECF_JACK_ANYONE(1<<3), ECF_WARP_PED(1<<4), JUST_PULL_PED_OUT(1<<19), DONT_WAIT_STOP(1<<6); **unkPed=0** |
| draft attach/detach | `_ATTACH_DRAFT_VEHICLE_HARNESS_PED` / `_DETACH_DRAFT_VEHICLE_HARNESS_PED` | `0x316CDB5B6E8F4110` (3, ret BOOL) / `0xB36D3EC70963BE60` (2, ret BOOL) | CONFIRMED; harness enum **0 LR, 1 RR, 2 LM, 3 RM, 4 LF, 5 RF**; verify `GET_DRAFT_ANIMAL_COUNT` expected/actual |
| draft free slots | `_GET_PED_IN_DRAFT_HARNESS` / `GET_DRAFT_ANIMAL_COUNT` | `0xA8BA0BAE0173457B` (2) / `0xA19447D83294E29F` (3, int* expected/actual) | CONFIRMED (уже в natives.h); ghost-horse attach — A/B |
| loot | `TASK_LOOT_ENTITY` / `_SET_FORCE_COACH_ROBBERY_LOOT` | `0x48FAE038401A2888` (2) / `0xF489F94BFEE12BB0` (2) | CONFIRMED sig / SIGNATURE-ONLY (P1 host-log only; generic vehicle inventory ABSENT) |
| controls (guest capture) | `IS_CONTROL_PRESSED` / `IS_CONTROL_JUST_PRESSED` / `GET_CONTROL_NORMAL` / `IS_CONTROL_ENABLED` / `DISABLE_CONTROL_ACTION` / `ENABLE_CONTROL_ACTION` | `0xF3A21BCD95725A4A` / `0x580417101DDB492F` / `0xEC3C9B8D5327B563` / `0x1CEA6BFDF248E5D9` / `0xFE99B66D079CF6BC` / `0x351220255D64C155` (все 2–3) | **CONFIRMED PRESENT** (R&D §3); control type 0 для vehicle, 3 — OnMount; `_IS_CONTROL_ACTION_VALID(action, control)` — порядок args reversed |
| proxy tangibility | `SET_ENTITY_COLLISION` (есть) / `SET_VEHICLE_IS_CONSIDERED_BY_PLAYER` / `SET_VEHICLE_NOT_STEALABLE_AMBIENTLY` | `0xF66F820909453B8C` (3) / `0x54800D386C5825E5` (2) / `0x09C970AE59ABF6B2` (2) | CONFIRMED sig; keepPhysics semantics UNCERTAIN (A/B); considered=false убирает enter prompt |
| proxy visual mirror | `SET_VEHICLE_CAN_BE_VISIBLY_DAMAGED` / `SET_VEHICLE_CAN_BREAK` / `SET_VEHICLE_STRONG` / `SET_ENTITY_INVINCIBLE` / `SET_ENTITY_CAN_BE_DAMAGED` | `0x4BF8131AE811541C` (2) / `0xC5ED9D59B4646611` (2) / `0xAB315515C9F8803D` (2) / `0xA5C38736C426FCB8` (2) / `0x0D06D522B90E861F` (2) | CONFIRMED sig/comment; для mirror временно снять invincible/strong и восстановить; mission-флаг — A/B |
| guest seat control gate | `SET_VEHICLE_UNDRIVEABLE` (+ controls suppress) | `0x6E884BAB713A2A94` (2) | SIGNATURE-ONLY/dangerous: только guest proxy; restore на exit/reset; плюс `DISABLE_CONTROL_ACTION` на vehicle-actions (§4.6) |

**Damage part → native (host):**
- подготовка host real vehicle (R&D §2.5, A/B): `SET_ENTITY_INVINCIBLE(veh,false)`,
  `SET_ENTITY_CAN_BE_DAMAGED(veh,true)`, `SET_VEHICLE_CAN_BE_VISIBLY_DAMAGED(veh,true)`,
  `SET_VEHICLE_CAN_BREAK(veh,true)`, `SET_VEHICLE_STRONG(veh,false)`; mission-флаг
  сам по себе persistent ≠ invincible, но mission-скрипт может блокировать (A/B);
- door part (0..7): `SET_VEHICLE_DAMAGE(veh, offsetDoor, amount, radius, false)` и при
  превышении порога прочности → `SET_VEHICLE_DOOR_BROKEN(veh, doorId, false)`
  (one-way; `SET_DOOR_ALLOWED_TO_BE_BROKEN_OFF` перед — A/B);
- wheel part (8..11): `SET_VEHICLE_DAMAGE` в зону колеса **может не менять**
  `_IS_VEHICLE_WHEEL_DESTROYED` (R&D §2.4); «detached» только через
  `_BREAK_OFF_VEHICLE_WHEEL` (A/B mapping wheelIndex); readback destroyed 0/2/3;
- engine (254): `SET_VEHICLE_ENGINE_HEALTH` (clamp -4000..1000; 0 — fire),
  body (255): `SET_VEHICLE_DAMAGE` + fallback `SET_VEHICLE_BODY_HEALTH` (0..1000);
- offsets — **локальная модель** (R&D §2.2), не world; body `(0,0,0.5)` + radius 0.6
  стартовый A/B, engine `(0,0,1.0)`; door/wheel offsets — A/B по модели wagon;
  p6=false в v1 (p6 — UNCERTAIN);
- `Applied` выдавать только по фактическому readback (следующий тик; body/engine
  float, дверь fullyOpen/broken), иначе `Correction`/fallback health write.

**Enter/Exit/Hijack (host):**
- Enter: `IS_VEHICLE_SEAT_FREE(veh, seat)`; при seat=127 — первый свободный
  (driver? policy: passenger by default; driver -1 разрешён только если
  `vehicle_interaction_host_vehicle` или vehicle parked non-player);
  `SET_PED_INTO_VEHICLE(partnerGhost, veh, seat)` — **non-local ped UNCERTAIN**
  (host ghost может не работать; A/B, R&D §2.6); readback
  `GET_PED_IN_VEHICLE_SEAT(veh, seat) == partnerGhost`;
- Exit: `TASK_LEAVE_VEHICLE(partnerGhost, veh, 0, 0)` — **4 аргумента**
  (Ped, Vehicle, flags, unkPed; R&D §2.7); если не выходит за 2 с →
  warp рядом (`SET_ENTITY_COORDS` 8 args, уже исправлен) + log `Correction`;
- Hijack: `GET_DRIVER_OF_VEHICLE(veh)` → NPC driver:
  `TASK_LEAVE_VEHICLE(npcDriver, veh, 0, 0)` (или `TASK_ENTER_VEHICLE(...ECF_JACK_ANYONE|ECF_WARP_PED)`
  A/B), подождать 1 тик, затем `SET_PED_INTO_VEHICLE(partnerGhost, veh, -1)`;
  mission/player-driven → reject. `GET_VEHICLE_PED_IS_ENTERING` после warp может
  вернуть 0 — не использовать как единственный readback (R&D §2.6).
- P1 не двигает vehicle; guest local controls suppressed (§4.6).

### 4.4. Guest capture points (`src/coop.cpp`)

Новая функция `UpdateLocalVehicleInteraction(unsigned int ped)` в `coop.cpp`,
вызывается в `Tick()` рядом с shot/presence (`coop.cpp:6954-6957`, после
`UpdateLocalShotEvents`). Внутри — 3 детектора; всё только при `!s_amHost`,
`vehicle_interaction=1`, `actor_sync::GuestEpoch() != 0`, `world_sync` enabled.

**C1. Damage (реализуется в P1) — из существующего shot-пайплайна:**
- В конце `UpdateLocalShotEvents` (`coop.cpp:5006-5137`, после
  `TrySendNpcDamage(ped, weapon, ...)` на `:5137`) вызвать
  `TrySendVehicleDamage(ped, weapon, haveImpact ? &impact : nullptr)`.
- `TrySendVehicleDamage`: `GET_ENTITY_PLAYER_IS_FREE_AIMING_AT` (уже есть) →
  `world_sync::GetVehicleProxyNetId(handle, netId)`; distance ≤12 м; part:
  - impact/aim точка → локальные координаты vehicle (`GET_ENTITY_COORDS`) →
    классификация: |dz|>0.9 → body/engine (engine если у капота), lateral offset →
    door side (doorId по ближайшей двери из таблицы offset), низ/край → wheelIndex;
  - fallback без impact: `kVehiclePartBody`, param=weapon damage class (как
    `NpcDamageForWeapon`), `weaponHash`.
- rate: ≤4/с, per-vehicle 150 мс, seq++.
- Локальный прокси при этом остаётся invincible → локального визуального урона нет;
  mirror только после `Applied`/state.

**C2. Enter/Exit/Hijack (реализуется в P1) — из локальной посадки в осязаемый proxy:**
- `vehicle_interaction` + прокси в радиусе; player can enter local proxy (§4.6).
- `GET_VEHICLE_PED_IS_ENTERING(ped)` (native `0xF92691AED837A5FC`, CONFIRMED) или
  `GET_VEHICLE_PED_IS_IN(ped, 0)` (уже в natives.h) → handle → netId.
- переход «нет → proxy»: определить локальный seat (перебор
  `GET_PED_IN_VEHICLE_SEAT(proxy, s)` для s=-1,0..8, сравнение с `ped`; ≤10 нативов
  один раз на вход) → action Enter (seat) ; если seat=-1 и в 0x21-состоянии на этом
  seat есть NPC driver (`g_occupantVehicles[netId]`, `kOccupantFlagDriver`) →
  action Hijack (targetId=driverActorId).
- переход «proxy → нет/другой»: action Exit.
- Если результат `NotAllowed`/`SeatOccupied` → guest выходит из локального proxy
  (`TASK_LEAVE_VEHICLE`) и лог; если `Applied` → остаётся (controls disabled).
- rate: 1 интент за переход, не чаще 500 мс.

**C3. Двери (реализуется в P1 через controls; R&D §3 PRESENT) — control capture:**
- `IS_CONTROL_*` подтверждены (R&D §3): `IS_CONTROL_PRESSED 0xF3A21BCD95725A4A`,
  `IS_CONTROL_JUST_PRESSED 0x580417101DDB492F`, `GET_CONTROL_NORMAL 0xEC3C9B8D5327B563`,
  `IS_CONTROL_ENABLED 0x1CEA6BFDF248E5D9`, `DISABLE_CONTROL_ACTION 0xFE99B66D079CF6BC`,
  `ENABLE_CONTROL_ACTION 0x351220255D64C155`; control type 0 (vehicle), 3 — OnMount;
  `_IS_CONTROL_ACTION_VALID(action,control)` — args reversed.
- Capture (guest near interactive proxy ≤5 м): `IS_CONTROL_JUST_PRESSED(0, INPUT_ATTACK)`
  / `INPUT_ENTER` (0xCEFD9220) / `INPUT_VEH_EXIT` (0xFEFAB9B4) + readback состояния
  локального proxy (`IS_VEHICLE_DOOR_FULLY_OPEN`/`_IS_VEHICLE_DOOR_BROKEN`) →
  DoorOpen/DoorClose/DoorBreak/DoorLock/DoorUnlock intent. Точный mapping
  «control → action» фиксируется in-game A/B (I2); `kVehicleInteractionCaptureDoors`
  (constexpr, default **true** в U4; gate для rollback, если A/B покажет конфликт
  pitch/attack).
- `GET_VEHICLE_DOOR_STATUS`/`IS_VEHICLE_DOOR_CLOSED` **ABSENT** — только
  fullyOpen/broken (R&D §2.3); ajar не читается, 0 = closed/ajar.
- DoorLock/Unlock capture **не включать** до A/B enum (R&D §4.2): wire-ready/фейк,
  в игре default `NotAllowed`/gated.
- `openInstantly=true`/`closeInstantly=true` для deterministic mirror; DoorBreak
  one-way (deleteDoor A/B).

**C4. Loot / Hitch / Unhitch — capture через controls (R&D §3 PRESENT), не blocker:**
- wire/actions валидируются и тестируются fake-клиентом; control-prompt R&D **выполнен**
  (`IS_CONTROL_*` PRESENT) — P1.1 больше не «fallback-only»: capture можно строить
  на `IS_CONTROL_JUST_PRESSED(0, INPUT_ENTER/INPUT_ATTACK/INPUT_VEH_*)` + readback
  (`GET_DRAFT_ANIMAL_COUNT`, `_GET_PED_IN_DRAFT_HARNESS`, loot-target нативы).
- Полный loot/hitch остаётся P1.1 по объёму/семантике (инвентарь не передаём;
  coach-loot hashes SIGNATURE-ONLY, R&D §4.4), а не из-за отсутствия controls.
- В P1 включать только после A/B соответствующего action; отдельные INI-ключи не нужны.

### 4.5. Guest proxy: «осязаемый» локальный proxy

`world_sync.cpp` (гость):
- `CreateProxy` (`:2356-2414`) для vehicle остаётся mission/invincible/no-collision
  (host authority сохраняется);
- новая `UpdateInteractiveProxy(Proxy&, localPlayerPed, tick)` из guest-branch
  `world_sync::Tick` (после `InterpolateVehicleProxies`, `:3531`):
  - near = dist(proxy, localPed) ≤ `kVehicleInteractionProxyRadius` (20 м) &&
    !vehSnapWarp && proxy не trainAttached;
  - near → впервые: `SetNoCollision(veh, false)` (включить collision; существующий
    wrapper: `SetNoCollision(veh,false)` → toggle=1), `SET_VEHICLE_IS_CONSIDERED_BY_PLAYER(veh,1)`,
    `SET_VEHICLE_NOT_STEALABLE_AMBIENTLY(veh,1)` (опц.); пометить `interactive=true`;
  - далеко/варп → вернуть collision off (`SetNoCollision(veh,true)`),
    `SET_VEHICLE_IS_CONSIDERED_BY_PLAYER(veh,0)`, `interactive=false`;
  - на жёстком warp/snap: collision off на этот тик, чтобы не толкать player;
  - при `Reset()`/despawn — снять флаги до `DeleteProxy` (как occupant cleanup).
- Вход в proxy: если mission-флаг блокирует посадку/двери (A/B), допустим временный
  `SET_ENTITY_AS_MISSION_ENTITY(veh,false,false)` для interactive-прокси с обратным
  маркированием при уходе; это **A/B-решение** (риск cleanup/despawn, §6 R5),
  по умолчанию пробуем без снятия mission.

Mirror (guest, только после `Applied`/`VehicleState`; R&D §2.5):
- body damage: временно `SET_ENTITY_INVINCIBLE(proxy,false)` +
  `SET_ENTITY_CAN_BE_DAMAGED(proxy,true)` + `SET_VEHICLE_CAN_BE_VISIBLY_DAMAGED(proxy,1)`
  + `SET_VEHICLE_CAN_BREAK(proxy,1)` + `SET_VEHICLE_STRONG(proxy,0)`, применить
  `SET_VEHICLE_DAMAGE(proxy, offsetModel, amount, radius, false)`, затем **вернуть**
  invincible/strong/can-break; при неудаче (mission/invincible блок) — только
  лог/wheel/door визуал; body не обязателен для P1 PASS;
- doors: `SET_VEHICLE_DOOR_OPEN(proxy, doorId, false, true)` /
  `SET_VEHICLE_DOOR_SHUT(proxy, doorId, true)` / `SET_VEHICLE_DOOR_BROKEN(…)` по
  `doorBits` (1 = fully open, 2 = broken; ajar/closed не различаем);
- wheels: `_BREAK_OFF_VEHICLE_WHEEL(proxy, wheel)` только при wheelBits=2 (A/B;
  fallback log); wheelBits=1 (damaged/burst) — только лог, readback-ом не приходит;
- seat: guest local player уже в proxy; host state `guestSeat` — источник истины:
  если не совпал — re-seat локально (`SET_PED_INTO_VEHICLE(localPed, proxy, seat)`)
  или exit + лог `Correction`;
- collision: остаётся включённой пока near; host position остаётся авторитетной
  (интерполяция).

### 4.6. Guest expectations: успех/неудача

- **Успех (Applied):** host real vehicle реально изменён (урон/дверь/seat NPC);
  guest видит mirror на proxy (best-effort) + authoritative state heartbeat;
  guest лог: `vehicle: interaction action=damage result=applied net=… seq=…`.
- **Отказ:** никакого host-эффекта; guest не меняет локальный авторитетный вид
  (collision/door rollback к последнему state), лог с кодом; при seat-reject —
  guest выходит из proxy.
- **Дубль/retry:** тот же seq → тот же result без повторного натива; guest
  дедуплицирует по seq.
- **P1 UI:** при посадке в driver seat guest видит лог/строку
  `vehicle: guest seat driver (driving disabled until P3)`;
  controls suppressed каждый тик через `DISABLE_CONTROL_ACTION(0, action, true)` для
  `INPUT_VEH_ACCELERATE 0x5B9FD4E2`, `INPUT_VEH_BRAKE 0x6E1F639B`,
  `INPUT_VEH_MOVE_LR 0xF1E2852C`, `INPUT_VEH_MOVE_UD 0x8A81C00C`,
  `INPUT_VEH_HANDBRAKE 0x65D24C98` (R&D §3.2); restore `ENABLE_CONTROL_ACTION` на
  Exit/Reset/role change; `DISABLE_ALL_CONTROL_ACTIONS` — **не** использовать
  (камера/UI). `SET_VEHICLE_UNDRIVEABLE(proxy,1)` — дополнительный/альтернативный
  gate (SIGNATURE-ONLY/dangerous: только proxy, restore на exit/reset; R&D §2.9).
  `IS_DISABLED_CONTROL_PRESSED` читает нажатия даже после disable — для prompt/hotkey.
- **Потеря сообщений:** 0x26 heartbeat 1/s чинит door/health/seat; 0x24 повтор
  по тому же seq; 0x10/интерполяция — position; epoch mismatch → drop + log.

### 4.7. Конфиг (v20 P1)

`src/config.h` — new bool fields (после `npcVehicleWheels`, `:92-93`):

```cpp
// v20 P1: guest→host interactions (событийные интенты, без вождения).
bool vehicleInteraction = true;          // vehicle_interaction
bool vehicleInteractionMission = false;  // vehicle_interaction_mission
bool vehicleInteractionHostVehicle = false; // vehicle_interaction_host_vehicle
bool vehicleInteractionDebug = false;    // vehicle_interaction_debug
```

`src/config.cpp`:
- `WriteDefaultIni` — после `npc_vehicle_wheels` (`:121-123`) добавить блок
  `# v20 P1: guest→host vehicle interactions ...` + **4 ключа** (0/1) — INI снапшота
  станет **32 приёмочных ключа / 42 строки** (было 28/38);
- `ApplyBoolKey` (`:204-232`) — 4 новые записи; иначе key unknown → warn/ignore;
- A/B-гейты capture (`kVehicleInteractionCaptureDoors`,
  `kVehicleInteractionDoorBreak`) — **constexpr в world_sync.cpp**, не INI:
  временно, до in-game A/B; при необходимости вынести в INI в P1.1 отдельным bump'ом
  acceptance-INI (не в v20 P1);
- config warn (`:320-340`): `vehicle_interaction=1 && npc_actor=0` →
  `config: warn vehicle_interaction=1 requires npc_actor=1` (нет epoch → интенты
  не пойдут); `vehicle_interaction_mission=1` — предупреждение о риске mission.

Конфиг читают оба клиента; сервер конфиг не использует (pinned tests не нужны для
этих ключей, но acceptance INI должен совпадать host/guest).

### 4.8. Роль/миграция/reset

- `HostChanged` (`coop.cpp:6028-6050` и role-reset блоки `:7236-7370`): очистить
  host intents queue, кеш результатов, dirty-set, tracked states; guest — pending
  intents/results/state caches, выйти из proxy, снять collision/considered flags;
- `world_sync::Reset` (`:3380+`): очистка новых очередей/кешей + восстановление
  флагов proxy (до `DeleteProxy`);
- `Leave`/disable/`npc_actor=0`: то же; `s_amHost` change → повторно не
  инициализировать epoch (epoch даёт actor_sync).

---

## 5. Тесты и приёмка

### 5.1. C++ unit (`tests/vehicle_interaction_protocol_test.cpp`, новый target)

- `static_assert` размеров 24/16/24 и offset'ов; `trivially_copyable`;
- таблицы validator'ов: good/bad для каждого action/seat/door/flags/param/epoch/seq;
- `VehicleResult`/`VehicleAction` значения; known-flags masks; result-detail
  helpers (если появятся inline-функции);
- C++ тесты **7/7 → 8/8** (`tests/CMakeLists.txt:52-57` + новый exe).

### 5.2. Integration (`tests/integration_server.py`, константы auto-read из protocol.h)

Добавить auto-константы рядом `:280-330`: `VEHICLE_INTERACTION_SIZE=_struct_size(...)`,
`..._RESULT_SIZE`, `..._STATE_SIZE`, `TYPE_VEHICLE_INTERACTION/RESULT/STATE = MSG[...]`,
pins для rate-констант (`_server_const` + pinned: 8/20/10). Новые тесты (8, к
группе v19 vehicle-тестов `:1973-2170`, регистрация в списке `:3020-3060`):

1. `test_vehicle_interaction_guest_only_relay` — fake guest шлёт 0x23 (валидный) →
   fake host получает байт-в-байт; гость не получает эхо; seq/поля совпадают.
2. `test_vehicle_interaction_validation` — host/guest: размеры 23/25 B, неизвестный
   action/seat/door/flags/param=0/epoch=0/netId=0/seq=0/TargetActor+Entity вместе →
   drop (host ничего не получает), сервер не падает.
3. `test_vehicle_interaction_rate_limit` — 12 интентов в окно при cap 8/с →
   ровно ≤8 релеев, окно сбрасывается (как `test_vehicle_occupants_rate_limit`).
4. `test_vehicle_result_host_only_relay` — host шлёт 0x24 → guest получает;
   guest-отправитель 0x24 → drop + warn; нет эха хосту.
5. `test_vehicle_result_validation_rate` — размер/поля/result>9/rate 20/с.
6. `test_vehicle_state_host_only_relay` — 0x26 host→guest; guest-отправитель drop;
   невалидные health/flags/seat → drop; rate 10/с.
7. `test_vehicle_interaction_no_host_drop` — один guest без хоста шлёт 0x23 →
   drop, сервер жив, subsequent JOIN работает.
8. `test_vehicle_interaction_after_migration` — guest→host relay, host уходит
   (Leave/soft timeout), новый host: 0x23 relay идёт новому хосту; 0x24 от старого
   (уже guest) дропается; stale epoch не ломает сервер.
- Итог: integration **42/42 → 50/50**; существующие 42 не меняются (кроме
  version-derived значений — они auto).
- `tests/README.md` — описание новых тестов.

### 5.3. Fake client (`tools/fake_vehicle_client.py`, новый)

Отдельный маленький инструмент (не трогать 108 KB `fake_actor_client.py`), читает
`kProtocolVersion`/ID/размеры из `net/protocol.h`, framing `[len:u32 LE][type:u8]`.
CLI:
- `--mode guest --vehicle-net-id N --action damage --part wheel_fl --damage 60
  --count 5 --rate 2 --seq-start 1 --expect-result applied` — шлёт 0x23, ждёт 0x24/0x26,
  печатает/ассертит `result`, `seq`, `detail`, state поля; exit code 0/1;
- `--mode host --accept 0x23 --reply applied --state` — принимает 0x23 от реального
  game-guest, печатает payload, отвечает 0x24 Applied и 0x26; ассертит поля;
- `--burst`, `--invalid` — для rate/validation A/B.
Сценарии (single-PC, game+tool):
1. game-host + fake guest: fake получает 0x10 WorldSnapshot/0x21, берёт
   vehicleNetId, шлёт Damage/Door на real host vehicles → real host применяет
   (`result=applied`), host-игрок видит в мире; fake проверяет 0x24/0x26.
2. game-guest + fake host: fake host шлёт WorldSnapshot+0x21 (real guest видит
   proxies), real guest стреляет по прокси → ASI шлёт 0x23; fake host ассертит
   полученный intent, отвечает Applied и State → real guest mirror/лог.
3. негативные: mission-protected (real host reject) в 2-PC; rate burst; stale epoch
   (fake host меняет epoch → guest drop); host migration.
`tools/README.md` — раздел про fake_vehicle_client.

### 5.4. 2-ПК приёмка — новый блок I в `docs/ACCEPTANCE_2PC.md`

Предусловия: пара npc-e20/v20 на обоих ПК; INI 32/42; `npc_actor=1`,
`vehicle_interaction=1`, mission/host_vehicle=0, debug=1.

| # | сценарий | pass criteria |
|---|---|---|
| I1 | Гость стреляет по не-mission wagon (body/wheel) | host real vehicle health падает, `result=applied` у гостя, host видит `vehicle: interaction`; guest mirror wheels/body best-effort; state heartbeat совпадает |
| I2 | Двери через controls: guest у proxy нажимает attack/enter по двери | control-capture (INPUT_*) → host door open/close/break; host+guest дверь в одном состоянии; state doorBits 0/1/2 (3 unknown); ajar не проверять; lock — `NotAllowed` до A/B enum |
| I3 | Гость входит в припаркованный wagon (passenger) | host сажает guest ghost; guest local player в proxy; `VehicleState.guestSeat` совпадает; нет второго vehicle/дубля |
| I4 | Гость hijack не-mission NPC-водителя | NPC выходит/изгнан; guest в driver seat (host ghost); `driving disabled` лог; no driving |
| I5 | Mission wagon: damage/hijack | `result=not_allowed` (reason=mission_protected), host мир не изменён, guest лог/тост; mission не ломается |
| I6 | Host едет на своей wagon, guest пытается damage/hijack | `result=not_allowed` (reason=host_vehicle; default `vehicle_interaction_host_vehicle=0`), host vehicle не изменён |
| I7 | Loss/dup: отключить сеть на 2–3 с mid-intent (или fake burst) | нет двойного применения (idempotency seq), state heartbeat чинит; без duplicate damage/seat |
| I8 | Host migration во время P1 (alt+F4/soft) | intents/results/state старого epoch дропаются; новые интенты идут новому хосту; no crash/leak; proxies reset |
| I9 | Rate/anti-grief: fake guest 30 интентов/с | сервер дропает >8/с; host не спайкает; `perf: frame host` p95/max в норме; лог `rate_limited` при host-бюджете |
| I10 | Collision/tangibility: гость упирается в wagon | локальный proxy solid, host state wins; при уходе/варпе collision off; игрок не застревает внутри |

Плюс обязательные grep-фильтры (как блок G): `vehicle: interaction`,
`vehicle: intent`, `vehicle: state`, `config: warn vehicle`, отсутствие `ERROR`/`warn`
кроме ожидаемых.

### 5.5. Трафик

Формат кадра v19/v20: `[len:u32][type:u8]` = 5 B overhead.

| stream | wire bytes | typical | worst (cap) |
|---|---:|---:|---:|
| 0x23 VehicleInteraction | 24+5=29 | 0.1–1/с (≈0.03 KiB/s) | 8/с → 0.23 KiB/s guest→host |
| 0x24 Result | 16+5=21 | = intents (≈0.02 KiB/s) | 20/с → 0.41 KiB/s host→guest |
| 0x26 VehicleState | 24+5=29 | ≤4 tracked × 1/с + changes (≈0.12–0.25 KiB/s) | 10/с → 0.28 KiB/s host→guest |

P1 total: typical **<0.3 KiB/s**, worst **<0.7 KiB/s** на направление — <5 % от G3
(≤15 KiB/s); transform не дублируется (0x10 остаётся единственным потоком позиции).
Сервер: новый bucket `vehicle` в `net:`; `perf_report.py` — учесть (если парсит
поля net:).

### 5.6. Perf-гейты

- host: ≤2 интента/тик, ≤1 damage-группа/тик; `perf: frame host` p95 ≤4 ms,
  max ≤10 ms не хуже e19; `perf: npc warn`=0; новые функции — маленькие
  (CRAP < ~100), `quality_scan` strict gate vs baseline (не расти CRAP>5000/dup≥4/TODO);
- guest: 0 доп. нативов в тик, кроме near-proxy collisions (≤16 proxies, только
  при изменении near/варп; poll дверей ≤1 door/тик при near);
- server: точные размеры/rate — без очередей; host timeout не зависит от 0x23.

---

## 6. Риски и R&D-гейты

| # | риск | вероятность/эффект | митигация |
|---|---|---|---|
| R1 | `SET_VEHICLE_DAMAGE`/health имеет неожиданные ограничения (p6, invincible/mission/strong) | мед/высокий визуал | A/B в игре до кода; fallback `SET_VEHICLE_BODY_HEALTH`/`ENGINE_HEALTH`; P1 PASS — реальное изменение health host |
| R2 | door readback/lock enum: per-door status/closed/ajar ABSENT; lock enum не подтверждён | высокий для lock/state | doorBits 0/1/2 (3 unknown, ajar→0, не обещать closed); lock actions `NotAllowed`/gated, `Locked` из host cache; `_BREAK_LOCKS` one-way (A/B) |
| R3 | `TASK_ENTER` flags/animated vs `SET_PED_INTO` warp; **non-local SET_PED_INTO UNCERTAIN**; `TASK_LEAVE` — **4 арг** (Ped,Vehicle,flags,unkPed) | средний | v1 `SET_PED_INTO` для Enter; `TASK_LEAVE(...,0,0)` для Exit/hijack; A/B host ghost; `GET_PED_IN_VEHICLE_SEAT` readback |
| R4 | ~~control/prompt ABSENT~~ **исправлено R&D**: `IS_CONTROL_*` PRESENT (R&D §3); capture дверей/loot/hitch через controls возможен, P1.1 не fallback-only | низкий | C3/C4 строить на `IS_CONTROL_JUST_PRESSED` + readback; A/B mapping `INPUT_*`; `DISABLE_CONTROL_ACTION` для suppress, restore |
| R14 | wheelIndex mapping UNCERTAIN; damaged/burst getter ABSENT | средний | A/B loop wheelIndex 0..5 для `_IS_VEHICLE_WHEEL_DESTROYED`/`_BREAK_OFF`; wheelBits только 0/2/3 (1 — host cache/log) |
| R5 | локальная посадка/двери на mission/invincible proxy могут быть заблокированы | средний | A/B: `SET_VEHICLE_IS_CONSIDERED_BY_PLAYER`; fallback — временно снять mission/invincible у near-proxy; если не работает — P1 enter/hijack в игре не включать (wire/fake остаются) |
| R6 | guest local player сидит в proxy, host двигает real vehicle → interpolation тянет игрока | средний | controls suppressed/UNDRIVEABLE; seat reconciliation по state; при жёсткой коррекции — warp игрока вместе с proxy; P1 не обещает езду |
| R7 | server не может валидировать netId/мир → spoof/flood | низкий | host validation (netId map, distance, mission, cooldown, budget); server rate 8/с + exact size; логи |
| R8 | host native spike при очереди интентов | низкий | ≤2/тик, ≤1 damage-группа, queue cap 8, RateLimited |
| R9 | host migration mid-intent | средний | epoch check, drop stale, reset queues; I8 |
| R10 | 0x24/0x26 broadcast на >2 игроков попадёт «не тому» | будущее | P1 = `kMaxPlayers=2`; для >2 добавить target player id в будущем bump (в reserved/новом сообщении) |
| R11 | proxy cleanup/despawn при снятии mission для local enter | низкий | restore mission flags при leave/reset, не снимать для host-authoritative path |
| R12 | damage replay grief (spam) | средний | seq/idempotency, cooldown 250/500 мс, budget 200 HP/с/vehicle, mission/host-vehicle policy, server 8/с |
| R13 | ~~GET health hashes отсутствуют~~ **исправлено R&D §2.1**: возвращают float (body 0..1000, engine -4000..1000; invalid entity→1000) | низкий | gate `DoesEntityExist`+`IS_ENTITY_A_VEHICLE`, квантование u16/i16, `DamageValid` только finite; petrol не поле 0x26 |

**R&D-гейт:** R&D **выполнен** — `docs/NATIVE_RESEARCH_V20_HEALTH.md` (2026-10-27);
результаты внесены в §2.4/§4.3/§4.4/§6. Ни один натив не реализуется без A/B, если
его статус SIGNATURE-ONLY/UNCERTAIN (lock enum, wheelIndex mapping, p6/offset/clamp,
non-local SET_PED_INTO, keepPhysics, TASK flags). Открытые A/B — §10.2; hashes —
только из R&D-дока/NativeDecl 1491.50, не из локального native-DB 1207.

---

## 7. Порядок реализации (5 unit'ов) и proof gates

Каждый unit: failing test first (где честно), build + `scripts\ci.ps1`, quality strict,
worktree clean, commit с why/what/proof. Промежуточные коммиты не деплоить
(после U2 версия v20, но поведение не полное).

### U1 — protocol v20 P1: константы/structs/validators + C++ тест

- `net/protocol.h:22` (version 20), `:36-78` (Msg 0x23–0x27), после `:659` — блок
  enum'ов/consts/structs/validators (§2), `static_assert`/offset'ы;
- `tests/vehicle_interaction_protocol_test.cpp` (новый) + `tests/CMakeLists.txt:52-57`
  (`add_executable` + include dirs; target `vehicle_interaction_protocol_test`);
- proof: build client+server+tests, C++ **8/8**, quality strict PASS, `git show --check`.
- NB: сервер пока не relays (ID'ов не знает никто, кроме enum) — integration 42/42
  не меняется; `kProtocolVersion=20` уже ломает v19 JOIN (ожидаемо).

### U2 — server relay/rate/role/validation + metrics + integration tests

- `server/server.cpp:53-74` (RateWindow поля), `:193-202` (константы),
  `:287-316` (IsGameThreadMessage: 0x24/0x26 true), `:104-148`/`:1038-1048`
  (TrafficBucket::Vehicle + `vehicle=`), `:588-668` (валидаторы через net inline
  helpers), `:947-995` (helper `RelayGuestToHostRate` + dryer-вынос общего кода
  из ActorDamage/ActorResync), `:1714-1810` (новый `VehicleMessagesPhase` или
  кейсы в Misc), `:1947-1956` (dispatch), pinned-константы в интеграционных тестах;
- `tests/integration_server.py`: auto-константы/pins (§5.2), 8 новых тестов,
  список `:3020-3060`; `tests/README.md`;
- proof: build + CI: integration **50/50**, C++ 8/8, quality strict; 3 последовательных
  прогона (lock).

### U3 — host apply path (world_sync) + fake host

- `src/world_sync.h:28-33` (Tick param `partnerGhost` или coop accessor —
  рекомендуется `coop::PartnerGhostHandle()` в `src/coop.h`),
  `src/world_sync.cpp`: `HostIdState.kind` (`:320-347`),
  `FindHostVehicleHandleByNetId` (рядом `:365`), новые inbox/apply/state функции
  (новый блок после `CollectVehicleOccupants` ~`:484` или перед guest-секцией
  `:2264`), `ApplyVehicleInteractionIntents` из host-branch `Tick` (`:3509-3523`),
  `Reset()` (`:3380+`), native wrappers (`:60-216`) + `src/natives.h` P1-блок;
- `src/coop.cpp:6221-6295` send-обёртки (`SendVehicleInteractionResult`,
  `SendVehicleState`), `:6180-6200` callbacks host inbox;
- `tools/fake_vehicle_client.py` (host mode) + `tools/README.md`;
- proof: build + CI + fake guest→real-host сценарий; `perf: frame` без регрессии;
- в этом unit'е host apply ещё не вызывается из-за отсутствия guest capture —
  проверяется fake-клиентом/интеграцией.

### U4 — guest capture + mirror + config + role reset

- `src/coop.cpp`: `UpdateLocalVehicleInteraction` (рядом `UpdateLocalShotEvents`
  `:5006`, вызов в Tick после `:6954-6957`), `TrySendVehicleDamage`
  (рядом `TrySendNpcDamage` `:4905`, вызов после `:5137`),
  `world_sync::Tick` call `:6982` (partnerGhost/accessor), reset-блоки
  `:6028-6050`, `:7236-7370`, `:7430`;
- `src/world_sync.cpp`: `GetVehicleProxyNetId`, `UpdateInteractiveProxy`
  (после `InterpolateVehicleProxies` `:2989-3044`, вызов в guest-branch `:3526-3534`),
  guest inbox/mirror/rollback, proxy flags в `CreateProxy`/`UpdateProxy`/`Reset`;
- `src/config.h:92` поля, `src/config.cpp:121-123` INI, `:204-232` parse,
  `:320-340` warn; `tests/config_test.cpp` — покрыть 4 ключа/дефолты;
- `src/actor_sync.h` — `HostEpoch()/GuestEpoch()` accessors, реализации у
  `actor_sync.cpp:1819`/`:3470`; `src/coop.h` — `PartnerGhostHandle()`;
- `tests/integration_server.py` (при необходимости) + `tools/fake_vehicle_client.py`
  guest mode;
- proof: build + CI (config tests), fake host↔real guest сценарий; in-game
  single-PC A/B (host+guest fake) для damage/enter/door C3; `npc_actor=0` →
  интентов нет.

### U5 — docs/acceptance/traffic/fake scenarios + quality journal + deploy

- docs: `docs/PROTOCOL.md` (version block `:3-70`, новые разделы после 0x22
  `:483-505`, topology/commands `:694-760`, ACTOR-раздел `:285+`), `docs/PERF.md`
  (новый §10e после §10d, конец файла ≈`:316-331`), `docs/ACCEPTANCE_2PC.md` (v20 header/INI 32/42,
  блок I после F/G, откат), `docs/NPC_E1_TESTING.md` (S32), `docs/INTERACTIVITY.md`
  (P1 implemented), `docs/UB_MODE.md` (journal row + quality numbers),
  `README.md`/`ROADMAP.md`, `tools/README.md`, `tests/README.md`;
- quality: `quality_scan` на чистом коммите, delta vs baseline; обновить
  `tools/quality_baseline.json` только по сигналу Lead (как e19);
- `dist\npc-e20` snapshot (`tools/snapshot_build.ps1`), хеши ASI/server, INI 32/42;
- deploy парой local+ARINA, сверка хешей; rollback `dist\npc-e19`;
- proof: full CI 50/50 + C++ 8/8 + quality strict; 2-ПК блок I; traffic/perf из §5.5–5.6;
- только после этого объявлять v20 P1 доступным.

### Сводная карта правок (file:line, HEAD `1e336e9`)

| файл | точки |
|---|---|
| `net/protocol.h` | `:22`; `:36-78`; после `:659` — новые structs/enums/validators/limits |
| `net/client.h` | callbacks `~:104`; senders `~:172`; setters `~:225`; handlers `~:268`; members `~:326` |
| `net/client.cpp` | validators `:320-410`; handlers `:1057-1100`; dispatch `:1215-1226`; senders `:769-837` |
| `server/server.cpp` | `:53-74`; `:104-148`; `:193-202`; `:287-316`; `:588-668`; `:947-995`; `:1714-1810`; `:1947-1956`; `:1038-1048` |
| `src/world_sync.h` | `:28-33` Tick; `~:55` new API |
| `src/world_sync.cpp` | `:60-216` natives; `:320-347`; `:365`; `:2266-2295`; `:2356-2450`; `:2989-3044`; `:3285-3331`; `:3380-3455`; `:3486-3554` |
| `src/world_sync.h/.cpp` (guest) | `:2264-2295` proxy fields; `:3115-3135` despawn; `:3526-3534` guest tick |
| `src/coop.cpp` | `:6028-6050`; `:6180-6200`; `:6221-6295`; `:6850-6885`; `:6954-6957`; `:6982`; `:7236-7370`; `:7430` |
| `src/coop.h` | PartnerGhostHandle / senders declarations |
| `src/actor_sync.h/.cpp` | accessors; `:1819`, `:3470` |
| `src/config.h` | `:92-93` fields |
| `src/config.cpp` | `:121-123`; `:204-232`; `:320-340` |
| `src/natives.h` | новый P1-блок перед `:397-402` |
| `tests/CMakeLists.txt` | `:52-57` новый target |
| `tests/vehicle_interaction_protocol_test.cpp` | новый файл |
| `tests/integration_server.py` | `:280-330`; `:1973-2170`; `:3020-3060` |
| `tests/config_test.cpp` | новые ключи |
| `tools/fake_vehicle_client.py` | новый файл; `tools/README.md` |
| docs | PROTOCOL/PERF/ACCEPTANCE/NPC_E1_TESTING/INTERACTIVITY/UB_MODE/README/ROADMAP |

Качество: новые функции держать <120 строк и CRAP <100; общий switch'и
server/client — по одному case на сообщение; дублирование validators — единые
`net::IsValid*` (dryer), не копии server/client. Baseline quality: e19 =
45 файлов / 505 функций / CRAP>5000=3 / dup≥4=3 / TODO=0; v20 P1 добавит ~2 файла
(protocol test, fake tool не сканируется — `EXTRA_FILES=["tests"]`) и ~25–35
функций; strict gate сравнивает только high_crap/dup≥4/TODO — новых hotspot'ов не
создавать.

---

## 8. Документация, которую надо обновить при реализации

- `docs/PROTOCOL.md` — version block (v20, 0x23–0x27, paired deploy/rollback),
  спецификации 0x23/0x24/0x26 (payload tables, размеры, rates, направления),
  topology/«Какие сообщения шлёт …», история версий v20.
- `docs/PERF.md` — §10e: Vehicle bucket, rate/лимиты, traffic <0.7 KiB/s,
  host native budget, как снимать (`net: … vehicle=`, `perf: net`).
- `docs/ACCEPTANCE_2PC.md` — header npc-e20/v20, INI 32/42, блок I (I1–I10),
  F/G совместимость, откат npc-e19, «что прислать при FAIL».
- `docs/NPC_E1_TESTING.md` — S32 сценарии P1 (in-game).
- `docs/INTERACTIVITY.md` — статус P1 «реализовано», ссылка на этот файл.
- `docs/UB_MODE.md` — журнал unit'ов U1–U5, quality-числа, action log.
- `README.md`/`ROADMAP.md` — статус v20 P1, снапшот npc-e20, откат.
- `tools/README.md` — fake_vehicle_client; `tests/README.md` — новые тесты.

---

## 9. Финальные решения Lead (2026-10-27) — Decisions

Решения получены после ревью спеки и **обязательны** для U1–U5. Отступления —
только письменно до U1.

| # | Решение | Где в спеке |
|---|---|---|
| D1 | **Scope P1:** интенты без вождения/инвентаря/окон (part 12–15 reject); двери — capture через **controls** (`IS_CONTROL_*`, R&D §3) в P1, constexpr-гейт только как rollback при A/B-конфликте; loot/hitch/unhitch — wire/fake-ready, полный capture/семантика в P1.1 (controls больше не fallback-only) | §2.2, §4.4, §5.3, §7 U4 |
| D2 | **Defaults:** mission-vehicle и host-vehicle — reject, `result=NotAllowed` (reason в detail bits 8..15); seat/door/harness/lock валидируются; defaults безопасные. `NotAllowed` — единый policy-код (заменяет прежний `Rejected`; `MissionProtected`/`NoAuthority` стали reason-битами detail) | §2.3, §4.2 |
| D3 | **Лимиты:** rate 8/20/10 в/с; дистанции 12/5/6 м; 200 HP/с/vehicle; cooldown 250 мс; IDs/размеры 0x23=24 B, 0x24=16 B, 0x26=24 B; v20 bump в U2; deploy-пара только U5 (`npc-e20`, rollback `npc-e19`) | §2.6, §2.7 |
| D4 | **Seat P1:** host сажает guest ghost на host-vehicle; guest — в local proxy с disabled controls (`SET_VEHICLE_UNDRIVEABLE`); `hostSeat`/`guestSeat` в 0x26 до P2; 0x24/0x26 broadcast non-host при `kMaxPlayers=2` — ок | §4.3, §4.6 |
| D5 | **INI/CI:** 4 ключа → 32 приёмочных/42 строки; CI gates 50 integration / 8 C++ / pinned rate 8/20/10 | §4.7, §5.1–5.2 |
| D6 | **Протокол/flow:** принято как в спеке; отступления — только письменно до U1 | весь файл |
| D7 | **R&D выполнен** (`docs/NATIVE_RESEARCH_V20_HEALTH.md`): health readback float→u16/i16, `DamageValid` только finite; controls PRESENT; TASK_LEAVE 4 арг; door status/closed ABSENT; lock enum/wheel mapping не подтверждены; petrol не поле. Результаты внесены в §2.4/§4.3/§4.4/§6; открытые A/B — §10.3. P1 не блокируется | §4.3, §6, §10 |
| D8 | **Отступления от спеки после R&D:** `NotAllowed`-политика и wire не меняются; lock actions в игре default `NotAllowed`/gated до A/B enum; wheelBits 0/2/3 (1 не из readback); doorBits 0/1/2 (3 unknown, ajar→0) | §2.4, §4.3 |

**Статус:** все 11 вопросов ревью закрыты (D1–D6); R&D закрыт и внесён (D7–D8) —
открытых вопросов к Lead нет. Остались только in-game A/B (§10.3), решаются в
U3/U4 без изменения wire/protocol.

---

## 10. R&D — выполнен (2026-10-27; `docs/NATIVE_RESEARCH_V20_HEALTH.md`)

**Статус:** R&D-запрос закрыт отдельным researcher'ом; результаты внесены в спеку.
Источники: NativeDecl 1491.50, latest nativedb, rdr3-discoveries controls, Halen84
flags. Ниже — §10.1 что изменилось, §10.2 исходный запрос (архив), §10.3 остаточные A/B.

### 10.1. Что R&D изменил в спеке (внесено)

| тема | результат | где |
|---|---|---|
| health readback | `GET_VEHICLE_BODY_HEALTH 0x42113B857E33C16E` / `GET_VEHICLE_ENGINE_HEALTH 0x90DBFFAC43B22081` / `GET_VEHICLE_PETROL_TANK_HEALTH 0x1E5A9B356D5098BE` → **float**; body 0..1000, engine -4000..1000 (invalid entity → 1000, gate by entity); квантование u16/i16; `DamageValid` только finite | §2.4, §4.3, §6 R13 |
| controls | `IS_CONTROL_PRESSED 0xF3A21BCD95725A4A` + `IS_CONTROL_JUST_PRESSED 0x580417101DDB492F`/RELEASED, `GET_CONTROL_NORMAL 0xEC3C9B8D5327B563`/VALUE, `IS_DISABLED_CONTROL_*`, `DISABLE_CONTROL_ACTION 0xFE99B66D079CF6BC`/`ENABLE_CONTROL_ACTION 0x351220255D64C155` — **PRESENT**; control type 0 (vehicle), 3 OnMount; `_IS_CONTROL_ACTION_VALID(action,control)` args reversed; INPUT_* hashes: ENTER 0xCEFD9220, VEH_EXIT 0xFEFAB9B4, ATTACK 0x07CE1E61, VEH_ACCELERATE 0x5B9FD4E2, VEH_MOVE_LR 0xF1E2852C, VEH_HANDBRAKE 0x65D24C98 | §4.4 C3/C4, §4.6 |
| TASK_LEAVE_VEHICLE | arity **4**: (Ped, Vehicle, flags, unkPed); unkPed=0 | §4.3 |
| door readback | `GET_VEHICLE_DOOR_STATUS`/`IS_VEHICLE_DOOR_CLOSED` **ABSENT**; только `IS_VEHICLE_DOOR_FULLY_OPEN` + `_IS_VEHICLE_DOOR_BROKEN`; ajar не читается (false = closed/ajar); 0/1/2, 3 unknown (не locked) | §2.4, §4.3 |
| lock enum | `SET_VEHICLE_DOORS_LOCKED`/individual + `GET_VEHICLE_DOOR_LOCK_STATUS` enum **не подтверждён**; per-door readback ABSENT → `Locked` из host cache, lock actions gated/`NotAllowed`; `_BREAK_LOCKS_ON_VEHICLE` one-way | §2.2, §4.3, §6 R2 |
| wheels | `_IS_VEHICLE_WHEEL_DESTROYED`/`_ARE_ANY` CONFIRMED, но wheelIndex mapping **UNCERTAIN**; damaged/burst getter **ABSENT** → wheelsBits 0/2/3, 1 не из readback | §2.4, §4.3, §6 R14 |
| 0x26 | petrol **не поле**; doorBits closed/fullyOpen/broken; lock — глобальный flag | §2.4 |
| seat/draft | non-local `SET_PED_INTO_VEHICLE` — **A/B**; `GET_VEHICLE_PED_IS_ENTERING` после warp A/B; seat enum -1/0..8; harness enum 0 LR,1 RR,2 LM,3 RM,4 LF,5 RF; attach/detach BOOL + `GET_DRAFT_ANIMAL_COUNT` verify | §4.3 |
| damage | `SET_VEHICLE_DAMAGE`: offset — **локальная модель**, p6 UNCERTAIN; mission/invincible/strong блокируют (A/B); минимальный набор флагов host real — R&D §2.5 | §4.3 |
| proxy | `SET_ENTITY_COLLISION` keepPhysics UNCERTAIN; `SET_VEHICLE_UNDRIVEABLE` SIGNATURE-ONLY/dangerous (proxy only, restore); `IS_CONSIDERED_BY_PLAYER` CONFIRMED | §4.5, §4.6 |

### 10.2. Исходный запрос (архив; выполнен — статусы §10.1)

**P0 — блокирует U3 (host apply/state) и P1 state**

1. `GET_VEHICLE_BODY_HEALTH(Vehicle)` — hash, arity, return int, диапазон
   (0..1000?), поведение при destroyed/mission/invincible.
2. `GET_VEHICLE_ENGINE_HEALTH(Vehicle)` — hash, arity, return int, диапазон
   (−4000..1000?), destroyed state.
3. `GET_VEHICLE_PETROL_TANK_HEALTH(Vehicle)` — hash/arity/range (если есть).
4. `GET_VEHICLE_DOOR_LOCK_STATUS(Vehicle)` — hash, arity, return enum: все значения
   и mapping (unlocked/locked/locked-for-player/…), сверить с
   `SET_VEHICLE_DOORS_LOCKED(veh, status)` и
   `SET_VEHICLE_INDIVIDUAL_DOORS_LOCKED(veh, doorId, status)` (enum `status`).
5. `IS_VEHICLE_DOOR_FULLY_OPEN(Vehicle, doorId)` и
   `_IS_VEHICLE_DOOR_BROKEN(Vehicle, doorId)` — hash/arity/return; семантика после
   `SET_VEHICLE_DOOR_OPEN/SHUT/BROKEN`; поведение при `deleteDoor=true`.
6. `_IS_VEHICLE_WHEEL_DESTROYED(Vehicle, wheelIndex)` — hash/arity/return;
   **mapping wheelIndex** (DB comment «0 left, 1 right, 4/5 unknown» — подтвердить
   для wagon/cart), `_ARE_ANY_VEHICLE_WHEELS_DESTROYED(Vehicle)`; как damage в зону
   колеса через `SET_VEHICLE_DAMAGE` отражается на wheel state.
7. `SET_VEHICLE_DAMAGE(veh,x,y,z,damage,radius,p6)` — hash/arity уже подтверждены;
   проверить: p6=true/false семантика, система координат offset (локальная модель
   или мир), clamp damage/radius, поведение на mission/invincible/strong/
   player-driven, порог «деталь отвалилась» (wheel/door), воспроизводимость.
8. `SET_VEHICLE_BODY_HEALTH` / `SET_VEHICLE_ENGINE_HEALTH` — range/clamp
   (body 0..1000, engine −4000..1000?), destroyed behavior, взаимодействие с
   invincible/mission; нужно ли писать body health после damage.
9. Флаги видимого урона/посадки: `SET_ENTITY_AS_MISSION_ENTITY(entity,bool,bool)`,
   `SET_ENTITY_INVINCIBLE`, `SET_VEHICLE_STRONG`, `SET_VEHICLE_CAN_BREAK`,
   `SET_VEHICLE_CAN_BE_VISIBLY_DAMAGED` — что из этого блокирует visible damage /
   открытие дверей / посадку local player в scripted proxy; минимальный набор,
   чтобы (a) host real vehicle принимал damage, (b) guest proxy показывал
   visual mirror. Отдельно: можно ли временно снять mission/invincible у proxy и
   вернуть без cleanup/despawn.
10. `IS_VEHICLE_SEAT_FREE(veh, seat)` для wagon/draft на -1/0..8;
    `GET_VEHICLE_MAX_NUMBER_OF_PASSENGERS(veh)` и
    `GET_VEHICLE_MODEL_NUMBER_OF_SEATS(model)` — сколько реально seats у wagon/coach
    (для валидации seat); `GET_PED_IN_VEHICLE_SEAT` для readback.
11. `SET_PED_INTO_VEHICLE(ped, veh, seat)` — работает ли для **не-local** ped
    (host-side ghost игрока/ NPC) в host world; seat -1/0..8; поведение для
    mission/invincible vehicle; что видит local player.
12. `TASK_ENTER_VEHICLE(ped,veh,timeout,seat,speed,flag,p6)` — подтвердить arity 7
    и flags: `ECF_JACK_ANYONE(1<<3)`, `ECF_WARP_PED(1<<4)`,
    `ECF_DONT_WAIT_FOR_VEHICLE_TO_STOP(1<<6)`, `ECF_JUST_PULL_PED_OUT(1<<19)`,
    p6 тип/значение; `TASK_LEAVE_VEHICLE(ped,veh,flag)` — flags (0?), для host NPC
    driver; сравнить animated enter vs `SET_PED_INTO_VEHICLE`.
13. `GET_DRIVER_OF_VEHICLE(veh)`, `GET_VEHICLE_PED_IS_ENTERING(ped)`,
    `IS_PED_IN_VEHICLE(ped,veh,atGetIn)` (p3 semantics),
    `GET_VEHICLE_PED_IS_IN(ped,lastVehicle)` — arity/return; поведение
    `GET_VEHICLE_PED_IS_ENTERING` при warp-`SET_PED_INTO_VEHICLE` (возвращает ли
    handle до входа).
14. Draft/wagon: `_ATTACH_DRAFT_VEHICLE_HARNESS_PED(horse,veh,harness)` — return
    BOOL и harness enum 0..5 (LR,RR,LM,RM,LF,RF) порядок; поведение при занятом
    harness/неверном типе лошади; `_DETACH_DRAFT_VEHICLE_HARNESS_PED(veh,horse)`;
    `_GET_PED_IN_DRAFT_HARNESS(veh,harness)`; `GET_DRAFT_ANIMAL_COUNT(veh,&expected,
    &actual)` — точные типы/return; можно ли attach/detach host-side ghost.
15. Proxy tangibility/controls: `SET_VEHICLE_UNDRIVEABLE(veh,bool)` — эффект
    (нельзя тронуться) и совместимость с player input/AI;
    `SET_VEHICLE_IS_CONSIDERED_BY_PLAYER(veh,bool)` — делает ли proxy «enterable»
    для local player и показывает ли prompt; `SET_VEHICLE_NOT_STEALABLE_AMBIENTLY`;
    `SET_ENTITY_COLLISION(entity,toggle,keepPhysics)` — keepPhysics semantics для
    локального proxy.

**P1 — нужно для дверей C3 capture / P1.1 (не блокирует U1–U3)**

16. **Control/prompt R&D:** есть ли в 1491.50 чтение действий игрока:
    `IS_CONTROL_PRESSED` / `IS_CONTROL_JUST_PRESSED` / `GET_CONTROL_VALUE` /
    `GET_CONTROL_NORMAL` / `IS_DISABLED_CONTROL_PRESSED`. В локальном NativeDecl
    найдены только `IS_CONTROL_ENABLED(control,action)` `0x1CEA6BFDF248E5D9`,
    `UI_PROMPT_IS_CONTROL_ACTION_ACTIVE` `0x1BE19185B8AFE299`,
    `_IS_CONTROL_ACTION_VALID` `0xBC0884BC590951C7`. Нужно: вердикт ABSENT/PRESENT,
    корректные hashes/сигнатуры, RDR2 control/action enum для ENTER/CONTEXT/ATTACK
    у vehicle prompt; если чтения контроля нет — подтвердить, что единственный
    путь — game-native взаимодействие (локальные двери/prompt на proxy) и что оно
    доступно для scripted/mission proxy.
17. `SET_VEHICLE_DOOR_CONTROL(veh,doorId,speed,angle)`,
    `SET_VEHICLE_DOOR_LATCHED(veh,doorId,p2,p3,p4)` (p2..p4 unknown),
    `SET_VEHICLE_DOORS_LOCKED_FOR_ALL_PLAYERS`, `SET_VEHICLE_DOORS_TO_OPEN_AT_ANY_DISTANCE`
    — нужны ли для плавного mirror/локов, параметры/статус.
18. Door IDs/window mapping для wagon/coach: какие `eDoorId` реально
    существуют/видны на wagon/coach (0..11: DSIDE_F/M/M1/M2/R, PSIDE_F/M/M1/M2/R,
    BONNET, BOOT); `windowIndex` 0 FR,1 FL,2 BR,3 BL — подтвердить.
19. Lock readback: `GET_VEHICLE_DOOR_LOCK_STATUS` vs
    `_GET_BREAKABLE_VEHICLE_LOCKS_STATE(veh)` — какой стабилен/что возвращает;
    `_BREAK_LOCKS_ON_VEHICLE(veh)` — эффект (swing doors? какие двери?) и readback.
20. (P1.1 loot/hitch) `TASK_LOOT_ENTITY`, `_SET_FORCE_COACH_ROBBERY_LOOT`,
    `_GET_LOOTING_PICKUP_TARGET_ENTITY`, `_REQUEST_ENTITY_LOOT_LIST`,
    coach-robbery loot hashes — статус/параметры для wagon; hitch capture —
    `GET_DRAFT_ANIMAL_COUNT`/harness poll.

**P2 — P3/P4, не блокирует**

21. `_SET_DRAFT_VEHICLE_DESIRED_SPEED`, AI tasks (`TASK_VEHICLE_TEMP_ACTION` /
    `TASK_VEHICLE_DRIVE_TO_COORD`), `SET_VEHICLE_FORWARD_SPEED`/`STEER_BIAS` —
    только P3; сейчас достаточно статуса из R&D-дока.
22. Ghost ped seat visibility: что нужно, чтобы local player увидел host-side
    ghost сидящим в vehicle (текущий occupant attach) — mission/network flags.

### 10.3. Остаточные A/B (U3/U4; wire/protocol не меняют)

1. Health: mission/invincible/strong/player-driven блокируют `SET_VEHICLE_DAMAGE`?
   p6 true/false; clamp `SET_VEHICLE_BODY_HEALTH`/`ENGINE_HEALTH`.
2. Damage offsets door/wheel для модели wagon; повторяемость; fallback health write;
   `Applied` только по readback (не по факту вызова).
3. Door: `openInstantly/closeInstantly` timing; `deleteDoor=true` →
   `_IS_VEHICLE_DOOR_BROKEN`?; какие `eDoorId` реально есть у wagon/coach (0..11).
4. Lock: enum 0/1/2 для `SET_VEHICLE_DOORS_LOCKED`/individual +
   `GET_VEHICLE_DOOR_LOCK_STATUS`; restore 0; `_BREAK_LOCKS_ON_VEHICLE` readback
   (one-way).
5. Wheel: mapping wheelIndex 0..5 для `_IS_VEHICLE_WHEEL_DESTROYED`/`_BREAK_OFF`;
   fallback wheelsBits=0xFF.
6. Seat: wagon seat count; `SET_PED_INTO_VEHICLE` для host-side ghost;
   `GET_PED_IN_VEHICLE_SEAT` readback; hijack TASK_LEAVE flags vs SET_PED_INTO.
7. Draft: attach/detach host-side ghost horse; harness occupied/invalid;
   `GET_DRAFT_ANIMAL_COUNT` expected/actual.
8. Proxy: `SET_ENTITY_COLLISION` keepPhysics; control context InVehicle у local
   proxy; `DISABLE_CONTROL_ACTION` restore; mission off для local enter/doors;
   prompt `IS_CONSIDERED_BY_PLAYER`.
9. Loot/hitch: coach loot hashes; `TASK_LOOT_ENTITY` на wagon; harness poll.
10. UI prompt (`UI_PROMPT_*`) — только если понадобится; не требование P1.

---

## 11. Ссылки

- `docs/INTERACTIVITY.md` — общий дизайн P0–P4, authority model, §4.1–4.5, §9, §10.
- `docs/NATIVE_RESEARCH_INTERACT.md` — hashes/arity/door/seat/draft/loot таблицы.
- `docs/NATIVE_RESEARCH_V20_HEALTH.md` — R&D v20 P1: health float, controls PRESENT,
  door/wheel readback, lock enum, seat/draft, mission/invincible matrix.
- `docs/PROTOCOL.md` — v19 wire, конвенции сообщений, версия/деплой.
- `docs/PERF.md` — G2/G3, traffic counters, методика.
- `docs/ACCEPTANCE_2PC.md` — 2-ПК приёмка e19 (база для блока I).
- Код: `net/protocol.h`, `net/client.*`, `server/server.cpp`,
  `src/world_sync.*`, `src/coop.*`, `src/actor_sync.*`, `src/config.*`,
  `src/natives.h`, `tests/*`, `tools/fake_actor_client.py` (как образец).
