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

### E2a: внешность NPC (metaOutfit) и паттерны движения

E2a проверяет, что вместе с `ActorSpawn` уходит `metaOutfit` (внешность NPC),
а стоящие NPC не «дёргаются». Нужен probe-лог игрового хоста с `npc_debug=1`
(строки `probe: model=0x... metaOutfit=0x...`; пары с `metaOutfit=0`
игнорируются).

**Визуальный тест: fake host → игровой гость (как ТЕСТ A)**
```powershell
# сервер v15
.\build-server-ci\Release\rdr2_coop_server.exe 7777

# fake host: берём до 5 разных пар model+metaOutfit из лога игры,
# смешанный паттерн (2 круга, 2 линии, 1 стоит)
python tools\fake_actor_client.py --mode host --name TestHost `
  --outfits-from-log "E:\Games\Red Dead Redemption 2\scripts\rdr2-coop.log" `
  --pattern mixed --num 5 --duration 300 --despawn-after 30

# затем запустить игру-гостя (npc_actor=1); смотреть на 5 NPC рядом.
```
Ожидаемые строки:
```
fakehost: outfits from log: N pair(s): 0x...:0x...
fakehost: outfits pairs=N pattern=mixed gait=1.0
fakehost: spawn epoch=1 n=5
fakehost: frames sent=...
```
В игре-госте: 5 NPC с разной внешностью; 2 идут по кругу, 2 — вперёд-назад
по 4 м, 1 стоит неподвижно (не дёргается); actorId=5 — стоящий, через 30 с
исчезает.

**Проверка, что metaOutfit уходит в сеть: игровой хост → fake guest**
```powershell
# игра подключена ПЕРВОЙ (хост) с npc_actor=1, npc_debug=1;
# затем:
python tools\fake_actor_client.py --mode guest --name TestGuest --duration 120
```
Ожидаемые строки:
```
fakeguest: spawn batch epoch=... n=...
fakeguest: outfits nonzero=N
fakeguest: models nonzero=N
```
Если `outfits nonzero=0` — игровой хост не шлёт metaOutfit (проверить E2a
и `npc_debug`), либо в логе нет пар с `metaOutfit != 0`.

### E2a.2: speed scale 8 м/с и gait в ActorFrame

E2a.2 использует два поля `ActorFrame` (размер кадра не менялся):
- `speed` (u8) — **фактическая скорость в м/с на шкале 0..8**:
  `speed_u8 = clamp(round(actual_m_s / 8 * 255), 0, 255)`;
- `pad` (u16) — **gait** (`GET_PED_DESIRED_MOVE_BLEND_RATIO * 1000`):
  walk = 1000, run = 2000, sprint = 3000, idle = 0. Если `pad=0`, гость
  использует fallback по `moveState`; для чистого A/B фейковый хост всегда
  шлёт gait.

Fake host: `--gait 1.0` (walk, default) или `--gait 2.0` (run). Скорость
движения = `--speed * --gait` (default `--speed 1.0`), spawn `moveStyle`
согласован (0 idle / 1 walk / 2 run). Стоящие акторы шлют `speed=0, pad=0`.

**A/B-тест ходьбы (игра-гость):**
```powershell
# A: walk, круг
python tools\fake_actor_client.py --mode host --name TestHost `
  --pattern circle --gait 1.0 --duration 300

# B: run, линия вперёд-назад 4 м (перезапустить после A)
python tools\fake_actor_client.py --mode host --name TestHost `
  --pattern line --gait 2.0 --duration 300
```
Ожидаемые строки host:
```
fakehost: outfits pairs=N pattern=circle gait=1.0
fakehost: spawn epoch=1 n=5
fakehost: frames sent=...
```
В игре-госте: NPC идут плавно без рывков; в B — быстрее (run), скорость
пропорциональна gait; стоящих нет (pattern circle/line).

**Проверка полей в сети (игровой хост → fake guest):**
```powershell
# игра-хост первой с npc_actor=1, затем:
python tools\fake_actor_client.py --mode guest --name TestGuest --duration 120
```
Ожидаемые строки (E2a.2):
```
fakeguest: speed avg=... gait avg=... pad_zero=N
fakeguest: speed min=... max=... u8 (scale 8 m/s); gait min=... max=...
```
Для walk-круга на 1 м/с: `speed avg≈32`, `gait avg≈1.000`, `pad_zero=0`;
для `--pattern mixed` `pad_zero>0` (стоящий actorId5), `gait avg<1.0`.

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
| `--pattern` | `circle` | `circle` / `line` (4 м назад-вперёд) / `mixed` (2 круга, 2 линии, остальные стоят) |
| `--gait` | 1.0 | E2a.2 gait движущихся актёров: `1.0` walk, `2.0` run (уходит в `ActorFrame.pad=gait*1000`; idle=0) |
| `--outfits-from-log` | `E:\Games\Red Dead Redemption 2\scripts\rdr2-coop.log` | пары `model+metaOutfit` из probe-лога; пустая строка — выкл. |
| `--circle-radius` | 3.0 | радиус круга движения NPC, м |
| `--speed` | 1.0 | базовая скорость движения, м/с (фактическая = speed*gait; в кадр идёт speed/8*255) |
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
