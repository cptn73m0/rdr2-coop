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
| `test_payload_sizes_and_host_only_messages` | точные размеры Frame/Outfit/Action/Vehicle/Shot/Mission; host-only MissionEvent/State; barrier host-only/guest-only фазы |
| `test_oversized_and_truncated` | len > 64 КБ и обрезанные кадры не роняют сервер |
| `test_host_migration` | при выходе хоста гость получает HostChanged(1) и может слать WorldSnapshot |
| `test_idle_timeout` | молчащий клиент отключается по таймауту (2-й аргумент сервера) |
| `test_fuzz_frames` | 300 случайных кадров/типов/размеров + oversize + fragmented Ping |
| `test_persistence` | координаты сохраняются в `players.txt` и восстанавливаются после рестарта |

## Важно

- Тесты не запускают игру и не проверяют нативы/визуал.
- Для игровых проверок см. `docs/TESTING.md` (два ПК).