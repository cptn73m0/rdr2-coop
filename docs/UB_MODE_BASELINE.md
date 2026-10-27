# ub-mode quality baseline

Сгенерировано `tools/quality_scan.py`. Read-only статический аудит; CRAP-прокси = (branches+1)^2+(branches+1) без покрытия (худший случай).

## Inventory

| файл | LOC | code LOC | строк>120 | magic | TODO | warning-disable | функций |
|---|---|---|---|---|---|---|---|
| `src/coop.cpp` | 8118 | 6403 | 0 | 429 | 0 | 0 | 175 |
| `src/actor_sync.cpp` | 7178 | 6259 | 0 | 382 | 0 | 0 | 50 |
| `src/world_sync.cpp` | 3554 | 3207 | 0 | 114 | 0 | 0 | 22 |
| `server/server.cpp` | 2338 | 1942 | 0 | 83 | 0 | 0 | 71 |
| `src/gen_metaped_outfits.h` | 1506 | 1494 | 0 | 0 | 0 | 0 | 1 |
| `net/client.cpp` | 1450 | 1234 | 0 | 59 | 0 | 0 | 93 |
| `net/protocol.h` | 669 | 474 | 0 | 129 | 0 | 0 | 0 |
| `src/natives.h` | 425 | 296 | 0 | 0 | 0 | 0 | 0 |
| `tests/frame_reader_fuzz.cpp` | 405 | 360 | 0 | 44 | 0 | 0 | 3 |
| `net/client.h` | 341 | 250 | 0 | 2 | 0 | 0 | 0 |
| `src/config.cpp` | 340 | 309 | 0 | 27 | 0 | 0 | 13 |
| `tests/config_test.cpp` | 326 | 276 | 0 | 43 | 0 | 0 | 4 |
| `src/world_scan.cpp` | 227 | 191 | 0 | 16 | 0 | 0 | 1 |
| `net/socket.cpp` | 210 | 179 | 0 | 1 | 0 | 0 | 16 |
| `tests/world_entity_budget_test.cpp` | 188 | 168 | 0 | 17 | 0 | 0 | 6 |
| `tests/mission_barrier_rules_fuzz.cpp` | 176 | 147 | 0 | 21 | 0 | 0 | 3 |
| `tests/world_snapshot_parse_test.cpp` | 167 | 138 | 0 | 20 | 0 | 0 | 4 |
| `src/persist.cpp` | 166 | 136 | 0 | 5 | 0 | 0 | 4 |
| `tests/client_reconnect_test.cpp` | 160 | 137 | 0 | 11 | 0 | 0 | 3 |
| `src/mission_catalog.h` | 136 | 122 | 0 | 7 | 0 | 0 | 2 |
| `src/config.h` | 132 | 49 | 0 | 9 | 0 | 0 | 0 |
| `tests/mission_catalog_test.cpp` | 117 | 96 | 0 | 13 | 0 | 0 | 3 |
| `net/socket.h` | 113 | 56 | 0 | 6 | 0 | 0 | 0 |
| `src/native_call.h` | 101 | 70 | 0 | 0 | 0 | 0 | 11 |
| `src/actor_sync.h` | 100 | 51 | 0 | 0 | 0 | 0 | 0 |
| `src/script_hook.cpp` | 94 | 71 | 0 | 0 | 0 | 0 | 3 |
| `net/frame.cpp` | 74 | 58 | 0 | 9 | 0 | 0 | 5 |
| `src/mission_rewards.h` | 63 | 49 | 2 | 4 | 0 | 0 | 0 |
| `src/script_hook.h` | 63 | 32 | 0 | 0 | 0 | 0 | 0 |
| `src/coop.h` | 61 | 30 | 0 | 0 | 0 | 0 | 0 |
| `src/mission_barrier_rules.cpp` | 56 | 47 | 0 | 0 | 0 | 0 | 5 |
| `src/world_sync.h` | 56 | 23 | 0 | 1 | 0 | 0 | 0 |
| `src/world_entity_budget.cpp` | 54 | 48 | 0 | 0 | 0 | 0 | 1 |
| `src/main.cpp` | 51 | 35 | 0 | 0 | 0 | 0 | 3 |
| `src/log.cpp` | 46 | 35 | 0 | 0 | 0 | 0 | 2 |
| `src/world_snapshot_parse.cpp` | 40 | 32 | 0 | 0 | 0 | 0 | 1 |
| `net/frame.h` | 39 | 17 | 0 | 0 | 0 | 0 | 0 |
| `src/world_entity_budget.h` | 38 | 28 | 0 | 3 | 0 | 0 | 0 |
| `src/mission_barrier_rules.h` | 34 | 20 | 0 | 0 | 0 | 0 | 0 |
| `src/ghost_frame.h` | 33 | 20 | 0 | 1 | 0 | 0 | 0 |
| `src/world_snapshot_parse.h` | 25 | 15 | 0 | 0 | 0 | 0 | 0 |
| `src/world_scan.h` | 22 | 6 | 0 | 0 | 0 | 0 | 0 |
| `server/common.h` | 22 | 12 | 0 | 0 | 0 | 0 | 0 |
| `src/persist.h` | 18 | 5 | 0 | 0 | 0 | 0 | 0 |
| `src/log.h` | 16 | 5 | 0 | 0 | 0 | 0 | 0 |

## Топ 20 функций по CRAP-прокси

| CRAP | branches | lines | params | функция |
|---|---|---|---|---|
| 66306 | 256 | 1222 | 0 | `src/coop.cpp:6889` Tick` |
| 15750 | 124 | 578 | 9 | `src/world_sync.cpp:1685` SendHostSnapshot` |
| 7140 | 83 | 263 | 2 | `src/actor_sync.cpp:3911` ProcessGuestHorseOutfits` |
| 3192 | 55 | 312 | 0 | `tests/frame_reader_fuzz.cpp:93` main` |
| 2256 | 46 | 201 | 5 | `src/coop.cpp:4131` ApplyRemoteFramePositionPhase` |
| 1806 | 41 | 172 | 1 | `src/coop.cpp:6543` SendLocalPlayerFrame` |
| 1560 | 38 | 147 | 1 | `src/coop.cpp:2866` ApplyActionToGhost` |
| 1560 | 38 | 168 | 5 | `src/world_sync.cpp:978` TickHostTrains` |
| 1560 | 38 | 80 | 2 | `server/server.cpp:1883` DispatchMessagesPhase` |
| 1482 | 37 | 211 | 0 | `net/client.cpp:1237` RunOnce` |
| 1332 | 35 | 185 | 1 | `src/coop.cpp:3105` ApplyPedStateToGhost` |
| 1332 | 35 | 168 | 3 | `server/server.cpp:1714` MiscMessagesPhase` |
| 1260 | 34 | 132 | 1 | `src/coop.cpp:5006` UpdateLocalShotEvents` |
| 1260 | 34 | 179 | 3 | `server/server.cpp:1486` WorldRelayMessagesPhase` |
| 1122 | 32 | 168 | 2 | `server/server.cpp:1246` JoinRequestPhase` |
| 1056 | 31 | 141 | 4 | `src/coop.cpp:3988` ApplyRemoteFrameMountPhase` |
| 1056 | 31 | 106 | 4 | `src/world_sync.cpp:871` SendTrainOccupants` |
| 1056 | 31 | 41 | 3 | `net/client.cpp:1195` HandleServerMessage` |
| 930 | 29 | 83 | 0 | `src/coop.cpp:3018` UpdateGhostWeaponAnim` |
| 812 | 27 | 39 | 0 | `src/coop.cpp:715` DetectLocalPresenceUi` |

## Топ 20 функций по длине

| lines | branches | CRAP | функция |
|---|---|---|---|
| 1222 | 256 | 66306 | `src/coop.cpp:6889` Tick` |
| 578 | 124 | 15750 | `src/world_sync.cpp:1685` SendHostSnapshot` |
| 312 | 55 | 3192 | `tests/frame_reader_fuzz.cpp:93` main` |
| 278 | 15 | 272 | `tests/config_test.cpp:48` main` |
| 263 | 83 | 7140 | `src/actor_sync.cpp:3911` ProcessGuestHorseOutfits` |
| 229 | 11 | 156 | `src/coop.cpp:5986` InitNetworkClient` |
| 211 | 37 | 1482 | `net/client.cpp:1237` RunOnce` |
| 201 | 46 | 2256 | `src/coop.cpp:4131` ApplyRemoteFramePositionPhase` |
| 185 | 35 | 1332 | `src/coop.cpp:3105` ApplyPedStateToGhost` |
| 179 | 34 | 1260 | `server/server.cpp:1486` WorldRelayMessagesPhase` |
| 172 | 41 | 1806 | `src/coop.cpp:6543` SendLocalPlayerFrame` |
| 168 | 38 | 1560 | `src/world_sync.cpp:978` TickHostTrains` |
| 168 | 32 | 1122 | `server/server.cpp:1246` JoinRequestPhase` |
| 168 | 35 | 1332 | `server/server.cpp:1714` MiscMessagesPhase` |
| 147 | 38 | 1560 | `src/coop.cpp:2866` ApplyActionToGhost` |
| 141 | 26 | 756 | `src/coop.cpp:3628` ApplyOutfitToGhost` |
| 141 | 31 | 1056 | `src/coop.cpp:3988` ApplyRemoteFrameMountPhase` |
| 132 | 34 | 1260 | `src/coop.cpp:5006` UpdateLocalShotEvents` |
| 125 | 24 | 650 | `src/coop.cpp:2255` MoveMountTo` |
| 123 | 13 | 210 | `src/coop.cpp:5326` CheckRemoteModelChange` |

## Дублирование (6-строчные шинглы, топ 20)

| повторений | места |
|---|---|
| 4 | `src/actor_sync.cpp:2718` |
| 4 | `src/actor_sync.cpp:2720` |
| 4 | `src/world_sync.cpp:1756` |
| 3 | `src/actor_sync.cpp:1108`, `src/world_sync.cpp:173` |
| 3 | `src/actor_sync.cpp:1109`, `src/world_sync.cpp:174` |
| 3 | `src/actor_sync.cpp:2035`, `src/world_sync.cpp:1739` |
| 3 | `src/actor_sync.cpp:2044` |
| 3 | `src/actor_sync.cpp:2045` |
| 3 | `src/actor_sync.cpp:2046` |
| 3 | `src/actor_sync.cpp:2047` |
| 3 | `src/actor_sync.cpp:2711` |
| 3 | `src/actor_sync.cpp:2712` |
| 3 | `src/actor_sync.cpp:2713` |
| 3 | `src/actor_sync.cpp:2714` |
| 3 | `src/actor_sync.cpp:2715` |
| 3 | `src/actor_sync.cpp:2716` |
| 3 | `src/actor_sync.cpp:557`, `src/coop.cpp:2699` |
| 3 | `src/actor_sync.cpp:558`, `src/coop.cpp:2700` |
| 3 | `src/actor_sync.cpp:559`, `src/coop.cpp:2701` |
| 3 | `src/actor_sync.cpp:5841` |

## natives.h

- объявлений: 288
- групп с одинаковым хешем: 0
- групп с одинаковым именем: 0

## TODO/FIXME/HACK

Всего маркеров: 0; warning-disable: 0

## Зачем это

Baseline нужен, чтобы каждая единица шлифовки (ub-mode) имела проверяемую цель: уменьшить CRAP/длину/дублирование, убрать мёртвый код, не меняя поведение (build + CI 41/41). Повторный запуск после шлифовки даёт дельту.
