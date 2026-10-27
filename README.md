# RDR2 CO-OP — кооперативный мод для одиночного сюжета

[![CI](https://github.com/cptn73m0/rdr2-coop/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/cptn73m0/rdr2-coop/actions/workflows/ci.yml)

Кооперативный мод для **Red Dead Redemption 2** (Story Mode): два игрока
играют по сети, каждый проходит сюжет у себя, а второй игрок виден в мире как
**пед-призрак** — с синхронизацией движения, анимаций, оружия, лошади и
частично одежды (shop-предметы и wearable states). Плюс общее время/погода и
сетевые функции (роли, reconnect, host migration).

Кооператив **не является полноценным совместным прохождением**: сюжетные
миссии и транспорт мира по-прежнему не реплицируются; NPC-репликация
(люди + животные + свободные лошади, E1–E16) реализована как экспериментальная
ветка и требует `npc_actor=1` (см. «Известные ограничения»).

## Статус

Проверено в игре двумя игроками на билде **1.0.1491.50**; последний игровой
стабильный тег — **stable-v14** (`OutfitState` 784 B, wearable states одежды).
**Текущий протокол — v19** (actor-репликация NPC E1–E16:
`ActorDamage` 0x1F + `PlayerPresence` 0x20 + `VehicleOccupants` 0x21 +
`TrainSnapshotV2` 0x22). E5.2/E5.3 (трупы/сглаживание всадников), **E6**
(presence-суффиксы), **E7.1** (visitor mode), **E7.2** (плавное движение
60 Гц), **E7.3** (E3d-a host-транспорт, host migration hardening,
mission-aware visitor), **E8** (E3d-c водители/пассажиры/draft, E3d-b
mission-train), **E9** (внешность животных/лошадей: assets 0x1E + horse
tack 0x1C), **E10** (внешность лошади под NPC-всадником через 0x1C),
**E11** (точные пассажиры поездов через 0x21 carIndex), **E12**
(полный `perf: frame`/`perf: net`, Player Frame throttle, P0/P1 перф-фиксы),
**E13** (wall-clock нормализация интервалов, FPS-independent, `tick_hz` и
`config: warn` для опасных комбинаций) реализованы. **E14–E16** — ub-mode
refactor-only сборки (net/server/src, без изменения протокола и поведения):
ammo helpers/host-netId dedupe, apply-outfit dedupe/Tick perf-window.
E5/E5.1 подтверждены в игре. CI **41/41** integration + 7/7 C++ (HEAD
0e64a22, тег **npc-e16**). Локальная пара v19: ASI `C4E4CF70`, server
`1BE28084`; **ARINA online** — задеплоено локально и на ARINA. Флаг
`npc_actor` по умолчанию **0**; для теста включать на обоих ПК. Клиенты
v14–v19 несовместимы (сервер отклоняет JOIN с `VersionMismatch`). Теги
этапов NPC: `npc-e0`…`npc-e16`. Финальная refactor-сборка перед 2-ПК
приёмкой. Чек-лист: [docs/ACCEPTANCE_2PC.md](docs/ACCEPTANCE_2PC.md).

### Что работает

- Сервер-релей: JOIN / FRAME / LEAVE, таблица игроков, персистентность
  последней позиции (`players.txt`), лог подключений с IP.
- Роли HOST/GUEST, host migration при выходе хоста (game-timeout 4 с,
  Ping не считается, client role-change reset), автоматический reconnect
  с backoff (1→10 с).
- Кадр состояния 60 Гц (~5 КБ/с на клиента), Ping/Pong и RTT в логе.
- Общее время и погода от хоста.
- Пед-призрак второго игрока: спавн по модели персонажа, позиция/поворот,
  движение по навмешу (стоять/идти/бежать), интерполяция, ragdoll/укрытие/
  плавание/лазание/прыжок/падение, смерть и возрождение.
- Имя над головой и метка на карте; скрытие при смерти.
- Оружие: выдача, доставание, прицел, перезарядка; стрельба удалённого игрока
  воспроизводится по событиям (аммо-детект + очередь), включая автоматическое
  оружие.
- Лошадь: модель, посадка/спешивание, движение; снаряжение — частично.
- Одежда: переносятся **shop-предметы** (куртки, шляпы, патронташи и т.п.)
  вместе с **wearable states** (открытая/закрытая куртка, рукава и т.п.;
  протокол v14, `OutfitState` 784 B).
  Базовая внешность (лицо, волосы, борода, дефолтные слои модели) не
  синхронизируется — в публичном RDR2 SDK нет getter'а этих компонентов.
- **NPC-репликация (E1–E16, `npc_actor=1` на обоих ПК).** Хост реплицирует
  human-NPC до `npc_cap` (1..12, default 8) в радиусе `npc_radius` (20..100 м,
  default 60) и животные/лошади до `npc_animal_cap`: discovery 10 Гц, roster
  actorId/epoch, spawn-heartbeat 2.5 с, кадры 10 Гц, despawn (ушёл/умер/
  вытеснен), resync 0x1D; гость — прокси-педы с плавной навигацией и gait с
  хоста.
  - **E2a:** meta-outfit внешность NPC (probe: hash есть у ~22% ambient;
    в E2a-тесте 5/5 применено, failed=0).
  - **E2b:** всадники rider+horse (mount spawn ×9, failed=0), seat watchdog,
    очистка при dismount/despawn; плавное движение (gait_restart=0,
    err_p95≈0).
  - **E3a:** сценарии (optimistic confirm, анти-наложение: skip 8 м + warp
    revert 3 м; визуально «двое курят на разных местах»), смерть/возрождение
    (труп остаётся).
  - **E3b:** оружие (валидные hash через GIVE_WEAPON_TO_PED), draw/aim/
    shoot/reload; state=shoot=1450, reload=385, weapon fail=0.
  - **E2c-a:** хост-перебор metaped-outfits (890 моделей / 4568 кандидатов,
    бюджет 24 натива/тик, heartbeat-пересылка, guest reapply) — механически
    работает; для random ambient NPC resolved=0/failed=11.
  - **E2c-b (v16):** точная внешность asset/tint — хост захватывает
    `_GET_NUM_COMPONENTS_IN_PED` (0-based) + `GET_META_PED_ASSET_GUIDS/TINT`,
    шлёт `ActorAppearance` (0x1E) при спавне/изменении/join (budget 1
    актор/тик, период 2 с); гость применяет reset + `_SET_META_PED_TAG` +
    commit + `UPDATE_PED_VARIATION` с readback/fallback; asset приоритетнее
    metaOutfit. Валидация: host `capture count=8-10 valid=8-10`, fake guest
    `appearance_msgs=504 appearance_actors=8 entry_max=10 errors=0`.
  - **E3c:** животные (`kind=1`, HP 100) и свободные лошади (`kind=2`, HP 100)
    реплицируются и двигаются; при смерти — труп/лошадиный труп до despawn.
  - **E5 (host-authoritative урон):** гость детектит выстрел в прокси через
    `GET_ENTITY_PLAYER_IS_FREE_AIMING_AT` и шлёт `ActorDamage` 0x1F; урон по
    классу оружия (пистолет/револьвер 35, винтовка/repeater/sniper 60,
    дробовик 80, иначе 30), лимиты гостя 100 мс/actor и ≤10/с. Урон применяет
    только хост (`APPLY_DAMAGE_TO_PED`, fallback `SET_ENTITY_HEALTH`), лимиты
    ≤8/тик, ≤15/с, дистанция ≤150 м; прокси у гостя остаются invincible.
    Смерть → dead-флаг → труп; animals/horses тем же путём.
  - **E5.1 (хедшоты):** гость проверяет кость `SKEL_Head` + impact ≤0.30 м
    (fallback — луч камеры) и шлёт `damage=250` → ваншот human HP 200 /
    animal HP 100; в логе `headshot=1`. Подтверждено в игре.
  - **E5.2/E5.3:** труп держится, пока существует host-труп
    (`npc_corpse_max_sec=300`, лог `actor: corpse hold/despawn`); сглаживание
    всадников (retarget 1.75 м, lead 0.28, stuck fallback, лог
    `actor: motion mounted err_p95/retargets`).
  - **E6 (protocol v18):** `PlayerPresence` 0x20 — карта/пауза/сатчель/магазин
    в имени над головой (`Name - in map`, `pause menu`, `in satchel`,
    `in shop`); статус 2 Гц/изменение, timeout 12 с; `npc_presence=1`,
    debug `ui: pause=...`. UIAPP-детект (`IS_PAUSE_MENU_ACTIVE` + `IS_UIAPP_*`).
  - **E7.1 visitor mode:** у гостя подавляется локальная ambient-толпа
    (cleanup раз в ~10 с, ≤8/тик, фейд); транспорт — при
    `npc_visitor_vehicles=1` (default 1 после E3d-a; локальный трафик
    подавляется, машины едут из реплик хоста); продавцы/квестовые/событийные
    NPC сохраняются (`npc_visitor_preserve_events=1`, whitelist 23 моделей).
    В миссии/катсцене cleanup толпы паузится (`npc_visitor_pause_scenes=1`,
    density/road suppression продолжаются). Логи `actor: visitor mode on/off`,
    `visitor cleanup removed=`, `visitor scene pause on/off`.
  - **E7.2 плавное движение:** интерполяция кадров 60 Гц, far goal впереди
    хоста (lead speed*0.7, clamp 4–12 м; кони 5–20), одна задача на фазу,
    in-place update `_UPDATE_TASK_GO_TO_COORD_WITH_OFFSET`, MBR через
    `_COMPUTE_SPEED_FOR_PED_MOVE_BLEND_RATIO`, stop/start гистерезис,
    телепорт >25 м/|dz|>15/нет кадров >2 с с логом `actor: teleport`.
    Диагностика `actor: motion id=… reissue=…/s snap=…/s target_err_p95=…
    status=…`; baseline-порог: **reissue <0.05/s, snap=0**.
    Флаги `npc_motion_update=1`, `npc_motion_marker=0`,
    `npc_motion_status_p2=1`.
  - **E3d-a host-транспорт (v18; occupants v19):** vehicles-only `WorldSnapshot` при
    `world_sync=0 && npc_actor=1 && world_vehicles=1`: до 16 машин в радиусе
    `npc_vehicle_radius=80` вокруг обоих игроков, отправка раз в 6 тиков;
    пустой snapshot удаляет прокси у гостя. Гость создаёт
    `CREATE_VEHICLE` mission/invincible/no-collision, dedupe `engineNetId`,
    timeout 3 с. Водители/пассажиры — E3d-c (0x21). Логи `world: vehicles
    send n=`, `world: vehicle spawn/despawn`.
  - **E3d-c (v19):** водители/пассажиры/draft-лошади: хост собирает
    occupants (driver/passenger/draft harness) → `VehicleOccupants` 0x21
    (8+8×count, max 72 B, host-only, 20/с); гость ждёт vehicle+actor,
    invisible→`SET_PED_INTO_VEHICLE` warp→`IS_PED_IN_VEHICLE`→visible;
    draft — `_ATTACH_DRAFT_VEHICLE_HARNESS_PED`; seated/draft пропускают
    `MoveProxy`; vehicle-прокси интерполируются 60 Гц (snap>8 м — warp+
    пересадка). `npc_occupants=1`, логи `world: occupants vehicle=`,
    `actor: seat attach … ok=`, `actor: draft attach … ok=`, `occupants errors=`.
  - **E3d-b (v19):** mission-train: 87 config-хэшей, матч forward/reverse,
    бюджет 5/тик/80 model-вызовов, `TrainSnapshotV2` 0x22 (48 B, host-only,
    ≤4/с, flags waiting/halted/reversing/playerAboard); гость
    `SET_RANDOM_TRAINS(false)` + `_CREATE_MISSION_TRAIN`, sync speed/coords/
    warp/halt/leave/reverse, cleanup и восстановление random trains.
    `world_trains=0` (тестовый opt-in; активен при `npc_actor=1 &&
    world_vehicles=1 && world_trains=1`, не при `world_sync=1`).
  - **E9 animal/horse appearance (v19 без bump):** при `npc_animal_appearance=1`
    (host+guest синхронно) хост захватывает assets животных/лошадей
    (`MPT_ANIMAL` type3, cap16, GUIDS/TINT) → `ActorAppearance` 0x1E
    (≤460 B), budget 1 animal/tick ~2 с; horse tack (kind=2) →
    `ActorOutfit` 0x1C 792 B on change (budget 1 horse/tick ~2 с). Гость:
    staged apply/readback, fallback random; при 0 — random как раньше.
    Probe `actor: animal appearance probe …` (host, 1/модель ≤1/10 с).
  - **E10 mounted NPC-horse appearance (v19 без bump):** `npc_mount_appearance=1`
    (host+guest синхронно) — внешность лошади под NPC-всадником через
    `ActorOutfit` 0x1C с actorId всадника; `actor: mount outfit send/apply`,
    guest `mount outfit applied/failed`; fallback random, retry после reseat/
    позднего mount; при 0 — random.
  - **E11 (v19):** точные пассажиры поездов через `VehicleOccupants` 0x21
    с `carIndex` (driver/passenger на вагонах состава), сверка с mission-train.
  - **E12 (v19 без bump):** `perf: frame host/guest` (полный кадр + world/
    actor/coop p95/max), server `net: bytes_in/out` + per-type, client
    `perf: net tx/rx`; Player Frame throttle (`player_frame_hz=20`,
    `player_frame_change_only=1`: покой 5 Гц, движение до 20 Гц) и P0/P1
    перф-фиксы (outfit ≤10 Гц, capture budget 2/тик, общий vehicle-скан,
    occupants 1/с, TTL, resync, world_scan debug).
  - **E13 (v19 без bump):** wall-clock нормализация (`GetTickCount64`) —
    интервалы actor frames/discovery/appearance/heartbeats/trains/occupants/
    presence/player frame/world_scan/outfit в **ms**, не зависят от FPS
    (при 144 FPS больше нет ускорения ×2.4); `perf: frame` дополнен
    `tick_hz=` (реальный FPS); `config: warn …` для опасных комбинаций
    (`world_sync+npc_actor`, `world_sync+world_trains`, `!world_vehicles+
    occupants/visitor`, `!npc_actor+vehicles/visitor`), без падения.
  - **Host migration hardening:** серверный host game-timeout 4 с (не Ping —
    только игровые сообщения; 4-й argv 2..15 с, idle 30 с отдельно) →
    close → PlayerLeft + promote + HostChanged. Клиент при HostChanged
    сбрасывает world_sync/visitor/presence/actor/боевые состояния и делает
    `ForceFullSpawnBatch`/`RequestImmediateResync`; лог
    `coop: role change -> HOST/GUEST reset complete`.
- Headless-тесты сервера/протокола (**28 базовых + 13 actor/presence/vehicle/
  train-тестов v19**; integration **41/41**), C++ 7/7, CI-workflow (артефакты,
  кеш) и локальный `scripts\ci.ps1`.

**INI-флаги NPC:** `npc_actor = 1` (включает E1–E16), `npc_cap = 8`
(1..12), `npc_radius = 60` (20..100), `npc_appearance = 1` (meta-outfit E2a
и asset/tint E2c-b), `npc_animal_cap = 4` (0..12), `npc_damage = 1`
(E5-урон), `npc_corpse_max_sec = 300`, `npc_presence = 1` (E6),
`npc_visitor = 1`, `npc_visitor_vehicles = 1`,
`npc_visitor_preserve_events = 1`, `npc_visitor_pause_scenes = 1` (E7.1),
`npc_motion_update = 1`, `npc_motion_marker = 0`,
`npc_motion_status_p2 = 1` (E7.2), `world_vehicles = 1`,
`npc_vehicle_radius = 80` (20..200, E3d-a), `npc_occupants = 1` (E3d-c),
`npc_animal_appearance = 0` (E9; host+guest синхронно),
`npc_mount_appearance = 0` (E10; host+guest синхронно),
`player_frame_hz = 20` (P0; 10..60), `player_frame_change_only = 1` (P0),
`world_trains = 0` (E3d-b, тестовый opt-in), `npc_debug = 1` —
диагностические probe/perf/visitor/motion/world-логи (host и guest).

### Отключено / не работает (экспериментальные флаги = 0)

Код этих функций присутствует в репозитории, но в текущей рабочей сборке они
**выключены**, в игре не проверены или работают нестабильно. Не включать без
отдельной отладки:

- `world_sync` — полная репликация NPC/транспорта/mission-объектов
  (экспериментально, даёт дёргание/пропажи прокси); не путать с E3d-a
  vehicles-only (`world_vehicles=1`, работает без `world_sync`).
- `action_sync` — сценарии/укрытие/переноска/лутание удалённого игрока.
- `vehicle_sync` — посадка удалённого игрока в транспорт (дилижанс/повозку).
- `mission_sync` / `mission_credit` — активная миссия хоста, барьер старта,
  совместный зачёт MissionData (в игре не проверялось; `mission_credit`
  потенциально опасен для сейва).
- `world_trains` / `train_debug` — mission-train v19 (E3d-b) реализован, но
  по умолчанию **0** (тестовый opt-in); старый proxy-attach устарел, см.
  [docs/TRAINS.md](docs/TRAINS.md).
- NPC-репликация (E1–E16) — экспериментальная и **не включена по
  умолчанию**: `npc_actor=0`; mission-NPC не входят; E3d-c occupants/draft и
  E3d-b mission-train — тестовые (train default0); двух-ПК визуальная приёмка
  pending (ARINA offline). Прошлая ветка v14 (NpcSpawn/NpcFrame) в рабочую
  сборку не входит.

## Известные ограничения

- Сюжетные миссии не проходятся совместно в одном инстансе; общего прогресса
  и барьера старта в стабильной сборке нет.
- NPC-репликация — **экспериментальная (E1–E16: люди, животные, свободные
  лошади, транспорт)**: cap 12 + `npc_animal_cap`; mission-NPC исключены;
  транспорт E3d-a vehicles-only, E3d-c водители/пассажиры/draft; поезда
  E3d-b mission-train (opt-in `world_trains=1`); E9/E10 animal/horse
  appearance (flags `npc_animal_appearance=0`/`npc_mount_appearance=0`); E11
  точные пассажиры поездов; E12 `perf: frame`/`perf: net` и Player Frame
  throttle; E13 wall-clock/FPS-independent и `config: warn`; ambient у гостя
  подавляется visitor mode; двух-ПК приёмка pending; лицо/волосы, точные
  анимации/сценарии и катсцены не синхронизируются.
- **E9:** cap 16 компонентов; RESET_PED_COMPONENTS на животных не проверен
  в игре; horse tack model-specific (старые shop items не снимаются);
  assets coverage MPT_ANIMAL может быть неполным → random fallback;
  MetaPedOutfit только probe; `npc_animal_appearance` должен совпадать на
  host+guest.
- **E10:** лошадь под NPC-всадником может получить внешность с задержкой/
  не примениться при позднем mount (heartbeat/retry повторит); horse tack
  model-specific (старые shop items не снимаются); in-game S28 pending;
  `npc_mount_appearance` host+guest синхронно.
- **E11/E12/E13:** E11-пассажиры поездов — по 0x21 `carIndex` (проверять D3);
  `player_frame_hz=20` может слегка сглаживать ghost в движении на 2-ПК
  (поднять до 60 при необходимости); P2 config validator/wall-clock — сделаны
  в E13, но часть таймеров осталась тиковой (corpse hold, action/presence poll,
  motion/scenario, attach/dead windows, vehicle-scan TTL, train engine cache) —
  при 144 FPS их реальные окна короче; трафик — по серверным bytes и клиентскому
  `perf: net` (типично ≤10–12, worst ≤15 КБ/с).
- **Story Mode:** нельзя реально «подселить» второго игрока в один мир —
  каждый проходит сюжет у себя, реплицируются только NPC/животные/транспорт;
  RedM — другой стек и несовместим с этим ASI.
- **E5.2/E6/E7:** труп держится до исчезновения host-трупа или
  `npc_corpse_max_sec` (300 с); presence-суффикс истекает через 12 с без
  обновлений; при `world_vehicles=0`/`npc_visitor_vehicles=0` локальный
  трафик гостя остаётся; smooth motion — A/B; хедшот-радиус 0.30 м, урон
  аппроксимирован; mission-aware visitor и adaptive 15 Гц — backlog.
- **E3d-a/c/b:** E3d-c seat=-1 (driver) требует in-game проверки; точные
  пассажиры поездов — E11 (0x21 carIndex, D3); direction/reversing семантика поезда
  приближённая; ambient-поезда не чистим в v1; при миграции мир не
  переносится — новый хост отдаёт свой.
- **E5/E5.1:** детект урона без raycast — free-aim target + impact; урон
  аппроксимирован по классу оружия/HP движка; животные умирают быстро; труп
  до despawn; mission NPC не получают урон; host migration hardening (4 с
  game-timeout) реализован, resync под нагрузкой — backlog.
- **ARINA (второй ПК) online** — npc-e16 задеплоен (ASI `C4E4CF70`, server
  без изменений `1BE28084`); локальная пара та же. Текущие E5–E13/E3d-
  подтверждения — одиночный ПК + fake host/гость; визуальная 2-ПК приёмка
  pending.
- Базовая внешность персонажа (лицо/волосы/борода, дефолтные слои одежды) не
  переносится; синхронизируются только shop-предметы.
- Точные клипы AnimGraph/AnimScene не передаются.
- Стрельба педа-призрака наносит реальный урон (не вставать на линии огня).
- Снаряжение лошади синхронизируется не полностью.
- Хост — первый подключившийся к серверу. Чтобы хостом был нужный ПК,
  запускайте/подключайтесь на нём первым.

## Требования

- **Red Dead Redemption 2** для PC, билд **1.0.1491.50** (только Story Mode).
- **ScriptHookRDR2.dll** (V2 2.0 / Alexander Blade) рядом с `RDR2.exe`.
- **ASI-лоадер** (`dinput8.dll` / `dsound.dll`) в папке игры.
- Компилятор: **Visual Studio 2019 Build Tools** (MSVC x64) + CMake.

## Сборка

Клиент (ASI-плагин):

```
cmake -S . -B build -G "Visual Studio 16 2019" -A x64
cmake --build build --config Release
```

Сервер:

```
cmake -S server -B build-server -G "Visual Studio 16 2019" -A x64
cmake --build build-server --config Release
```

Результат: `build/Release/rdr2_coop.asi` и `build-server/Release/rdr2_coop_server.exe`.

## Тесты и CI

```powershell
# headless-тесты сервера/протокола (без игры)
python tests\integration_server.py --exe build-server-ci\Release\rdr2_coop_server.exe

# полный CI: клиент + сервер + 41 integration + 7 C++ + restore-тест
powershell -ExecutionPolicy Bypass -File scripts\ci.ps1
```

CI включает **ub-mode strict quality gate** (по умолчанию): `quality_scan.py`
против `tools/quality_baseline.json`, падение при росте CRAP>5000/dup>=4/TODO.
После P3-фикса многострочных сигнатур baseline перегенерирован:
45 файлов/**489** функций, CRAP>5000=**4**, dup≥4=**3**, TODO=0; скрытые
`HandleClient`/`SendHostSnapshot`/`ProcessGuestHorseOutfits` — не регрессия
кода, а артефакт старого сканера. Детали и hotspots —
[docs/UB_MODE.md](docs/UB_MODE.md).

Двух-ПК чек-лист: [docs/TESTING.md](docs/TESTING.md).
Финальная приёмка 2-ПК: [docs/ACCEPTANCE_2PC.md](docs/ACCEPTANCE_2PC.md).

## Установка клиента

1. `rdr2_coop.asi` — в `...\Red Dead Redemption 2\scripts\` (папку создать).
2. `rdr2-coop.ini` — рядом с `.asi` (в той же `scripts\`).
3. `ScriptHookRDR2.dll` и ASI-лоадер (`dinput8.dll`) — в корень игры.
4. Запустить игру → Сюжет. Лог: `scripts\rdr2-coop.log`.

```ini
# rdr2-coop.ini
server_host = 192.168.1.103   ; ПК с сервером
server_port = 7777
player_name = Player2
ped_model   = 0xCF7E73BE      ; запасная модель, пока не пришла модель персонажа
```

> Без `ScriptHookRDR2.dll` плагин не загрузится — в логе будет
> `ScriptHookRDR2.dll not found`.

### Флаги `rdr2-coop.ini`

Стабильная конфигурация — **экспериментальные world/mission-флаги ниже = 0**;
NPC-флаги включаются только для однопК/двух-ПК тестов NPC (дефолты
`npc_appearance`, `npc_presence`, `npc_visitor`, `npc_visitor_preserve_events`,
`npc_motion_update`, `npc_motion_status_p2` = 1).

| Флаг | По умолчанию | Состояние |
|---|---|---|
| `world_sync` | 0 | экспериментально, в игре нестабильно — не включать |
| `action_sync` | 0 | не проверено в игре |
| `vehicle_sync` | 0 | не проверено в игре |
| `mission_sync` | 0 | не проверено в игре |
| `mission_credit` | 0 | не проверено, опасно для сейва |
| `save_backup` | 1 | бэкап сейва перед зачётом (защита) |
| `mission_reward_log` | 1 | dry-run лог плана наград (нативы не вызываются) |
| `world_trains` | 0 | E3d-b mission-train v19 (тестовый opt-in; старый proxy-attach deprecated) |
| `train_debug` | 0 | диагностика mission-train |
| `npc_actor` | 0 | E1–E16 NPC-репликация (экспериментально, только для теста) |
| `npc_cap` | 8 | 1..12, максимум human NPC-прокси |
| `npc_radius` | 60 | 20..100 м, радиус discovery (keep +10 м) |
| `npc_appearance` | 1 | meta-outfit E2a + asset/tint E2c-b (asset приоритетнее) |
| `npc_animal_cap` | 4 | 0..12, максимум животных/свободных лошадей (E3c) |
| `npc_damage` | 1 | E5: host-authoritative урон по NPC/животным (0 = выключить) |
| `npc_corpse_max_sec` | 300 | E5.2: предел удержания host-трупа, сек (0 = старое) |
| `npc_presence` | 1 | E6: суффикс UI-статуса в имени (карта/пауза/сатчель/магазин) |
| `npc_visitor` | 1 | E7.1: гость подавляет свою ambient-толпу |
| `npc_visitor_vehicles` | 1 | E7.1/E3d-a: подавлять локальный трафик (машины с хоста) |
| `npc_visitor_preserve_events` | 1 | E7.1: сохранять scenario/vendor/quest/event NPC |
| `npc_visitor_pause_scenes` | 1 | E7.1: паузить cleanup толпы в миссии/катсцене |
| `npc_motion_update` | 1 | E7.2: in-place update go-to задачи (A/B) |
| `npc_motion_marker` | 0 | E7.2: fallback marker-entity follow (A2) |
| `npc_motion_status_p2` | 1 | E7.2: p2 в GET_SCRIPT_TASK_STATUS (A/B) |
| `world_vehicles` | 1 | E3d-a: vehicles-only WorldSnapshot хоста (при `npc_actor=1`) |
| `npc_vehicle_radius` | 80 | 20..200 м, радиус сканирования транспорта (E3d-a) |
| `npc_occupants` | 1 | E3d-c: водители/пассажиры/draft-лошади (0 = старое E3d-a) |
| `npc_animal_appearance` | 0 | E9: assets животных/лошадей (0x1E) + horse tack (0x1C); host+guest синхронно |
| `npc_mount_appearance` | 0 | E10: внешность лошади под NPC-всадником (0x1C); host+guest синхронно |
| `player_frame_hz` | 20 | P0: Player Frame 10..60 Гц (idle heartbeat 5 Гц) |
| `player_frame_change_only` | 1 | P0: слать Frame при изменении/капе, в покое реже |
| `npc_debug` | 0 | probe/perf/visitor/motion/world-логи (host и guest) |

## Откат на предыдущую сборку

Каждая сборка сохраняется локально в `dist\<label>\` (ASI + server + INI +
`README.md` с SHA256, INI и пошаговой инструкцией восстановления). Снапшоты
в `.gitignore` и в репозиторий не попадают.

| label | protocol | ASI | server | README |
|---|---|---|---|---|
| `npc-e16` | v19 | `C4E4CF70` | `1BE28084` | [dist\npc-e16\README.md](dist/npc-e16/README.md) |
| `npc-e15` | v19 | `3CC80BE3` | `1BE28084` | [dist\npc-e15\README.md](dist/npc-e15/README.md) |
| `npc-e14` | v19 | `AD3C3481` | `1BE28084` | [dist\npc-e14\README.md](dist/npc-e14/README.md) |
| `npc-e13` | v19 | `B43B7EE0` | `F3EBD6D1` | [dist\npc-e13\README.md](dist/npc-e13/README.md) |
| `npc-e12` | v19 | `D47B91A5` | `F3EBD6D1` | [dist\npc-e12\README.md](dist/npc-e12/README.md) |
| `npc-e11` | v19 | `785B2D05` | `E26C715A` | [dist\npc-e11\README.md](dist/npc-e11/README.md) |
| `npc-e10` | v19 | `B8BA1B93` | `E26C715A` | [dist\npc-e10\README.md](dist/npc-e10/README.md) |
| `npc-e9` | v19 | `DC0C4AB7` | `E26C715A` | [dist\npc-e9\README.md](dist/npc-e9/README.md) |
| `npc-e8.1` | v19 | `1EB0A633` | `E26C715A` | [dist\npc-e8.1\README.md](dist/npc-e8.1/README.md) |
| `npc-e8` | v19 | `9E3E8D7A` | `E26C715A` | [dist\npc-e8\README.md](dist/npc-e8/README.md) |
| `npc-e7.3` | v18 | `C9BFA565` | `BEC3428B` | [dist\npc-e7.3\README.md](dist/npc-e7.3/README.md) |
| `npc-e7.2` | v18 | `E8971A62` | `C4DD6B1A` | [dist\npc-e7.2\README.md](dist/npc-e7.2/README.md) |

**Короткий откат:** закрыть игры/сервер → ASI из снапшота в
`E:\Games\Red Dead Redemption 2\scripts\rdr2_coop.asi` → server в
`E:\rdr2-coop\build-server\Release\rdr2_coop_server.exe` → INI привести к
значениям из снапшота → запустить сервер и проверить протокол по JOIN.
Компоненты **только из одного снапшота**: v18↔v19 несовместимы
(`VersionMismatch`). Полная процедура, структура `dist\<label>` и команда
создания снапшота — в [tools/README.md](tools/README.md).

Текущий локальный деплой **npc-e16 / v19**: ASI `C4E4CF70`, server
`1BE28084`; **ARINA online** — тот же e16. v18-пара:
ASI `C9BFA565` / server `BEC3428B`; v17: ASI `35163CC2` / server
`B7AAABAE`; v16: ASI `63388AD3` / server `4EDAE4B6`; v15:
`dist\npc-e2c-v15\`. v15–v19 несовместимы (`JOIN` → `VersionMismatch`),
обновлять/откатывать нужно **все компоненты сразу**.

**Правило сборок:** канонические `build/` и `build-server/` обновляет только
Lead; WIP-сборки — в отдельных каталогах; `dist/` — локальные снапшоты, в
git не коммитятся.

## Запуск сервера

```
E:\rdr2-coop\run_server.bat 7777
```

или вручную:

```
rdr2_coop_server.exe 7777
```

Сервер хранит последние позиции игроков в `players.txt`, пишет лог в консоль.
Порт должен быть доступен клиентам (локальная сеть или туннель).

## Протокол

Все сообщения: `u32 length | u8 type | payload`. **Актуальная версия — v19** (actor-репликация NPC E1–E16: ActorAppearance 0x1E + ActorDamage 0x1F + PlayerPresence 0x20 + VehicleOccupants 0x21 + TrainSnapshotV2 0x22; E9–E16 используют существующие 0x1E/0x1C/0x21 без новых сообщений). Клиенты v14–v19 несовместимы: сервер отклоняет JOIN с `VersionMismatch`. v18-пара: ASI `C9BFA565`/server `BEC3428B`; v17 — `35163CC2`/`B7AAABAE`; v16 — `63388AD3`/`4EDAE4B6`; v15 — `dist\npc-e2c-v15\`.

| type | сообщение      | направление            | payload |
|------|----------------|------------------------|---------|
| 0x01 | Join           | клиент → сервер        | имя, модель, protocol_version |
| 0x02 | JoinOk         | сервер → клиент        | sid, сохранённая позиция, isHost |
| 0x03 | PlayerJoined   | сервер → остальным     | имя, модель, позиция |
| 0x04 | Frame          | клиент → сервер → все  | `FramePayload` (88 байт) |
| 0x05 | Leave          | клиент → сервер        | — |
| 0x06 | PlayerLeft     | сервер → остальным     | имя |
| 0x07 | Died           | клиент → сервер → все  | имя |
| 0x08 | WorldState     | хост → сервер → все    | время, погода |
| 0x09 | Outfit         | клиент → сервер → все  | `OutfitState` (784 байта, v14): model + components[96] + wearable[96] |
| 0x0A | Respawned      | клиент → сервер → все  | имя |
| 0x0B | Shot           | клиент → сервер → все  | точка попадания (x,y,z) |
| 0x0C | Ping           | клиент → сервер        | метка времени (u64) |
| 0x0D | Pong           | сервер → клиент        | та же метка (RTT) |
| 0x0E | HorseOutfit    | клиент → сервер → все  | `OutfitState` (784 байта): снаряжение лошади |
| 0x0F | JoinRejected   | сервер → клиент        | причина (версия/полный/ник) |
| 0x10 | WorldSnapshot  | хост → сервер → гость  | P1; E3d-a: vehicles-only при `world_vehicles=1` (≤16×`WorldEntity` 32 B, радиус 80 м, пустой snapshot удаляет прокси) |
| 0x11 | ActionState    | клиент → сервер → все  | P2, флаг выключен |
| 0x12 | PlayerVehicle  | клиент → сервер → все  | P2, флаг выключен |
| 0x13 | MissionEvent   | хост → сервер → гость  | P3, флаг выключен |
| 0x14 | MissionState   | хост → сервер → гость  | P3, флаг выключен |
| 0x15 | MissionBarrier | host ↔ guest           | P3, флаг выключен |
| 0x16 | HostChanged    | сервер → клиент        | смена роли (host migration hardening: 4 с game-timeout, role-change reset) |
| 0x17 | TrainSnapshot  | хост → сервер → гость  | устаревший R&D proxy-attach; заменён v19 TrainSnapshotV2 0x22 |
| 0x18 | ActorSpawnBatch | хост → сервер → гость | v15–v19 E1–E16: до 12×`ActorSpawn` (64 B), max 776 B; host-only |
| 0x19 | ActorFrames   | хост → сервер → гость  | v15–v19: до 12×`ActorFrame` (32 B), max 392 B; движение/флаги боя |
| 0x1A | ActorDespawn  | хост → сервер → гость  | v15–v19: epoch/actorId/reason, 12 B; host-only |
| 0x1B | ActorEvent    | хост → сервер → гость  | v15–v19: событие актора, 32 B; host-only (в текущем клиенте не применяется) |
| 0x1C | ActorOutfit   | хост → сервер → гость  | v15–v19: epoch+actorId+`OutfitState` 784 B = 792 B; host-only; E9 horse tack (kind2), count=0 не шлётся |
| 0x1D | ActorResync   | гость → сервер → хост  | v15–v19: resync-запрос, 12 B; только от гостя и только хосту |
| 0x1E | ActorAppearance | хост → сервер → гость | v16–v19 E2c-b: header 12 B + count×28 B (count 1..16, max 460 B); host-only relay; точный размер `12+count*28` |
| 0x1F | ActorDamage   | гость → сервер → хост  | v17 E5: 16 B `{epoch u32, actorId u32, damage u16, pad u16, weaponHash u32}`; только гостю и только хосту; damage 1..1000 |
| 0x20 | PlayerPresence | клиент → сервер → другому | v18 E6: 8 B `{epoch u32, flags u16, pad u16}`; без эха; rate-limit 10/с; flags in_map/pause/satchel/shop/cutscene/menu_other |
| 0x21 | VehicleOccupants | хост → сервер → гость | v19 E3d-c: 8 B header + count×8 B (count 0..8, max 72 B); host-only; rate-limit 20/с; count=0 = clear; seat -1..8, flags driver/passenger/draftHorse |
| 0x22 | TrainSnapshotV2 | хост → сервер → гость | v19 E3d-b: 48 B `{epoch,trainId,configHash,flags,stationIndex,carCount,speed±40,xyz,heading,trackIndex,pad2[8]}`; host-only; ≤4/с; carCount 1..16 |

Полная спецификация: [docs/PROTOCOL.md](docs/PROTOCOL.md).

`FramePayload` (88 байт): позиция (x,y,z), heading, `move`, `state`, `flags`,
`action`, здоровье, точка прицела (aimX/Y/Z + дистанция), хэш оружия, `seq`,
резерв (модель лошади, мета-наряд игрока, модель персонажа), наряд лошади.

`OutfitState` (784 байта, v14): `model` (u64) + `count` (u32) +
`components[96]` (u32, shop-предметы) + `wearable[96]` (u32, wearable states
слоёв одежды). Сервер проверяет точный размер и ретранслирует byte-identical;
сообщения 0x09 Outfit и 0x0E HorseOutfit используют одну и ту же структуру.

**v15 (NPC E1–E3b base):** actor-сообщения 0x18–0x1D — host-only relay, exact
size, count ≤12, epoch/actorId != 0, finite coords. Используются
`ActorSpawnBatch`/`ActorFrames`/`ActorDespawn` (E1 + heartbeat + kind
human/animal/horse), `ActorResync` 0x1D (E1); `ActorEvent` зарезервирован,
`ActorOutfit` 0x1C используется E9 horse tack. E2a/E2b/E3a/E3b/E3c
передаются через поля и флаги тех же сообщений: `metaOutfit`, `flags`
(mounted/dead/drawn/combat/scenario/reloading/aiming), `reserved` (mount
model), `scenarioHash`, `weaponHash`, `targetActorId`.

**v16 (E2c-b):** добавлено **`ActorAppearance` (0x1E)** — host-only relay,
header 12 B `{epoch u32, actorId u32, count u8, pad[3]}`, entry 28 B
`{drawable, albedo, normal, material, palette u32; tint0/1/2 i16; reserved
u16}`, count 1..16, max payload 460 B, точный размер `12 + count*28`;
reserved каждой записи = 0. Хост шлёт snapshot при спавне/изменении/join
(budget 1 актор/тик, период 2 с), гость применяет через reset +
`_SET_META_PED_TAG` + commit + `UPDATE_PED_VARIATION` (readback/fallback);
asset приоритетнее metaOutfit. v15↔v16↔v17↔v18↔v19 несовместимы.

**v17 (E5):** добавлено **`ActorDamage` (0x1F)** — 16 B
`{epoch u32, actorId u32, damage u16, pad u16, weaponHash u32}`; направление
**гость → сервер → хост** (host-only relay, отправитель-хост дропается;
серверный rate-limit 32/с). Семантика host-authoritative: гость только
просит урон, хост применяет `APPLY_DAMAGE_TO_PED` (fallback
`SET_ENTITY_HEALTH`) с лимитами ≤8/тик, ≤15/с, ≤150 м, только live actor из
roster, epoch/actorId/damage валидируются; при hp≤0 — dead-флаг/труп.
E5.1: headshot передаётся как `damage=250`; в логах `headshot=1`.

**v18 (E6/E7):** добавлено **`PlayerPresence` (0x20)** — 8 B
`{epoch u32, flags u16, pad u16}`, любой клиент → сервер → **другому**
клиенту (без эха, rate-limit 10/с, unknown bits drop). flags:
1=in_map, 2=in_pause_menu, 4=in_satchel, 8=in_shop, 16=in_cutscene,
32=in_menu_other; суффикс `Name - in map`/`pause menu`/`in satchel`/`in shop`,
таймаут 12 с. E7.1/E7.2/E7.3 — клиентское поведение visitor mode/плавного
движения/транспорта и migration reset (0x18–0x1D поля/кадры не менялись).
**E3d-a vehicles-only WorldSnapshot (0x10)** и **host migration hardening
(0x16)** — v18 без bump. v17↔v18↔v19 несовместимы.

**v19 (E3d-c/E3d-b):** добавлены **`VehicleOccupants` (0x21)** — 8 B header
`{vehicleNetId u32, count u8, pad[3]}` + count×8 B
`{actorId u32, seat i8, flags u8, carIndex u8, harness u8}`, count 0..8,
max 72 B; flags driver(1)/passenger(2)/draftHorse(4); `count=0` очищает
occupants; host-only, 20/с. **`TrainSnapshotV2` (0x22)** — 48 B
`{epoch,trainId,configHash,flags,stationIndex,carCount,speed,xyz,heading,
trackIndex,pad2[8]}`; flags waiting(1)/halted(2)/reversing(4)/
playerAboard(8); carCount 1..16, speed ±40; host-only, ≤4/с. Сервер
валидирует размер/count/флаги/epoch/actorId/netId/speed и релеит
byte-identical только гостю.

**INI:** `npc_actor = 1` (включает репликацию; по умолчанию 0),
`npc_cap = 8` (1..12), `npc_radius = 60` (20..100 м),
`npc_appearance = 1` (meta-outfit E2a + asset/tint E2c-b),
`npc_animal_cap = 4` (0..12), `npc_damage = 1` (E5),
`npc_corpse_max_sec = 300`, `npc_presence = 1`, `npc_visitor = 1`,
`npc_visitor_vehicles = 1`, `npc_visitor_preserve_events = 1`,
`npc_visitor_pause_scenes = 1`, `npc_motion_update = 1`,
`npc_motion_marker = 0`, `npc_motion_status_p2 = 1`,
`world_vehicles = 1`, `npc_vehicle_radius = 80`, `npc_occupants = 1`
(E3d-c), `world_trains = 0` (E3d-b, opt-in), `npc_debug = 1` (диагностика).

Детали — [docs/PROTOCOL.md](docs/PROTOCOL.md), раздел «ACTOR-сообщения».

## Как устроено

- `src/main.cpp` — `DllMain` + цикл скрипта (`ScriptMain`).
- `src/script_hook.cpp/h` — резолв экспортов `ScriptHookRDR2.dll` по PE-таблице
  (проприетарный SDK не нужен).
- `src/native_call.h` — вызов нативов + маршаллинг `Vec3`.
- `src/natives.h` — проверенные хэши нативов (сверены с `rdr2-runtime-scripthook`
  и `rdr3-nativedb-data`).
- `src/coop.cpp` — игровая логика: спавн/рендер педа, движение, одежда, оружие,
  лошадь, смерть/возрождение, время/погода, кадры 60 Гц; E6 presence
  (UIAPP-детект, суффикс ника), E7.1 visitor/mission-aware, role-change reset.
- `src/actor_sync.cpp/h` — NPC-репликация E1–E16: discovery/roster, guest
  proxies, сценарии/смерть, всадники/животные, оружие/бой, appearance,
  host-authoritative damage (E5), corpse retention (E5.2), smooth motion
  (E7.2), occupants/draft (E3d-c), mission-aware/role reset.
- `src/world_sync.cpp` — E3d-a vehicle-прокси и E3d-b mission-train
  (`TrainSnapshotV2`, `SET_RANDOM_TRAINS(false)` + cleanup).
- `src/gen_metaped_outfits.h` — сгенерированная таблица metaped-outfit
  кандидатов (890 моделей / 4568 хэшей) для E2c.
- `tools/fake_actor_client.py` — fake host/guest v18 для однопК-тестов NPC
  (см. `tools/README.md`); `src/config.cpp`, `src/persist.cpp`, `src/log.cpp` —
  конфиг, персистентность, логгер.
- `net/protocol.h` — типы сообщений и структуры пакетов.
- `net/client.cpp`, `net/frame.cpp`, `net/socket.cpp` — сетевой клиент,
  сборка/разбор кадров, сокеты.
- `server/server.cpp` — релей, таблица игроков, `players.txt`.
- `dist/` — готовые клиентские пакеты и ini (без сторонних DLL).
- `third_party/` — справочные исходники (ScriptHook runtime, nativedb, каталог
  одежды), используются только для сверки хэшей и параметров.

## Задержка и сеть

- `TCP_NODELAY` включён на клиенте и сервере: без него алгоритм Нагла копит
  мелкие кадры до ~40 мс (плюс delayed ACK) — это главный источник лага.
- Сокеты неблокирующие, у каждого своя очередь на отправку: медленный клиент
  не тормозит ретрансляцию остальным.
- Раз в секунду клиент шлёт `Ping`, сервер отвечает `Pong` с той же меткой;
  RTT пишется в лог плагина (`net: rtt N ms`).

## Практические заметки

- Человеческий пед **не рендерится** без обвязки
  `SET_ENTITY_AS_MISSION_ENTITY` + `SET_PED_RANDOM_COMPONENT_VARIATION` +
  `SET_ENTITY_VISIBLE`.
- `RESET_PED_COMPONENTS` ломает рендер тела (остаются только одёжки) — вместо
  него используется random variation + применение наряда в том же тике.
- Число параметров нативов важно: `GET_CURRENT_PED_WEAPON` принимает 5
  аргументов, `GIVE_WEAPON_TO_PED` — 13; при меньшем числе движок читает мусор.
- Короткие события (выстрел) нельзя опрашивать по кадру — только отдельным
  сообщением/latch.

## Дорожная карта

См. [ROADMAP.md](ROADMAP.md).

## Лицензия

Проект распространяется для некоммерческого ознакомления. Мод одиночного
режима и не затрагивает RDR Online.
