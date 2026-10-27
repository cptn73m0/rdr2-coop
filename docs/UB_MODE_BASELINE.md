# ub-mode quality baseline

Сгенерировано `tools/quality_scan.py`. Read-only статический аудит; CRAP-прокси = (branches+1)^2+(branches+1) без покрытия (худший случай).

## Inventory

| файл | LOC | code LOC | строк>120 | magic | TODO | warning-disable | функций |
|---|---|---|---|---|---|---|---|
| `src/coop.cpp` | 7939 | 6282 | 0 | 430 | 0 | 0 | 139 |
| `src/actor_sync.cpp` | 7132 | 6225 | 0 | 381 | 0 | 0 | 16 |
| `src/world_sync.cpp` | 3481 | 3151 | 0 | 114 | 0 | 0 | 7 |
| `server/server.cpp` | 2119 | 1795 | 0 | 84 | 0 | 0 | 44 |
| `src/gen_metaped_outfits.h` | 1506 | 1494 | 0 | 0 | 0 | 0 | 0 |
| `net/client.cpp` | 1455 | 1274 | 0 | 63 | 0 | 0 | 45 |
| `net/protocol.h` | 669 | 474 | 0 | 129 | 0 | 0 | 0 |
| `src/natives.h` | 413 | 293 | 0 | 0 | 0 | 0 | 0 |
| `tests/frame_reader_fuzz.cpp` | 373 | 332 | 0 | 40 | 0 | 0 | 2 |
| `src/config.cpp` | 337 | 311 | 0 | 36 | 0 | 0 | 11 |
| `net/client.h` | 305 | 217 | 0 | 2 | 0 | 0 | 0 |
| `tests/config_test.cpp` | 248 | 207 | 0 | 22 | 0 | 0 | 4 |
| `src/world_scan.cpp` | 227 | 191 | 0 | 16 | 0 | 0 | 1 |
| `net/socket.cpp` | 210 | 179 | 0 | 1 | 0 | 0 | 16 |
| `tests/world_entity_budget_test.cpp` | 173 | 155 | 0 | 14 | 0 | 0 | 4 |
| `src/persist.cpp` | 166 | 136 | 0 | 5 | 0 | 0 | 4 |
| `tests/client_reconnect_test.cpp` | 160 | 137 | 0 | 11 | 0 | 0 | 3 |
| `tests/world_snapshot_parse_test.cpp` | 138 | 115 | 0 | 18 | 0 | 0 | 4 |
| `src/mission_catalog.h` | 136 | 122 | 0 | 7 | 0 | 0 | 2 |
| `tests/mission_barrier_rules_fuzz.cpp` | 135 | 112 | 0 | 5 | 0 | 0 | 3 |
| `src/config.h` | 128 | 48 | 0 | 9 | 0 | 0 | 0 |
| `tests/mission_catalog_test.cpp` | 117 | 96 | 0 | 13 | 0 | 0 | 3 |
| `net/socket.h` | 113 | 56 | 0 | 6 | 0 | 0 | 0 |
| `src/native_call.h` | 101 | 70 | 0 | 0 | 0 | 0 | 11 |
| `src/actor_sync.h` | 100 | 51 | 0 | 0 | 0 | 0 | 0 |
| `src/script_hook.cpp` | 94 | 71 | 0 | 0 | 0 | 0 | 3 |
| `net/frame.cpp` | 74 | 58 | 0 | 9 | 0 | 0 | 5 |
| `src/mission_rewards.h` | 63 | 49 | 2 | 4 | 0 | 0 | 0 |
| `src/script_hook.h` | 63 | 32 | 0 | 0 | 0 | 0 | 0 |
| `src/coop.h` | 59 | 29 | 0 | 0 | 0 | 0 | 0 |
| `src/mission_barrier_rules.cpp` | 56 | 47 | 0 | 0 | 0 | 0 | 2 |
| `src/world_sync.h` | 56 | 23 | 0 | 1 | 0 | 0 | 0 |
| `src/world_entity_budget.cpp` | 54 | 48 | 0 | 0 | 0 | 0 | 0 |
| `src/main.cpp` | 51 | 35 | 0 | 0 | 0 | 0 | 3 |
| `src/log.cpp` | 46 | 35 | 0 | 0 | 0 | 0 | 2 |
| `src/world_snapshot_parse.cpp` | 40 | 32 | 0 | 0 | 0 | 0 | 0 |
| `net/frame.h` | 39 | 17 | 0 | 0 | 0 | 0 | 0 |
| `src/world_entity_budget.h` | 38 | 28 | 0 | 3 | 0 | 0 | 0 |
| `src/mission_barrier_rules.h` | 34 | 20 | 0 | 0 | 0 | 0 | 0 |
| `src/ghost_frame.h` | 33 | 20 | 0 | 1 | 0 | 0 | 0 |
| `src/world_snapshot_parse.h` | 25 | 15 | 0 | 0 | 0 | 0 | 0 |
| `src/world_scan.h` | 22 | 6 | 0 | 0 | 0 | 0 | 0 |
| `server/common.h` | 22 | 12 | 0 | 0 | 0 | 0 | 0 |
| `src/persist.h` | 18 | 5 | 0 | 0 | 0 | 0 | 0 |
| `src/log.h` | 16 | 5 | 0 | 0 | 0 | 0 | 0 |

## Топ 30 функций по CRAP-прокси

| CRAP | branches | lines | params | функция |
|---|---|---|---|---|
| 140250 | 373 | 1712 | 0 | `src/coop.cpp:6225` Tick` |
| 24492 | 155 | 574 | 0 | `net/client.cpp:879` RunOnce` |
| 8010 | 88 | 386 | 1 | `src/coop.cpp:3943` ApplyRemoteFrame` |
| 7656 | 86 | 129 | 2 | `src/config.cpp:150` ApplyLine` |
| 2550 | 49 | 280 | 0 | `tests/frame_reader_fuzz.cpp:93` main` |
| 2352 | 47 | 181 | 1 | `src/coop.cpp:4893` UpdateLocalShotEvents` |
| 1892 | 42 | 168 | 1 | `src/coop.cpp:2816` ApplyActionToGhost` |
| 1332 | 35 | 185 | 1 | `src/coop.cpp:3076` ApplyPedStateToGhost` |
| 1122 | 32 | 165 | 1 | `src/coop.cpp:3580` ApplyOutfitToGhost` |
| 1122 | 32 | 122 | 1 | `src/coop.cpp:5467` ApplyRemoteActionV2` |
| 1122 | 32 | 209 | 2 | `server/server.cpp:1910` main` |
| 930 | 29 | 83 | 0 | `src/coop.cpp:2989` UpdateGhostWeaponAnim` |
| 812 | 27 | 39 | 0 | `src/coop.cpp:694` DetectLocalPresenceUi` |
| 756 | 26 | 109 | 0 | `src/coop.cpp:1192` UpdateVisitorMode` |
| 650 | 24 | 125 | 2 | `src/coop.cpp:2234` MoveMountTo` |
| 600 | 23 | 49 | 1 | `src/coop.cpp:1526` ProcessGuestMissionBarrier` |
| 600 | 23 | 29 | 1 | `server/server.cpp:114` TrafficBucketFor` |
| 552 | 22 | 74 | 2 | `tests/mission_barrier_rules_fuzz.cpp:25` Worker` |
| 462 | 20 | 28 | 1 | `server/server.cpp:266` IsGameThreadMessage` |
| 420 | 19 | 108 | 0 | `src/coop.cpp:1581` EnsureGuestSaveBackup` |
| 380 | 18 | 50 | 1 | `src/coop.cpp:1423` UpdateHostMissionBarrier` |
| 380 | 18 | 26 | 2 | `server/server.cpp:685` ParseSavedLine` |
| 342 | 17 | 61 | 1 | `src/coop.cpp:4602` UpdateLocalActionState` |
| 342 | 17 | 86 | 0 | `tests/mission_catalog_test.cpp:31` main` |
| 306 | 16 | 106 | 1 | `src/coop.cpp:3835` RespawnGhostPed` |
| 272 | 15 | 47 | 1 | `src/coop.cpp:1476` ProcessHostMissionBarrier` |
| 272 | 15 | 64 | 1 | `src/coop.cpp:3266` ApplyPlayerOutfitToGhost` |
| 272 | 15 | 59 | 2 | `src/coop.cpp:4794` IsEntityHeadshot` |
| 272 | 15 | 67 | 1 | `src/coop.cpp:5654` ApplyRemotePlayerVehicle` |
| 272 | 15 | 65 | 2 | `src/world_scan.cpp:159` Tick` |

## Топ 30 функций по длине

| lines | branches | CRAP | функция |
|---|---|---|---|
| 1712 | 373 | 140250 | `src/coop.cpp:6225` Tick` |
| 574 | 155 | 24492 | `net/client.cpp:879` RunOnce` |
| 386 | 88 | 8010 | `src/coop.cpp:3943` ApplyRemoteFrame` |
| 280 | 49 | 2550 | `tests/frame_reader_fuzz.cpp:93` main` |
| 229 | 11 | 156 | `src/coop.cpp:5892` InitNetworkClient` |
| 209 | 32 | 1122 | `server/server.cpp:1910` main` |
| 200 | 15 | 272 | `tests/config_test.cpp:48` main` |
| 185 | 35 | 1332 | `src/coop.cpp:3076` ApplyPedStateToGhost` |
| 181 | 47 | 2352 | `src/coop.cpp:4893` UpdateLocalShotEvents` |
| 168 | 42 | 1892 | `src/coop.cpp:2816` ApplyActionToGhost` |
| 165 | 32 | 1122 | `src/coop.cpp:3580` ApplyOutfitToGhost` |
| 129 | 86 | 7656 | `src/config.cpp:150` ApplyLine` |
| 125 | 24 | 650 | `src/coop.cpp:2234` MoveMountTo` |
| 123 | 13 | 210 | `src/coop.cpp:5262` CheckRemoteModelChange` |
| 122 | 32 | 1122 | `src/coop.cpp:5467` ApplyRemoteActionV2` |
| 109 | 26 | 756 | `src/coop.cpp:1192` UpdateVisitorMode` |
| 108 | 19 | 420 | `src/coop.cpp:1581` EnsureGuestSaveBackup` |
| 108 | 14 | 240 | `src/coop.cpp:5095` ApplyMoveToGhost` |
| 106 | 16 | 306 | `src/coop.cpp:3835` RespawnGhostPed` |
| 104 | 9 | 110 | `src/world_sync.cpp:3307` Reset` |
| 92 | 13 | 210 | `src/coop.cpp:2659` EnsureGhostWeaponInHands` |
| 88 | 1 | 6 | `src/config.cpp:51` WriteDefaultIni` |
| 86 | 17 | 342 | `tests/mission_catalog_test.cpp:31` main` |
| 85 | 3 | 20 | `src/coop.cpp:4363` CleanupGhost` |
| 83 | 29 | 930 | `src/coop.cpp:2989` UpdateGhostWeaponAnim` |
| 79 | 11 | 156 | `src/coop.cpp:2065` RequestMountModel` |
| 76 | 7 | 72 | `server/server.cpp:960` PrintStatus` |
| 75 | 13 | 210 | `src/coop.cpp:5389` DetectLocalActionV2` |
| 74 | 22 | 552 | `tests/mission_barrier_rules_fuzz.cpp:25` Worker` |
| 70 | 14 | 240 | `src/coop.cpp:5725` PollHostMissions` |

## Дублирование (6-строчные шинглы, топ 30)

| повторений | места |
|---|---|
| 4 | `src/actor_sync.cpp:2689` |
| 4 | `src/actor_sync.cpp:2691` |
| 4 | `src/world_sync.cpp:1738` |
| 4 | `src/world_sync.cpp:1739` |
| 4 | `src/world_sync.cpp:1741` |
| 4 | `src/world_sync.cpp:1742` |
| 4 | `src/world_sync.cpp:1744` |
| 3 | `src/actor_sync.cpp:1103`, `src/world_sync.cpp:168` |
| 3 | `src/actor_sync.cpp:1104`, `src/world_sync.cpp:169` |
| 3 | `src/actor_sync.cpp:2030`, `src/world_sync.cpp:1721` |
| 3 | `src/actor_sync.cpp:2039` |
| 3 | `src/actor_sync.cpp:2040` |
| 3 | `src/actor_sync.cpp:2041` |
| 3 | `src/actor_sync.cpp:2042` |
| 3 | `src/actor_sync.cpp:2682` |
| 3 | `src/actor_sync.cpp:2683` |
| 3 | `src/actor_sync.cpp:2684` |
| 3 | `src/actor_sync.cpp:2685` |
| 3 | `src/actor_sync.cpp:2686` |
| 3 | `src/actor_sync.cpp:2687` |
| 3 | `src/actor_sync.cpp:557`, `src/coop.cpp:2678` |
| 3 | `src/actor_sync.cpp:558`, `src/coop.cpp:2679` |
| 3 | `src/actor_sync.cpp:559`, `src/coop.cpp:2680` |
| 3 | `src/actor_sync.cpp:5812` |
| 3 | `src/actor_sync.cpp:5813` |
| 3 | `src/coop.cpp:3451` |
| 3 | `src/world_sync.cpp:260` |
| 3 | `src/world_sync.cpp:261` |
| 2 | `net/client.h:160`, `src/coop.h:44` |
| 2 | `net/client.h:161`, `src/coop.h:45` |

## natives.h

- объявлений: 285
- групп с одинаковым хешем: 0
- групп с одинаковым именем: 0

## TODO/FIXME/HACK

Всего маркеров: 0; warning-disable: 0

## Зачем это

Baseline нужен, чтобы каждая единица шлифовки (ub-mode) имела проверяемую цель: уменьшить CRAP/длину/дублирование, убрать мёртвый код, не меняя поведение (build + CI 41/41). Повторный запуск после шлифовки даёт дельту.
