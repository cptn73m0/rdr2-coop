# Headless-тесты сервера (без игры)

`tests/integration_server.py` запускает `rdr2_coop_server.exe` и проверяет протокол
по TCP: JOIN, роли, host-only сообщения, размеры, WorldSnapshot, мусорные кадры,
`players.txt` persistence.

## Требования

- Python 3.8+
- собранный `rdr2_coop_server.exe`

## Запуск

```powershell
# из корня репозитория
python tests\integration_server.py --exe build-server-v10\Release\rdr2_coop_server.exe

# или через runner (соберёт v10 и запустит)
powershell -ExecutionPolicy Bypass -File tests\run_server_tests.ps1
```

Флаг `--keep` сохраняет временную папку с логами/`players.txt` для разбора.

## Что покрыто

| Тест | Проверка |
|---|---|
| `test_join_rejections` | версия, пустой/длинный/`:`-ник, сервер жив после отказов |
| `test_roles_and_full` | HOST/GUEST, занятый ник, третий игрок отклоняется |
| `test_prejoin_and_ping` | сообщения до JOIN закрывают соединение; Ping работает до JOIN |
| `test_world_host_only` | WorldSnapshot 64 ретранслируется, >64 отбрасывается, гость не подменяет мир |
| `test_payload_sizes_and_host_only_messages` | точные размеры Frame/Outfit/Action/Vehicle/Shot/Mission; host-only MissionEvent/State/barrier/TrainSnapshot; carCount ≤ 16 |
| `test_oversized_and_truncated` | len > 64 КБ и обрезанные кадры не роняют сервер |
| `test_host_migration` | при выходе хоста гость получает HostChanged(1) и может слать WorldSnapshot |
| `test_idle_timeout` | молчащий клиент отключается по таймауту (2-й аргумент сервера) |
| `test_players_txt_recovery` | битые строки и stale `.tmp` не мешают восстановлению двух игроков |
| `test_concurrent_fuzz` | 3 rogue + 2 sender потока 3 с; сервер жив и отвечает Ping |
| `test_connection_cap` | флуд 40 TCP-соединений; лимит не роняет сервер, новый клиент входит |
| `test_per_ip_rate_limit` | >12 подключений/с с одного IP отсекаются; после окна вход работает |
| `test_graceful_shutdown` | сервер корректно завершается по тест-хуку и сохраняет players.txt |
| `test_fuzz_frames` | 300 случайных кадров/типов/размеров + oversize + fragmented Ping |
| `test_persistence` | координаты сохраняются в `players.txt` и восстанавливаются после рестарта |

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
powershell -File tests\restore_saves_test.ps1
```

- `frame_reader_fuzz` — фрагментация, пары кадров, мусор, oversize length,
  truncated frame, byte-by-byte stream, 4 потока × 20k случайных буферов;
  `FrameReader` не зависает и восстанавливается.
- `client_reconnect_test` — фейковый TCP-сервер отвечает JOIN_OK и рвёт
  соединение; клиент должен переподключиться (join ≥ 2, connection-lost ≥ 1).
- `config_test` — `LoadFromFile`: флаги/hex/битые строки/defaults.
- `world_snapshot_parse_test` — 4 потока × 20k: валидные снапшоты 0..64
  сущностей и произвольный мусор; строгая проверка размера/count.
- `mission_catalog_test` — 80 уникальных миссий, runtime/title, 23 награды
  на 17 миссий, корректные binding/amount, нет дублей.
- `world_entity_budget_test` — 4 потока × 10k: mission-приоритет, лимиты
  Ped/Vehicle/Object и порядок против независимой reference-реализации.
- `restore_saves_test.ps1` — restore + pre-restore snapshot на temp-каталогах.

## CI

`.github/workflows/ci.yml` на Windows собирает клиент/сервер и запускает все
тесты. Локально: `powershell -File scripts\ci.ps1`.

## Важно

- Тесты не запускают игру и не проверяют нативы/визуал.
- Для игровых проверок см. `docs/TESTING.md` (два ПК).