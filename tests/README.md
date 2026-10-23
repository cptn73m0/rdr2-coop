# Headless-тесты сервера (без игры)

`tests/integration_server.py` запускает `rdr2_coop_server.exe` и проверяет протокол
по TCP: JOIN (включая гонку одновременных JOIN), роли/лимиты, host-only
сообщения, точные размеры, WorldSnapshot, мусорные кадры, ретрансляцию
Leave/PlayerLeft/Died/Respawned/HorseOutfit, connection cap/per-IP rate limit,
`players.txt` persistence/recovery, graceful shutdown, а также actor-сообщения
v16 (spawn/frames/despawn/event/outfit/resync/appearance) — когда они есть в
`net/protocol.h`.

## Требования

- Python 3.8+ (запускать обычным `python`; режим `-O`/`PYTHONOPTIMIZE` отклоняется —
  проверки в тестах явные, `require()`, а не `assert`).
- собранный `rdr2_coop_server.exe` в каноническом каталоге `build-server-ci`
  (или ином, переданном через `--exe`).

Версия протокола, ID сообщений, размеры payload'ов и лимиты сервера
(`kMaxNameLength`, `kMaxPreJoinConnections`, `kMaxActiveConnections`,
`kMaxNewConnectionsPerIpPerSecond`) читаются тестом **из `net/protocol.h` и
`server/server.cpp`**, а затем сверяются с запиненными значениями
(32/8/32/12). Случайное ослабление лимита в server.cpp валит suite, а не
подхватывается тестом автоматически.

Для протокола v14 дополнительно запинен размер `OutfitState`: `_struct_size`
обязан вернуть **784 B** (model u64 + count u32 + components[96] u32 +
wearable[96] u32). Если размер в `net/protocol.h` изменится, тест упадёт и
потребует осознанного обновления тестов/документации под новую версию.

Для протокола v16 при наличии `Actor*`-сообщений в header также пинятся:
`kMaxActors=12`, `ActorSpawn=64 B`, `ActorFrame=32 B`,
`ActorSpawnBatch≤776 B`, `ActorFrames≤392 B`, `ActorDespawn/ActorResync=12 B`,
`ActorEvent=32 B`, `ActorOutfit=792 B`, `ActorAppearanceHeader=12 B`,
`ActorAppearanceEntry=28 B`, `kActorAppearanceMaxComponents=16`,
`ActorAppearance max=460 B`; ID 0x18–0x1E. Если header откатить на v14,
actor-тесты скипаются с `[skip]` и не валят базовый suite.

## Запуск

```powershell
# из корня репозитория (server уже собран в build-server-ci)
python tests\integration_server.py --exe build-server-ci\Release\rdr2_coop_server.exe

# или через runner: при необходимости соберёт build-server-ci и запустит
powershell -ExecutionPolicy Bypass -File tests\run_server_tests.ps1
```

Опции: `--keep` сохраняет временную папку с логами/`players.txt`;
`--strict` считает known-fail тесты падениями (механизм сохранён, сейчас
`KNOWN_FAIL_TESTS` пуст — известных-падающих 0).

## Что покрыто (20 базовых сценариев + 9 actor-тестов v16)

| Тест | Проверка |
|---|---|
| `test_join_rejections` | несовпадение версии → VersionMismatch; `:`-ник → BadName; сервер жив после отказов |
| `test_join_name_validation` | пустой/33/40-байтный/control (`\n`,`\r`,`\x01`) ник → JoinRejected(BadName), а не молчаливый close; имя ровно 32 Б принимается; control-ник не инжектится в `players.txt` |
| `test_roles_and_full` | HOST/GUEST, занятый ник → NameTaken, третий игрок → ServerFull; payload PlayerJoined (имя/модель) |
| `test_prejoin_and_ping` | до JOIN Frame/Died закрывают соединение; Ping (8 байт) работает до JOIN |
| `test_world_host_only` | WorldSnapshot 64 ретранслируется, >64 отбрасывается, гость не подменяет мир |
| `test_payload_sizes_and_host_only_messages` | точные размеры Frame/Outfit/Action/Vehicle/Shot/Mission; OutfitState 784 B + wearable (v14), short/extra Frame дропаются; host-only MissionEvent/State/barrier/TrainSnapshot, carCount ≤ 16 |
| `test_relay_misc_messages` | Died/Respawned ретранслируются с ником отправителя; HorseOutfit (784 Б OutfitState + wearable) и Leave/PlayerLeft |
| `test_worldstate_host_only` | WorldState от хоста ретранслируется, от гостя — блокируется (P0-фикс сервера) |
| `test_oversized_and_truncated` | len > 64 КБ и обрезанные кадры не роняют сервер |
| `test_host_migration` | при выходе хоста гость получает HostChanged(1) и может слать WorldSnapshot |
| `test_join_race` | 50 раундов по 3 одновременных JOIN (threading.Barrier): ровно 2 JOIN_OK, ровно один is_host, третий → ServerFull; после JOIN гость→хост FRAME релеится; сервер чистит сессии |
| `test_idle_timeout` | молчащий клиент отключается по таймауту (2-й аргумент сервера) |
| `test_fuzz_frames` | 300 случайных кадров/типов/размеров + oversize + fragmented Ping (8-байтовый payload) |
| `test_players_txt_recovery` | битые строки и stale `.tmp` не мешают восстановлению двух игроков |
| `test_players_txt_corrupt_joining_player` | испорченные строки **входящего** игрока (`Alice:junk:...`, `Alice:`, NaN/Inf, мало полей) → сервер не падает, JOIN без сохранённой позиции; валидный Bob восстанавливается |
| `test_concurrent_fuzz` | 3 rogue + 2 sender + 2 drain потока 3 с; сервер жив и отвечает Ping; host/guest читают relay, чтобы не упереться в серверный лимит очереди отправки |
| `test_connection_cap` | flood: срабатывает pre-JOIN лимит (`kMaxPreJoinConnections`), принятых «сырых» сокетов не больше лимита, активный cap не срабатывает раньше; после flood валидный клиент входит |
| `test_per_ip_rate_limit` | 30 быстрых подключений: проверяются записи `per-IP rate limit`, реально закрытые сервером сокеты; после окна вход работает |
| `test_graceful_shutdown` | сервер корректно завершается по тест-хуку и сохраняет players.txt |
| `test_persistence` | координаты сохраняются в `players.txt` и восстанавливаются после рестарта; первый игрок = HOST |

### actor-тесты (v15 base + v16 appearance; активны при наличии `Actor*` в header)

| Тест | Проверка |
|---|---|
| `test_actor_spawn_host_only` | 0x18: host→guest byte-identical; count 0/1/12 reley, count 13 drop; short/long drop; guest→host blocked |
| `test_actor_frames_host_only` | 0x19: relay byte-identical; count 0/12/13; short/long; NaN/Inf/|x|≥100000 drop; guest→host blocked |
| `test_actor_despawn_event_outfit` | 0x1A/0x1B/0x1C: exact 12/32/792 B, relay byte-identical, short/long drop, guest→host blocked |
| `test_actor_resync_direction` | 0x1D: guest→host relay only to host; host→guest blocked; short/long/epoch=0 drop |
| `test_actor_validation_drops` | epoch=0, actorId=0, non-finite/absurd coords, unknown type drop (0x18–0x1C) |
| `test_actor_duplicate_actor_ids` | duplicate actorId в spawn/frames-батче → сервер дропает батч целиком (O(count²), лог не пишется) |
| `test_actor_gating_no_actor_log` | обычная сессия без actor-сообщений → в `server_console.log` **0 строк `actor:`** (серверный gating) |
| `test_actor_appearance_host_only` | 0x1E: host→guest byte-identical; count 1/16 (max 460 B) relay; count 17/oversized drop; guest→host blocked |
| `test_actor_appearance_validation` | 0x1E: точный размер `12+count*28`; count 0/17, epoch=0, actorId=0, reserved!=0, short/long drop; сервер жив |

### NPC actor-репликация (E1–E3b + E2c-b) — что проверено

Wire/relay/валидация actor-сообщений покрыты actor-тестами выше
(**29/29** автоматических сценариев на `build-server-ci`, protocol v16:
appearance=on, 16 компонентов / 460 B). Клиентская логика
NPC/E2c-b проверена на **одном ПК** с fake host/guest и реальным хостом
(`tools/fake_actor_client.py`, см. `tools/README.md`); двух-ПК визуальная
приёмка E2c-b ещё pending (ARINA v16 задеплоена, второй ПК пока не запускается).

- **E1 (human-only):** discovery/roster, spawn heartbeat/frames/despawn,
  guest-прокси, resync, gating `npc_actor=0` → 0 `actor:`/`probe:` строк.
- **E2a (meta-outfit):** host присылает `metaOutfit`; в тесте 5/5 применено,
  `failed=0` (probe: ~22% ambient имеют outfit hash).
- **E2b (всадники):** rider+horse, mount spawn ×9, `failed=0`, очистка при
  dismount/despawn; движение плавное (gait_restart=0, err_p95≈0).
- **E3a:** сценарии/анти-наложение (skip 8 м + warp revert 3 м) — визуально
  подтверждено «двое курят на разных местах»; смерть/возрождение, труп.
- **E3b:** валидные weapon hashes, draw/aim/shoot/reload;
  `state=shoot=1450`, `reload=385`, `weapon fail=0`.
- **E2c-a:** host outfit brute-force resolver (890 моделей / 4568 кандидатов)
  механически работает, но random ambient NPC без именованного outfit:
  `resolved=0/failed=11`; fake guest отслеживает `outfits nonzero=N
  outfit_updates=K resolved_late=Q` и `--outfit-wait`.
- **E2c-b (v16):** `ActorAppearance` (0x1E) — host-only relay, exact
  `12+count*28` (count 1..16, max 460 B), `reserved=0`; host capture
  `count=8-10 valid=8-10`, fake guest `appearance_msgs=504
  appearance_actors=8 entry_max=10 errors=0`. Визуал — 2-ПК pending.
  Fake guest отслеживает appearance-статистику (`--outfit-wait`).
- Используемые сообщения: **0x18** `ActorSpawnBatch` (спавн + heartbeat),
  **0x19** `ActorFrames` (10 Гц), **0x1A** `ActorDespawn` (gone/dead/evicted),
  **0x1D** `ActorResync` (guest→host; Client::SendActorResync, троттлинг ≤1/с),
  **0x1E** `ActorAppearance` (host→guest, E2c-b); 0x1B/0x1C
  зарезервированы/не применяются.
- Флаги INI: `npc_actor=1` (default 0), `npc_cap` 1..12 (default 8),
  `npc_radius` 20..100 (default 60), `npc_appearance=1` (E2a + E2c-b),
  `npc_debug=1` — probe-логи.
- `test_actor_gating_no_actor_log` — **серверный** тест (mock-клиенты не шлют
  actor-сообщения), поэтому клиентский gating проверяется вручную
  (`npc_actor=0` → 0 строк `actor:`/`probe:` в `rdr2-coop.log`);
  чек-лист/метрики — `docs/NPC_E1_TESTING.md`, E3b-команды — `tools/README.md`.

## C++ headless-тесты (сеть)

```powershell
cmake -S tests -B tests\build -A x64
cmake --build tests\build --config Release
.\tests\build\Release\frame_reader_fuzz.exe
.\tests\build\Release\client_reconnect_test.exe
.\tests\build\Release\config_test.exe
.\tests\build\Release\world_snapshot_parse_test.exe
.\tests\build\Release\mission_catalog_test.exe
.\tests\build\Release\world_entity_budget_test.exe
.\tests\build\Release\mission_barrier_rules_fuzz.exe
powershell -File tests\restore_saves_test.ps1
```

- `frame_reader_fuzz` — фрагментация, пары кадров, мусор, oversize length,
  truncated frame, byte-by-byte stream, 4 потока × 20k случайных буферов,
  **границы `kMaxPayload` (65536 принимается, 65537/0xFFFFFFFF/oversized Feed
  сбрасываются без выдачи кадра и без зависания)**.
- `client_reconnect_test` — фейковый TCP-сервер отвечает JOIN_OK и рвёт
  соединение; клиент должен переподключиться (join ≥ 2, connection-lost ≥ 1).
- `config_test` — `LoadFromFile`: дефолты, флаги/hex/битые строки, unknown/
  дубликаты ключей, регистронезависимые bool и inline `;`-комментарии,
  порт 0/70000/abc/65535. Все проверки строгие (`Check`): регрессия парсера
  валит тест, non-fatal pending-механизма нет.
- `world_snapshot_parse_test` — 4 потока × 20k: валидные снапшоты 0..64
  сущностей и произвольный мусор; строгая проверка размера/count.
- `mission_catalog_test` — 80 уникальных миссий, runtime/title, 23 награды
  на 17 миссий, корректные binding/amount, нет дублей.
- `world_entity_budget_test` — 4 потока × 10k: mission-приоритет, лимиты
  Ped/Vehicle/Object и порядок против reference-реализации.
- `mission_barrier_rules_fuzz` — 4×100k случайных фаз/ролей/offer/epoch:
  host/guest события, disjoint role phases.
- `restore_saves_test.ps1` — restore + pre-restore snapshot на temp-каталогах.

## CI

`.github/workflows/ci.yml` на Windows собирает клиент/сервер и запускает все
тесты. Локально: `powershell -File scripts\ci.ps1`.
Локальный CI использует тот же канонический `build-server-ci`, что и GitHub
Actions; fallback на старые `build-server-v10/v12` удалён.

## Важно

- Тесты не запускают игру и не проверяют нативы/визуал.
- Механизм known-fail (`KNOWN_FAIL_TESTS` + `--strict`) сохранён для будущих
  ожидаемых падений; сейчас он пуст — все сценарии строгие.
- Лимит одновременных соединений (`kMaxActiveConnections=32`) после P0
  недостижим снаружи: сначала срабатывают per-IP rate limit (12/с) и
  pre-JOIN limit (8 сокетов). Тест проверяет, что cap остаётся
  defense-in-depth выше достижимого числа сокетов и не срабатывает раньше
  pre-JOIN лимита.
- Для игровых проверок см. `docs/TESTING.md` (два ПК); E1 NPC-акторы —
  `docs/NPC_E1_TESTING.md` (S0–S9, метрики `actor:`, откат).
