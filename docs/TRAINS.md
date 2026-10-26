# Поезда/состав — mission-train v19 (npc-e8)

**Статус (26.10.2026):** **mission-train v19 реализован** (тег **npc-e8**,
protocol v19); старый R&D proxy-attach `world_trains=1` **устарел**
(deprecated) и не развивается. Mission-train по умолчанию **выключен**
(`world_trains = 0`, тестовый opt-in; активен при `npc_actor=1 &&
world_vehicles=1 && world_trains=1`, не при `world_sync=1`). ARINA offline —
2-ПК приёмка pending.

## Как работает v19

- **Protocol:** `TrainSnapshotV2 0x22` (48 B, host → сервер → гость, ≤4/с) —
  epoch/trainId/configHash, flags waitingAtStation/halted/reversing/
  playerAboard, stationIndex, carCount 1..16, speed ±40, xyz/heading,
  trackIndex, pad2[8]. Сервер валидирует размер/флаги/скорость/carCount и
  релеит byte-identical только гостю.
- **Хост:** 87 TrainConfigs из `TrainConfigs.json`; ResolveTrainConfig
  матчит forward/reverse по модели, бюджет 5/тик + 80 model-вызовов, кэш;
  ambiguity/unknown — skip + лог. FindNearestTrain радиус 250; поезда
  исключены из vehicle-бюджета (0x10).
- **Гость:** `SET_RANDOM_TRAINS(false)`;
  `_IS_POSITION_VALID_FOR_TRAIN` → fallback
  `_GET_NEAREST_TRAIN_TRACK_POSITION` → `_CREATE_MISSION_TRAIN(config, x,y,z,
  direction,false,false,true)`; maxSpeed 30/avoidance/stops/destruction,
  `_FORCE_TRAIN_WAGON_POPULATION`. Sync: speed Δ>0.5, coords >5 м (soft
  ≤1/0.5 с), warp >25 м/|dz|>15, halt/leave/reverse. Cleanup timeout 2 с,
  при role change/leave/connectionLost — удалить mission-train и
  восстановить `SET_RANDOM_TRAINS(true)`. Логи
  `world: train send/spawn/despawn`.

## Почему proxy-attach устарел

Старая схема слала каждый вагон как обычный `Vehicle`-прокси и на госте
привязывала их через `ATTACH_ENTITY_TO_ENTITY`: не было состава/порядка/
сцепки, рельсов, станций/скорости; прокси скакали 10 Гц; вагоны вытесняли
машины из бюджета 16 Vehicle; не было машиниста/пассажиров. Оставлено как
исторический код; `world_trains=1` в старом понимании не использовать.

## Ограничения и риски v1

- Точные пассажиры поездов — **P1**; `VehicleOccupants 0x21` покрывает
  транспорт/повозки, но не всех пассажиров состава.
- Ambient-поезда **не чистим** в v1; возможны фоновые поезда у гостя
  (SET_RANDOM_TRAINS(false) отключает random-спавн, но уже существующие не
  удаляются).
- direction/reversing семантика приближённая; config matching (87 хэшей)
  может дать skip/ambiguity — смотреть лог.
- Поезд создаётся как mission-train, не как физическая копия состава;
  точные двери/машинист/станционные скрипты не синхронизируются.
- `world_trains=0` — безопасный default; включать только для S26.

## Как проверить (S26)

1. Оба ПК: `world_sync=0`, `npc_actor=1`, `world_vehicles=1`,
   `npc_vehicle_radius=80`, `npc_visitor_vehicles=1`, `npc_occupants=1`,
   `world_trains=1`, `npc_debug=1`.
2. Хост у станции/едет на mission-train, останавливается, разворачивается;
   гость видит один состав на рельсах, правильные вагоны/направление,
   локального второго поезда нет; при выключении/leave/role change состав
   удаляется и random trains восстанавливаются.
3. Логи: `world: train send/spawn/despawn`, `train_debug=1` для
   offset/rails; при рассинхроне — `world_trains=0`, сохранить логи.
