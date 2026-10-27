# RDR2 native research for v20 P1: host apply / 0x26 readback / controls

**Статус:** R&D, read-only; файл закоммичен в ветке `feature/v20-p1` (docs-коммиты
`a5b8cde`/`e581d95`/`07283a5`). Сам R&D код мода не изменял — реализация по его
выводам сделана отдельно в U3/U4 (`8a34e7d`); решения — `docs/INTERACTIVITY_P1.md`.
**Target:** RDR2 PC **1491.50**.
**Источники:**
- `third_party/rdr2-runtime-scripthook/source/scripting_v3/RDR2.Native/NativeDecl.cs` (ScriptHook RDR2 runtime; ниже `ND:<line>`);
- latest `alloc8or/rdr3-nativedb-data` master (`natives.json`; ниже `DB`);
- `third_party/rdr3-discoveries/Controls/README.md` (input action hashes/contexts);
- Halen84 `eEnterExitVehicleFlags`/`eDoorId/README.md`;
- `docs/INTERACTIVITY_P1.md` §2.4/§4.3/§10 (requirements), `docs/NATIVE_RESEARCH_INTERACT.md` (previous R&D).

**Легенда:**
- **CONFIRMED** — hash + arity/типы есть в NativeDecl 1491.50 и/или latest DB, семантика подтверждена комментарием/кодом.
- **SIGNATURE-ONLY** — hash/сигнатура подтверждены, поведение/значения/enum не проверены в игре.
- **UNCERTAIN** — параметры/семантика не документированы; обязателен A/B.
- **ABSENT** — публичного натива в 1491.50 нет (поиск по NativeDecl + latest DB).

**Важно:** в P1-спеке для body/engine health указан `int`; фактически оба readback-натива возвращают **float**. В P1-спеке также сказано, что `IS_CONTROL_PRESSED` в NativeDecl нет — **это неверно**, нативы есть (см. §3). Эти два места надо поправить в P1-спеке.

---

## 1. TL;DR для U3/U4

- **P0 health readback найден и подтверждён:**
  - `GET_VEHICLE_BODY_HEALTH 0x42113B857E33C16E(Vehicle) -> float` (0..1000);
  - `GET_VEHICLE_ENGINE_HEALTH 0x90DBFFAC43B22081(Vehicle) -> float` (-4000..1000);
  - `GET_VEHICLE_PETROL_TANK_HEALTH 0x1E5A9B356D5098BE(Vehicle) -> float` (0..1000).
- **P0 `SET_VEHICLE_DAMAGE`/health write** — сигнатуры подтверждены; offset — локальная модель; `p6` неизвестен; mission/invincible/strong блокируют урон — только A/B.
- **P0 readback дверей:** `IS_VEHICLE_DOOR_FULLY_OPEN` + `_IS_VEHICLE_DOOR_BROKEN` (per-door); **`GET_VEHICLE_DOOR_STATUS`/`IS_VEHICLE_DOOR_CLOSED` ABSENT**; `GET_VEHICLE_DOOR_LOCK_STATUS` — только **общий** lock status без публичного enum; per-door lock readback нет.
- **P0 wheels:** `_IS_VEHICLE_WHEEL_DESTROYED` и `_ARE_ANY_VEHICLE_WHEELS_DESTROYED` подтверждены, но **mapping wheelIndex на FL/FR/RL/RR не подтверждён**; **damaged/burst getter ABSENT** — 2-битное состояние «damaged/burst» из readback не выводится, только destroyed/unknown.
- **P0 seat/occupancy/draft** — все хэши/арности подтверждены; wagon seat count и non-local `SET_PED_INTO_VEHICLE` — A/B.
- **P1 controls — PRESENT:** `IS_CONTROL_PRESSED/JUST_PRESSED/RELEASED`, `GET_CONTROL_NORMAL/VALUE`, `IS_CONTROL_ENABLED`, `IS_DISABLED_CONTROL_*`, `DISABLE_CONTROL_ACTION`/`ENABLE_CONTROL_ACTION`/`DISABLE_ALL_CONTROL_ACTIONS`, `_SET_CONTROL_CONTEXT`/`_GET_CURRENT_CONTROL_CONTEXT`. P1 §6/R4 надо обновить.
- **Опасные:** `SET_VEHICLE_UNDRIVEABLE` и `SET_VEHICLE_DOORS_LOCKED` — сигнатуры есть, поведение/enum — SIGNATURE-ONLY; не применять к real/mission vehicle без A/B и restore.

---

## 2. P0: host apply / readback

### 2.1. Health: readback (U4/0x26)

| Поле 0x26 | Native | Hash (arity) | Return/диапазон | Статус | Источник/рецепт |
|---|---|---|---|---|---|
| `bodyHealth` (u16, 0..1000; 0xFFFF unknown) | `GET_VEHICLE_BODY_HEALTH` | `0x42113B857E33C16E` (1: Vehicle) | **float**, 0..1000 | **CONFIRMED** signature/comment | ND:14736; DB comment: max 1000, min 0; vehicle не обязательно взрывается при 0 |
| `engineHealth` (i16, -4000..1000; -32768 unknown) | `GET_VEHICLE_ENGINE_HEALTH` | `0x90DBFFAC43B22081` (1) | **float**, -4000..1000 | **CONFIRMED** signature/comment | ND:14562; DB comment: 1000 max, -4000 destroyed, 0 fire, 300 smoke; invalid возвращает 1000 |
| (не поле payload; диагностика) | `GET_VEHICLE_PETROL_TANK_HEALTH` | `0x1E5A9B356D5098BE` (1) | **float**, 0..1000 | **CONFIRMED** signature/comment | ND:14581; DB comment: 1000 max |

**U4 readback recipe:**
```cpp
bool vehValid = DoesEntityExist(veh) && IsEntityAVehicle(veh);
uint16_t bodyQ = kVehicleBodyHealthUnknown;
int16_t  engineQ = kVehicleEngineHealthUnknown;
bool damageValid = false;
if (vehValid) {
    const float body = GetVehicleBodyHealth(veh);    // 0x42113B857E33C16E
    const float eng  = GetVehicleEngineHealth(veh);  // 0x90DBFFAC43B22081
    if (std::isfinite(body) && std::isfinite(eng)) {
        bodyQ = static_cast<uint16_t>(std::lround(std::clamp(body, 0.0f, 1000.0f)));
        engineQ = static_cast<int16_t>(std::lround(std::clamp(eng, -4000.0f, 1000.0f)));
        damageValid = true;
    }
}
```
- `GET_VEHICLE_ENGINE_HEALTH` на невалидном vehicle возвращает 1000 — **сначала** `DoesEntityExist` + `IS_ENTITY_A_VEHICLE 0xC3D96AF45FCCEC4C(1)`, иначе можно записать ложное 1000.
- `GET_VEHICLE_BODY_HEALTH`/petrol тоже gate’ить валидностью; отсутствие failed-сигнала у натива означает, что validity только по entity.
- Petrol tank **не входит** в 24-B `VehicleStatePayload`; не расширять payload. Использовать как debug/для будущего reserved/флагов, не как поле 0x26.

### 2.2. Health: write (U3 host apply)

| Native | Hash (arity) | Signature | Статус | Рецепт/риск |
|---|---|---|---|---|
| `SET_VEHICLE_DAMAGE` | `0x1D7678F81452BB41` (7) | `(Vehicle, float xOffset, float yOffset, float zOffset, float damage, float radius, BOOL p6)` | **CONFIRMED** signature/comment (ND:14549); p6 — **UNCERTAIN** | offset **локальный модели** (DB comment: relative to vehicle model), не world; p6 в v1 = `false`; radius sphere; clamp damage/radius неизвестен; mission/invincible/strong могут блокировать |
| `SET_VEHICLE_BODY_HEALTH` | `0x55CCAAE4F28C67A0` (2) | `(Vehicle, float value)` | **CONFIRMED** signature; range — **SIGNATURE-ONLY** (ожидаемо 0..1000) | clamp 0..1000; писать после damage, если нужен точный body state; readback через GET |
| `SET_VEHICLE_ENGINE_HEALTH` | `0x8BDC5B998B4654EF` (2) | `(Vehicle, float value)` | **CONFIRMED** signature; range — **SIGNATURE-ONLY** (comment: 1000 max, -4000 min) | clamp -4000..1000; 0 и ниже — fire; не отправлять отрицательное без необходимости |
| `SET_VEHICLE_PETROL_TANK_HEALTH` | `0x6AB2918EE3BEC94C` (2) | `(Vehicle, float health)` | **CONFIRMED** signature/comment (1000 max) | clamp 0..1000; petrol не в 0x26 |
| `SET_VEHICLE_FIXED` | `0x79811282A9D1AE56` (1) | `(Vehicle)` | **CONFIRMED** signature/comment | fixes body, но engine не всегда (comment); после — readback |

**U3 apply recipe для damage:**
1. Проверить mission/protected (`IS_ENTITY_A_MISSION_ENTITY`, `_GET_ENTITY_CAN_BE_DAMAGED`, `IS_VEHICLE_WRECKED`) — при reject не вызывать.
2. `SET_VEHICLE_DAMAGE(veh, offsetX/Y/Z, damage, radius, false)`; offset body: `(0,0,0.5)` — стартовый A/B (P1 spec); door/wheel offsets — отдельный A/B (у дизеля/вагона своя геометрия).
3. Подождать 1 тик, прочитать body/engine; если health не изменился — fallback `SET_VEHICLE_BODY_HEALTH`/`SET_VEHICLE_ENGINE_HEALTH` (по бюджету).
4. Сформировать `DamageValid` по фактическому readback; result `Applied`/`Correction`; не выдавать `Applied` только по факту вызова натива.

**A/B для `SET_VEHICLE_DAMAGE`:**
- p6 `false` vs `true` на одном и том же vehicle/offset; смотреть body/engine readback, visible damage, wheel/door break.
- mission/invincible/strong/player-driven: 4 сценария × p6; фиксировать, что блокирует.
- offset: `(0,0,0.5)` (body), `(0,0,1.0)` (engine/капот), door/wheel offsets — подобрать по модели wagon; повторяемость 3 раза.

### 2.3. Doors: readback/apply

| Назначение | Native | Hash (arity) | Return/семантика | Статус | Рецепт/риск |
|---|---|---|---|---|---|
| Дверь открыта полностью | `IS_VEHICLE_DOOR_FULLY_OPEN` | `0x7AE191143C7A9107` (2: Vehicle, doorId) | BOOL | **CONFIRMED** signature/comment | `true` только fully open; **ajar не отличим** от closed (false) |
| Дверь сломана | `_IS_VEHICLE_DOOR_BROKEN` | `0xE979BB5602AD3402` (2: Vehicle, doorId) | BOOL | **CONFIRMED** signature/comment | после `SET_VEHICLE_DOOR_BROKEN`; при `deleteDoor=true` дверь удалена, readback скорее всего true — проверить |
| Открыть дверь | `SET_VEHICLE_DOOR_OPEN` | `0x550CE392A4672412` (4: veh, doorId, BOOL loose, BOOL openInstantly) | void | **CONFIRMED** signature/comment (работает trains/wagons) | `openInstantly=true` — детерминированный mirror; иначе poll; host real |
| Закрыть дверь | `SET_VEHICLE_DOOR_SHUT` | `0x6A3C24B91FD0EA09` (3: veh, doorId, BOOL closeInstantly) | void | **CONFIRMED** signature/comment | `closeInstantly=true` для детерминизма |
| Сломать/удалить дверь | `SET_VEHICLE_DOOR_BROKEN` | `0x9666CF20A1C6D780` (3: veh, doorId, BOOL deleteDoor) | void | **CONFIRMED** signature/comment | one-way; `deleteDoor=true` удаляет; readback `_IS_VEHICLE_DOOR_BROKEN`; no restore |
| Все двери закрыть | `SET_VEHICLE_DOORS_SHUT` | `0xA4FFCD645B11F25A` (2: veh, BOOL closeInstantly) | void | **CONFIRMED** signature | |
| Плавный угол/скорость | `SET_VEHICLE_DOOR_CONTROL` | `0xD57F10EBBA814ECF` (4: veh, doorId, int speed, float angle) | void | **CONFIRMED** signature; semantics — UNCERTAIN | A/B для mirror; readback только fully open |
| Latch/p2..p4 | `SET_VEHICLE_DOOR_LATCHED` | `0x06F8A202EB312A3C` (5: veh, doorId, BOOL p2, BOOL p3, BOOL p4) | void | **SIGNATURE-ONLY**; p2..p4 неизвестны | только A/B, не в v1 |
| Двери открыты на любом расстоянии | `SET_VEHICLE_DOORS_TO_OPEN_AT_ANY_DISTANCE` | `0x362CEDD2A41E0747` (2: veh, BOOL toggle) | void | **SIGNATURE-ONLY** | A/B для remote mirror; не блокирует U3 |
| `GET_VEHICLE_DOOR_STATUS` | — | — | — | **ABSENT in 1491.50** | нет такого натива; аналог — комбинация двух выше |
| `IS_VEHICLE_DOOR_CLOSED` | — | — | — | **ABSENT in 1491.50** | `IS_DOOR_CLOSED 0xC531EE8A1145A149(Hash doorHash)` — это **world door**, не vehicle |
| `GET_VEHICLE_DOOR_LOCK_STATUS` | `0xC867FD144F2469D3` (1: Vehicle) | int | **SIGNATURE-ONLY**; публичного enum нет | только общий lock status; per-door lock readback ABSENT |

**doorBits recipe (0x26; 2 bits на door 0..7):**
```cpp
uint16_t doorBits = 0;
bool doorsValid = vehValid;
if (vehValid) {
    for (int d = 0; d < 8; ++d) {
        const bool broken = IsVehicleDoorBroken(veh, d);      // 0xE979BB5602AD3402
        const bool open   = !broken && IsVehicleDoorFullyOpen(veh, d); // 0x7AE191143C7A9107
        const uint8_t st = broken ? 2 : (open ? 1 : 0);       // 3 = unknown, не locked
        doorBits |= static_cast<uint16_t>(st) << (2 * d);
    }
}
```
- `3` в P1-спеке commented как «locked/unknown»; **per-door lock readback отсутствует**, поэтому для locked использовать **глобальный** `kVehicleStateFlagLocked`, а `3` — только при readback failure/unknown. Это нужно зафиксировать в U4.
- `IS_VEHICLE_DOOR_FULLY_OPEN=false` означает closed **или ajar**; ajar не читается; P1 mirror должен считать 0 «closed/ajar», не обещать точное closed.
- P1 использует doorId 0..7; реальный `eDoorId` 0..11 (bonnet/boot); не складывать 8..11 в doorBits, если wire не расширен.
- После `SET_VEHICLE_DOOR_OPEN(openInstantly=false)` readback может быть false 1–2 тика; ждать/повторять; `true` — сразу.

### 2.4. Wheel readback/break

| Назначение | Native | Hash (arity) | Return/семантика | Статус | Рецепт/риск |
|---|---|---|---|---|---|
| Колесо уничтожено | `_IS_VEHICLE_WHEEL_DESTROYED` | `0xCB2CA620C48BC875` (2: Vehicle, int wheel) | BOOL | **CONFIRMED** signature; mapping index — **UNCERTAIN** | DB comment только для `_BREAK_OFF...`: 0 left, 1 right, 4/5 unknown; для 4-колёсного wagon FL/FR/RL/RR **не подтверждено**; A/B loop i=0..5 |
| Любое колесо уничтожено | `_ARE_ANY_VEHICLE_WHEELS_DESTROYED` | `0x18714953CCED17D3` (1) | BOOL | **CONFIRMED** signature/comment | aggregate; `IS_VEHICLE_DRIVEABLE` может быть true при destroyed wheel (comment DB) |
| Все колёса на земле | `IS_VEHICLE_ON_ALL_WHEELS` | `0x0D5D119529654EE0` (1) | BOOL | **CONFIRMED** signature | aggregate; не заменяет per-wheel |
| Отломать колесо | `_BREAK_OFF_VEHICLE_WHEEL` | `0xD4F5EFB55769D272` (2: veh, wheelIndex) | Entity | **CONFIRMED** signature; mapping — UNCERTAIN | one-way; wheelIndex 0 left/1 right/4-5 unknown; readback `_IS...` |
| Отломать draft-колесо | `_BREAK_OFF_DRAFT_WHEEL` | `0xC372B6A88F6E4AD8` (3: veh, wheelIndex, float destroyingForce) | void | **CONFIRMED** signature/comment | force ~100; wagon/draft |
| Wheel health/damage/burst getter | — | — | — | **ABSENT in 1491.50** | `SET_VEHICLE_TYRES_CAN_BURST` только setter; 2-bit state 1 (damaged/burst) из readback **не выводится** |

**wheelsBits recipe (0x26; 2 bits на колесо 0..3):**
```cpp
uint8_t wheelsBits = 0;
if (vehValid) {
    for (int w = 0; w < 4; ++w) {
        const bool destroyed = IsVehicleWheelDestroyed(veh, w); // 0xCB2CA620C48BC875
        const uint8_t st = destroyed ? 2 : 0; // 1 (damaged/burst) readback-ом не получить
        wheelsBits |= static_cast<uint8_t>(st << (2 * w));
    }
    if (AreAnyVehicleWheelsDestroyed(veh) && wheelsBits == 0)
        wheelsBits = 0xFF; // mapping/readback mismatch -> unknown
}
```
- **Нельзя** ставить 1 (damaged/burst) по readback: только host cache из damage intent или 3 unknown. P1 mirror по wheelBits=2 — `_BREAK_OFF_VEHICLE_WHEEL` (A/B); wheelBits=1 — только log.
- `SET_VEHICLE_DAMAGE` в зону колеса может не менять `_IS_VEHICLE_WHEEL_DESTROYED` (только health body); «detached» — отдельный `_BREAK_OFF_VEHICLE_WHEEL`; A/B.

### 2.5. Flags / mission/invincible/damage matrix

| Native | Hash (arity) | Signature | Статус | Заметка |
|---|---|---|---|---|
| `SET_ENTITY_AS_MISSION_ENTITY` | `0xDC19C288082E586E` (3) | `(Entity, BOOL scriptHostObject, BOOL grabFromOtherScript)` | **CONFIRMED** signature/comment | делает persistent, **не** invincible; не удалять без unmark `(false,false)` |
| `IS_ENTITY_A_MISSION_ENTITY` | `0x138190F64DB4BBD1` (1) | `(Entity)` BOOL | **CONFIRMED** | readback `kVehicleStateFlagMissionProtected` |
| `SET_ENTITY_INVINCIBLE` | `0xA5C38736C426FCB8` (2) | `(Entity, BOOL)` | **CONFIRMED** comment: «totally invincible, no damage» | блокирует damage, ped ragdoll; для real vehicle host снять для damage |
| `SET_VEHICLE_STRONG` | `0xAB315515C9F8803D` (2) | `(Vehicle, BOOL)` | **CONFIRMED** comment | блокирует только crash damage; bullets/explosions — нет |
| `SET_VEHICLE_CAN_BREAK` | `0xC5ED9D59B4646611` (2) | `(Vehicle, BOOL)` | **CONFIRMED** signature | разрешает поломку частей; readback нет |
| `SET_VEHICLE_CAN_BE_VISIBLY_DAMAGED` | `0x4BF8131AE811541C` (2) | `(Vehicle, BOOL)` | **CONFIRMED** signature | visible damage; readback нет |
| `SET_ENTITY_CAN_BE_DAMAGED` | `0x0D06D522B90E861F` (2) | `(Entity, BOOL)` | **CONFIRMED** signature | более прямой disable damage |
| `_GET_ENTITY_CAN_BE_DAMAGED` | `0x75DF9E73F2F005FD` (1) | `(Entity)` BOOL | **CONFIRMED** signature | readback для MissionProtected |
| `_GET_ENTITY_PROOFS` | `0x6CF0DAD7FA1088EA` (1) | `(Entity)` int | **CONFIRMED** signature; bitset semantics — UNCERTAIN | readback proofs; 0 = none |
| `SET_ENTITY_PROOFS` | `0xFAEE099C6F890BB8` (3) | `(Entity, int proofsBitset, BOOL specialFlag)` | **CONFIRMED** signature; bitset — UNCERTAIN | не использовать без A/B |
| `IS_VEHICLE_WRECKED` | `0xDDBEA5506C848227` (1) | `(Vehicle)` BOOL | **CONFIRMED** | `kVehicleStateFlagDead` |
| `IS_ENTITY_DEAD` | `0x7D5B1F88E7504BBA` (1) | `(Entity)` BOOL | **CONFIRMED** | альтернатива dead |
| `IS_ENTITY_A_VEHICLE` | `0xC3D96AF45FCCEC4C` (1) | `(Entity)` BOOL | **CONFIRMED** | gate readback |
| `IS_VEHICLE_DRIVEABLE` | `0xB86D29B10F627379` (3) | `(Vehicle, BOOL p1, BOOL p2)` | **SIGNATURE-ONLY**; p1/p2 unknown | readback после UNDRIVEABLE — A/B |
| `SET_VEHICLE_UNDRIVEABLE` | `0x6E884BAB713A2A94` (2) | `(Vehicle, BOOL)` | **SIGNATURE-ONLY**; dangerous | см. §4 |

**Минимальный набор для host real damage (рекомендация, A/B):**
1. `SET_ENTITY_INVINCIBLE(veh,false)`.
2. `SET_ENTITY_CAN_BE_DAMAGED(veh,true)` (если стояло false).
3. `SET_VEHICLE_CAN_BE_VISIBLY_DAMAGED(veh,true)`.
4. `SET_VEHICLE_CAN_BREAK(veh,true)`.
5. `SET_VEHICLE_STRONG(veh,false)`.
Mission entity **сама по себе не делает vehicle immune** (persistent ≠ invincible), но mission-скрипт/engine может блокировать; проверить `_GET_ENTITY_CAN_BE_DAMAGED`/proofs.
Для guest proxy: временно `SET_ENTITY_INVINCIBLE(proxy,false)` + `SET_ENTITY_CAN_BE_DAMAGED(true)` + visible-damage, применить damage, затем **вернуть** invincible; mission-флаг трогать только A/B; `SET_ENTITY_AS_MISSION_ENTITY(proxy,false,false)` — только если cleanup/despawn безопасны (риск потерять proxy). Guest local enter/doors могут требовать mission off; A/B.

**Мешает ли mission entity открытию дверей/посадке local player?** Прямого документа нет; `SET_VEHICLE_IS_CONSIDERED_BY_PLAYER(false)` точно убирает enter prompt; mission/invincible — A/B. `SET_ENTITY_AS_MISSION_ENTITY` не должен блокировать `SET_PED_INTO_VEHICLE` (script warp), но может влиять на engine interaction; тест.

### 2.6. Seats / occupancy

| Native | Hash (arity) | Signature/return | Статус | Рецепт/риск |
|---|---|---|---|---|
| `IS_VEHICLE_SEAT_FREE` | `0xE052C1B1CAA4ECE4` (2) | `(Vehicle, int seat)` BOOL | **CONFIRMED** | seat enum ниже; last seat = `maxPass-1` (DB comment) |
| `GET_VEHICLE_MAX_NUMBER_OF_PASSENGERS` | `0xA9C55F1C15E62E06` (1) | `(Vehicle)` int | **CONFIRMED** | wagon/coach actual count — A/B |
| `GET_VEHICLE_MODEL_NUMBER_OF_SEATS` | `0x9A578736FF3A17C3` (1) | `(Hash model)` int | **CONFIRMED** | model-level count; может включать driver |
| `GET_PED_IN_VEHICLE_SEAT` | `0xBB40DD2270B65366` (2) | `(Vehicle, int seat)` Ped | **CONFIRMED** | readback seat; 0 если пусто |
| `GET_LAST_PED_IN_VEHICLE_SEAT` | `0x74583B19FEEAFDA7` (2) | `(Vehicle, int seat)` Ped | **CONFIRMED** | history |
| `ARE_ANY_VEHICLE_SEATS_FREE` | `0xA0A424505A1B6429` (1) | `(Vehicle)` BOOL | **CONFIRMED** | быстрый gate |
| `GET_DRIVER_OF_VEHICLE` | `0x2963B5C1637E8A27` (1) | `(Vehicle)` Ped | **CONFIRMED** | driver = seat -1 |
| `SET_PED_INTO_VEHICLE` | `0xF75B0D629E1C063D` (3) | `(Ped, Vehicle, int seat)` void | **CONFIRMED** signature; non-local ped — **UNCERTAIN** | host-side ghost/NPC: скриптовый warp должен работать; A/B; mission/invincible vehicle — A/B; `GET_PED_IN_VEHICLE_SEAT` readback |
| `CREATE_PED_INSIDE_VEHICLE` | `0x7DD959874C1FD534` (6) | `(Vehicle, Hash model, int seat, BOOL p3, BOOL p4, BOOL p5)` Ped | **CONFIRMED** signature; p3..p5 — UNCERTAIN | альтернатива spawn в seat; appearance после |
| `GET_VEHICLE_PED_IS_ENTERING` | `0xF92691AED837A5FC` (1) | `(Ped)` Vehicle | **CONFIRMED** signature; после warp — UNCERTAIN | может вернуть 0 при SET_PED_INTO_VEHICLE; A/B |
| `IS_PED_IN_VEHICLE` | `0xA3EE4A07279BB9DB` (3) | `(Ped, Vehicle, BOOL atGetIn)` BOOL | **CONFIRMED** signature; p3 semantics — UNCERTAIN | atGetIn=true учитывает вход; A/B |
| `GET_VEHICLE_PED_IS_IN` | `0x9A9112A0FE9A4713` (2) | `(Ped, BOOL lastVehicle)` Vehicle | **CONFIRMED** signature/comment | last=true возвращает last vehicle, если не в машине |
| `GET_VEHICLE_PED_IS_USING` | `0x6094AD011A2EA87D` (1) | `(Ped)` Vehicle | **CONFIRMED** | текущая включая entering — A/B |
| `IS_PED_IN_ANY_VEHICLE` | `0x997ABD671D25CA0B` (2) | `(Ped, BOOL atGetIn)` BOOL | **CONFIRMED** | |
| `GET_LAST_PED_IN_VEHICLE_SEAT` | см. выше | | | |

**Seat enum (eVehicleSeat, CONFIRMED из DB comment):** `-2` any passenger, `-1` driver, `0` front right, `1` back left, `2` back right, `3` extra left1, `4` extra right1, `5` extra left2, `6` extra right2, `7` extra left3, `8` extra right3, `9` num. `kVehicleSeatNone=127` — wire sentinel, **не** передавать в нативы.

**Wagon/coach:** сколько реально seats — надо A/B на модели: `GET_VEHICLE_MODEL_NUMBER_OF_SEATS(model)` vs `GET_VEHICLE_MAX_NUMBER_OF_PASSENGERS(veh)`; `IS_VEHICLE_SEAT_FREE` для 0..8; в wagon могут быть только driver + N; extra invalid seats могут возвращать true/false — не считать валидными без проверки.

### 2.7. TASK_ENTER / TASK_LEAVE / hijack

| Native | Hash (arity) | Signature | Статус | Рецепт/риск |
|---|---|---|---|---|
| `TASK_ENTER_VEHICLE` | `0xC20E50AA46D09CA8` (7) | `(Ped, Vehicle, int timeout, int seat, float speed, int flag, Any p6)` | **CONFIRMED** signature; flags — Halen84 | `timeout=-1`; `seat` enum; `p6` обычно 0 (native types ulong); animated, AI/door-dependent |
| `TASK_LEAVE_VEHICLE` | `0xD3DBCE61A490BE02` (**4**) | `(Ped, Vehicle, int flags, Ped unkPed)` | **CONFIRMED** signature | P1-спека пишет 3 арг — на самом деле 4; `unkPed=0`; flags see TASK_ENTER |
| `TASK_LEAVE_ANY_VEHICLE` | `0x504D54DF3F6F2247` (3) | `(Ped, int p1, int taskFlag)` | **CONFIRMED** signature | альтернатива, если vehicle handle неизвестен; p1=0 |
| `TASK_BREAK_VEHICLE_DOOR_LOCK` | `0xBB28D1BC9EA8A6A5` (2) | `(Ped, Vehicle)` | **CONFIRMED** signature; semantics — UNCERTAIN | host real; likely coach robbery/hijack; A/B; mission/player-driven risk |
| `_TASK_BOARD_VEHICLE` | `0xE53D17AD837CBF7C` (6) | `(Ped, Vehicle, int p2, Any p3, int speed, int boardingFlags)` | **SIGNATURE-ONLY**; NativeDecl ulong p2..p5 vs DB int/Any — mismatch | не в v1; только A/B |
| `_TASK_BOARD_VEHICLE_2` | `0xE41A09C8DDFF7AA4` (5) | `(Ped, Any p1, int p2, float speed, int boardingFlags)` | **UNCERTAIN** | не в v1 |
| `SET_PED_INTO_VEHICLE` | см. §2.6 | | | детерминированный warp; для P1 v1 |
| `GET_DRIVER_OF_VEHICLE` | см. §2.6 | | | hijack: взять NPC driver, `TASK_LEAVE_VEHICLE(driver,veh,0,0)` или `SET_PED_INTO_VEHICLE` после; A/B |

**Enter/exit flags (`eEnterExitVehicleFlags`, CONFIRMED значения из Halen84):**
`ECF_NONE=0`, `ECF_RESUME_IF_INTERRUPTED=1<<0`, `ECF_WARP_ENTRY_POINT=1<<1`, `ECF_INTERRUPT_DURING_GET_IN=1<<2`, `ECF_JACK_ANYONE=1<<3`, `ECF_WARP_PED=1<<4`, `ECF_INTERRUPT_DURING_GET_OUT=1<<5`, `ECF_DONT_WAIT_FOR_VEHICLE_TO_STOP=1<<6`, `ECF_ALLOW_EXTERIOR_ENTRY=1<<7`, `ECF_DONT_CLOSE_DOOR=1<<8`, `ECF_WARP_IF_DOOR_IS_BLOCKED=1<<9`, `ECF_ENTER_USING_NAVMESH=1<<10`, `ECF_JUMP_OUT=1<<12`, `ECF_PREFER_DISMOUNT_SIDE_WITH_FEWER_PEDS=1<<14`, `ECF_DONT_DEFAULT_WARP_IF_DOOR_BLOCKED=1<<16`, `ECF_USE_LEFT_ENTRY=1<<17`, `ECF_USE_RIGHT_ENTRY=1<<18`, `ECF_JUST_PULL_PED_OUT=1<<19`, `ECF_BLOCK_SEAT_SHUFFLING=1<<20`, `ECF_WARP_IF_SHUFFLE_LINK_IS_BLOCKED=1<<22`, `ECF_DONT_JACK_ANYONE=1<<23`, `ECF_WAIT_FOR_ENTRY_POINT_TO_BE_CLEAR=1<<24`, `ECF_USE_HITCH_DISMOUNT_VARIANT=1<<25`, `ECF_EXIT_SEAT_ON_TO_VEHICLE=1<<26`, `ECF_ALLOW_SCRIPTED_TASK_ABORT=1<<27`, `ECF_WILL_SHOOT_AT_TARGET_PEDS=1<<28`, `ECF_INTERRUPT_ALWAYS=1<<29`, `ECF_IGNORE_ENTRY_FROM_CLOSEST_POINT=1<<30`, `ECF_ALLOW_JACK_PLAYER_PED_ONLY=1<<31`.
- P1 v1: **warp** `SET_PED_INTO_VEHICLE` для Enter; `TASK_LEAVE_VEHICLE` для Exit; hijack `TASK_LEAVE_VEHICLE(driver,veh,0,0)` (или `SET_PED_INTO_VEHICLE` после) + `SET_PED_INTO_VEHICLE(ghost,veh,-1)`.
- `TASK_ENTER_VEHICLE` flags A/B: `ECF_WARP_PED|ECF_JACK_ANYONE` может быть достаточно для hijack; animated door/task behavior — UNCERTAIN.
- `p6` type Any; native expects ulong; 0.

### 2.8. Draft harness

| Native | Hash (arity) | Signature/return | Статус | Рецепт/риск |
|---|---|---|---|---|
| `_ATTACH_DRAFT_VEHICLE_HARNESS_PED` | `0x316CDB5B6E8F4110` (3) | `(Ped horse, Vehicle draft, int harnessId)` **BOOL** | **CONFIRMED** signature; behavior on occupied/invalid horse — UNCERTAIN | harness 0..5 (см. ниже); host real; return bool; проверять `GET_DRAFT_ANIMAL_COUNT` |
| `_DETACH_DRAFT_VEHICLE_HARNESS_PED` | `0xB36D3EC70963BE60` (2) | `(Vehicle draft, Ped horse)` **BOOL** | **CONFIRMED** signature | cleanup; return bool |
| `_DETACH_DRAFT_VEHICLE_HARNESS_FROM_INDEX` | `0x4402960666000E62` (2) | `(Vehicle draft, int harnessId)` | **CONFIRMED** signature | удобно для cleanup по harness |
| `_GET_PED_IN_DRAFT_HARNESS` | `0xA8BA0BAE0173457B` (2) | `(Vehicle draft, int harnessId)` Ped | **CONFIRMED** signature/comment | harness enum 0..5 |
| `GET_DRAFT_ANIMAL_COUNT` | `0xA19447D83294E29F` (3) | `(Vehicle, int* expected, int* actual)` BOOL | **CONFIRMED** signature | verify attach (expected/actual) |
| `_GET_NUM_DRAFT_VEHICLE_HARNESS_PED` | `0x5B1A26BB18E7D451` (1) | `(Hash model)` int | **CONFIRMED** signature | число harness slots по модели |
| `_GET_VEHICLE_DRAFT_HORSE_IS_ATTACHED_TO` | `0xE4770DA1B8FF4FD1` (1) | `(Ped horse)` Vehicle | **CONFIRMED** signature | readback |
| `_GET_LAST_VEHICLE_DRAFT_HORSE_WAS_ATTACHED_TO` | `0x5064DB5083C29921` (1) | `(Ped horse)` Vehicle | **CONFIRMED** signature | history |
| `IS_DRAFT_VEHICLE` | `0xEA44E97849E9F3DD` (1) | `(Vehicle)` BOOL | **CONFIRMED** | gate |
| `_SET_DRAFT_VEHICLE_ANIMALS_CAN_DETACH` | `0x6090A031C69F384E` (2) | `(Vehicle, BOOL)` | **CONFIRMED** | false — запрет detach |
| `_SET_DRAFT_VEHICLE_YOKE_CAN_BREAK` | `0x226C6A4E3346D288` (2) | `(Vehicle, BOOL)` | **CONFIRMED** | false — запрет yoke break |
| `_SET_DRAFT_VEHICLE_ALLOW_DRAFT_ANIMAL_AUTO_CREATION` | `0x87344305778E5415` (2) | `(Vehicle, BOOL)` | **CONFIRMED** | после manual attach false |
| `_SET_DRAFT_ANIMAL_RANDOM_SEED` | `0x8C6D9A399126C194` (2) | `(Vehicle, int seed)` | **CONFIRMED** signature | deterministic auto-create |
| `_SET_DRAFT_VEHICLE_DESIRED_SPEED` | `0x0C3F0F7F92CA847C` (2) | `(Vehicle, float)` | **SIGNATURE-ONLY** | P2/P3; возможно тянет лошадей; A/B |

**Harness enum (eDraftHarness, CONFIRMED из DB comment):** `0 LR`, `1 RR`, `2 LM`, `3 RM`, `4 LF`, `5 RF`; count per model через `_GET_NUM_DRAFT_VEHICLE_HARNESS_PED(model)`.
- Attach occupied harness/неверный тип лошади — return false/undefined; A/B; не считать успех без `GET_DRAFT_ANIMAL_COUNT` expected/actual.
- Host-side ghost/NPC horse attach/detach — A/B; если attach host-side guest horse невозможен, P1 draft ограничить host NPC wagon (document).

### 2.9. Proxy tangibility / controls

| Native | Hash (arity) | Signature | Статус | Рецепт/риск |
|---|---|---|---|---|
| `SET_VEHICLE_UNDRIVEABLE` | `0x6E884BAB713A2A94` (2) | `(Vehicle, BOOL)` | **SIGNATURE-ONLY**; dangerous | P1: ставить только на guest proxy, пока guest driver/без lease; restore на exit/reset; **не** на host real; readback `IS_VEHICLE_DRIVEABLE` A/B |
| `IS_VEHICLE_DRIVEABLE` | `0xB86D29B10F627379` (3) | `(Vehicle, BOOL p1, BOOL p2)` BOOL | **SIGNATURE-ONLY**; p1/p2 unknown | readback после UNDRIVEABLE; p1/p2 A/B |
| `SET_VEHICLE_IS_CONSIDERED_BY_PLAYER` | `0x54800D386C5825E5` (2) | `(Vehicle, BOOL)` | **CONFIRMED** signature/comment | false = local player не пытается войти; true для interactive proxy; A/B prompt |
| `SET_VEHICLE_NOT_STEALABLE_AMBIENTLY` | `0x09C970AE59ABF6B2` (2) | `(Vehicle, BOOL)` | **CONFIRMED** signature | theft prompt; proxy/host A/B |
| `SET_ENTITY_COLLISION` | `0xF66F820909453B8C` (3) | `(Entity, BOOL toggle, BOOL keepPhysics)` | **CONFIRMED** signature; keepPhysics semantics — **UNCERTAIN** | toggle=true/false; keepPhysics true/false A/B; для proxy near — включить collision; far — off |
| `SET_ENTITY_COMPLETELY_DISABLE_COLLISION` | `0xE0580EC84813875A` (3) | `(Entity, BOOL toggle, BOOL keepPhysics)` | **CONFIRMED** signature | альтернатива; A/B |
| `GET_ENTITY_COLLISION_DISABLED` | `0xAA2FADD30F45A9DA` (1) | `(Entity)` BOOL | **CONFIRMED** signature | readback collision |
| `SET_ENTITY_CAN_BE_DAMAGED` | `0x0D06D522B90E861F` (2) | `(Entity, BOOL)` | **CONFIRMED** signature | для proxy mirror damage |
| `_GET_ENTITY_CAN_BE_DAMAGED` | `0x75DF9E73F2F005FD` (1) | `(Entity)` BOOL | **CONFIRMED** signature | readback |

**keepPhysics A/B:** `SET_ENTITY_COLLISION(proxy, true, false)` vs `(true, true)`; что происходит с player push/vehicle physics/wheel spin; P1: near → collision on, far/warp → off; не менять host real без нужды.

---

## 3. P1: control/prompt R&D

### 3.1. Вердикт: PRESENT (не ABSENT)

В NativeDecl **1491.50** есть полный набор `IS_CONTROL_*` (ND:7500–7520); P1 §6/R4 («NativeDecl не даёт IS_CONTROL_PRESSED») **устарел**.

| Native | Hash (arity) | Signature/return | Статус | Источник/рецепт |
|---|---|---|---|---|
| `IS_CONTROL_ENABLED` | `0x1CEA6BFDF248E5D9` (2) | `(int control, Hash action)` BOOL | **CONFIRMED** | ND:7500; action valid/enabled |
| `IS_CONTROL_PRESSED` | `0xF3A21BCD95725A4A` (2) | `(int control, Hash action)` BOOL | **CONFIRMED** | ND:7501; continuous hold |
| `IS_CONTROL_RELEASED` | `0x648EE3E7F38877DD` (2) | `(int control, Hash action)` BOOL | **CONFIRMED** | |
| `IS_CONTROL_JUST_PRESSED` | `0x580417101DDB492F` (2) | `(int control, Hash action)` BOOL | **CONFIRMED** | edge press |
| `IS_CONTROL_JUST_RELEASED` | `0x50F940259D3841E6` (2) | `(int control, Hash action)` BOOL | **CONFIRMED** | edge release |
| `GET_CONTROL_VALUE` | `0xD95E79E8686D2C27` (2) | `(int control, Hash action)` int | **CONFIRMED** | raw value |
| `GET_CONTROL_NORMAL` | `0xEC3C9B8D5327B563` (2) | `(int control, Hash action)` float | **CONFIRMED** | axis -1..1; для steer |
| `GET_CONTROL_UNBOUND_NORMAL` | `0x5B84D09CEC5209C5` (2) | int control, action → float | **CONFIRMED** | unbound variant |
| `SET_CONTROL_VALUE_NEXT_FRAME` | `0xE8A25867FBA3B05E` (3) | `(int control, Hash action, float value)` BOOL | **SIGNATURE-ONLY/UNCERTAIN** | simulate input; только lab/P3, не guest remote |
| `IS_DISABLED_CONTROL_PRESSED` | `0xE2587F8CBBD87B1D` (2) | `(int control, Hash action)` BOOL | **CONFIRMED** | читать disabled controls |
| `IS_DISABLED_CONTROL_JUST_PRESSED` | `0x91AEF906BCA88877` (2) | same | **CONFIRMED** | |
| `IS_DISABLED_CONTROL_JUST_RELEASED` | `0x305C8DCD79DA8B0F` (2) | same | **CONFIRMED** | |
| `GET_DISABLED_CONTROL_NORMAL` | `0x11E65974A982637C` (2) | same → float | **CONFIRMED** | |
| `DISABLE_CONTROL_ACTION` | `0xFE99B66D079CF6BC` (3) | `(int control, Hash action, BOOL disableRelatedActions)` | **CONFIRMED** | suppress per-action |
| `ENABLE_CONTROL_ACTION` | `0x351220255D64C155` (3) | `(int control, Hash action, BOOL enableRelatedActions)` | **CONFIRMED** | restore |
| `DISABLE_ALL_CONTROL_ACTIONS` | `0x5F4B6931816E599B` (1) | `(int control)` | **CONFIRMED** | **слишком широко** — не использовать в P1 |
| `_SET_CONTROL_CONTEXT` | `0x2804658EB7D8A50B` (2) | `(int control, Hash context)` | **CONFIRMED** signature | менять context — риск; не в P1 |
| `_GET_CURRENT_CONTROL_CONTEXT` | `0xDDCEB0F26C89C00F` (1) | `(int control)` Hash | **CONFIRMED** signature | read-only context |
| `_IS_CONTROL_ACTION_VALID` | `0xBC0884BC590951C7` (2) | `(Hash action, int control)` BOOL | **CONFIRMED** signature | **порядок args reversed** vs IS_CONTROL_* |
| `UI_PROMPT_IS_CONTROL_ACTION_ACTIVE` | `0x1BE19185B8AFE299` (1) | `(Hash controlAction)` BOOL | **CONFIRMED** signature | только prompt UI; не замена IS_CONTROL_* |

**Контексты/control type:** из `third_party/rdr3-discoveries/Controls/README.md`: все контексты — control type **0**, кроме `OnMount`/`AnimalControlSet` = **3**, `InteractionLockOn` = **2**. Для vehicle context `InVehicle` использовать `control=0`. `_GET_CURRENT_CONTROL_CONTEXT(0)` возвращает hash контекста (`OnFoot`/`InVehicle`/…), если нужно диагностировать.

**Vehicle input action hashes (CONFIRMED input hashes из discoveries; native signature — ND):**
| Action | Hash | Контекст |
|---|---:|---|
| `INPUT_VEH_ACCELERATE` | `0x5B9FD4E2` | InVehicle |
| `INPUT_VEH_CAR_ACCELERATE` | `0xB9F544B0` | InVehicle |
| `INPUT_VEH_DRAFT_ACCELERATE` | `0xE99D2B05` | InVehicle |
| `INPUT_VEH_BRAKE` | `0x6E1F639B` | InVehicle |
| `INPUT_VEH_CAR_BRAKE` | `0xD1887B3F` | InVehicle |
| `INPUT_VEH_DRAFT_BRAKE` | `0xD648E48D` | InVehicle |
| `INPUT_VEH_HANDBRAKE` | `0x65D24C98` | InVehicle |
| `INPUT_VEH_MOVE_LR` | `0xF1E2852C` | InVehicle (axis) |
| `INPUT_VEH_CAR_TURN_LR` | `0x3BD38D43` | InVehicle |
| `INPUT_VEH_DRAFT_TURN_LR` | `0xA7DFAE8A` | InVehicle |
| `INPUT_VEH_MOVE_UD` | `0x8A81C00C` | InVehicle |
| `INPUT_VEH_DRAFT_MOVE_UD` | `0x23595CEA` | InVehicle |
| `INPUT_VEH_DRAFT_SWITCH_DRIVERS` | `0x70B87844` | InVehicle |
| `INPUT_ENTER` | `0xCEFD9220` | OnFoot/OnMount/InVehicle |
| `INPUT_VEH_EXIT` | `0xFEFAB9B4` | InVehicle |
| `INPUT_ATTACK` | `0x07CE1E61` | OnFoot/OnMount/InVehicle |
| `INPUT_ATTACK2` | `0x0283C582` | (context set) |
| `INPUT_VEH_ATTACK` | `0xF4330038` | InVehicle |
| `INPUT_VEH_ATTACK2` | `0xF1C341BA` | InVehicle |
| `INPUT_VEH_AIM` | `0xD7CAFCEF` | InVehicle |
| `INPUT_VEH_DRAFT_ATTACK` | `0xF40AB198` | InVehicle |
| `INPUT_VEH_DRAFT_ATTACK2` | `0x886F12DD` | InVehicle |
| `INPUT_VEH_DRAFT_AIM` | `0xBDD5830D` | InVehicle |

### 3.2. P1.1 minimal recipe

Detect (guest in local proxy):
```cpp
const int kControlTypeVehicle = 0;
bool exitPressed = IS_CONTROL_JUST_PRESSED(0, 0xFEFAB9B4); // INPUT_VEH_EXIT
bool attack      = IS_CONTROL_PRESSED(0, 0xF4330038);      // INPUT_VEH_ATTACK
float steer      = GET_CONTROL_NORMAL(0, 0xF1E2852C);      // INPUT_VEH_MOVE_LR (-1..1)
bool accel       = IS_CONTROL_PRESSED(0, 0x5B9FD4E2);      // INPUT_VEH_ACCELERATE
```
- Для wagon/draft использовать `INPUT_VEH_DRAFT_*` action hashes; для on-foot prompt — `INPUT_ENTER`/`INPUT_ATTACK`.
- `IS_CONTROL_ENABLED(0, action)` перед проверкой; `_IS_CONTROL_ACTION_VALID(action, 0)` (порядок action,control) для валидации.
- Контекст `InVehicle` включается автоматически после enter; control=0. Если guest local player в proxy, но vehicle proxy считается invalid/не mission, context может остаться OnFoot — A/B.

Suppress (P1 controls disabled):
```cpp
DISABLE_CONTROL_ACTION(0, 0x5B9FD4E2, true); // accelerate
DISABLE_CONTROL_ACTION(0, 0x6E1F639B, true); // brake
DISABLE_CONTROL_ACTION(0, 0xF1E2852C, true); // steer LR
DISABLE_CONTROL_ACTION(0, 0x8A81C00C, true); // move UD
DISABLE_CONTROL_ACTION(0, 0x65D24C98, true); // handbrake
```
- каждый тик; restore `ENABLE_CONTROL_ACTION` на Exit/Reset/role change. `DISABLE_ALL_CONTROL_ACTIONS(0)` — **не** использовать (камера/UI).
- `SET_VEHICLE_UNDRIVEABLE(proxy,true)` — альтернатива/дополнение; P1 spec; restore.
- `IS_DISABLED_CONTROL_PRESSED` позволяет читать нажатия даже после `DISABLE_CONTROL_ACTION`, если нужен prompt/hotkey.
- `SET_CONTROL_VALUE_NEXT_FRAME` **не** использовать для guest remote; только lab (host player input simulation, P3).

**Control/prompt verdict для P1.1:** PRESENT; P1 spec R4 must be updated. Если нужен **UI prompt** (кнопка на экране), `IS_CONTROL_*` даёт ввод, но создание prompt — отдельные `UI_PROMPT_*` (не в P1); C3/C4 capture можно строить на `IS_CONTROL_JUST_PRESSED` + `GET_VEHICLE_*` состояниях, а не выдумывать game-native interaction.

---

## 4. P1: doors/locks/IDs/loot/hitch

### 4.1. Door control/lock natives

| Native | Hash (arity) | Signature | Статус | Заметка |
|---|---|---|---|---|
| `SET_VEHICLE_DOOR_CONTROL` | `0xD57F10EBBA814ECF` (4) | `(Vehicle, int doorId, int speed, float angle)` | **CONFIRMED** signature; semantics — UNCERTAIN | плавный mirror; readback только fully open; A/B |
| `SET_VEHICLE_DOOR_LATCHED` | `0x06F8A202EB312A3C` (5) | `(Vehicle, int doorId, BOOL p2, BOOL p3, BOOL p4)` | **SIGNATURE-ONLY** | p2..p4 неизвестны; не в v1 |
| `SET_VEHICLE_DOORS_LOCKED_FOR_ALL_PLAYERS` | `0x2381977DA948F8DC` (2) | `(Vehicle, BOOL)` | **CONFIRMED** signature | guest ghost не Player; host player only; A/B |
| `SET_VEHICLE_DOORS_LOCKED_FOR_PLAYER` | `0x359A8EA1FB8D6F0F` (3) | `(Vehicle, Player, BOOL)` | **CONFIRMED** signature | local player index |
| `GET_VEHICLE_DOORS_LOCKED_FOR_PLAYER` | `0xFA2CDDFEB8BC898B` (2) | `(Vehicle, Player)` BOOL | **CONFIRMED** signature | readback for local player |
| `SET_VEHICLE_DOORS_TO_OPEN_AT_ANY_DISTANCE` | `0x362CEDD2A41E0747` (2) | `(Vehicle, BOOL)` | **CONFIRMED** signature | remote/stream; A/B |
| `SET_VEHICLE_DOORS_LOCKED` | `0x96F78A6A075D55D9` (2) | `(Vehicle, int doorLockStatus)` | **SIGNATURE-ONLY**; enum — **UNCERTAIN** | см. §4.2; dangerous |
| `SET_VEHICLE_INDIVIDUAL_DOORS_LOCKED` | `0xA9F1D75195CC40F6` (3) | `(Vehicle, int doorId, int doorLockStatus)` | **SIGNATURE-ONLY**; enum — UNCERTAIN | per-door setter; per-door readback ABSENT |
| `GET_VEHICLE_DOOR_LOCK_STATUS` | `0xC867FD144F2469D3` (1) | `(Vehicle)` int | **SIGNATURE-ONLY**; enum ABSENT | общий статус |
| `_GET_BREAKABLE_VEHICLE_LOCKS_STATE` | `0xE015CF1F2C0959D8` (1) | `(Vehicle)` int | **SIGNATURE-ONLY** | состояние breakable lock objects (coach), не door-lock enum |
| `_GET_NUM_BREAKABLE_VEHICLE_LOCK_OBJECTS` | `0x2FA86833E3617E2D` (1) | `(Vehicle)` int | **CONFIRMED** signature | coach/lock objects |
| `_GET_BREAKABLE_VEHICLE_LOCK_OBJECT` | `0x58F2244C1286D09A` (2) | `(Vehicle, int index)` Object | **CONFIRMED** signature | |
| `_BREAK_LOCKS_ON_VEHICLE` | `0x9D12796EF4BF9EA9` (1) | `(Vehicle)` | **CONFIRMED** signature/comment | one-way: pops/destroys lock objects, swings open doors/compartments; host real; cannot undo |
| `SET_BREAKABLE_VEHICLE_LOCKS_UNBREAKABLE` | `0xBC4735F48CD983EF` (2) | `(Vehicle, BOOL)` | **CONFIRMED** signature | prevent break |
| `TASK_BREAK_VEHICLE_DOOR_LOCK` | `0xBB28D1BC9EA8A6A5` (2) | `(Ped, Vehicle)` | **CONFIRMED** signature; effect UNCERTAIN | host real ped; hijack/coach robbery A/B |
| `SET_DOOR_ALLOWED_TO_BE_BROKEN_OFF` | `0x081FB9D6422F804C` (3) | `(Vehicle, int doorId, BOOL)` | **CONFIRMED** signature | must be breakable before break; A/B for wagon |
| `SET_VEHICLE_DOOR_BROKEN` | `0x9666CF20A1C6D780` (3) | `(Vehicle, int doorId, BOOL deleteDoor)` | **CONFIRMED** signature | one-way; deleteDoor A/B; `_IS_VEHICLE_DOOR_BROKEN` readback |
| `_IS_VEHICLE_DOOR_BROKEN` | `0xE979BB5602AD3402` (2) | `(Vehicle, int doorId)` BOOL | **CONFIRMED** signature | readback |
| `IS_VEHICLE_DOOR_FULLY_OPEN` | `0x7AE191143C7A9107` (2) | `(Vehicle, int doorId)` BOOL | **CONFIRMED** signature | only fully open |
| `GET_VEHICLE_DOOR_STATUS` | — | — | **ABSENT in 1491.50** | нет |
| `IS_VEHICLE_DOOR_CLOSED` | — | — | **ABSENT in 1491.50** | `IS_DOOR_CLOSED` — world door hash, не vehicle |

### 4.2. Lock enum — что подтверждено

- Публичного `eVehicleLockState`/enum в NativeDecl/latest DB **нет**; `SET_VEHICLE_DOORS_LOCKED` принимает `int doorLockStatus` без комментария.
- Поэтому **0/1/2 mapping НЕ подтверждён**; нельзя писать в U3 «0=unlocked,1=locked,2=...» без A/B.
- `GET_VEHICLE_DOOR_LOCK_STATUS` возвращает `int`, но диапазон/enum не документирован; до A/B host `kVehicleStateFlagLocked` должен брать из **host cache** последнего действия, а readback использовать как raw diagnostic.
- **A/B:** на тестовом wagon/coach:
  1. записать `GET_VEHICLE_DOOR_LOCK_STATUS` до;
  2. `SET_VEHICLE_DOORS_LOCKED(veh, v)` для v=0,1,2 (каждый раз restore/read);
  3. `SET_VEHICLE_INDIVIDUAL_DOORS_LOCKED(veh,d,v)` для doorId 0..7;
  4. `GET_VEHICLE_DOOR_LOCK_STATUS` + попытка `TASK_ENTER_VEHICLE`/локального enter.
  Записать mapping; если стабильного нет — lock actions остаются wire-ready, но в игре `Rejected`/лог до подтверждения.
- `_GET_BREAKABLE_VEHICLE_LOCKS_STATE` — **другой** объект: состояние breakable lock objects (coach boot), не door-lock enum; для `_BREAK_LOCKS_ON_VEHICLE` readback; не заменяет GVL.
- `_BREAK_LOCKS_ON_VEHICLE` — **one-way**; только если реально нужен взлом; после него двери/компартменты открыты, восстановления нет; host real; mission/player drive риск.
- `TASK_BREAK_VEHICLE_DOOR_LOCK` — 2 арг, требует Ped; возможно, только coach robbery/hijack; тест; не использовать для программного lock/unlock.

### 4.3. Door/window ID mapping (wagon/coach)

- `eDoorId` (CONFIRMED, DB comment): `-1 invalid`, `0 DSIDE_F`, `1 DSIDE_M`, `2 DSIDE_M1`, `3 DSIDE_M2`, `4 DSIDE_R`, `5 PSIDE_F`, `6 PSIDE_M`, `7 PSIDE_M1`, `8 PSIDE_M2`, `9 PSIDE_R`, `10 BONNET`, `11 BOOT`.
- P1 wire `doorId` 0..7 (side doors); bonnet/boot 10/11 не кодируются в 2-битном doorBits (P1); если нужен boot — отдельное расширение/wire.
- **Wagon/coach** фактически может иметь только часть doorId: DSIDE_F/PSIDE_F/BOOT и т.п.; `SET_VEHICLE_DOOR_OPEN(...,id,...)` на несуществующей двери может no-op; `IS_VEHICLE_DOOR_FULLY_OPEN`/`_IS_VEHICLE_DOOR_BROKEN` вернут false. A/B перебором 0..11 на конкретной модели: open/readback/false; не считать все 12 валидными.
- Window indices (CONFIRMED, DB comment): `0 Front Right`, `1 Front Left`, `2 Back Right`, `3 Back Left`; `REMOVE_VEHICLE_WINDOW 0x745F15A215F2DDF1(Vehicle,int)`; `IS_VEHICLE_WINDOW_INTACT 0x0E7910A63E05B12C(Vehicle,int)`; wagon может не иметь части окон — A/B; P1 windowBits нет (только future).
- `SET_VEHICLE_DOOR_OPEN` comment: «Can also be used on trains and its wagons» — CONFIRMED for open; `SET_VEHICLE_DOOR_SHUT/BROKEN` wagon-specific A/B.

### 4.4. Loot/hitch

| Native | Hash (arity) | Signature/return | Статус | Рецепт/риск |
|---|---|---|---|---|
| `_SET_FORCE_COACH_ROBBERY_LOOT` | `0xF489F94BFEE12BB0` (2) | `(Vehicle, Hash coachrobberyLoot)` | **SIGNATURE-ONLY** | DB comment hashes: `COACH2_BOOT_LOOT_ITEMS_COACHROB_RSC`, `COACH2_BOOT_LOOT_ITEMS_COACHROB`, `COACH2_MARY3`; numeric hashes надо получить joaat/скриптами и A/B |
| `TASK_LOOT_ENTITY` | `0x48FAE038401A2888` (2) | `(Ped, Entity)` | **CONFIRMED** signature | host real; vehicle cargo/loot table depends |
| `TASK_PICKUP_CARRIABLE_ENTITY` | `0x502EC17B1BED4BFA` (2) | `(Ped, Entity)` | **CONFIRMED** signature | host real |
| `GET_LOOTING_PICKUP_TARGET_ENTITY` | `0x14169FA823679E41` (1) | `(Ped)` Entity | **CONFIRMED** signature | readback host-side looting target |
| `_GET_LOOTING_PICKUP_TARGET_ENTITY` (alt name) | тот же `0x14169FA823679E41` (1) | `(Ped)` Entity | **CONFIRMED** | в latest DB имя без `_` |
| `_REQUEST_ENTITY_LOOT_LIST` | `0xA88E215CEB0435C0` (6) | `(Ped mount, Any* argStruct, Hash visiblelootslotrequestType, int flag, int p4, BOOL p5)` BOOL | **SIGNATURE-ONLY** | «mount» параметр; A/B |
| `_LOOT_TABLES_GET_INFO` | `0x48E4D50F87A96AA5` (6) | `(Ped, BOOL p1, BOOL p2, Hash lootTableKey, Any* p4, Any p5)` void | **SIGNATURE-ONLY** | A/B |
| `_REFRESH_LOOT_STATE_FOR_PED` | `0x5463C962BC7777C3` (5) | `(Ped, int p1, Ped* lootTarget, int p3, int p4)` int | **CONFIRMED** signature; return enum `eLootState` (LAP_NONE..LAP_EXITING) | readback state |
| `_GET_RANSACK_SCENARIO_CONTAINER_REMAINING_LOOT_COUNT` | `0x01AF8A3729231A43` (1) | `(Entity)` int | **CONFIRMED** signature/comment | container count |
| `_GET_LOOTING_EVENT_HAS_FIRED` | `0xF9B91C5129EABC08` (2) | `(Ped, const char* eventName)` BOOL | **SIGNATURE-ONLY** | event names MGBegin/MGEnd/ReadyForCut |

- **Generic vehicle inventory / loot-from-vehicle ABSENT**: нет натива «взять предмет из vehicle inventory»; только coach robbery loot hash/loot tables + `TASK_LOOT_ENTITY`/carriable. P1 P1.1 loot — host log only, не обещать инвентарь.
- Hitch: `_HITCH_HORSE 0x06AADE17334F7A40(4: horse,x,y,z)` — **world hitch**, не wagon; `_UNHITCH_HORSE 0x0348469DAA17576C(1)`; wagon harness — draft natives §2.8. `TASK_HITCH_ANIMAL 0x9030AD4B6207BFE8(3: ped,scenarioPoint,flag)` — SIGNATURE-ONLY, AI/scenario.
- Draft harness poll for hitch capture: `GET_DRAFT_ANIMAL_COUNT`, `_GET_PED_IN_DRAFT_HARNESS`, `_GET_NUM_DRAFT_VEHICLE_HARNESS_PED`, `_GET_VEHICLE_DRAFT_HORSE_IS_ATTACHED_TO`.

---

## 5. P2 (P3/P4, не блокирует U3/U4)

| Native | Hash (arity) | Signature | Статус | Заметка |
|---|---|---|---|---|
| `_SET_DRAFT_VEHICLE_DESIRED_SPEED` | `0x0C3F0F7F92CA847C` (2) | `(Vehicle, float)` | **SIGNATURE-ONLY** | возможный путь тянуть wagon/лошадей; A/B |
| `_GET_DRAFT_VEHICLE_DESIRED_SPEED` | `0xC6D7DDC843176701` (1) | `(Vehicle)` float | **CONFIRMED** | readback |
| `SET_VEHICLE_FORWARD_SPEED` | `0xF9F92AF49F12F6E7` (2) | `(Vehicle, float)` | **SIGNATURE-ONLY** | host real control; wheel spin uncertain |
| `SET_VEHICLE_STEER_BIAS` | `0x84DAAE11E9EE4FC3` (2) | `(Vehicle, float)` | **CONFIRMED** comment | -1..1, call each tick |
| `SET_VEHICLE_HANDBRAKE` | `0x91BE51AEC4E99710` (2) | `(Vehicle, BOOL)` | **CONFIRMED** signature | |
| `SET_VEHICLE_ON_GROUND_PROPERLY` | `0x7263332501E07F52` (2) | `(Vehicle, BOOL p1)` BOOL | **CONFIRMED** signature | p1 likely true |
| `TASK_VEHICLE_TEMP_ACTION` | `0xC429DCEEB339E129` (4) | `(Ped driver, Vehicle, int action, int time)` | **CONFIRMED** signature/comment | actions 1/3/9/23/27; 16/17/18 опасны; driver required |
| `TASK_VEHICLE_DRIVE_TO_COORD` | `0xE2A2AA2F659D77A7` (11) | `(Ped,Vehicle,x,y,z,speed,style,Hash model,mode,stopRange,straightLineDist)` | **CONFIRMED** signature/comment | driver required |
| `TASK_VEHICLE_DRIVE_STRAIGHT_TO_POINT` | `0x089FF2FB965F0A29` (8) | `(Ped,Vehicle,x,y,z,p5,p6,flag)` | **CONFIRMED** signature/comment | driver required |
| `TASK_VEHICLE_DRIVE_TO_DESTINATION` | `0x7F241A0D14354583` (11) | `(Ped,Vehicle,x,y,z,speed,flags,p7,stop1,stop2,BOOL p10)` | **CONFIRMED** signature/comment | driver required |
| `TASK_VEHICLE_GOTO_NAVMESH` | `0x195AEEB13CEFE2EE` (8) | `(Ped,Vehicle,x,y,z,speed,behaviorFlag,stoppingRange)` | **CONFIRMED** signature | driver required |
| `TASK_VEHICLE_MISSION` | `0x659427E0EF36BCDE` (9) | `(Ped,Vehicle,targetVeh,missionType,p4,p5,p6,p7,driveAgainstTraffic)` | **CONFIRMED** signature | semantics uncertain |
| `TASK_VEHICLE_MISSION_PED_TARGET` | `0x9454528DF15D657A` (9) | `(Ped,Vehicle,Ped target,mode,maxSpeed,style,minDist,p7,driveAgainstTraffic)` | **CONFIRMED** signature | |
| `SET_VEHICLE_UNDRIVEABLE` | `0x6E884BAB713A2A94` (2) | `(Vehicle, BOOL)` | **SIGNATURE-ONLY** | P1 control gate; dangerous |
| `IS_VEHICLE_DRIVEABLE` | `0xB86D29B10F627379` (3) | `(Vehicle, BOOL p1, BOOL p2)` BOOL | **SIGNATURE-ONLY** | p1/p2 unknown; readback A/B |

- **Wheel spin native ABSENT** (no `SET_VEHICLE_WHEEL_ROTATION`); P2 idea: `SET_VEHICLE_FORWARD_SPEED` on dynamic proxy / host real; visual result A/B.
- **Ghost seat visibility:** публичного «networked seat visibility» натива нет; host-side guest ghost — `SET_PED_INTO_VEHICLE`/`CREATE_PED_INSIDE_VEHICLE` + `SET_ENTITY_VISIBLE`; mission/no-collision flags; local guest player видит свой proxy, host — ghost. Нужна архитектурная проверка (P2/P4), не U3.

---

## 6. U3 host apply / U4 readback 0x26 — рекомендуемый порядок

### U3 host apply (после валидации intent)

1. `damage`: mission/protected gate → `SET_VEHICLE_DAMAGE(veh,off,damage,radius,false)` → wait 1 tick → read body/engine; fallback `SET_VEHICLE_BODY_HEALTH`/`ENGINE_HEALTH`; result + flags.
2. `door open/close`: `SET_VEHICLE_DOOR_OPEN(veh,door,false,true)` / `SET_VEHICLE_DOOR_SHUT(veh,door,true)`; readback fully open; `door broken`: `SET_DOOR_ALLOWED_TO_BE_BROKEN_OFF(veh,door,true)` → `SET_VEHICLE_DOOR_BROKEN(veh,door,false/true)`; readback.
3. `lock/unlock`: **до A/B enum** — только host cache/wire, иначе `Rejected`/`Locked`; после A/B — `SET_VEHICLE_DOORS_LOCKED`/individual + readback GVL status; guest never direct.
4. `wheel break`: `_BREAK_OFF_VEHICLE_WHEEL(veh,wheel)` (A/B mapping) / draft `_BREAK_OFF_DRAFT_WHEEL`; readback `_IS...`.
5. `enter/exit/hijack`: `SET_PED_INTO_VEHICLE(ghost/partner,veh,seat)`; exit `TASK_LEAVE_VEHICLE(ped,veh,0,0)`; hijack `TASK_LEAVE_VEHICLE(driver,veh,0,0)` → `SET_PED_INTO_VEHICLE(ghost,veh,-1)`; readback seat.
6. `draft hitch`: `_ATTACH_DRAFT_VEHICLE_HARNESS_PED(horse,veh,harness)`; check return + `GET_DRAFT_ANIMAL_COUNT`; detach inverse.
7. Обновить host cache, `VehicleStatePayload`, result; 0x26 только если changed/dirty/heartbeat.

### U4 0x26 readback formula (host)

```cpp
VehicleStatePayload BuildVehicleState(unsigned int veh) {
    VehicleStatePayload s{};
    const bool valid = veh && DoesEntityExist(veh) && IsEntityAVehicle(veh);
    if (!valid) return s; // flags=0, seat=127

    const float body = GetVehicleBodyHealth(veh);
    const float eng  = GetVehicleEngineHealth(veh);
    if (std::isfinite(body) && std::isfinite(eng)) {
        s.bodyHealth = (uint16_t)std::lround(std::clamp(body, 0.f, 1000.f));
        s.engineHealth = (int16_t)std::lround(std::clamp(eng, -4000.f, 1000.f));
        s.flags |= kVehicleStateFlagDamageValid;
    }

    s.flags |= kVehicleStateFlagDoorValid;
    for (int d = 0; d < 8; ++d) {
        const bool broken = IsVehicleDoorBroken(veh, d);
        const bool open = !broken && IsVehicleDoorFullyOpen(veh, d);
        s.doorBits |= (uint16_t)((broken ? 2 : (open ? 1 : 0)) << (2 * d));
    }

    for (int w = 0; w < 4; ++w) {
        const bool destroyed = IsVehicleWheelDestroyed(veh, w);
        s.wheelsBits |= (uint8_t)((destroyed ? 2 : 0) << (2 * w));
    }
    // damaged/burst (1) readback ABSENT; 3 unknown if mapping/readback inconsistent
    if (AreAnyVehicleWheelsDestroyed(veh) && s.wheelsBits == 0)
        s.wheelsBits = 0xFF;

    if (!GetEntityCanBeDamaged(veh) || IsEntityAMissionEntity(veh))
        s.flags |= kVehicleStateFlagMissionProtected;
    if (IsVehicleWrecked(veh) || IsEntityDead(veh))
        s.flags |= kVehicleStateFlagDead;

    const int lock = GetVehicleDoorLockStatus(veh); // SIGNATURE-ONLY enum
    if (lock != 0) s.flags |= kVehicleStateFlagLocked; // A/B enum; else host cache
    // hostSeat/guestSeat: см. ниже
    return s;
}
```

**hostSeat/guestSeat readback recipe:**
```cpp
int FindSeatForPed(unsigned int veh, unsigned int ped) {
    if (!veh || !ped || GetVehiclePedIsIn(ped, false) != veh) return 127;
    const int maxPass = GetVehicleMaxNumberOfPassengers(veh);
    for (int s = -1; s < maxPass; ++s) {
        if (GetPedInVehicleSeat(veh, s) == ped) return s; // -1=driver,0..=pass
    }
    return 127;
}
```
- `GET_VEHICLE_PED_IS_IN(ped,false)` — текущая; если entering, может быть 0; `GET_VEHICLE_PED_IS_ENTERING(ped)` для entering; readback valid only when in.
- hostSeat for host player; guestSeat for partner ghost handle; both 127 if absent.
- `kVehicleStateFlagHostSeatValid/GuestSeatValid` set if ped in vehicle and seat found; `Hijacked` — host runtime cache.

**Петроль:** `GET_VEHICLE_PETROL_TANK_HEALTH` — readback для диагностики/логов, **не поле** `VehicleStatePayload`; не расширять 24 B.

**Двери/health после apply:** читать на следующем тике (или через 1–2 тика), иначе `SET_VEHICLE_DOOR_OPEN(openInstantly=false)`/damage health могут не успеть; для детерминизма `openInstantly/closeInstantly=true`, `p6=false`.

---

## 7. Риски / A-B матрица

| Риск | Нативы | Статус | Что делать |
|---|---|---|---|
| Health readback не int, а float; invalid engine=1000 | GET_BODY/ENGINE/PETROL | CONFIRMED signature, range comment | gate `DOES_ENTITY_EXIST`/`IS_ENTITY_A_VEHICLE`; clamp/round; не писать unknown как 0 |
| Body/engine health setter clamp неизвестен | SET_BODY/ENGINE/PETROL | SIGNATURE-ONLY | A/B 0..1000/-4000..1000; после damage readback; не уходить в fire случайно |
| `SET_VEHICLE_DAMAGE` p6/offset/clamp | SET_VEHICLE_DAMAGE | CONFIRMED sig, p6 UNCERTAIN | A/B p6 true/false, offsets body/door/wheel; fallback health writes |
| Mission/invincible/strong блокируют damage | SET_ENTITY_AS_MISSION_ENTITY/INVINCIBLE/STRONG/CAN_BREAK/VISIBLY_DAMAGED/_GET_ENTITY_CAN_BE_DAMAGED | signatures CONFIRMED, behavior partly UNCERTAIN | A/B 4×; minimal host set; temporary unmark/invincible with restore |
| Door lock enum неизвестен | SET_VEHICLE_DOORS_LOCKED/INDIVIDUAL; GET_VEHICLE_DOOR_LOCK_STATUS | SIGNATURE-ONLY | A/B 0/1/2 each; до этого `Locked` из cache; lock actions gated |
| Нет per-door status/closed/ajar | IS_VEHICLE_DOOR_FULLY_OPEN/_IS_VEHICLE_DOOR_BROKEN | CONFIRMED sig; ABSENT closed/status | doorBits 0/1/2; 3 unknown; не заявлять closed; global Locked flag |
| Wheel mapping/health отсутствует | _IS_VEHICLE_WHEEL_DESTROYED/_ARE_ANY/_BREAK_OFF | CONFIRMED sig; mapping UNCERTAIN; damaged/burst ABSENT | A/B wheelIndex 0..5; wheelBits only 0/2/3; 1 из host cache |
| `deleteDoor=true` необратим | SET_VEHICLE_DOOR_BROKEN | CONFIRMED sig; readback UNCERTAIN | A/B; не в v1 без нужды |
| `SET_VEHICLE_UNDRIVEABLE` persistent, нет getter | SET_VEHICLE_UNDRIVEABLE/IS_VEHICLE_DRIVEABLE | SIGNATURE-ONLY | только proxy; restore false on exit/reset; A/B IS_DRIVEABLE p1/p2 |
| `SET_VEHICLE_DOORS_LOCKED` wrong enum lock | SET_VEHICLE_DOORS_LOCKED | SIGNATURE-ONLY | тестовый vehicle; restore 0; не на mission/real до A/B |
| `_BREAK_LOCKS_ON_VEHICLE` one-way | _BREAK_LOCKS_ON_VEHICLE | CONFIRMED signature/comment | только coach robbery/hijack; нет undo; A/B |
| keepPhysics semantics | SET_ENTITY_COLLISION | CONFIRMED sig; UNCERTAIN | A/B toggle/keepPhysics на proxy; не на host real |
| Non-local SET_PED_INTO_VEHICLE | SET_PED_INTO_VEHICLE | CONFIRMED sig; behavior UNCERTAIN | host ghost/NPC A/B; mission/invincible A/B; readback seat |
| TASK_LEAVE arity 4, не 3 | TASK_LEAVE_VEHICLE | CONFIRMED sig | P1 spec fix; unkPed=0 |
| P1 control myth | IS_CONTROL_PRESSED/… | PRESENT/CONFIRMED | обновить P1 §6/R4; control type 0; action hashes §3 |
| Loot generic inventory | TASK_LOOT/COACH | CONFIRMED sig; generic ABSENT | P1.1 host log only; coach loot hashes A/B |

---

## 8. Итог для Lead / U3-U4

- **P0 readback готов:** body/engine/petrol health, door fully/broken, wheel destroyed/any, seat/occupancy, driver, wrecked/entity flags — все hashes/arity подтверждены; **door lock enum, wheel index mapping и damaged/burst readback — SIGNATURE-ONLY/ABSENT**, их нельзя выдумывать.
- **P0 apply:** damage/health/door/wheel/seat/draft signatures подтверждены; p6/offset/clamp/mission/invincible — A/B.
- **P1 control:** `IS_CONTROL_*` **есть**, P1 §6/R4 надо исправить; P1.1 может читать ENTER/EXIT/ATTACK/steer и suppress controls через `DISABLE_CONTROL_ACTION`, не через `DISABLE_ALL_CONTROL_ACTIONS`.
- **Dangerous:** `SET_VEHICLE_UNDRIVEABLE`, `SET_VEHICLE_DOORS_LOCKED`, `_BREAK_LOCKS_ON_VEHICLE`, `SET_VEHICLE_DOOR_BROKEN`, `_BREAK_OFF_VEHICLE_WHEEL` — только host/proxy по политике, restore/one-way риски.
- **U4 0x26 formula** — см. §6; petrol не поле; doorBits 3 не использовать как locked; wheelBits 1 readback-ом не получить.

**Файлы/источники:** `docs/INTERACTIVITY_P1.md` §2.4/§4.3/§10; `docs/NATIVE_RESEARCH_INTERACT.md`; `src/natives.h` (текущие хэши); `third_party/rdr2-runtime-scripthook/.../NativeDecl.cs`; latest `alloc8or/rdr3-nativedb-data`; `third_party/rdr3-discoveries/Controls/README.md`; Halen84 `eEnterExitVehicleFlags`/`eDoorId`.
