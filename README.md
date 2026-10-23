# RDR2 CO-OP — кооперативный мод для одиночного сюжета

[![CI](https://github.com/cptn73m0/rdr2-coop/actions/workflows/ci.yml/badge.svg?branch=main)](https://github.com/cptn73m0/rdr2-coop/actions/workflows/ci.yml)

Кооперативный мод для **Red Dead Redemption 2** (Story Mode): два игрока
играют по сети, каждый проходит сюжет у себя, а второй игрок виден в мире как
**пед-призрак** — с синхронизацией движения, анимаций, оружия, лошади и
частично одежды (shop-предметы и wearable states). Плюс общее время/погода и
сетевые функции (роли, reconnect, host migration).

Кооператив **не является полноценным совместным прохождением**: сюжетные
миссии, животные и транспорт мира по-прежнему не реплицируются; NPC-репликация
(человеческие NPC) реализована как экспериментальная ветка E1–E3b и требует
`npc_actor=1` на обоих ПК (см. «Известные ограничения»).

## Статус

Проверено в игре двумя игроками на билде **1.0.1491.50**; последний игровой
стабильный тег — **stable-v14** (`OutfitState` 784 B, wearable states одежды).
**Текущий протокол — v16** (actor-репликация NPC E1–E3b + E2c-b); v15/v16
несовместимы. Этапы E1–E3b и **E2c-b (ActorAppearance 0x1E)** реализованы и
проверены на одном ПК: E1–E3b — fake host/guest и игровые прогоны, E2c-b —
по данным реального хоста (`asset capture count=8-10 valid=8-10`) и fake guest
v16 (`appearance_msgs=504 appearance_actors=8 entry_max=10 errors=0`).
Визуальная двух-ПК приёмка E2c-b ещё **pending** (пользователь пока не
запускает второй ПК). Деплой v16: ASI `63388AD3`, server `4EDAE4B6`; откат
v15-пары — `dist\npc-e2c-v15\`. Флаг `npc_actor` по умолчанию **0**; для теста
включать на обоих ПК. Клиенты v14/v15/v16 несовместимы (сервер отклоняет JOIN
с `VersionMismatch`). Теги этапов NPC: `npc-e0`, `npc-e1`, `npc-e2`,
`npc-e2b`, `npc-e3`, `npc-e3b`, `npc-e2c-b`.

### Что работает

- Сервер-релей: JOIN / FRAME / LEAVE, таблица игроков, персистентность
  последней позиции (`players.txt`), лог подключений с IP.
- Роли HOST/GUEST, host migration при выходе хоста, автоматический reconnect
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
- **NPC-репликация (E1–E3b, `npc_actor=1` на обоих ПК).** Хост реплицирует
  human-NPC до `npc_cap` (1..12, default 8) в радиусе `npc_radius` (20..100 м,
  default 60): discovery 10 Гц вокруг обоих игроков, roster с actorId/epoch,
  spawn-heartbeat 2.5 с, кадры 10 Гц, despawn (ушёл/умер/вытеснен), resync
  0x1D; гость — прокси-педы с плавной навигацией и gait с хоста.
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
- Headless-тесты сервера/протокола (**20 базовых + 9 actor-тестов v16**;
  integration **29/29**), C++ 7/7, CI-workflow (артефакты, кеш) и локальный
  `scripts\ci.ps1`.

**INI-флаги NPC:** `npc_actor = 1` (включает E1–E3b + E2c-b), `npc_cap = 8`
(1..12), `npc_radius = 60` (20..100), `npc_appearance = 1` (meta-outfit E2a
и asset/tint E2c-b), `npc_debug = 1` — только диагностические probe-логи.

### Отключено / не работает (экспериментальные флаги = 0)

Код этих функций присутствует в репозитории, но в текущей рабочей сборке они
**выключены**, в игре не проверены или работают нестабильно. Не включать без
отдельной отладки:

- `world_sync` — репликация NPC/транспорта/mission-объектов (экспериментально,
  на практике даёт дёргание/пропажи прокси).
- `action_sync` — сценарии/укрытие/переноска/лутание удалённого игрока.
- `vehicle_sync` — посадка удалённого игрока в транспорт (дилижанс/повозку).
- `mission_sync` / `mission_credit` — активная миссия хоста, барьер старта,
  совместный зачёт MissionData (в игре не проверялось; `mission_credit`
  потенциально опасен для сейва).
- `world_trains` / `train_debug` — R&D поездов (визуальный прокси состава без
  рельсов/сцепки; работает неполно).
- NPC-репликация (E1–E3b + E2c-b) — экспериментальная и **не включена по
  умолчанию**: `npc_actor=0`; mission-NPC, животные/лошадиные трупы, транспорт
  не входят в текущие этапы; визуальная двух-ПК приёмка E2c-b pending.
  Прошлая ветка v14 (NpcSpawn/NpcFrame) в рабочую сборку не входит.

## Известные ограничения

- Сюжетные миссии не проходятся совместно в одном инстансе; общего прогресса
  и барьера старта в стабильной сборке нет.
- NPC-репликация — **экспериментальная (E1–E3b + E2c-b, human-only)**: cap 12
  в радиусе 60 м, mission-NPC исключены; животные, лошадиные трупы и транспорт
  не реплицируются; ambient-популяция гостя не подавляется (возможны
  «двойники» похожих NPC); E2c-b реализован и провалидирован по данным, но
  **визуальная двух-ПК приёмка ещё pending**; лицо/волосы, точные
  анимации/сценарии и катсцены не синхронизируются.
- **ARINA (второй ПК):** v16-пара задеплоена (ASI `63588AD3`, server
  `4EDAE4B6`, INI ок), но двух-ПК прогон пока не проводился — пользователь
  не запускает второй ПК. Текущие результаты — однопК fake host/guest.
- Поезда не реплицируются.
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

# полный CI: клиент + сервер + 29 integration + 7 C++ + restore-тест
powershell -ExecutionPolicy Bypass -File scripts\ci.ps1
```

Двух-ПК чек-лист: [docs/TESTING.md](docs/TESTING.md).

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

Стабильная конфигурация — **экспериментальные флаги ниже = 0** (кроме
`save_backup`, `mission_reward_log` и дефолтного `npc_appearance=1`).
NPC-флаги включаются только для однопК/двух-ПК тестов NPC.

| Флаг | По умолчанию | Состояние |
|---|---|---|
| `world_sync` | 0 | экспериментально, в игре нестабильно — не включать |
| `action_sync` | 0 | не проверено в игре |
| `vehicle_sync` | 0 | не проверено в игре |
| `mission_sync` | 0 | не проверено в игре |
| `mission_credit` | 0 | не проверено, опасно для сейва |
| `save_backup` | 1 | бэкап сейва перед зачётом (защита) |
| `mission_reward_log` | 1 | dry-run лог плана наград (нативы не вызываются) |
| `world_trains` | 0 | R&D, работает неполно |
| `train_debug` | 0 | диагностика R&D |
| `npc_actor` | 0 | E1–E3b + E2c-b NPC-репликация (экспериментально, включать только для теста) |
| `npc_cap` | 8 | 1..12, максимум NPC-прокси |
| `npc_radius` | 60 | 20..100 м, радиус discovery (keep +10 м) |
| `npc_appearance` | 1 | meta-outfit E2a + asset/tint E2c-b (asset приоритетнее) |
| `npc_debug` | 0 | probe-логи `probe:` (только хост) |

## Откат на v15 (E2c-b)

v15-пара сохранена в `dist\npc-e2c-v15\`: скопировать оттуда `rdr2_coop.asi`
и `rdr2_coop_server.exe` на **оба** ПК/сервер. v15↔v16 несовместимы
(`JOIN` → `VersionMismatch`), поэтому откатывать нужно все компоненты сразу.
Текущий v16 деплой: ASI `63388AD3`, server `4EDAE4B6` (ARINA: `63588AD3` /
`4EDAE4B6`).

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

Все сообщения: `u32 length | u8 type | payload`. **Актуальная версия — v16** (actor-репликация NPC E1–E3b + E2c-b ActorAppearance 0x1E). Клиенты v14, v15 и v16 несовместимы: сервер отклоняет JOIN с `VersionMismatch`. v15-пара для отката — `dist\npc-e2c-v15\`.

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
| 0x10 | WorldSnapshot  | хост → сервер → гость  | P1, флаг выключен |
| 0x11 | ActionState    | клиент → сервер → все  | P2, флаг выключен |
| 0x12 | PlayerVehicle  | клиент → сервер → все  | P2, флаг выключен |
| 0x13 | MissionEvent   | хост → сервер → гость  | P3, флаг выключен |
| 0x14 | MissionState   | хост → сервер → гость  | P3, флаг выключен |
| 0x15 | MissionBarrier | host ↔ guest           | P3, флаг выключен |
| 0x16 | HostChanged    | сервер → клиент        | смена роли (host migration) |
| 0x17 | TrainSnapshot  | хост → сервер → гость  | R&D поездов, флаг выключен |
| 0x18 | ActorSpawnBatch | хост → сервер → гость | v15/v16 E1–E3b: до 12×`ActorSpawn` (64 B), max 776 B; host-only |
| 0x19 | ActorFrames   | хост → сервер → гость  | v15/v16: до 12×`ActorFrame` (32 B), max 392 B; движение/флаги боя |
| 0x1A | ActorDespawn  | хост → сервер → гость  | v15/v16: epoch/actorId/reason, 12 B; host-only |
| 0x1B | ActorEvent    | хост → сервер → гость  | v15/v16: событие актора, 32 B; host-only (в текущем клиенте не применяется) |
| 0x1C | ActorOutfit   | хост → сервер → гость  | v15/v16: epoch+actorId+`OutfitState` 784 B = 792 B; host-only (зарезервировано) |
| 0x1D | ActorResync   | гость → сервер → хост  | v15/v16: resync-запрос, 12 B; только от гостя и только хосту |
| 0x1E | ActorAppearance | хост → сервер → гость | v16 E2c-b: header 12 B + count×28 B (count 1..16, max 460 B); host-only relay; точный размер `12+count*28` |

Полная спецификация: [docs/PROTOCOL.md](docs/PROTOCOL.md).

`FramePayload` (88 байт): позиция (x,y,z), heading, `move`, `state`, `flags`,
`action`, здоровье, точка прицела (aimX/Y/Z + дистанция), хэш оружия, `seq`,
резерв (модель лошади, мета-наряд игрока, модель персонажа), наряд лошади.

`OutfitState` (784 байта, v14): `model` (u64) + `count` (u32) +
`components[96]` (u32, shop-предметы) + `wearable[96]` (u32, wearable states
слоёв одежды). Сервер проверяет точный размер и ретранслирует byte-identical;
сообщения 0x09 Outfit и 0x0E HorseOutfit используют одну и ту же структуру.

**v15 (NPC E1–E3b):** actor-сообщения 0x18–0x1D — host-only relay, exact size,
count ≤12, epoch/actorId != 0, finite coords. Используются
`ActorSpawnBatch`/`ActorFrames`/`ActorDespawn` (E1 + heartbeat),
`ActorResync` 0x1D (E1); `ActorEvent`/`ActorOutfit` зарезервированы и в
текущем клиенте не применяются. E2a/E2b/E3a/E3b передаются через поля и флаги
тех же сообщений: `metaOutfit`, `flags` (mounted/dead/drawn/combat/scenario/
reloading/aiming), `reserved` (mount model), `scenarioHash`, `weaponHash`,
`targetActorId`.

**v16 (E2c-b):** добавлено **`ActorAppearance` (0x1E)** — host-only relay,
header 12 B `{epoch u32, actorId u32, count u8, pad[3]}`, entry 28 B
`{drawable, albedo, normal, material, palette u32; tint0/1/2 i16; reserved
u16}`, count 1..16, max payload 460 B, точный размер `12 + count*28`;
reserved каждой записи = 0. Хост шлёт snapshot при спавне/изменении/join
(budget 1 актор/тик, период 2 с), гость применяет через reset +
`_SET_META_PED_TAG` + commit + `UPDATE_PED_VARIATION` (readback/fallback);
asset приоритетнее metaOutfit. v15↔v16 несовместимы.

**INI:** `npc_actor = 1` (включает репликацию; по умолчанию 0),
`npc_cap = 8` (1..12), `npc_radius = 60` (20..100 м),
`npc_appearance = 1` (meta-outfit E2a + asset/tint E2c-b), `npc_debug = 1`
(probe-логи).

Детали — [docs/PROTOCOL.md](docs/PROTOCOL.md), раздел «ACTOR-сообщения».

## Как устроено

- `src/main.cpp` — `DllMain` + цикл скрипта (`ScriptMain`).
- `src/script_hook.cpp/h` — резолв экспортов `ScriptHookRDR2.dll` по PE-таблице
  (проприетарный SDK не нужен).
- `src/native_call.h` — вызов нативов + маршаллинг `Vec3`.
- `src/natives.h` — проверенные хэши нативов (сверены с `rdr2-runtime-scripthook`
  и `rdr3-nativedb-data`).
- `src/coop.cpp` — игровая логика: спавн/рендер педа, движение, одежда, оружие,
  лошадь, смерть/возрождение, время/погода, кадры 60 Гц.
- `src/actor_sync.cpp/h` — NPC-репликация E1–E3b + E2c-a/E2c-b: discovery/
  roster, guest proxies, сценарии/смерть, всадники, оружие/бой, appearance
  capture/apply.
- `src/gen_metaped_outfits.h` — сгенерированная таблица metaped-outfit
  кандидатов (890 моделей / 4568 хэшей) для E2c.
- `tools/fake_actor_client.py` — fake host/guest v16 для однопК-тестов NPC
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
