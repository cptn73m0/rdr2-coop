# Протокол связи — RDR2 CO-OP

## Версия протокола (Spike 1 + P1 + P2, модель A)

- `net::kProtocolVersion = 15` (см. `net/protocol.h`). Версия передаётся в JOIN,
  сервер сверяет её и при несовпадении отклоняет подключение
  (`JoinRejected.VersionMismatch`); клиенты v14 и v15 несовместимы.
- v15 (E0): actor-сообщения NPC-репликации — `ActorSpawnBatch` (0x18),
  `ActorFrames` (0x19), `ActorDespawn` (0x1A), `ActorEvent` (0x1B),
  `ActorOutfit` (0x1C), `ActorResync` (0x1D). Пока каркас протокола/сервера:
  клиентская репликация NPC ещё не реализована, `npc_actor=0` по умолчанию.
- v14: `OutfitState` расширен до **784 байт** — добавлен `wearable[96]` (u32)
  для wearable states одежды (открытая/закрытая куртка, рукава и т.п.).
- v13: TrainSnapshot (0x17) — R&D: engine/direction/speed + вагоны по порядку.
- v12: HostChanged (0x16) — сервер повышает гостя до хоста при выходе хоста.
- v11: MissionBarrier (0x15) — P3: offer/ready/start-window/started/abort.
- v10: MissionState (0x14) — P3: хост сообщает активную Story-миссию.
- v9: MissionEvent (0x13) — P3: хост завершил миссию, гость пишет свой зачёт.
- v8: PlayerVehicle (0x12) — посадка удалённого игрока в транспорт.
- v7: WorldSnapshot до 64 сущностей — 32 Ped + 16 Vehicle + 16 mission Object.
- v6: WorldSnapshot до 48 сущностей — 32 Ped + 16 Vehicle (P1 транспорт:
  дилижансы/повозки/лодки/трамваи; поезда пока пропускаются).
- Лошади в RDR2 — это Ped, они приходят в общем списке педов; отдельная
  система синхронизирует лошадь удалённого игрока (посадку/движение).
- v4: `OutfitState` — изначально 400 байт (8 model + 4 count + 96 shop-компонентов).
  Лимит поднят с 32 до 96, потому что `GET_NUM_COMPONENT_CATEGORIES_IN_PED`
  на реальном педе возвращает 47–80 категорий; при 32 хвост одежды обрезался
  и у призрака просвечивали случайные слои/кожа.
  В v14 к `components[96]` добавлен `wearable[96]` → **784 байта** (см. 0x09).
- v5: `ActionState` (0x11) — 24 байта: наблюдаемое действие/сценарий
  (см. раздел 0x11 ниже).
- JOIN (0x01): `name_len:u8  name[]  ped_model:u64  protocol_version:u16` (LE).
- Отказ в JOIN — сообщение **0x0F JoinRejected**: `reason:u8`
  (1 = несовпадение версии, 2 = сервер полон, 3 = ник занят, 4 = пустой ник).
- Сессия на старте: **1 хост + 1 гость** (`net::kMaxPlayers = 2`). Роль
  назначается сервером и приходит в `JOIN_OK` (`is_host`); в логе сервера
  подключение помечается `[HOST]` / `[GUEST]`.

### 0x09 OUTFIT / 0x0E HORSE_OUTFIT (одежда игрока / снаряжение лошади, клиент → сервер → остальные)

```
payload: OutfitState (ровно 784 байта, v14)
  model:u64            — модель педа/лошади (для проверки)
  count:u32            — сколько компонентов заполнено (0..96)
  components:u32[96]   — хэши shop-компонентов одежды/снаряжения
  wearable:u32[96]     — wearable state для каждого компонента
                         (0 = нет состояния; открытая/закрытая куртка,
                          рукава/воротник и т.п.)
```

- v14: к `components[96]` добавлен `wearable[96]` — без него у гостя остаётся
  дефолтное состояние слоя (например, куртка «закрыта»), и рубашка
  просвечивает/пропадает не так, как у оригинала. Размер `OutfitState`
  вырос с 400 до **784 байт**; `static_assert` в `net/protocol.h` фиксирует
  раскладку, менять только вместе с `kProtocolVersion`.
- `count` относится к первым `count` записям `components`; `wearable`
  индексируется теми же позициями.
- Сервер принимает payload ровно `sizeof(OutfitState)` (784 Б) и ретранслирует
  его **byte-identical**; короткий/длинный payload отбрасывается, как и
  `Outfit`, отправленный не в свой тип.
- 0x0E HorseOutfit использует тот же `OutfitState` (модель лошади +
  компоненты снаряжения + их wearable states); сервер проверяет тот же размер.
- Синхронизация wearable states реализована, но **в игре ещё не проверена** —
  входит в двух-ПК чек-лист (`docs/TESTING.md`, раздел 3).

### 0x10 WORLD_SNAPSHOT (P1: хост → сервер → гость)

```
payload: count:u16  pad:u16  count * WorldEntity(32 байта)
WorldEntity:
  netId:u64  model:u32  x:f32 y:f32 z:f32 heading:f32
  kind:u8    flags:u8   state:u8 pad:u8
flags: 0x01 mission-entity, 0x02 dead, 0x04 mounted
state: 0 idle, 1 walk, 2 run (грубая локомоция)
```

- Хост шлёт снапшот ~10 Гц с NPC в радиусе 60 м (**≤64 сущностей**:
  до 32 Ped + 16 Vehicle + 16 mission Object; приоритет mission-entity,
  затем ближайшие). `netId` стабилен в рамках сессии.
- Сервер принимает `WorldSnapshot` **только от хоста** и ретранслирует гостю;
  снапшот от гостя игнорируется (лог `WorldSnapshot от не-хоста`).
- Гость создаёт визуальные прокси-педы, обновляет позиции и подавляет свою
  ambient-популяцию. Включается в `rdr2-coop.ini`: `world_sync = 1` на обоих ПК.
- Это P1-основа: реплицируются Ped (до 32, включая лошадей как педов),
  Vehicle (до 16: дилижансы, повозки, лодки, трамваи) и **только mission
  Object** (до 16; амбиентные пропсы уже есть в мире гостя). Поезда,
  миссионные акторы и точные анимации NPC пока не синхронизируются.
- Транспорт создаётся как mission-прокси без коллизии; салон/водитель,
  тягловые лошади и физика не синхронизируются (ambient-транспорт).
- R&D `world_trains = 1`: поезда/вагоны идут через тот же Vehicle-пайплайн
  как отдельные прокси; состав/рельсы/скорость не моделируются
  (см. `docs/TRAINS.md`).
- Прокси-педы оживлены: host передаёт `state` (idle/walk/run), гость ведёт
  их navmesh-задачей и доворачивает heading; точные клипы (сценарии, бой,
  анимации миссий) пока не передаются.

### 0x11 ACTION_STATE (P2: действие/сценарий, клиент → сервер → остальные)

```
payload: mode:u8 flags:u8 pad:u16
         scenarioHash:u32  animDictHash:u32  animNameHash:u32
         phase:f32  targetNetId:u32        // ровно 24 байта
```

- `mode`: 0 None, 1 Scenario, 2 Cover, 3 Carry, 4 Drag, 5 Hogtie, 6 Loot,
  7 Fishing.
- `flags`: 0x01 aiming, 0x02 shooting, 0x04 reloading, 0x08 in-cover,
  0x10 carrying, 0x20 dragging, 0x40 hogtied, 0x80 fishing.
- `scenarioHash` — `_GET_ACTIVE_DYNAMIC_SCENARIO` (сидение/курение/еда/питьё
  и т.п.); получатель запускает `TASK_START_SCENARIO_IN_PLACE_HASH`.
- `animDictHash`/`animNameHash`/`phase` — резерв под точный клип (публичный
  SDK не отдаёт текущий клип AnimGraph).
- `targetNetId` — цель переноски/лутания: хост маппит свой handle в netId
  (`world_sync`), гость находит прокси и вызывает
  `TASK_PICKUP_CARRIABLE_ENTITY` / `TASK_LOOT_ENTITY`.
- Каждый клиент шлёт состояние **своего** персонажа; сервер ретранслирует.
  Включается в `rdr2-coop.ini`: `action_sync = 1` на обоих ПК.
- Полностью применяются: сценарии (сидение/курение/еда/питьё), вход/выход
  из укрытия, переноска/лутание цели **host→guest**. Для целей нужен
  `world_sync = 1` на обоих ПК.
- Пока не применяются: таскание, хогтай, рыбалка (флаги передаются);
  guest→host цели — нужен обратный world_sync.

### 0x12 PLAYER_VEHICLE (P2: посадка в транспорт, клиент → сервер → остальные)

```
payload: vehicleModel:u32 netId:u32
         x:f32 y:f32 z:f32 heading:f32
         seat:i8 flags:u8 pad:u16 reserved:u32   // ровно 32 байта
```

- `vehicleModel = 0` / без флага Valid — игрок не в транспорте.
- `seat = -1` — место водителя; 0..4 — пассажирские.
- `flags`: 0x01 valid, 0x02 driver.
- Каждый клиент шлёт своё состояние; гость создаёт прокси-транспорт
  (`CREATE_VEHICLE`, mission, без коллизии), обновляет позицию/heading и
  сажает призрака через `SET_PED_INTO_VEHICLE` в то же место.
- Включается в `rdr2-coop.ini`: `vehicle_sync = 1` на обоих ПК.
- Салоны/анимация посадки приблизительные; точные клипы не передаются.

### 0x13 MISSION_EVENT (P3: MissionData-событие, только хост → гость)

```
payload: missionId:u32 rating:u8 flags:u8 pad:u16   // ровно 8 байт
```

- `missionId` — joaat 4/5-символьный MissionData ID из каталога (80 Story-миссий).
- `flags`: 0x01 completed (новое завершение), 0x02 rating update.
- `rating`: MISSION_RATING_* (0..5; 0 = incomplete, 1 skipped, 2 complete,
  3 bronze, 4 silver, 5 gold).
- Отправляет **только хост**; сервер отбрасывает событие от гостя.
- Как хост узнаёт: каждую секунду сравнивает `MISSIONDATA_IS_VALID` /
  `MISSIONDATA_WAS_COMPLETED` / `MISSIONDATA_GET_RATING` для каталога со
  своим baseline. Новое `false→true` completion (или изменение рейтинга)
  уходит гостю.
- Гость всегда логирует событие; при `mission_credit = 1` пишет зачёт в
  **свой** сейв: `_UILOG_MARK_MISSION_COMPLETED` + `_MISSIONDATA_SET_MISSION_RATING`.
  Сейв не копируется; автосейва нет — сохраните игру вручную.
- Перед первым зачётом плагин делает бэкап сейва гостя (каталоги
  `.1911\...\profile` и `Documents\Rockstar Games\...\Profiles`) в
  `Documents\rdr2-coop-saves\<timestamp>`; если бэкап не удался, зачёт
  отменяется. Управляется `save_backup` (по умолчанию 1).
- Опасно при разном прогрессе: зачёт миссии, которую гость ещё не открыл,
  может сломать его прогрессию. Включать `mission_credit` только при
  примерно равном сюжетном прогрессе и после бэкапа сейва.
- Текущая миссия хоста и барьер совместного старта реализованы отдельными
  сообщениями 0x14/0x15; in-game handshake ещё не проверялся. Синхронизируются
  завершения MissionData, активная миссия и координация окна старта.

### 0x14 MISSION_STATE (P3: активная Story-миссия, только хост → гость)

```
payload: missionId:u32 runtimeHash:u32 state:u8 flags:u8 pad:u16  // 12 байт
```

- `state`: 0 idle (миссии нет), 1 active.
- Хост раз в 0.5 с проверяет `GET_MISSION_FLAG` и перебирает каталог:
  для каждой миссии `GET_NUMBER_OF_THREADS_RUNNING_THE_SCRIPT_WITH_THIS_HASH(
  joaat(runtime)) > 0` → эта миссия активна.
- Шлёт смену состояния + heartbeat; сервер принимает только от хоста.
- Гость логирует `mission: host is in …` и показывает субтитр
  `Host mission: <название>`; автостарт миссии у гостя не выполняется.
- Барьер совместного старта (гость должен начать ту же миссию в своём мире)
  реализован протоколом 0x15; in-game handshake ещё не проверялся, поэтому
  для стабильной сборки это по-прежнему информационная синхронизация.

### 0x15 MISSION_BARRIER (P3: барьер старта, host ↔ guest)

```
payload: missionId:u32 epoch:u32 phase:u8 flags:u8 pad:u16  // 12 байт
```

- Фазы: 1 Offer, 2 Ready, 3 StartWindow, 4 Started, 5 Abort.
- Host шлёт Offer/StartWindow/Abort; guest — Ready/Started. Сервер проверяет
  роль и отбрасывает фазу не от той стороны.
- Host: при появлении активной миссии → Offer; получает Ready с флагом
  Eligible → StartWindow (окно 45 с) и ждёт Started. Если гость не начал
  миссию за 45 с — Abort(Timeout). Offer повторяется, пока гость не ответил
  (поздний Join).
- Guest: на Offer проверяет **свой** MissionData (valid, required Story,
  не completed) и отвечает Ready(Eligible); на StartWindow показывает
  `START NOW: <mission>` и ждёт, пока его собственный runtime-скрипт миссии
  станет активным → Started. Автостарта миссии нет — игрок запускает её сам.
- Ограничение: хост детектит миссию уже **после** её старта, поэтому барьер
  координирует окно, а не удерживает миссию до готовности гостя.

### 0x16 HOST_CHANGED (сервер → клиент, host migration)

```
payload: isHost:u8 pad[3]   // 4 байта
```

- Когда хост отключается, сервер выбирает первого оставшегося игрока,
  ставит ему `isHost=true` и рассылает каждому `HostChanged` с его ролью.
- Клиент обновляет `isHost`, сбрасывает/перестраивает P1-прокси world_sync
  и показывает субтитр «You are now HOST/GUEST».
- После этого бывший гость может слать WorldSnapshot/MissionEvent/MissionState:
  сервер валидирует host-only сообщения по новому хосту.

### 0x17 TRAIN_SNAPSHOT (R&D: поезд/состав, только хост → гость)

```
payload: TrainSnapshotHeader (16 B)
         engineNetId:u32 engineModel:u32 speed:f32
         direction:u8 carCount:u8 pad:u16
         carCount * WorldEntity (32 B; kind=Vehicle)
```

- `carCount ≤ 16`; сервер проверяет точный размер и host-only.
- Хост раз в WorldSnapshot-тик (если `world_trains=1`) дополняет состав через
  `_GET_TRAIN_CARRIAGE_TRAILER_NUMBER` + `GET_TRAIN_CARRIAGE` и шлёт
  direction/speed/список вагонов.
- Гость пока **только логирует** (R&D): `train: snapshot engine=... cars=...`.
- Визуально вагоны идут обычным WorldSnapshot-пайплайном, если попали в
  бюджет 16 Vehicle. Сцепка/рельсы/порядок на госте ещё не применяются.

Обновление v13 (R&D): гость **привязывает** вагоны-прокси к локомотиву
через `ATTACH_ENTITY_TO_ENTITY` с локальными offset; `UpdateProxy` не
перетирает привязанные вагоны; при удалении локомотива вагоны отцепляются.
Это экспериментально и требует игровой проверки.

## E0 (v15): ACTOR-СООБЩЕНИЯ (NPC-репликация, каркас)

E0 — этап каркаса: протокол, серверный relay/валидация и headless-тесты.
Клиентская репликация NPC (discovery/спавн/движение/анимации) появится в
E1+, флаг `npc_actor` по умолчанию **0**; в игре не проверялось.
Все структуры — little-endian, фиксированы `static_assert` в `net/protocol.h`.
`kMaxActors = 12`.

Правила сервера:
- **0x18–0x1C принимаются только от хоста** (как WorldSnapshot): от гостя —
  лог и игнор; payload валидируется (точный размер, count ≤12, epoch != 0,
  actorId != 0, конечные координаты `|v| < 100000`) и ретранслируется
  остальным **byte-identical**; невалидный payload дропается.
- В `ActorSpawnBatch`/`ActorFrames` `actorId` не должен повторяться внутри
  одного батча (проверка O(count²)): duplicate-батч дропается целиком;
  `count=0` и одиночные батчи валидны.
- **0x1D принимается только от гостя и ретранслируется только хосту**
  (не broadcast); от хоста — лог и игнор.

### 0x18 ACTOR_SPAWN_BATCH (хост → сервер → гость)
```
payload: ActorSpawnBatchHeader (8 B) + count * ActorSpawn (64 B)
         max = 8 + 12*64 = 776 B
header: epoch:u32  count:u16  pad:u16
ActorSpawn (64 B):
  actorId:u32        — стабильный id в рамках epoch (не 0)
  epoch:u16
  kind:u8            — 0 = human
  flags:u8           — bit0 mounted, 1 dead, 2 drawn, 3 changing,
                       4 combat, 5 scenario, 6 reloading, 7 aiming
  model:u32  metaOutfit:u32  mountActorId:u32
  weaponHash:u32  scenarioHash:u32
  x,y,z,heading:f32
  health:u8  moveStyle:u8  phase:u8  pad:u8
  vx,vy,vz:i16  pad2:u16  reserved:u32  reserved2:u32
```
### 0x19 ACTOR_FRAMES (хост → сервер → гость)
```
payload: ActorFramesHeader (8 B) + count * ActorFrame (32 B)
         max = 8 + 12*32 = 392 B
header: epoch:u32  seq:u16  count:u16
ActorFrame (32 B):
  actorId:u32  flags:u16  animState:u8  speed:u8
  x,y,z,heading:f32
  targetActorId:u32  seq:u16  pad:u16
```
### 0x1A ACTOR_DESPAWN (хост → сервер → гость, 12 B)
```
epoch:u32  actorId:u32  reason:u8  pad[3]
```
### 0x1B ACTOR_EVENT (хост → сервер → гость, 32 B)
```
epoch:u32  actorId:u32  type:u8  flags:u8  pad:u16
a:u32  b:u32  x:u32  y:u32  z:u32   // формат полей задаёт игровой слой
```
### 0x1C ACTOR_OUTFIT (хост → сервер → гость, 792 B)
```
epoch:u32  actorId:u32  + OutfitState (784 B; см. 0x09)
```
### 0x1D ACTOR_RESYNC (гость → сервер → хосту, 12 B)
```
epoch:u32  lastSeq:u32  reason:u8  pad[3]
```
- Назначение: гость сообщает хосту, что потерял поток кадров (epoch/seq) и
  просит resync; сервер пересылает запрос только текущему хосту.
- E0 не реализует обработку на хосте/госте — только транспорт и контракт.

### Reconnect (клиент)

- При разрыве TCP клиент автоматически переподключается с backoff
  (1 → 2 → 4 → 8 → макс. 10 с), повторяя JOIN с тем же ником/версией.
- Потеря соединения чистит удалённого педа; после успешного JOIN он
  восстанавливается из кадров/PLAYER_JOINED.
- JOIN_REJECTED: `VersionMismatch` и `BadName` — фатальны (без бесконечных
  попыток); `ServerFull` и `NameTaken` — не фатальны, клиент повторяет JOIN
  с backoff (сервер может ещё держать старую сессию до idle-timeout).
- После сетевого сбоя клиент считается «живым» до `kJoinTimeoutMs`/Pong-watchdog;
  при отсутствии Pong дольше 5 с соединение пересоздаётся.

- Этот раздел описывает актуальные для стабильной сборки изменения (Spike 1/P1/P2/P3);
  базовые разделы ниже сохранены как историческая спецификация Spike 1, но
  ключевые форматы (JOIN/JOIN_OK, FRAME 88 B, OUTFIT 784 B) обновлены под
  v14/v15 (v15 добавляет actor-сообщения E0, см. раздел выше).

Сетевой стек поверх **сервера-релея**. Один сервер, несколько клиентов
(ASI-плагинов). Клиент периодически шлёт свой `GhostFrame`, сервер
пересылает его остальным подключённым клиентам. Сервер хранит список
игроков и их last-position.

## Топология

```
[игрок A — ASI-плагин]  ⇄  [сервер-релей]  ⇄  [игрок B — ASI-плагин]
```

- Клиент отправляет: JOIN, FRAME, LEAVE, OUTFIT/HORSE_OUTFIT, PING; хост
  дополнительно — WorldSnapshot, MissionEvent/MissionState/MissionBarrier,
  actor-сообщения 0x18–0x1C (при включённых флагах); гость — ActorResync 0x1D.
- Сервер рассылает: JOIN_OK/JOIN_REJECTED, PLAYER_JOINED, PLAYER_LEFT, FRAME,
  OUTFIT/HORSE_OUTFIT, HOST_CHANGED, host-only сообщения от хоста и actor-
  сообщения 0x18–0x1C; ActorResync — только хосту.

## Транспорт

- TCP (надёжная доставка — проще и достаточно для частоты кадров
  ~60 кадров/с на игрока; FRAME 88 B ≈ 5 КБ/с).
- Кроссплатформенный сервер: POSIX/BSD-сокеты на Linux и Winsock на
  Windows за тонкой общей обёрткой.
- Порядок байт — **little-endian** (x86/x64). Без сети это пока ок;
  при необходимости добавим явную конвертацию.

## Кадр (frame)

Все сообщения оборачиваются в общий кадр:

```
┌───────────────┬──────────┬───────────────┐
│ len : u32      │ type : u8│ payload ...    │
└───────────────┴──────────┴───────────────┘
```

- `len` — длина payload (без 4 байт len и 1 байта type).
- `type` — код команды (см. ниже).
- Всё little-endian.

## Команды

### 0x01 JOIN (клиент → сервер)
```
payload: name_len:u8  name[name_len]  ped_model:u64  protocol_version:u16
```
Запрос клиента подключиться под именем `name` со своей моделью персонажа
`ped_model` (хэш модели, чтобы другие игроки видели правильный скин) и
версией протокола `protocol_version` (обязательна; при несовпадении сервер
отвечает `JoinRejected.VersionMismatch`).

### 0x02 JOIN_OK (сервер → клиент)
```
payload: session_id:u64  has_saved:u8  [x:f32 y:f32 z:f32 heading:f32]  is_host:u8
```
- `has_saved=1` → сервер вернул сохранённые координаты (персистентность),
  клиент спавнит педа на этих координатах.
- `has_saved=0` → новый игрок, спавн рядом с хостом.
- `is_host` идёт последним байтом (после координат, если они есть).
- Инвариант: **первое сообщение новичку — всегда JoinOk или JoinRejected**
  (сервер отправляет JoinOk под тем же мьютексом, что и регистрацию, чтобы
  `PlayerJoined` от другого потока не мог прийти раньше).

### 0x03 PLAYER_JOINED (сервер → все остальные клиенты)
```
payload: name_len:u8  name[name_len]  ped_model:u64  has_pos:u8  [x:f32 y:f32 z:f32 heading:f32]
```
Другой игрок подключился — клиент спавнит его педа. `has_pos=1` → за
`ped_model` идут последние/сохранённые координаты игрока (точка спавна
педа); `has_pos=0` → позиции нет (новый игрок; спавн рядом с хостом).
То же сообщение сервер шлёт новичку про игроков, уже находящихся онлайн.

### 0x04 FRAME (клиент → сервер → остальные клиенты)
```
payload: x:f32 y:f32 z:f32 heading:f32 move:u8 state:u8 flags:u8 pad0:u8
         health:f32 weaponHash:u64 seq:u64 reserved:u64[3]
         horseOutfitHash:u64
         aimX:f32 aimY:f32 aimZ:f32 aimDist:f32
                                                         (всего 88 байт)
```
Состояние игрока (трансформ + локомоция + особые состояния + здоровье +
оружие + точка прицела + порядковый номер). Сервер релеит всем остальным
клиентам в исходном виде.
- `move` — MoveState: 0=Idle, 1=Walk, 2=Run, 3=RideHorse.
- `state` — PedState: 0=Normal, 1=Ragdoll, 2=InCover, 3=Swimming,
  4=SwimUnderwater, 5=Climbing, 6=Jumping, 7=Falling.
- `flags` — битовые: 0x01=Sprinting, 0x02=WeaponReady.
- `health` — здоровье 0..1.
- `weaponHash` — хэш оружия в руке.
- `reserved[0]` — хэш модели лошади, если игрок верхом (`GET_ENTITY_MODEL(GET_MOUNT)`).
- `reserved[1]` — хэш мета-наряда игрока (`_GET_PED_META_OUTFIT_HASH`) — для
  синхронизации внешнего вида персонажа (наряд применяется на стороне
  получателя через `_REQUEST_META_PED_OUTFIT` + `_APPLY_PED_META_PED_OUTFIT`).
- `reserved[2]` — модель персонажа игрока (`GET_ENTITY_MODEL(playerPed)`) — пед
  пересоздаётся с этой моделью (копия Артура/Джона).
- `horseOutfitHash` — мета-наряд (снаряжение) лошади, если игрок верхом —
  применяется к лошади педа-призрака тем же механизмом, что наряд игрока.
ВНИМАНИЕ: `FramePayload` из `net/protocol.h` — struct размером ровно **88 байт**
(из-за выравнивания u64). Ручные сборщики кадров (тестовые PS-скрипты) обязаны
слать все 88 байт, иначе сервер отбросит кадр
(`pay.size() == sizeof(FramePayload)`).

### 0x05 LEAVE (клиент → сервер)
```
payload: (пусто)
```
Клиент сообщает о выходе. Сервер рассылает PLAYER_LEFT и сохраняет
последнюю известную позицию этого игрока.

### 0x06 PLAYER_LEFT (сервер → остальные клиенты)
```
payload: name_len:u8  name[name_len]
```
Другой игрок ушёл — убрать его педа и метку.

### 0x09 OUTFIT / 0x0E HORSE_OUTFIT (клиент → сервер → остальные)
```
payload: OutfitState — ровно 784 байта (model:u64 + count:u32 +
         components:u32[96] + wearable:u32[96]); подробно — раздел 0x09 выше.
```
Сервер проверяет точный размер (784) и ретранслирует byte-identical;
`wearable` передаёт состояния слоёв одежды/снаряжения (v14).

## Map прошёл

- `len`, `type` — общий кадр.
- Все числовые поля — little-endian.
- Строки — `name_len:u8` + байты (без UTF-8 гарантий в этой версии;
  имена ASCII-совместимые).

## GhostFrame ↔ провод

`GhostFrame` из `src/ghost_frame.h` сериализуется в payload команды FRAME
один-в-один:
- x, y, z — позиция
- heading — поворот
- move — enum `MoveState` (Idle=0, Walk=1, Run=2, RideHorse=3)
- seq — порядковый номер кадра для интерполяции/обнаружения потерь.