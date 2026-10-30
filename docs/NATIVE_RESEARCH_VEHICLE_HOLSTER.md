# Разведка нативов: повозка (draft-транспорт) + кобура (оружие в слоте)

Источник сигнатур — `research/rdr3-nativedb-data/natives.json` (build 1207 =
1.0.1491.50, минуя build 1232+). Все хеши ниже НЕ в `src/natives.h` — добавить
перед реализацией и проверить в игре (часть может быть nullsub/нужно p-флаги).

## Повозка (draft-транспорт) — ключ к «нативной» репликации

Проблема текущего подхода (`world_sync.cpp`): прокси-vehicle = `CREATE_VEHICLE`
+ `SetNoCollision(true)` + ручная интерполяция `SET_ENTITY_COORDS` → скользит,
колёса не крутятся, лошади/кучер «приклеены».

Нативные рычаги, которые это чинят:

| Хеш | Натив | Применение |
|---|---|---|
| 0x214651FB1DFEBA89 | `_CREATE_DRAFT_VEHICLE(model,x,y,z,heading,isNetwork,bScriptHostVeh,bDontAutoCreateDraftAnimals,draftAnimalPopGroup,p9)` | замена CREATE_VEHICLE: задаёт popGroup тягловых (DRAFT_HORSES_*) |
| 0x316CDB5B6E8F4110 | `_ATTACH_DRAFT_VEHICLE_HARNESS_PED(mount, draft, harnessId)` | **нативная прицепка** лошади в упряжь (вместо «приклеивания») |
| 0xB36D3EC70963BE60 | `_DETACH_DRAFT_VEHICLE_HARNESS_PED(draft, ped)` | отцепка |
| 0xA8BA0BAE0173457B | `_GET_PED_IN_DRAFT_HARNESS(vehicle, harnessId)` | кто из педов в упряжи (LR/RR/LM/RM/LF/RF) |
| 0x0C3F0F7F92CA847C | `_SET_DRAFT_VEHICLE_DESIRED_SPEED(vehicle, speed)` | **скорость повозки** — нативное движение, колёса крутятся |
| 0xC6D7DDC843176701 | `_GET_DRAFT_VEHICLE_DESIRED_SPEED(vehicle) -> float` | чтение текущей скорости |
| 0xF9F92AF49F12F6E7 | `SET_VEHICLE_FORWARD_SPEED(vehicle, speed)` | запасной способ задания движения |
| 0xE2A2AA2F659D77A7 | `TASK_VEHICLE_DRIVE_TO_COORD(ped,vehicle,x,y,z,speed,style,model,drivingMode,stopRange,straightLineDist)` | **кучер ведёт повозку** к точке нативно |
| 0x195AEEB13CEFE2EE | `TASK_VEHICLE_GOTO_NAVMESH(ped,vehicle,x,y,z,speed,behaviorFlag,stoppingRange)` | вождение по навмешу |
| 0x2963B5C1637E8A27 | `GET_DRIVER_OF_VEHICLE(vehicle) -> Ped` | кто за рулём/вожжами |

### Минимально-инвазивный план «Подход B+» (гибрид, без протокола)

1. В `world_sync.cpp` заменить `SET_ENTITY_COORDS`-интерполяцию на
   `_SET_DRAFT_VEHICLE_DESIRED_SPEED(+heading)` — повозка поедет нативно,
   колёса/качание включатся.
2. Draft-лошадей прицеплять `_ATTACH_DRAFT_VEHICLE_HARNESS_PED` (нативно),
   а не «вручную педами» (сейчас `actor_sync` держит их отдельными прокси).
3. Кучера вести `TASK_VEHICLE_GOTO_NAVMESH` (нативно управляет повозкой),
   вместо ручного `SET_ENTITY_COORDS` педа.

## Кобура (оружие-в-кобуре)

Проблема: хост шлёт `weaponHash=0`, когда оружие убрано (в кобуре), потому
что `_GET_PED_CURRENT_HELD_WEAPON` возвращает только «в руках». Призрак
создаётся с пустой кобурой.

Решение — читать оружие по СЛОТУ инвентаря (не только held):

| Хеш | Натив | Применение |
|---|---|---|
| 0xDBC4B552B2AE9A83 | `_GET_PED_WEAPON_IN_SLOT(ped, slotHash) -> Hash` | оружие в слоте (кобура — слот SLOT_*) |
| 0x46F032B8DDF46CDE | `_GET_WEAPONTYPE_SLOT(weaponHash) -> Hash` | слот оружия (WEAPON_* → SLOT_*) |
| 0xBDD9C235D8D1052E | `_IS_PED_CURRENT_WEAPON_HOLSTERED(ped) -> BOOL` | убрано ли (для флага) |

### Минимальный план кобуры

1. На хосте: для выбранного оружия получить `slot = _GET_WEAPONTYPE_SLOT(held)`,
   затем `_GET_PED_WEAPON_IN_SLOT(ped, slot)` — даже когда убрано (holstered).
2. Передать это «выбранное» оружие (отдельное поле/fлаг кадра, т.к. это не
   `weaponHash`=held). Либо переиспользовать `reserved` (есть свободные 8 байт?
   проверить) — но, возможно, протокол v23 (жёсткий bump).
3. Получатель: `GIVE_WEAPON_TO_PED` + оставить в кобуре (`_HIDE_PED_WEAPONS`
   or bForceInHolster=1), чтобы кобура призрака была наполнена.

Кобура — косметика (не влияет на стрельбу, которую мы уже починили
`equipNow`-фиксом). Приоритет ниже, чем повозка.

## Статус

- [x] Разведка нативов (повозка + кобура) — завершена, задокументирована.
- [ ] Повозка: внедрение «Подход B+» (нативная тяга/движение).
- [ ] Кобура: внедрение слота оружия (косметика, отдельно).
