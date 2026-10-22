# tools/ — вспомогательные однопК-инструменты

## `fake_actor_client.py` — headless-«игрок» протокола v15 для проверки NPC-репликации

Заменяет второй ПК при тестировании E1 (actor-репликация NPC) на одной машине.
Требуется только Python 3 (stdlib) и запущенный сервер v15; игра запускается
только на одном ПК.

Оба режима используют TCP-релей (`--host 127.0.0.1 --port 7777`), framing
v15 `[len:u32][type:u8][payload]`, JOIN с protocol_version=15 и пишут лог в
stdout и в `tools/fake_actor_client.log` (append; файл `*.log` уже
игнорируется корневым `.gitignore` и не коммитится).

### Быстрый старт

| Режим | Порядок запуска | Что проверяет |
|---|---|---|
| `--mode host` | сервер → **fake host** → игра-гость | клиентский apply на игровом госте: 5 NPC-прокси, движение, despawn/respawn |
| `--mode guest` | сервер → **игра-хост** → fake guest | игровой хост отдаёт actor-трафик: spawn/frames/despawn, валидация форматов |

### Результаты проверки на одном ПК (22.10.2026)

- **Тест A** (`--mode host`, fake host → игровой гость, E1 `npc_actor=1`):
  спавн 5 NPC, кадры 10 Гц, `despawn id=5` + респавн; в логе гостя
  `proxies=5 appliedFrames≈200 timeouts=0`, ошибок нет. **PASS**.
- **Тест B** (`--mode guest`, игровой хост → fake guest, cap=12):
  получен `spawn batch n=12 unique=12`, кадры ~13 Гц (фактическая частота
  игрового хоста), `errors=0`, 12 уникальных actorId. **PASS**.
- Инструмент проверен на реальном сервере v15 и реальной игре; заменяет
  второй ПК для wire/apply-части E1, но **не заменяет** двухПК-приёмку
  `docs/NPC_E1_TESTING.md` (метрики «неотличимости», FPS/tick, визуал).

### Режим A: fake HOST (эмулирует хоста, игра — гость)

**Порядок важен: сначала сервер, потом fake host, потом игра-гость.**

1. Запустить сервер (v15):
   ```powershell
   .\build-server-ci\Release\rdr2_coop_server.exe 7777
   ```
2. Запустить fake host **первым** (он должен стать HOST):
   ```powershell
   python tools\fake_actor_client.py --mode host --name TestHost --duration 300
   ```
   Дождаться `fakehost: join ok ... is_host=1` и `fakehost: waiting for guest FRAME`.
3. Запустить RDR2, подключиться к серверу как **второй игрок** (гость).
   В его `rdr2-coop.ini` должно быть `npc_actor = 1` (E1 включён) и
   желательно `npc_debug = 1` для `probe:`-логов.
4. Когда игра-гость пришлёт `FRAME` со своей позицией, fake host:
   - отправит `ActorSpawnBatch` (0x18) — 5 human NPC в 3–6 м от игрока;
   - 10 Гц шлёт `ActorFrames` (0x19) — NPC ходят по кругу радиусом 3 м;
   - через `--despawn-after` (default 30 с) отправит `ActorDespawn` (0x1A)
     для actorId=5 и перестанет слать его в кадрах;
   - через `--respawn-after` (default 10 с) может заспавнить actorId=5 снова;
   - по окончании `--duration` (default 300 с) или Ctrl+C отправит LEAVE.

   Ожидаемо в игре-госте: 5 NPC рядом с персонажем, плавно ходят; через ~30 с
   один исчезает, через ~10 с (если включён respawn) появляется снова.

   Лог fake host:
   ```
   fakehost: join ok ...
   fakehost: guest frame at x=... y=... z=...
   fakehost: spawn epoch=1 n=5
   fakehost: frames sent=N
   fakehost: despawn id=5
   ```

5. Проверить, что игра-гость не зависла/не задублировала NPC; остановить
   fake host (Ctrl+C → LEAVE).

### Режим B: fake GUEST (игра — хост, эмулируется гость)

**Порядок: сервер → игра-хост → fake guest.**

1. Запустить сервер v15.
2. Запустить RDR2 и подключиться **первым** (игра должна стать HOST);
   в INI: `npc_actor = 1`.
3. Запустить fake guest вторым:
   ```powershell
   python tools\fake_actor_client.py --mode guest --name TestGuest --duration 120
   ```
   Fake guest принимает `ActorSpawnBatch`/`ActorFrames`/`ActorDespawn` от
   игрового хоста, валидирует размеры/epoch/count/actorId/конечность координат/
   монотонность `seq`, считает spawn/frames/despawn/errors и раз в 2 с печатает:
   ```
   fakeguest: spawns=N unique=M frames=N fps≈X despawns=K errors=E
   ```
4. Итог: `fakeguest: PASS` и exit 0, если получен хотя бы один
   `ActorSpawnBatch` и не менее 10 `ActorFrames`; иначе `FAIL` и exit 2
   (обычно `npc_actor=0` у игрового хоста или хост стоит далеко от NPC).

### Полезные опции

| Опция | По умолчанию | Назначение |
|---|---|---|
| `--mode host/guest` | — | режим (обязателен) |
| `--host`, `--port` | `127.0.0.1`, `7777` | адрес сервера |
| `--name` | `TestHost` / `TestGuest` | ник в JOIN |
| `--duration` | 300 (host) / 120 (guest) | сколько секунд работать |
| `--model 0x...` | `0x54D8BE8E` | модель ped'а для JOIN |
| `--num` | 5 | число NPC (только host, 1..12) |
| `--circle-radius` | 3.0 | радиус круга движения NPC, м |
| `--speed` | 1.0 | скорость движения NPC, м/с |
| `--despawn-after` | 30 | через сколько секунд удалить actorId=5 |
| `--respawn-after` | 10 | через сколько после despawn вернуть actorId=5 (0 = выкл.) |
| `--peds-json` | `third_party/rdr2-hashdb/Peds.json` | откуда брать хэши моделей |
| `--models` | — | свои модели (hex через запятую, ≥`--num`) |
| `--log-file` | `tools/fake_actor_client.log` | лог-файл (append) |

### Важно / troubleshooting

- **Первый подключившийся — HOST.** Если fake host запустить после игры, он
  получит `is_host=0` и выйдет с кодом 3 (`fakehost: ERROR: this tool is not
  HOST`). Аналогично fake guest, запущенный до игры, станет HOST — exit 3.
- Сервер и игра должны быть **protocol v15** (`kProtocolVersion=15`); при
  несовпадении — `JOIN rejected: VersionMismatch`.
- В игре-госте (`--mode host`) должен быть `npc_actor = 1`, иначе E1 не примет
  Actor-сообщения и NPC не появятся.
- В игре-хосте (`--mode guest`) должен быть `npc_actor = 1`, иначе не будет
  `actor:`-трафика и fake guest завершится с exit 2.
- Fake host **не эмулирует FRAME/анимации реального игрока** (только JOIN и
  actor-сообщения): игровой гость может не увидеть нормального педа/призрака
  фейкового хоста. Цель теста — NPC-прокси, а не пед второго игрока.
- Fake guest **не реплицирует NPC** и не шлёт actor-сообщения: он только
  принимает и валидирует трафик игрового хоста (плюс JOIN/PING/LEAVE).
- Максимум 2 игрока на сервере: закройте лишние клиенты/старые процессы.
- Ctrl+C в любом режиме → корректный LEAVE; host завершается 0, если успел
  отправить кадры, иначе 1; guest — 0 при PASS, 2 при FAIL.
- Лог append-ится в `tools/fake_actor_client.log`; файл — runtime-артефакт:
  корневой `.gitignore` (`*.log`) его не коммитит. При повторных прогонах
  удобно удалить/переименовать его перед тестом.
- Реальный прогон выполняется вместе с пользователем/Lead: этот инструмент
  не заменяет игровую проверку, а позволяет проверить wire-часть и клиентский
  apply E1 без второго ПК.
