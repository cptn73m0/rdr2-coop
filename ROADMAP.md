# Дорожная карта — RDR2 CO-OP

Кооператив для одиночного сюжета RDR2. Дорожная карта показывает этапы,
порядок работ и текущий прогресс. Отмечаем галочками по мере выполнения.

**Актуально на:** 27.10.2026
**Целевой билд:** RDR2 1.0.1491.50, ScriptHookRDR2 1.0.1491.17
**Текущий протокол:** **v19** (actor-репликация NPC E1–E19,
0x18–0x22: ActorDamage 0x1F + PlayerPresence 0x20 + VehicleOccupants 0x21 +
TrainSnapshotV2 0x22; e18/e19 не меняют провод). v18↔v19 несовместимы; откат v18 —
ASI `C9BFA565`/server `BEC3428B`, v17 — `35163CC2`/`B7AAABAE`, v16 —
`63388AD3`/`4EDAE4B6`, v15 — `dist\npc-e2c-v15\`.
Рабочая база/игровой стабильный тег — **stable-v14** (пед-призрак, лошадь,
одежда shop+wearable, выстрелы, время/погода). NPC-репликация реализована:
E1 human-only, E2a meta-outfit, E2b всадники, E2c-a/b внешность, E3a
сценарии/смерть, E3b оружие/бой, E3c животные/лошади, E4 перф-инструментация,
E5 урон/E5.1 хедшоты/E5.2-E5.3 трупы/mounted, E6 presence, E7.1 visitor,
E7.2 плавное движение, E7.3 E3d-a/migration, E8 — E3d-c occupants/draft +
E3d-b mission-train, E9 — animal/horse appearance, E10 — mounted NPC-horse
appearance, E11 — точные пассажиры поездов, E12 — `perf: frame`/`perf: net`,
Player Frame throttle/P0/P1, **E13 — wall-clock нормализация (FPS-independent),
`tick_hz`, config warnings**, **E14–E17 — ub-mode refactor-only** (net/server/
src: ammo helpers, host-netId dedupe, Tick/ApplyOutfit/ApplyRemoteFrame фазы,
drain-inbox/combat-exit; протокол и поведение не менялись), **E18 — soft host
migration** (game-timeout демотирует хоста без закрытия сокета, join grace
15 с, presence-liveness; клиентский loading heartbeat; vendor dedupe у гостя;
server `HandleClient` phases; integration run lock), **E19 — blip name, кучер/
тягловые лошади повозки (occupant-first + полный allowlist), opt-in wheel
A/B `npc_vehicle_wheels=0`, SET_ENTITY_COORDS арность 8**.
CI **42/42** + 7 C++; HEAD `ef49ac8`; тег **npc-e19**; теги `npc-e0`…`npc-e19`.
`npc_actor=0` по умолчанию; локальная пара v19 — ASI `DD0EE210`/server
`DD9BD9D8`, снапшот `dist\npc-e19`, **оба ПК задеплоены** (local + ARINA,
хеши сверены), canonical CI 42/42 + quality strict PASS. Двух-ПК приёмка e19
pending — **ждём полный тест e19** (low/info: blip label, плотный трафик,
wheel A/B).

---

## Фаза 0 — Каркас и окружение — готово

- [x] Структура проекта `E:\rdr2-coop` (CMake, C++ ASI-плагин)
- [x] Резолв экспортов `ScriptHookRDR2.dll` без проприетарного SDK
- [x] Сборка `.asi`, совместимость с билдом 1.0.1491.50
- [x] Развёртывание в папку игры + базовый лог
- [x] Резолв `worldGetAll*`, `getGlobalPtr`, `getStaticPtr` (Spike 1)

## Фаза 1 — Локальный «призрак» — готово

- [x] Создание второй сущности в мире и чтение/запись трансформа
- [x] Рендер педа (mission + variation + visible)
- [x] Метка на карте/мини-карте, имя над головой (500 м)
- [x] Смерть/возрождение, блип/имя скрываются

## Фаза 2 — Персистентность и сессия — готово

- [x] Последняя позиция игрока на сервере (`players.txt`)
- [x] Спавн повторного игрока на сохранённой позиции
- [x] Обработка дисконнекта: удаление педа и метки
- [x] Явные роли HOST/GUEST, лимит 2 игрока, `JoinRejected`
- [x] Версия протокола в JOIN (v19), отказ при несовпадении; откат v18
      (`C9BFA565`/`BEC3428B`), v17 (`35163CC2`/`B7AAABAE`), v16 —
      `63388AD3`/`4EDAE4B6`, v15 — `dist\npc-e2c-v15\`

## Фаза 3 — Сеть — готово (база)

- [x] Протокол команд, бинарные кадры, little-endian
- [x] Кадр состояния 88 Б, отправка каждый тик (~60 Гц)
- [x] TCP_NODELAY, неблокирующие сокеты, own send queue
- [x] Ping/Pong, RTT/джиттер, HUD
- [x] WorldState (время/погода от хоста)
- [ ] UDP-канал для высокочастотных снапшотов (отложено)

## Фаза 4 — Игрок: движение и внешность — готово

- [x] Move/State/Action/aim/weapon в кадре
- [x] Движение задачей по навмешу, интерполяция, спецсостояния
- [x] Ragdoll/укрытие/плавание/лезть/прыжок/падение (грубо)
- [x] Действия: сценарий/сидение/пьян (грубо), прицел/перезарядка/выстрел
- [x] Оружие: модель, доставание, прицел, выстрелы, перезарядка
- [x] События выстрелов: аммо-детект + очередь 64 + независимый таймер
- [x] Лошадь: модель, посадка/спешивание, движение, коллизии, снаряжение
- [x] одежда: shop-компоненты, 96 категорий, протокол v14
- [x] Wearable states одежды (открытая/закрытая куртка, рукава и т.п.):
      `OutfitState` 784 B (+`wearable[96]`), протокол v14 — реализовано
- [ ] Игровая проверка wearable-синхронизации на двух ПК
- [x] Покой без «танцев»: гистерезис 1.5/2.5 м, порог скорости 0.35 м/с
- [x] Труп и снаряжение остаются, `RESURRECT_PED`
- [ ] Верховая стрельба/сложные mounted-анимации

## Фаза 5 — P1: репликация мира/NPC

### NPC-репликация — E1–E19 реализованы; 2-ПК/визуал pending
- [x] E-1: probe-режим `npc_debug=1` + анализатор probe-логов (22% NPC имеют
      metaOutfit hash; сценарии/модели/лошади/оружие)
- [x] E0: протокол actor-сообщения 0x18–0x1E, `kMaxActors=12`,
      static_assert размеров
- [x] E0: серверный host-only relay + валидация (exact size, count≤12,
      epoch/actorId!=0, finite coords, duplicate actorId), resync guest→host
- [x] E0: integration-тесты v19 (host-only, размеры/лимиты, epoch/actorId,
      NaN/Inf, resync-направление, gating `actor:`=0, appearance 16comp/460B,
      ActorDamage 16B, PlayerPresence 8B, VehicleOccupants 8+8*count≤72B,
      TrainSnapshotV2 48B, host game-timeout 4 c/Ping-only,
      rate-limit 0x21/0x22); **41/41** на build-server-ci; C++ 7/7
- [x] E1: human-only discovery 10 Гц вокруг обоих игроков, cap 12, roster
      actorId/epoch, spawn heartbeat 2.5 с, frames 10 Гц, despawn
      gone/dead/evicted, resync 0x1D; guest-прокси spawn≤2/тик + model load,
      trail-навигация/in-place task update, gait с хоста — **подтверждено в игре**
- [x] E2a: meta-outfit внешность NPC (probe hash; 5/5 applied, failed=0),
      heartbeat-пересылка и guest reapply (`npc_appearance=1`)
- [x] E2b: всадники rider+horse (`ActorSpawn.reserved` = mount model), seat
      watchdog, dismount/despawn cleanup; подтверждено mount spawn ×9, failed=0;
      плавное движение gait_restart=0, err_p95≈0
- [x] E3a: сценарии (optimistic confirm, анти-наложение skip 8 м + warp revert
      3 м — визуально «двое курят на разных местах»), death/revive
      (SET_ENTITY_HEALTH 0 / RESURRECT_PED, труп остаётся)
- [x] E3b: оружие (валидные hash через GIVE_WEAPON_TO_PED), боевая стойка,
      aim, серии выстрелов (TASK_SHOOT_AT_COORD + переприцеливание),
      reload с отключением infinite clip; state=shoot=1450, reload=385,
      weapon fail=0
- [x] E2c-a: host outfit brute-force resolver (`metaped_outfits.lua` →
      `gen_metaped_outfits.h`, 890 моделей / 4568 кандидатов,
      `_IS_META_PED_OUTFIT_EQUIPPED`, бюджет 24 натива/тик, heartbeat) —
      механически работает; для random ambient NPC resolved=0/failed=11
- [x] **E2c-b:** protocol v16 **ActorAppearance (0x1E)**: header 12B + entry
      28B (count 1..16, max 460B), host-only relay, точный размер; host
      capture (`_GET_NUM_COMPONENTS_IN_PED`/`GET_META_PED_ASSET_GUIDS/TINT`,
      fingerprint, budget 1 актор/тик период 2с) + guest apply
      (reset+`_SET_META_PED_TAG`+commit+`UPDATE_PED_VARIATION`, readback,
      fallback; asset > metaOutfit). Валидация: host `capture count=8-10
      valid=8-10`, fake guest `appearance_msgs=504 appearance_actors=8
      entry_max=10 errors=0`; визуал — 2-ПК pending
- [x] **E3c:** животные (`kind=1`, HP 100) и свободные лошади (`kind=2`,
      HP 100) — discovery, движение, трупы/лошадиные трупы до despawn;
      подтверждено fake host/guest (`kinds`, `corpses`), fake tool animals/horses
- [x] **E4:** perf-инструментация (`perf: npc host/guest`, tick/discover/
      capture/build/spawn/appearance/motion/weapons) + `tools/perf_report.py`
      и `docs/PERF.md`; пороги Tick p95 ≤2 мс, components ≤1.5 мс
- [x] **E5:** protocol v17 **ActorDamage (0x1F)** — host-authoritative урон:
      гость детектит free-aim target, урон по классу оружия (35/60/80/30),
      лимиты ≤10/с и 100 мс/actor; хост применяет APPLY_DAMAGE_TO_PED
      (fallback SET_ENTITY_HEALTH), ≤8/тик, ≤15/с, ≤150 м; dead flag/труп;
      animals/horses тем же путём. Подтверждено в игре (выстрелы убивают)
- [x] **E5.1:** хедшоты — `SKEL_Head` + impact ≤0.30 м (fallback луч камеры),
      `damage=250` → ваншот human 200/animal 100; `headshot=1`; пользователь
      подтвердил «хэдшот ваншотит»; тег **npc-e5** (4395095)
- [x] **E5.2:** труп держится, пока host-труп существует;
      `npc_corpse_max_sec=300` (0=старое); логи `actor: corpse hold/despawn`
- [x] **E5.3:** сглаживание всадников (retarget 1.75 м, lead 0.28, stuck
      fallback); лог `actor: motion mounted err_p95/retargets`
- [x] **E6 (v18):** `PlayerPresence` 0x20 — 8 B, bidirectional без эха,
      rate-limit 10/с, unknown bits drop; flags map/pause/satchel/shop/
      cutscene/menu_other; суффикс `Name - in map`/`pause menu`/`in satchel`/
      `in shop`, timeout 12 с; `npc_presence=1`; debug `ui: pause=...`;
      integration **41/41**
- [x] **E7.1 visitor mode:** гость подавляет ambient human/ped/animal density,
      cleanup ~10 с (≤8/тик, фейд); транспорт при
      `npc_visitor_vehicles=1` (default 1 после E3d-a); preserve
      scenario/vendor/quest/event (`npc_visitor_preserve_events=1`, whitelist
      23 моделей); road population on/off парно; логи `actor: visitor mode
      on/off`, `visitor cleanup removed=`
- [x] **E7.2:** плавное движение 60 Гц — far goal (lead speed*0.7 clamp 4–12;
      кони 5–20), одна задача на фазу, in-place update
      `_UPDATE_TASK_GO_TO_COORD_WITH_OFFSET` 0x3FFCD7BBA074CC80, MBR
      `_COMPUTE_SPEED_FOR_PED_MOVE_BLEND_RATIO` 0xCA95924C893A0C91, status p2
      A/B, stop/start гистерезис, teleport >25 м/|dz|>15/нет кадров >2 с +
      `actor: teleport`; `npc_motion_update=1`, `npc_motion_marker=0`,
      `npc_motion_status_p2=1`; диагностика `actor: motion id=… reissue=…/s
      snap=…/s target_err_p95=… status=…`; приёмка **reissue <0.05/s, snap=0**
- [x] **E3d-a (v18):** vehicles-only `WorldSnapshot` при `world_sync=0 &&
      npc_actor=1 && world_vehicles=1`; `npc_vehicle_radius=80` (20..200),
      ≤16 машин вокруг обоих игроков, пустой snapshot каждые 6 тиков удаляет
      прокси; guest `CREATE_VEHICLE` mission/invincible/no-collision,
      dedupe `engineNetId`, timeout 3 с; `npc_visitor_vehicles=1` подавляет
      локальный трафик гостя; логи `world: vehicles send n=`, `world: vehicle spawn/despawn`
- [x] **Host migration hardening (v18) + soft e18:** серверный host
      game-timeout 4 с (4-й argv 2..15 с; idle 30 с отдельно), liveness только
      по game-thread сообщениям (Ping/Pong не считаются). e18: по таймауту —
      **мягкая миграция** (демоция хоста + promote второго + HostChanged,
      **сокет старого хоста не закрывается**, `PlayerLeft` нет, Ping/Pong жив);
      **join grace 15 с** до первого game-сообщения хоста; presence =
      liveness; клиент role-change reset (world_sync/visitor/presence/
      actor/боевые/remote vehicle, ForceFullSpawnBatch/RequestImmediateResync),
      лог `coop: role change -> HOST/GUEST reset complete`; интеграционные
      тесты host migration/timeout в общих **41/41** (+grace → 42/42)
- [x] **Mission-aware visitor:** `npc_visitor_pause_scenes=1` — при
      `GET_MISSION_FLAG` 0xB15CD1CF58771DE1 или
      `IS_PLAYER_READY_FOR_CUTSCENE(PLAYER_ID())` 0xAA67BCB0097F2FA3
      паузится только cleanup толпы (density/road suppression продолжаются);
      лог `actor: visitor scene pause on/off mission=%d cutscene=%d`
- [x] **E3d-c (npc-e8, v19):** `VehicleOccupants` 0x21 — водители/пассажиры/
      draft-лошади; host discovery снова включает педов в транспорте
      (mission-occupants, draft animal pass); guest apply invisible→warp→
      visible, draft attach, seated/draft skip MoveProxy, 60 Гц vehicle
      interpolation, cleanup; `npc_occupants=1`; логи `world: occupants
      vehicle=`, `actor: seat/draft attach … ok=`
- [x] **E3d-b (npc-e8, v19):** mission-train — 87 TrainConfigs, forward/
      reverse match, `TrainSnapshotV2` 0x22 (48 B, ≤4/с, flags waiting/
      halted/reversing/playerAboard); guest `SET_RANDOM_TRAINS(false)` +
      `_CREATE_MISSION_TRAIN`, sync speed/coords/warp/halt/leave/reverse,
      cleanup+restore; `world_trains=0` opt-in; старый proxy-attach устарел
      (`docs/TRAINS.md`)
- [x] **E9 (npc-e9, v19):** animal/horse appearance — `npc_animal_appearance=1`
      (host+guest): host capture MPT_ANIMAL kind1/2 (cap16, GUIDS/TINT) →
      ActorAppearance 0x1E (≤460 B, budget 1 animal/tick ~2 с) + horse tack
      (kind2) → ActorOutfit 0x1C 792 B on change; guest staged apply/readback,
      fallback random, horse tack apply; `npc_animal_appearance=0` → random;
      probe `actor: animal appearance probe …`; tests 41/41
- [x] **E10 (npc-e10, v19):** mounted NPC-horse appearance — `ActorOutfit`
      0x1C с actorId всадника, `npc_mount_appearance=1` (host+guest), retry
      после reseat/позднего mount, fallback random; логи `mount outfit send/
      apply`, `guest mount outfit applied/failed`; S28
- [x] **E11 (v19):** точные пассажиры поездов через `VehicleOccupants` 0x21
      `carIndex` (driver/passenger на вагонах состава); S26/D3
- [x] **E12 (npc-e12, v19):** `perf: frame host/guest` (полный кадр +
      world/actor/coop p95/max), server `net: bytes_in/out` + per-type,
      client `perf: net`; Player Frame throttle (`player_frame_hz=20`,
      `player_frame_change_only=1`); P0/P1: outfit ≤10 Гц, capture budget
      2/тик, общий vehicle-скан+TTL, occupants 1/с, resync appearance,
      world_scan debug; tests 41/41
- [x] **E13 (npc-e13, v19):** wall-clock нормализация интервалов
      (`GetTickCount64`, ms) — FPS-independent (при 144 FPS больше нет
      ускорения ×2.4), `perf: frame += tick_hz`; `config: warn …` для
      опасных комбинаций (world_sync+npc_actor/world_trains,
      !world_vehicles+occupants/visitor, !npc_actor+vehicles/visitor);
      ASI `B43B7EE0`, server `F3EBD6D1`; snapshot `dist\npc-e13`; tests 41/41
- [x] **E14–E17 (npc-e14…npc-e17, v19, refactor-only):** ub-mode чистка без
      изменения протокола/поведения — net/server (`RunOnce` 574→211,
      server `main` 209→30, `HandleClient` 782→25 через phase-хелперы),
      src (`Tick` 1712→1523, `ApplyRemoteFrame` 386→230,
      `ApplyActionToGhost`/`ApplyOutfitToGhost`/`ApplyLine` dedupe,
      `UpdateLocalShotEvents`, host-netId helper), `quality_scan` многострочные
      сигнатуры + `--self-check`, `IntegrationRunLock`; baseline regenerated
      (45/489/4/3/0 на `2a58ee8`)
- [x] **E18 (npc-e18, v19 без bump):** **soft host migration** — host
      game-timeout 4 с (Ping не считается) демотирует хоста и повышает
      второго **без закрытия сокета** (`PlayerLeft` нет, Ping/Pong жив),
      **join grace 15 с** до первого game-thread сообщения, presence =
      liveness; клиентский loading presence heartbeat (~2 с, пока playerPed
      отсутствует); гость в visitor mode не спавнит host-прокси продавца
      (`actor: visitor vendor skip`); server `HandleClient` phases;
      integration run lock; honest grace-тест host-migration; ASI
      `E3FAB30F`, server `DD9BD9D8`; snapshot `dist\npc-e18`; **оба ПК
      задеплоены**, canonical CI 41/41 + C++7
- [x] **E19 (npc-e19, v19 без bump):** **имя игрока на карте/blip** —
      `MakeLiteralString` + `_SET_BLIP_NAME` (0x9CB1A1623062F402) +
      VAR_STRING, `SetBlipName`/`UpdateBlipName`, лог `coop: blip name '%s'`
      (резерв `BLIP_REMOVE_MODIFIER`, если останется «Спутник»);
      **occupant-first roster + полный allowlist** occupants всех vehicle в
      occ-радиусе — кучер и draft-лошади повозки у гостя (без T-pose/дублей);
      **opt-in wheel A/B** `npc_vehicle_wheels=0` (`SET_VEHICLE_FORWARD_SPEED`
      SIGNATURE-ONLY, лог `world: vehicle wheels speed=`); `SET_ENTITY_COORDS`
      арность 8 (clearArea=0, поведение не менялось); ASI `DD0EE210`
      (779 264 B), server `DD9BD9D8`; snapshot `dist/npc-e19`; **оба ПК
      задеплоены**, canonical CI **42/42** + C++7 + quality strict PASS;
      независимая проверка `architect-verify2` — «можно тестировать/деплоить»
      (low/info: blip label, плотный трафик, wheel A/B)
- [ ] Backlog: **2-ПК приёмка e19** (S31 soft migration, S22/S25
      occupants, S20 vendors, blip label, wheel A/B отдельным прогоном
      `npc_vehicle_wheels=1`→0), visual E2c-b/E5/E6/E7/E3d/E9/E10,
      ambient-trains cleanup, mission-aware visitor, adaptive 15 Гц,
      host migration/resync под нагрузкой, Tick p95/FPS;
      `npc_actor` не включать в рабочую сборку до приёмки
- [ ] Известные ограничения: E9 cap16/RESET на животных не проверен/
      horse tack model-specific/MPT_ANIMAL coverage fallback random/
      MetaPedOutfit только probe; E10 mounted horse late mount; E12
      player_frame_hz=20 может сглаживать ghost (поднять до 60); E13
      часть таймеров осталась тиковой (corpse hold, action/presence poll,
      motion/scenario, attach/dead windows, vehicle-scan TTL, train engine
      cache) — при 144 FPS их окна короче; config-асимметрия host/guest не
      проверяется; Story Mode нельзя «подселить»; RedM — другой стек; при
      `world_vehicles=0` пустые дороги; E3d-b opt-in и direction/reversing
      приближённые; seat=-1/driver — in-game; при миграции мир не
      переносится; лицо/волосы/борода, точные анимации/катсцены; E5 без
      raycast, урон/HP аппроксимированы, headshot-радиус 0.30 м, труп до
      despawn

- [x] `WorldSnapshot` (0x10), хост-авторитет
- [x] Сканер NPC 60 м, до 64 сущностей (32 Ped + 16 Vehicle + 16 Object),
      10 Гц, приоритет mission
- [x] Гость: прокси-педы (model request/mission/variation/visible, без коллизии)
- [x] Подавление ambient-популяции гостя density-множителями
- [x] Таймаут снапшотов 3 с → удаление прокси
- [ ] **Тест на двух ПК** (`world_sync = 1` на обоих) — ждём прогона
- [x] Транспорт в снапшоте (Vehicle до 16: дилижансы/повозки/лодки/трамваи;
      mission-прокси без коллизии; E3d-a vehicles-only — без `world_sync`)
- [x] Объекты/предметы в снапшоте: mission Object до 16 (амбиентные пропсы
      не дублируются — они уже есть в мире гостя)
- [x] E3d-b поезда: mission-train v19 (`TrainSnapshotV2 0x22`); старый
      proxy-attach устарел (`docs/TRAINS.md`); точные пассажиры/ambient-
      cleanup — backlog
- [x] E3d-c водители/пассажиры/draft (`VehicleOccupants 0x21`, `npc_occupants=1`)
- [ ] Произвольные/динамические объекты и миссионные акторы — backlog
- [x] Двусторонний маппинг целей handle↔netId (carry/loot host→guest и guest→host)
- [ ] Очистка старых ambient-педов у гостя при входе в сессию
- [x] Интерполяция/задачи анимации для прокси-NPC (walk/run/idle,
      navmesh-задачи, heading; без точных клипов сценариев/боя)
- [ ] Реконсиляция и tombstones при массовых изменениях мира

## Фаза 6 — P2: Action v2 — действия и анимации

**Цель:** синхронизировать не «все клипы» (публичный SDK их не отдаёт), а
наблюдаемые действия через state/task/scenario-replication. Полный
клип-в-клип AnimGraph невозможен без версионного реверса/хуков — см. ниже.

- [x] Базовые Action: None/Shooting/Reloading/Sitting/InScenario/Aiming/Drunk
- [x] Синхронизация перезарядки и выстрелов
- [x] **Action v5:** сообщение `ActionState` (mode, flags, scenarioHash)
- [ ] Сценарии: сидение/курение/еда/питьё через `_GET_ACTIVE_DYNAMIC_SCENARIO`
      + `TASK_START_SCENARIO_IN_PLACE_HASH`
- [x] Укрытие: флаги `IS_PED_IN_COVER` / `IS_PED_AIMING_FROM_COVER`,
      `TASK_ENTER_COVER`/`TASK_EXIT_COVER` (cover-point приблизительный)
- [x] Переноска тела: `IS_PED_CARRYING_SOMETHING` + target netId (host→guest)
      (guest→host потребует обратного world_sync)
- [ ] Таскание/хогтай: `_IS_PED_DRAGGING`, `IS_PED_HOGTIED` (флаги есть)
- [x] Лутание: `GET_LOOTING_PICKUP_TARGET_ENTITY` + `TASK_LOOT_ENTITY`
      (host→guest, через netId из P1)
- [ ] Рыбалка: `_GET_TASK_FISHING` (флаг есть, задача позже)
- [ ] Скиннинг/лагерные действия: сценарии/задачи по таблице
- [x] Таблица «детект → задача», fallback в Idle (без «танцев»)
- [x] Посадка в транспорт удалённого игрока (PlayerVehicle 0x12:
      прокси-транспорт + место; приблизительная анимация посадки)
- [ ] Диагностика неизвестных Actions в лог для расширения таблицы

**Ограничения (зафиксировано):**
- Публичный SDK не умеет получить текущий клип/фазу/бленды AnimGraph;
  `rdr2-coop-story` подтверждает это и оставляет `VersionedMemoryReader`
  как будущий версионный реверс.
- Нет API записи позы скелета → стримить «позу» нельзя, только задачи.
- AnimScene можно ловить/реплеить инлайн-хуками по RVA (как в coop-story),
  но это версионно, только хосту и не «все анимации».

## Фаза 7 — Миссии, прогресс, сейвы

- [x] Каталог 80 Story MissionData ID (MIT-атрибуция rdr2-coop-story)
- [x] Детект новых завершений хоста: baseline `WAS_COMPLETED` + rating
- [x] `MissionEvent` (v9): хост → гость, только хост; гость логирует
- [x] Зачёт в свой сейв гостя по флагу `mission_credit`
      (`_UILOG_MARK_MISSION_COMPLETED` + rating; без копирования сейва)
- [x] Определение текущей миссии: runtime-скрипт через
      `GET_NUMBER_OF_THREADS_RUNNING_THE_SCRIPT_WITH_THIS_HASH` + каталог
- [x] MissionState (v10): гость видит активную миссию хоста (лог/субтитр)
- [x] MissionBarrier v11: offer → ready → start-window → started/abort
      (протокол + mock-тесты сервера; in-game handshake не тестирован)
- [ ] Барьер совместного старта: удержание миссии до готовности гостя
      (сейчас окно открывается уже после старта хоста)
- [ ] Награды/allow-list 17 миссий, деньги/Honor по умолчанию не трогать
- [x] Journal + backup сейва гостя перед зачётом (`save_backup=1`, без restore)
- [ ] Restore/сравнение сейва и откат зачёта при ошибке
- [ ] Миссионные AnimScene/катсцены — capture/replay (R&D, версионно)
- [ ] Тест: одна миссия (FUD1/HNT1) на двух ПК, save/reload

## Фаза 8 — Надёжность, сервер, мультиплеер

- [x] Сервер: keepalive/timeouts, лимиты очередей, атомарный players.txt,
      защита от пустых/повторных JOIN, host-only сообщения
- [x] Headless-тесты протокола/сервера (**42 integration test**, без игры;
      e18/e19: grace-тест host-migration + soft-семантика)
- [x] Лимит одновременных соединений сервера (32) + flood-тест
- [x] Mission catalog/rewards validation test (80 миссий, 23 записи/17)
- [x] Idle timeout сервера (настраивается 2-м аргументом, по умолчанию 30 с)
- [x] Атомарная запись players.txt (tmp + MoveFileEx/rename)
- [x] scripts\restore_saves.ps1: восстановление сейва из бэкапа (dry-run/-Force)
- [x] Проверка и фикс: probe-подключения больше не затирают players.txt
- [x] Reconnect с backoff (1→10 с), очистка педа, повторный JOIN
- [x] Host migration: сервер повышает гостя, шлёт HostChanged (v12);
      E7.3 hardening — game-timeout 4 с/Ping не считается, role-change reset;
      **e18 soft migration** — демоция без закрытия сокета (нет
      reconnect-шторма), join grace 15 с, presence-liveness
- [x] Headless-тест host migration: новый хост принимается для WorldSnapshot;
      game-timeout ping-only/frame-activity тесты; **e18**: soft-семантика
      (demote/no-socket-close/presence) + join-grace тест (host без
      game-сообщений не демотируется внутри grace 15 с)
- [x] Fuzz-тест: 300 случайных кадров/размеров + C++ FrameReader fuzz
- [x] C++ тест client reconnect с фейковым сервером
- [x] WorldSnapshot parser stress: 4 потока × 20k, строгий размер/count,
      вынесен в src/world_snapshot_parse.cpp
- [x] GitHub Actions badge + build artifacts (asi/server)
- [x] GitHub Actions workflow: сборка клиента/сервера + все тесты
- [x] Mission rewards dry-run: каталог 23 записей/17 миссий в логе, без нативов
- [x] Config-тесты: LoadFromFile, флаги/hex/битые строки/defaults
- [x] players.txt recovery-тест: битые строки и stale .tmp игнорируются
- [x] restore_saves.ps1 автотест (restore + pre-restore snapshot)
- [ ] TrainSnapshot v13 + attach вагонов к локомотиву на госте —
      R&D, в игре нестабильно, по умолчанию выключено (`world_trains=0`)
- [x] C++ stress candidate budget: 4×10k против reference-выборки
- [x] Mission barrier rules fuzz: 4×100k фаз/ролей/offer/epoch
- [x] Per-IP rate limit (12 новых TCP/сек) + flood-тест
- [x] Graceful shutdown: Ctrl+C/nonblocking accept + SaveAll; тест-хук
- [x] Метрики сервера в статусной таблице (active/peak/total/rate/cap/uniqueIPs)
- [x] GitHub Actions cache для build/server/tests + artifacts
- [ ] Применение наград allow-list после игрового теста
- [x] CI-скрипт `scripts/ci.ps1`: сборка клиента+сервера и тесты
- [x] Убрано предупреждение C4005 WIN32_LEAN_AND_MEAN
- [ ] Проверка reconnect/host migration на двух ПК (нужна игра)
- [ ] 3+ игроков: `map<sessionId, RemotePeer>`, player id в кадрах
- [ ] Anti-desync: метрики, resync по запросу, tombstones
- [ ] Логи: ротация, уровни, без спама outfit categories
- [ ] Документация: PROTOCOL/README под текущую версию, ROADMAP

## Идеи на перспективу

- Озвучка появления/ухода игрока
- Метка с настраиваемым цветом, настройка дистанции имени
- Чат/меню/список игроков (по мотивам RDR2Coop)
- Host-authoritative ambient encounters (по мотивам coop-story)
- WireGuard/туннель для интернета (без публичного сервера)

## Архитектурная заметка

- Модель A: два процесса, два мира; хост авторитетен, гость — ведомый.
  Видимость «одного мира» достигается репликацией сущностей и действий,
  а не единой симуляцией движка.
- Сюжетные миссии не синхронизируются «из коробки»: каждый проходит свою
  копию; совместный зачёт — через MissionData-нативы и барьер старта.
- Связь по интернету — через WireGuard/туннель; публичный сервер не
  предполагается.
- Юридически: Story Mode only, non-commercial, без бандла ScriptHook/dinput8
  и без изменения R*-скриптов/ассетов для публичной раздачи.
