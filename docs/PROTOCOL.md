# Протокол связи — RDR2 CO-OP

## Версия протокола (Spike 1 + P1 + P2, модель A)

- `net::kProtocolVersion = 20` (см. `net/protocol.h`, ветка `feature/v20-p1`).
  Версия передаётся в JOIN, сервер сверяет её и при несовпадении отклоняет
  подключение (`JoinRejected.VersionMismatch`); клиенты v14–v20 несовместимы.
  **v20 — жёсткий bump:** v19-клиенты и v20-клиенты несовместимы, deploy только
  парой ASI+server из одного снапшота (`dist\npc-e20`); задеплоенная сейчас
  пара — **npc-e19/v19** (ASI `DD0EE210`/server `DD9BD9D8`) — откат на неё
  полной парой. Предыдущие пары: v18 — ASI `C9BFA565`/server `BEC3428B`;
  v17 — `35163CC2`/`B7AAABAE`; v16 — `63388AD3`/`4EDAE4B6`; v15 —
  `dist\npc-e2c-v15\`.
- v20 (P1, guest influence): **`VehicleInteraction` (0x23)** — интент гостя
  (урон/двери/посадка/hijack/loot/hitch), guest → сервер → хост;
  **`VehicleInteractionResult` (0x24)** — ответ хоста host → сервер → гость;
  **`VehicleState` (0x26)** — authoritative состояние транспорта host → сервер
  → гость; **0x25/0x27 зарезервированы** под P3 (управление) и в P1 не шлются.
  Wire-часть/релей реализованы (U1/U2); host apply/guest capture — U3/U4.
  Спецификация — раздел «VEHICLE-СООБЩЕНИЯ (v20 P1)».
- v19 (E3d-b/E3d-c): **`VehicleOccupants` (0x21)** — водители/пассажиры/
  draft-лошади, host → сервер → гость; **`TrainSnapshotV2` (0x22)** —
  mission-train/состав, host → сервер → гость. Спецификация — раздел
  «ACTOR-СООБЩЕНИЯ».
- **e18/e19 (без изменения версии/провода):** soft host migration
  (демоция без закрытия сокета, join grace 15 с, presence-liveness),
  visitor vendor dedupe, имя игрока на карте/blip (`_SET_BLIP_NAME`),
  occupant-first roster (кучер/draft-лошади повозки) и opt-in wheel A/B
  (`npc_vehicle_wheels`, default 0). Провод v19 не менялся.
- v18 (E7.3, без bump): **E3d-a vehicles-only WorldSnapshot** (0x10),
  host migration hardening (game-timeout 4 с, Ping не считается), client
  role-change reset и mission-aware visitor (`npc_visitor_pause_scenes`).
- v17 (E5): **`ActorDamage` (0x1F)** — host-authoritative урон по NPC,
  гость → сервер → хост. E5.1: хедшот → `damage=250` (ваншот). Подтверждено
  в игре. Спецификация — раздел «ACTOR-СООБЩЕНИЯ».
- v18 (E6): **`PlayerPresence` (0x20)** — UI-статус игрока (карта/пауза/
  сатчель/магазин), клиент → сервер → другому клиенту без эха; суффикс имени
  `Name - in map` и т.п. Спецификация — раздел «ACTOR-СООБЩЕНИЯ».
- v16 (E2c-b): к actor-сообщениям добавлен **`ActorAppearance` (0x1E)** —
  asset/tint внешность NPC, хост → сервер → гость. Реализованы host capture
  (компоненты/asset guids/tints) и guest apply; проверено на одном ПК
  (host `capture count=8-10 valid=8-10`, fake guest `appearance_msgs=504
  appearance_actors=8 entry_max=10 errors=0`). Визуальная двух-ПК приёмка —
  pending. Спецификация — раздел «ACTOR-СООБЩЕНИЯ».
- v15 (E1–E3b): actor-репликация NPC — `ActorSpawnBatch` (0x18),
  `ActorFrames` (0x19), `ActorDespawn` (0x1A), `ActorEvent` (0x1B),
  `ActorOutfit` (0x1C), `ActorResync` (0x1D). Реализованы E1 (human-only),
  E2a (meta-outfit), E2b (всадники), E3a (сценарии/смерть), E3b (оружие/бой)
  и E2c-a (host brute-force outfit resolver); проверено на одном ПК (fake
  host/guest), `npc_actor=0` по умолчанию.
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

### 0x10 WORLD_SNAPSHOT (P1 / E3d-a vehicles-only: хост → сервер → гость)

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
  ambient-популяцию. Полный P1 включается `world_sync = 1` на обоих ПК.
- **E3d-a vehicles-only (v18):** при `world_sync=0 && npc_actor=1 &&
  world_vehicles=1` хост шлёт `WorldSnapshot` только с Vehicle: до 16 машин
  в радиусе `npc_vehicle_radius=80` (20..200 м) вокруг обоих игроков, раз в
  6 тиков; пустой snapshot каждые 6 тиков удаляет прокси. Гость создаёт
  `CREATE_VEHICLE` mission/invincible/no-collision, dedupe `engineNetId`,
  timeout 3 с; водители/пассажиры не входят как human actors (E3d-c).
  Логи `world: vehicles send n=`, `world: vehicle spawn/despawn`.
- Это P1-основа: реплицируются Ped (до 32, включая лошадей как педов),
  Vehicle (до 16: дилижансы, повозки, лодки, трамваи) и **только mission
  Object** (до 16; амбиентные пропсы уже есть в мире гостя). Поезда,
  миссионные акторы и точные анимации NPC пока не синхронизируются.
- Транспорт создаётся как mission-прокси без коллизии; салон/водитель,
  тягловые лошади и физика не синхронизируются (ambient-транспорт).
- R&D `world_trains = 1` **устарел** (proxy-attach, не развивать); план
  mission-train v19 — `docs/TRAINS.md`.
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

### 0x16 HOST_CHANGED (сервер → клиент, host migration hardening)

```
payload: isHost:u8 pad[3]   // 4 байта
```

- Когда хост отключается, сервер выбирает первого оставшегося игрока,
  ставит ему `isHost=true` и рассылает каждому `HostChanged` с его ролью.
- **Host migration hardening (v18):** сервер закрывает зависший хост по
  game-timeout **4 с** (4-й argv 2..15 с; idle timeout 30 с отдельно).
  Liveness продлевают только game-thread сообщения (Frame/Died/Respawned/
  WorldState/Outfit/Shot/HorseOutfit/WorldSnapshot/ActionState/PlayerVehicle/
  Mission*/TrainSnapshot/actor 0x18–0x1E); **Ping/Pong не считаются**.
  Затем `[!] host game-timeout (N ms), promoting remaining` → close →
  PlayerLeft + promote + HostChanged.
- Клиент при `HostChanged` делает явный role-change reset: world_sync::Reset,
  visitor cleanup, mission barrier (host/guest+очередь), presence epoch,
  action/aim/shoot, remote player vehicle, `actor_sync::Reset()` +
  `ForceFullSpawnBatch()` (новый хост) / `RequestImmediateResync()` (новый
  гость); лог `coop: role change -> HOST/GUEST reset complete`.
- После этого бывший гость может слать WorldSnapshot/MissionEvent/MissionState:
  сервер валидирует host-only сообщения по новому хосту. Мир при миграции
  **не переносится** — новый хост отдаёт свой.

### 0x17 TRAIN_SNAPSHOT (устаревший R&D proxy-attach; заменён v19 0x22)

```
payload: TrainSnapshotHeader (16 B)
         engineNetId:u32 engineModel:u32 speed:f32
         direction:u8 carCount:u8 pad:u16
         carCount * WorldEntity (32 B; kind=Vehicle)
```

- `carCount ≤ 16`; сервер проверяет точный размер и host-only.
- **Статус: устаревший R&D** — proxy-attach `world_trains=1` не развивается,
  не включать. Mission-train реализован в **v19 `TrainSnapshotV2` 0x22**
  (+ `VehicleOccupants` 0x21); см. `docs/TRAINS.md`.
- Старое поведение (только для истории): хост при `world_trains=1` дополнял
  состав через `_GET_TRAIN_CARRIAGE_TRAILER_NUMBER`/`GET_TRAIN_CARRIAGE`,
  гость логировал `train: snapshot`, вагоны привязывались через
  `ATTACH_ENTITY_TO_ENTITY`; сцепка/рельсы/скорость не моделировались.

## ACTOR-СООБЩЕНИЯ (v19, E1–E9)

v19 содержит реализованную actor-репликацию **людей, животных и свободных
лошадей + транспорт**: E1 — discovery/lifecycle/движение; E2a — meta-outfit;
E2b — всадники; E2c-a/b — внешность (outfit + asset/tint 0x1E); E3a —
сценарии, смерть/возрождение; E3b — оружие/бой; E3c — животные/лошади;
E5/E5.1/E5.2/E5.3 — урон/трупы/mounted; **E6 — PlayerPresence 0x20**;
E7.1/E7.2/E7.3 — visitor/motion/vehicles+host-migration reset;
**E3d-c — VehicleOccupants 0x21**; **E3d-b — TrainSnapshotV2 0x22**.
Всё проверено на одном ПК (fake host/guest + реальный хост) и включается
флагом `npc_actor=1` (default 0; при выключенном флаге не должно быть
`actor:`-логов). Все структуры little-endian, фиксированы `static_assert` в
`net/protocol.h`. `kMaxActors = 12`; `npc_cap` 1..12 (default 8),
`npc_radius` 20..100 м (default 60), `npc_animal_cap` 0..12 (default 4),
despawn-гистерезис +10 м.

Правила сервера:
- **0x18–0x1C и 0x1E принимаются только от хоста** (как WorldSnapshot): от
  гостя — лог и игнор; payload валидируется (точный размер, count ≤12,
  epoch != 0, actorId != 0, конечные координаты `|v| < 100000`) и
  ретранслируется остальным **byte-identical**; невалидный payload дропается.
- **0x1E дополнительно:** count 1..16, точный размер `12 + count*28`, максимум
  460 B, `reserved=0` каждой записи; иначе дроп.
- В `ActorSpawnBatch`/`ActorFrames` `actorId` не должен повторяться внутри
  одного батча (проверка O(count²)): duplicate-батч дропается целиком;
  `count=0` и одиночные батчи валидны.
- **0x1D принимается только от гостя и ретранслируется только хосту**
  (не broadcast); от хоста — лог и игнор.
- **0x1F ActorDamage принимается только от гостя и ретранслируется только
  хосту**; от хоста — drop/лог. Валидация: размер 16 B, epoch/actorId != 0,
  damage 1..1000; серверный rate-limit 32/с на клиента, лишние молча
  дропаются. Хост сам перепроверяет epoch/actorId/живой actor/дистанцию.
- **0x20 PlayerPresence принимается от любого клиента и ретранслируется
  ДРУГОМУ клиенту без эха**; валидация: размер 8 B, epoch != 0, flags без
  неизвестных битов; rate-limit 10/с. При одном подключённом некому релеить.

### 0x18 ACTOR_SPAWN_BATCH (хост → сервер → гость)
```
payload: ActorSpawnBatchHeader (8 B) + count * ActorSpawn (64 B)
         max = 8 + 12*64 = 776 B
header: epoch:u32  count:u16  pad:u16
ActorSpawn (64 B):
  actorId:u32        — стабильный id в рамках epoch (не 0)
  epoch:u16
  kind:u8            — 0 = human, 1 = animal, 2 = horse
  flags:u8           — bit0 mounted, 1 dead, 2 drawn, 3 changing,
                       4 combat, 5 scenario, 6 reloading, 7 aiming
  model:u32  metaOutfit:u32  mountActorId:u32
  weaponHash:u32  scenarioHash:u32
  x,y,z,heading:f32
  health:u8  moveStyle:u8  phase:u8  pad:u8
  vx,vy,vz:i16  pad2:u16  reserved:u32  reserved2:u32
```
- E1: хост шлёт батч на новые спавны и heartbeat (~2.5 с,
  `kSpawnHeartbeatTicks=150`), чтобы гость/join/resync получили roster.
- E2a: `metaOutfit` — реальный outfit hash для NPC (probe: есть у ~22% ambient;
  heartbeat 0→hash пересылается и гостем переприменяется).
- E2b: `flags bit0 mounted`, `reserved` (u32) = mount model hash;
  `mountActorId` — actorId лошади (0 = нет).
- E3a: `flags bit5 scenario`, `scenarioHash` — сценарий (валидные имена
  `WORLD_HUMAN_*` и т.п.); `flags bit1 dead` + `health=0` — труп/возрождение.
- E3b: `flags` — Drawn(4)/Changing(8)/Combat(16)/Reloading(64)/Aiming(128);
  `weaponHash` — валидный RDR2 weapon hash (например 0x169F59F7 Cattleman).
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
- E1: движение/прокси 10 Гц; `speed` — u8 на шкале 0..8 м/с,
  `pad` — gait (`GET_PED_DESIRED_MOVE_BLEND_RATIO*1000`, walk=1000/run=2000).
- E3b: боевые флаги в `flags`; `targetActorId=1` в shoot-окне (вне 0).
### 0x1A ACTOR_DESPAWN (хост → сервер → гость, 12 B)
```
epoch:u32  actorId:u32  reason:u8  pad[3]
```
- E1: reason 1 = ушёл (>70 м)/таймаут, 2 = умер, 3 = вытеснен
  (cap/смена модели); хост шлёт не чаще 1 сообщения за тик.
### 0x1B ACTOR_EVENT (хост → сервер → гость, 32 B)
```
epoch:u32  actorId:u32  type:u8  flags:u8  pad:u16
a:u32  b:u32  x:u32  y:u32  z:u32   // формат полей задаёт игровой слой
```
### 0x1C ACTOR_OUTFIT (хост → сервер → гость, 792 B)
```
epoch:u32  actorId:u32  + OutfitState (784 B; см. 0x09)
```
- **0x1B** зарезервирован: принимается и считается в inbox, но игровым слоем
  не применяется. E2a использует `metaOutfit` в 0x18 (heartbeat), а не 0x1C.
  **0x1C используется E9 horse tack** (kind=2): `OutfitState` 784 B on change,
  count=0 не шлётся, model mismatch/count=0 skip; после tack randomize не
  выполняется. E2c-b asset/tint использует отдельный **0x1E** (см. раздел
  0x1E) — 784 B `OutfitState` для asset GUID не подходит.
### 0x1D ACTOR_RESYNC (гость → сервер → хосту, 12 B)
```
epoch:u32  lastSeq:u32  reason:u8  pad[3]
```
- Назначение: гость сообщает хосту, что потерял поток кадров (epoch/seq) и
  просит resync; сервер пересылает запрос только текущему хосту.
- E1: реализовано: `Client::SendActorResync` + `coop::SendActorResync`
  (троттлинг ≤1 запроса/с, лог `actor: resync request epoch=… seq=… reason=…`);
  хост обрабатывает запрос через `OnActorResyncRequest` и на следующем
  discovery-тике шлёт полный spawn-batch (0x18) вместо ожидания heartbeat.

### 0x1E ACTOR_APPEARANCE (хост → сервер → гость, v16 / E2c-b)
```
payload: ActorAppearanceHeader (12 B) + count * ActorAppearanceEntry (28 B)
         максимальный размер = 12 + 16*28 = 460 B
header:
  epoch:u32  actorId:u32  count:u8  pad[3]
entry (28 B):
  drawable:u32  albedo:u32  normal:u32  material:u32  palette:u32
  tint0:i16  tint1:i16  tint2:i16  reserved:u16 (=0)
```
- Направление: **только хост → сервер → гость** (host-only relay, как 0x18).
  Сервер валидирует: точный размер `12 + count*28`, `count` 1..16,
  `epoch != 0`, `actorId != 0`, `reserved == 0` каждой записи; payload
  ретранслируется byte-identical, иначе дропается.
- Хост: capture компонентов (`_GET_NUM_COMPONENTS_IN_PED` 0-based +
  `GET_META_PED_ASSET_GUIDS/TINT`), fingerprint; шлёт snapshot при
  спавне/изменении/join (budget 1 актор/тик, период 2 с).
- Гость: apply reset + `_SET_META_PED_TAG` + commit +
  `UPDATE_PED_VARIATION`, readback, fallback randomize; asset приоритетнее
  `metaOutfit` (E2a). Данные реального хоста: `capture count=8-10`
  `valid=8-10`; fake guest v16: `appearance_msgs=504 appearance_actors=8
  entry_max=10 errors=0`.

### 0x1F ACTOR_DAMAGE (гость → сервер → хост, v17 / E5)
```
payload: ActorDamagePayload (16 B)
  epoch:u32       — эпоха хоста
  actorId:u32     — id актёра, по которому попали (не 0)
  damage:u16      — 1..kMaxActorDamage (1000)
  pad:u16
  weaponHash:u32  — оружие (0 = неизвестно)
```
- Направление: **только гость → сервер → хост**. Хост-отправитель
  дропается; сервер валидирует 16 B, epoch/actorId != 0, damage 1..1000 и
  релеит только текущему хосту; rate-limit **≤32 запроса/с** на клиента.
- Хост применяет урон сам (**host-authoritative**): лимиты ≤8 применений/тик,
  ≤15/с, per-actor 100 мс, дистанция до стрелка ≤150 м, actorId должен быть
  live в roster/epoch; `APPLY_DAMAGE_TO_PED` (fallback
  `SET_ENTITY_HEALTH`). Гость-прокси остаются invincible.
- Гость-отправитель: free-aim target через
  `GET_ENTITY_PLAYER_IS_FREE_AIMING_AT`, урон по оружию:
  пистолет/револьвер 35, винтовка/repeater/sniper 60, дробовик 80, иначе 30;
  лимиты ≤10/с и 100 мс/actor.
- **E5.1:** хедшот определяется по `SKEL_Head` + точке impact ≤0.30 м
  (fallback — луч камеры) и отправляется как `damage=250` → ваншот
  human HP 200 / animal HP 100; в логе `headshot=1`.
- Смерть естественная (движок) → dead flag + health=0 → труп у гостя;
  animals/horses тем же путём.

### 0x20 PLAYER_PRESENCE (любой клиент → сервер → другому, v18 / E6)
```
payload: PlayerPresencePayload (8 B)
  epoch:u32   — эпоха сессии отправителя
  flags:u16   — bit0 in_map, bit1 in_pause_menu, bit2 in_satchel,
                bit3 in_shop, bit4 in_cutscene, bit5 in_menu_other
  pad:u16
```
- Направление: **любой клиент → сервер → другому клиенту**, без эха
  отправителю; при одном подключённом сообщение некому релеить.
- Валидация сервера: ровно 8 B, `epoch != 0`, неизвестные биты flags
  (6..15) → drop; rate-limit **≤10/с** на клиента.
- Клиент: детект UI через `IS_PAUSE_MENU_ACTIVE` + `IS_UIAPP_*`, шлёт при
  изменении/каждые ~2 с; удалённый статус устаревает через **12 с**.
  Суффикс имени: `in cutscene` > `in map` > `in satchel` > `in shop` >
  `pause menu` > `in menu` (ASCII `Name - <suffix>`); `npc_presence=1`.
  Debug: `ui: pause=… anyApp=… pause_menu=… map=… satchel=… shop=… inv=…`.

### 0x21 VEHICLE_OCCUPANTS (хост → сервер → гость, v19 / E3d-c)
```
payload: VehicleOccupantsHeader (8 B) + count * VehicleOccupantEntry (8 B)
         count 0..8, max = 8 + 8*8 = 72 B
header: vehicleNetId:u32  count:u8  pad[3]
entry (8 B):
  actorId:u32
  seat:i8         — -1..8 (-1 = нет места/неизвестно)
  flags:u8        — 1 driver, 2 passenger, 4 draftHorse
  carIndex:u8     — 0..16 (поезда)
  harness:u8      — 0..5 (draft)
```
- Направление: **только хост → сервер → гость**; `count=0` — очистить
  occupants у vehicle-прокси. Сервер валидирует точный размер
  `8+8*count`, `count≤8`, `vehicleNetId/actorId != 0`, известные flags,
  диапазоны seat/carIndex/harness и релеит byte-identical только гостю;
  rate-limit **20/с** (host throttle ≤18/с).
- Хост шлёт при изменении occupants + heartbeat **1/с**; гость ждёт
  vehicle+actor прокси (до 3 попыток), `invisible → SET_PED_INTO_VEHICLE
  warp → IS_PED_IN_VEHICLE → visible`; draft —
  `_ATTACH_DRAFT_VEHICLE_HARNESS_PED`; seated/draft пропускают `MoveProxy`;
  cleanup при despawn/grace/role change.

### 0x22 TRAIN_SNAPSHOT_V2 (хост → сервер → гость, v19 / E3d-b)
```
payload: TrainSnapshotV2Payload (48 B)
  epoch:u32  trainId:u32  configHash:u32
  flags:u8   — 1 waitingAtStation, 2 halted, 4 reversing, 8 playerAboard
  stationIndex:u8  carCount:u8  pad:u8
  speed:f32        — -40..40
  x,y,z,heading:f32
  trackIndex:u32
  pad2[8]
```
- Направление: **только хост → сервер → гость**; сервер валидирует ровно
  48 B, `epoch/trainId != 0`, известные flags, `carCount 1..16`,
  `speed` в ±40 и конечные coords/heading; релеит byte-identical, лимит
  **≤4/с**.
- Хост: config-хэши TrainConfigs (87), forward/reverse match, кэш; при
  flags waiting/halted/reversing/playerAboard, speed/coords/heading/
  trackIndex/station; поезда не входят в vehicle-бюджет.
  Гость: `SET_RANDOM_TRAINS(false)` + `_CREATE_MISSION_TRAIN`, sync speed
  (Δ>0.5)/coords (>5 м soft ≤1/0.5 с)/warp (>25 м/|dz|>15)/halt/leave/
  reverse, cleanup timeout 2 с и restore random trains (`world_trains=1`).

## VEHICLE-СООБЩЕНИЯ (v20 P1 — guest influence, wire U1/U2)

> **Статус:** wire-структуры/валидаторы (U1) и серверный релей/rate/роли (U2)
> реализованы (ветка `feature/v20-p1`); host apply (U3) и guest capture (U4)
> ещё в работе — поэтому **v20-пара не деплоится** до их завершения. Полная
> спека: `docs/INTERACTIVITY_P1.md`.

### Направления и роли

| ID | сообщение | размер | направление | роль отправителя |
|---|---|---:|---|---|
| 0x23 | `VehicleInteraction` | 24 B | guest → сервер → **host** | только guest (host-sender → warn+drop) |
| 0x24 | `VehicleInteractionResult` | 16 B | host → сервер → guest | только host |
| 0x26 | `VehicleState` | 24 B | host → сервер → guest | только host |
| 0x25 | `VehicleControlInput` | — | RESERVED (P3) | в P1 не шлётся/не релеится |
| 0x27 | `VehicleControlState` | — | RESERVED (P3) | в P1 не шлётся/не релеится |

Сервер **не знает мир**: он проверяет точный размер, inline-валидаторы
(`net::IsValidVehicleInteraction/…Result/…State`), роль/направление и
per-client rate; семантику (netId/distance/mission/seat/health) проверяет
хост при применении. Нет хоста online → 0x23 молча drop; сервер никогда не
эмулирует ответ от себя. При 2 игроках 0x24/0x26 = unicast оставшемуся
гостю; broadcast-направление выбрано осознанно (P1 = `kMaxPlayers=2`).

### 0x23 VEHICLE_INTERACTION (гость → сервер → хост; 24 B)

```
epoch:u32          — host epoch (actor_sync); 0 невалиден
vehicleNetId:u32   — netId транспорта из WorldSnapshot; 0 невалиден
targetId:u32       — actorId/netId по flags; 0 = нет
seq:u16            — idempotency key; 0 невалиден, монотонный на сессию
action:u8          — 1 Damage, 2 DoorOpen, 3 DoorClose, 4 DoorLock,
                     5 DoorUnlock, 6 DoorBreak, 7 Enter, 8 Exit, 9 Hijack,
                     10 Loot, 11 Hitch, 12 Unhitch (0 None и >12 reject)
seat:i8            — -1 driver / 0..8 passenger / 127 none
door:u8            — doorId 0..7 (doors), part (Damage: 0..11, 254/255
                     body, 255 — все двери для lock), harness 0..5 (draft)
flags:u8           — 1 TargetActor, 2 TargetEntity, 4 Urgent,
                     8 LootContainer, 16 GuestHorse, 32 Force
param:u16          — Damage: 1..1000; иначе 0
weaponHash:u32     — контекст Damage (0 = неизвестно; для не-Damage обязан 0)
```

Валидаторы (inline, единый источник для server/client/tests): epoch/netId/seq
≠ 0, action 1..12, известные flags, TargetActor и TargetEntity не вместе,
seat -1/0..8/127; per-action: Damage part 0..11/254/255 + param 1..1000;
DoorOpen/Close/Break door 0..7 + param 0; DoorLock/Unlock door 0..7/255 +
param 0; Enter seat -1/0..8/127, door 255, target 0; Exit seat 127, door 255;
Hijack seat -1/127, door 255, target 0 или TargetActor; Loot
TargetEntity↔targetId↔seat 127; Hitch/Unhitch harness 0..5.
Окна (part 12..15) в P1 reject. Rate-limit сервера — **8/с** на клиента.

### 0x24 VEHICLE_INTERACTION_RESULT (хост → сервер → гость; 16 B)

```
epoch:u32        — echo epoch
vehicleNetId:u32 — echo netId
seq:u16          — echo запроса (idempotency)
action:u8        — echo action (1..12)
result:u8        — 0 Applied, 1 NotAllowed, 2 StaleEpoch, 3 NotFound,
                   4 OutOfRange, 5 SeatOccupied, 6 Locked, 7 RateLimited,
                   8 Timeout, 9 Correction
detail:u32       — action-specific: для NotAllowed reason = (detail>>8)&0xFF
                   (1 config_off, 2 mission_protected, 3 host_vehicle,
                   4 bad_param, 5 dead_guest, 6 bad_role, 7 policy);
                   Damage: part/resultFlags/appliedDamage; doors: doorId/
                   doorState/lock; seats: guestSeat/hostSeat; harness: readback
```

Сервер валидирует точный размер, epoch/netId/seq ≠ 0, action 1..12,
`result 0..9`; **20/с**.

### 0x26 VEHICLE_STATE (хост → сервер → гость; 24 B)

```
epoch:u32         — host epoch
vehicleNetId:u32  — id транспорта
bodyHealth:u16    — 0..1000; 0xFFFF = unknown (DamageValid не ставится)
engineHealth:i16  — -4000..1000; -32768 = unknown
doorBits:u16      — 2 бита на дверь 0..7: 0 closed, 1 open, 2 broken, 3 unknown
seq:u16           — монотонный per vehicle (диагностика/recovery)
flags:u8          — 1 DoorValid, 2 DamageValid, 4 Locked, 8 MissionProtected,
                    16 HostSeatValid, 32 GuestSeatValid, 64 Hijacked, 128 Dead
wheelsBits:u8     — 2 бита на колесо FL/FR/RL/RR: 0 intact, 2 destroyed,
                    3 unknown (damaged(1) readback отсутствует)
hostSeat:i8       — место игрока-хоста (-1/0..8/127)
guestSeat:i8      — место игрока-гостя (-1/0..8/127)
reserved:u32      — 0 (в P3: leaseId)
```

Хост шлёт change-driven + heartbeat **1/с** для отслеживаемых vehicle
(≤8); суммарно ≤10/с, при переполнении — round-robin (heartbeat чинит).
Гость отбрасывает state с чужим epoch или более старым `seq` для vehicle.
Сервер валидирует размер, epoch/netId ≠ 0, известные flags, health-диапазоны
и seats; rate **10/с**. `bodyHealth/engineHealth = unknown` до подтверждённых
readback-нативов (R&D: `docs/NATIVE_RESEARCH_V20_HEALTH.md`).

### Game-thread semantics, migration, bump/deploy

- **Host liveness** (`IsGameThreadMessage`): 0x24/0x26 — game-thread сообщения
  (продлевают жизнь хоста), **0x23 — нет** (это guest-интент; хост их не шлёт).
  Поэтому host-клиент обязан слать state heartbeat/результаты, иначе
  сработает host game-timeout.
- **Host migration:** 0x23 маршрутизируется по текущему `isHost` на момент
  отправки; старые epoch/seq у нового хоста дают `StaleEpoch`/drop. Очередей
  сервер не хранит; 0x24/0x26 от старого хоста после миграции дропаются как
  role violation.
- **v20 — жёсткий bump:** `kProtocolVersion 19→20`; v19 JOIN отклоняется
  (`VersionMismatch`), смешанная пара невозможна. Deploy только парой
  ASI+server из одного снапшота `dist\npc-e20` (оба ПК, одинаковые хеши);
  откат — `dist\npc-e19` (v19, ASI `DD0EE210`/server `DD9BD9D8`) полной парой.
  Промежуточные U1–U4 **не деплоить**.
- Серверный трафик-бакет: heartbeat `net: … occupants=… vehicle=… other=…`;
  `tools/perf_report.py` показывает `vehicle` per-type (сумма/проценты), старые
  v19-логи (без `vehicle=`) остаются без новой категории.

### E1–E3b (actor_sync, v15) — реализовано и проверено на одном ПК
- База: **0x18** (`ActorSpawnBatch`: спавн + heartbeat 2.5 с), **0x19**
  (`ActorFrames` 10 Гц), **0x1A** (`ActorDespawn`: reasons 1/2/3);
  discovery 10 Гц по human NPC рядом с обоими игроками, cap `npc_cap`,
  радиус `npc_radius` (keep +10 м), стабильные `actorId` в epoch,
  mission NPC исключены, мёртвые не спавнятся.
- Гость: inbox под mutex, прокси-педы (CREATE_PED, mission, no-collision),
  request model ≤2/тик, create/delete ≤2/тик, trail-навигация + in-place task
  update, gait с хоста (`pad`), resync 0x1D после 2 с без кадров, удаление
  через 10 с; epoch/leave/reconnect/host migration → reset+resync.
- **E2a:** `metaOutfit` в heartbeat: 22% ambient имеют hash; в тесте 5/5
  применено, failed=0; `npc_appearance=1`.
- **E2b:** rider+horse, `reserved` = mount model, seat watchdog,
  dismount/despawn cleanup; подтверждено mount spawn ×9, failed=0;
  gait_restart=0, err_p95≈0.
- **E3a:** scenarioHash + optimistic confirm; анти-наложение (skip 8 м,
  warp revert 3 м) — визуально «двое курят на разных местах»;
  death/revive (health=0/RESURRECT_PED, труп остаётся).
- **E3b:** валидный `weaponHash` (GIVE_WEAPON_TO_PED), flags
  Drawn/Aiming/Reloading/Combat, aim/shoot/reload; state=shoot=1450,
  reload=385, weapon fail=0.
- **E2c-a:** host brute-force resolver (`gen_metaped_outfits.h`: 890 моделей /
  4568 кандидатов, `_IS_META_PED_OUTFIT_EQUIPPED`, бюджет 24 натива/тик,
  heartbeat-пересылка, guest reapply) — механически работает; для random
  ambient NPC resolved=0/failed=11 (нет именованного outfit).
- **E3c:** kind=1 animals / kind=2 loose horses; HP animal/horse 100,
  движение/трупы до despawn. **E5/E5.1:** 0x1F damage, host apply,
  свободные животные/лошади получают урон тем же путём; headshot=250.
- При `npc_actor=0`: `actor_sync::Tick`/callbacks ничего не делают и не пишут
  `actor:`/`probe:`; сервер не получает actor-сообщений.
- Статус: E1–E19 реализованы и проверены на одном ПК (fake host/guest +
  реальный хост; E5/E5.1 подтверждены в игре). Локальная пара npc-e19/v19 —
  ASI `DD0EE210`, server `DD9BD9D8`; **оба ПК задеплоены** (local + ARINA,
  хеши сверены, 28 INI-ключей). Двух-ПК приёмка e19 pending (ждём полный
  тест), host migration/resync — backlog. Флаги:
  `npc_actor`, `npc_cap`, `npc_radius`, `npc_animal_cap`, `npc_appearance`,
  `npc_damage`, `npc_corpse_max_sec`, `npc_presence`, `npc_visitor`,
  `npc_visitor_vehicles` (default 1), `npc_visitor_preserve_events`,
  `npc_visitor_pause_scenes`, `npc_motion_update`, `npc_motion_marker`,
  `npc_motion_status_p2`, `world_vehicles`, `npc_vehicle_radius`,
  `npc_occupants`, `npc_vehicle_wheels` (e19, default 0),
  `npc_animal_appearance`, `world_trains`, `npc_debug`.
- Ограничения: mission NPC исключены; E3d-a/c транспорт и occupants;
  E3d-b mission-train opt-in (`world_trains=0`), ambient-поезда не чистим v1,
  точные пассажиры поездов P1, seat=-1/driver и direction/reversing требуют
  in-game проверки; E9 cap16/RESET на животных не проверен/horse tack
  model-specific/mounted-horse не покрыт/MPT_ANIMAL coverage fallback random/
  MetaPedOutfit только probe; Story Mode нельзя «подселить»; RedM — другой
  стек; при `world_vehicles=0` пустые дороги; при миграции мир не
  переносится; лицо/волосы, точные анимации/катсцены не синхронизируются.

### E2c-b (actor_sync, v16): host capture + guest apply
- Хост: `_GET_NUM_COMPONENTS_IN_PED` (0-based) + `GET_META_PED_ASSET_GUIDS`/
  `GET_META_PED_ASSET_TINT`, fingerprint изменившихся компонентов; отправка
  0x1E при спавне/изменении/join игрока; budget 1 актор/тик, период 2 с.
- Гость: reset + `_SET_META_PED_TAG` + commit + `UPDATE_PED_VARIATION`,
  readback; fallback randomize при неудаче; asset приоритетнее metaOutfit.
- Валидация на реальном хосте: `asset capture count=8-10 valid=8-10`
  (единичные valid=0); fake guest v16: `appearance_msgs=504
  appearance_actors=8 entry_max=10 errors=0`. Визуальная двух-ПК приёмка —
  pending (пара npc-e19 задеплоена на оба ПК, ждём полный тест).
- v15↔v16↔v17↔v18 несовместимы; откат v17 — ASI `35163CC2`/server
  `B7AAABAE`, v16 — `63388AD3`/`4EDAE4B6`, v15 — `dist\npc-e2c-v15\`.

### E5 (actor_sync, v17): host-authoritative damage + headshots
- Гость: детект реального выстрела/попадания в free-aim target
  (`GET_ENTITY_PLAYER_IS_FREE_AIMING_AT`), урон по классу оружия, лимиты
  ≤10/с и 100 мс/actor; отправляет 0x1F только если `npc_damage=1` и есть
  живой прокси. Хедшот — `SKEL_Head` + impact ≤0.30 м (fallback луч камеры),
  `damage=250`.
- Хост: очередь damage, `APPLY_DAMAGE_TO_PED` + fallback
  `SET_ENTITY_HEALTH`; лимиты ≤8/тик, ≤15/с, per-actor 100 мс, ≤150 м;
  проверяет epoch/actorId/live/roster. Смерть — естественная, далее dead
  flag/труп как E1–E3c.
- Проверено: host `damage id=… dmg=… hp_left=…` → `killed id=… by damage`;
  fake tool smoke (human 200/animal 100, guest `--damage-after`), игровая
  проверка «выстрелы убивают, хэдшот ваншотит», CI 42/42.

### E5.2/E5.3 (v18 client behavior): corpse retention + mounted smoothing
- E5.2: гость держит corpse-прокси, пока host-труп существует;
  `npc_corpse_max_sec=300` (0 = старое поведение); логи
  `actor: corpse hold id=… kind=…`, `actor: corpse despawn id=… reason=dead
  after=…`.
- E5.3: сглаживание всадников — retarget 1.75 м, lead 0.28, stuck fallback;
  лог `actor: motion mounted err_p95/retargets`.

### E6 (v18): PlayerPresence / name-tag suffix
- 0x20 bidirectional relay (см. выше), flags map/pause/satchel/shop/
  cutscene/menu_other, `npc_presence=1`; суффикс `Name - in map`/и т.п.
- Debug UIAPP `ui: pause=…`; таймаут 12 с; при `npc_presence=0` — ноль
  presence/суффиксов.

### E7.1 visitor mode
- Гость (`!host`, `npc_actor=1`, `npc_visitor=1`) подавляет локальную
  ambient human/ped/animal density; cinematic/vehicle population — при
  `npc_visitor_vehicles=1` (default 1 после E3d-a; при 0 локальный трафик
  остаётся).
- Cleanup существующей толпы раз в ~10 с (≤8/тик, фейд ~0.4 с);
  `npc_visitor_preserve_events=1` сохраняет scenario/vendor/quest/event NPC
  (whitelist 23 моделей); логи `actor: visitor mode on/off`,
  `actor: visitor cleanup removed=%u`.
- Road population disable/enable парно при `npc_visitor_vehicles=1`;
  `npc_actor=0`/`npc_visitor=0` — прежнее поведение; хост не подавляет.
- **Mission-aware (E7.3):** `npc_visitor_pause_scenes=1` — при миссии
  (`GET_MISSION_FLAG` 0xB15CD1CF58771DE1) или готовности к катсцене
  (`IS_PLAYER_READY_FOR_CUTSCENE(PLAYER_ID())` 0xAA67BCB0097F2FA3) паузится
  только cleanup толпы; density/road suppression продолжаются; лог
  `actor: visitor scene pause on/off mission=%d cutscene=%d`.

### E7.2 smooth NPC motion
- 60 Гц интерполяция кадров, far goal впереди хоста (lead speed*0.7,
  clamp 4–12 м; кони 5–20), одна задача на фазу, in-place update
  `_UPDATE_TASK_GO_TO_COORD_WITH_OFFSET` 0x3FFCD7BBA074CC80, MBR через
  `_COMPUTE_SPEED_FOR_PED_MOVE_BLEND_RATIO` 0xCA95924C893A0C91, status p2 A/B,
  stop/start гистерезис.
- Телепорт/снап: >25 м, |dz|>15, нет кадров >2 с → `actor: teleport id=…
  dist=…`; диагностика `actor: motion id=%u reissue=%.2f/s snap=%.2f/s
  target_err_p95=%.2f status=…`; auto-warn `warn update-native A/B` при
  высоком reissue.
- Флаги `npc_motion_update=1`, `npc_motion_marker=0`,
  `npc_motion_status_p2=1`; приёмка baseline: **reissue <0.05/s, snap=0**.

### E7.3 (v18): E3d-a vehicles + host-migration reset
- **E3d-a:** `WorldSnapshot` vehicles-only (см. 0x10); `world_vehicles=1`,
  `npc_vehicle_radius=80`; guest proxies mission/invincible/no-collision,
  dedupe `engineNetId`, timeout 3 с; водители/пассажиры — E3d-c/v19.
- **Host migration:** server game-timeout 4 с по game-thread сообщениям
  (Ping не считается); client при `HostChanged` делает полный reset
  (world_sync/visitor/presence/actor/боевые/remote vehicle) +
  `ForceFullSpawnBatch`/`RequestImmediateResync`; мир не переносится.
- **Mission-aware visitor:** см. E7.1; новых сообщений нет (v18 без bump).

### E3d-c (v19): vehicles occupants + draft
- Хост: actor discovery снова включает педов в транспорте (mission-occupants
  через occupant-handles, приоритет в selection; draft-лошади — animal pass);
  `CollectVehicleOccupants` (driver/passengers seats 0..max-1/draft harness
  0..5) → 0x21 при изменении + heartbeat 1/с, throttle ≤18/с.
- Гость `ApplyPendingOccupants` (ждёт vehicle+actor прокси, до 3 попыток),
  seat attach/видимость, draft attach; seated/draft skip MoveProxy;
  vehicle-прокси 60 Гц interpolation, snap>8 м warp+пересадка; cleanup.
- `npc_occupants=1` (0 = старое E3d-a поведение); логи `world: occupants
  vehicle=`, `actor: seat attach … ok=`, `actor: draft attach … ok=`,
  `actor: occupants errors=`.

### E3d-b (v19): mission-train
- 87 TrainConfigs, ResolveTrainConfig бюджет 5/тик+80 model-вызовов,
  forward/reverse match, кэш, ambiguity/unknown skip+лог; FindNearestTrain
  radius 250.
- Host TrainSnapshotV2 ≤4/с; trains excluded from vehicle budget.
- Guest `SET_RANDOM_TRAINS(false)`, `_IS_POSITION_VALID_FOR_TRAIN` →
  fallback `_GET_NEAREST_TRAIN_TRACK_POSITION` → `_CREATE_MISSION_TRAIN`,
  maxSpeed/avoidance/stops/destruction, `_FORCE_TRAIN_WAGON_POPULATION`,
  sync/cleanup/restore random trains; `world_trains=0` opt-in (active при
  `npc_actor=1 && world_vehicles=1 && world_trains=1`, not `world_sync=1`).
  Логи `world: train send/spawn/despawn`.

### E9 (v19): animal/horse appearance
- Без новых сообщений: host → `ActorAppearance` 0x1E (assets/tint,
  MPT_ANIMAL type3, kind1/2, cap16, budget 1 animal/tick ~2 с) и
  `ActorOutfit` 0x1C (horse tack, kind2, 792 B on change, budget 1 horse/tick
  ~2 с) → сервер relay → гость.
- Guest staged apply/readback, fallback random; horse tack apply; dead/
  despawn skip; при `npc_animal_appearance=0` — random как раньше;
  флаг host+guest должен совпадать. Probe/logs:
  `actor: animal appearance probe …`, `actor: animal appearance apply … ok=`,
  `actor: horse tack apply … ok=`.

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
  v14–v19 (v19: actor-сообщения E1–E9, 0x18–0x22, см. раздел выше).

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
  actor-сообщения 0x18–0x1C, 0x1E, 0x21, 0x22 (при включённых флагах);
  гость — ActorResync 0x1D, ActorDamage 0x1F; PlayerPresence 0x20 — **любой**
  клиент.
- Сервер рассылает: JOIN_OK/JOIN_REJECTED, PLAYER_JOINED, PLAYER_LEFT, FRAME,
  OUTFIT/HORSE_OUTFIT, HOST_CHANGED, host-only сообщения от хоста и actor-
  сообщения 0x18–0x1C, 0x1E, 0x21, 0x22; ActorResync/ActorDamage — только
  хосту; PlayerPresence 0x20 — другому клиенту без эха.

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