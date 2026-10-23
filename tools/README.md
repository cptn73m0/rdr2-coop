# tools/ — вспомогательные однопК-инструменты

## `fake_actor_client.py` — headless-«игрок» протокола v16 для проверки NPC-репликации

Заменяет второй ПК при тестировании E1–E3c/E2c (actor-репликация NPC) на
одной машине. Требуется только Python 3 (stdlib) и запущенный сервер v16;
игра запускается только на одном ПК.

Оба режима используют TCP-релей (`--host 127.0.0.1 --port 7777`), framing
`[len:u32][type:u8][payload]`, JOIN с protocol_version, **автоматически
прочитанным из `net/protocol.h` (`kProtocolVersion`, fallback 16)**; сейчас
это v16. Лог пишется в stdout и в `tools/fake_actor_client.log` (append; файл
`*.log` уже игнорируется корневым `.gitignore` и не коммитится).

**Позиция fake guest:** в guest-режиме инструмент принимает player FRAME
реального хоста и шлёт свой FRAME **в позиции хоста** — гость (и discovery NPC)
появляется рядом с персонажем-хостом, а не на старом спавне. Если хостовых
FRAME нет (fake host), через 2 с используется фиксированная точка
(100,100,100).

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
- Инструмент проверен на реальном сервере v16 и реальной игре; заменяет
  второй ПК для wire/apply-части E1, но **не заменяет** двухПК-приёмку
  `docs/NPC_E1_TESTING.md` (метрики «неотличимости», FPS/tick, визуал).

### Режим A: fake HOST (эмулирует хоста, игра — гость)

**Порядок важен: сначала сервер, потом fake host, потом игра-гость.**

1. Запустить сервер (v16):
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
# сервер v16
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

### E2b: конные NPC (`--mounted`)

E2b сдаёт в `ActorSpawn`:
- `flags |= mounted (bit0)` для всадников;
- `reserved` (u32) = **хэш модели лошади** (`A_C_HORSE_*` из `Peds.json`,
  fallback 3–4 известных модели);
- `moveStyle = RideHorse (3)`; кадры несут mounted-флаг, скорость/gait —
  как задано (`--gait 1.0` шаг / `2.0` галоп).

Первые `--mounted N` акторов (из `--num`) — верхом; позиции/heading по общему
паттерну. `--mount-model` задаёт один явный хэш для всех всадников.

**A/B-тест конных (fake host → игровой гость):**
```powershell
# шаг: 3 всадника из 5, круг
python tools\fake_actor_client.py --mode host --name TestHost `
  --pattern circle --mounted 3 --gait 1.0 --duration 300

# галоп: перезапустить после «шага»
python tools\fake_actor_client.py --mode host --name TestHost `
  --pattern circle --mounted 3 --gait 2.0 --duration 300

# все 5 верхом, своя лошадь:
python tools\fake_actor_client.py --mode host --name TestHost `
  --mounted 5 --gait 2.0 --mount-model 0x88D6A59E
```
Ожидаемые строки host:
```
fakehost: mounted=3/5 mount_models=0x... (A_C_HORSE_...), ...
fakehost: spawn epoch=1 n=5
fakehost: frames sent=...
```
В игре-госте: 3 всадника (первые 3 actorId) и 2 пеших; при `--gait 1.0`
лошади идут шагом, при `--gait 2.0` — быстрее (галоп); лошади не должны
дёргаться/телепортироваться.

**Проверка полей в сети (игровой хост → fake guest):**
```powershell
python tools\fake_actor_client.py --mode guest --name TestGuest --duration 120
```
Ожидаемые строки:
```
fakeguest: mounted=N
fakeguest: mounted reserved_ok=N reserved_zero=0
```
`reserved_zero>0` или `ERROR ActorSpawnBatch: mounted actorId=... reserved=0` —
игровой хост не передал модель лошади (проверить E2b).

### E3a: сценарии + смерть/возрождение

E3a сдаёт в `ActorSpawn`:
- `flags |= scenario (bit5)` для первых `--scenario N` акторов;
- `ActorSpawn.scenarioHash` = `joaat(lowercase name)` для имён из
  `--scenario-names` (default:
  `WORLD_HUMAN_SMOKE_CIGARETTE,WORLD_HUMAN_STAND_IMPATIENT` — «стоячие»
  сценарии без пропсов), назначаются сценарным акторам по кругу. Это
  **валидные сценарные имена** для `TASK_START_SCENARIO_IN_PLACE_HASH`
  (dyn-хэши из probe для неё не подходят).
  `--scenario-names none` — fallback на уникальные `dynScenario=0x...` из
  `--scenario-from-log`; `--scenario-hash 0x...` — явный хэш.
- у сценарных акторов `moveStyle=0`, в кадрах — scenario-флаг,
  `animState=0, speed=0, pad=0`, позиция фиксирована (стоят/сидят);
  остальные акторы двигаются по `--pattern`.
- **Разные spawn-позиции:** движущиеся акторы спавнятся на кольце вокруг
  гостя, угол `2π*i/num`, радиус **3–6 м** (первый 3 м, последний 6 м), и
  двигаются вокруг СВОЕЙ точки спавна. Сценарные акторы (первые
  `--scenario N`) спавнятся на **широком кольце радиусом
  `--scenario-spacing` (default 12 м)**, равномерно по углу `2π*i/N` —
  чтобы движок не варпал их в одну ближайшую scenario point и они не
  накладывались друг на друга. При старте пишется
  `fakehost: scenario positions distance_min=...`.
- `--dead-after SEC` (default 0 = off): через SEC после первого спавна
  **последний actorId** получает `dead flag (bit1) + health=0`; кадры
  мёртвого — `speed=0, animState=0`, позиция зафиксирована (труп).
- `--revive-after SEC` (default 10): через SEC после смерти dead-флаг
  снимается, `health=200`, кадры снова обычные; `0` — остаётся мёртвым.
  Для `PROP_HUMAN_SEAT_CHAIR` / `WORLD_HUMAN_CAMP_FIRE_SIT` нужен
  соответствующий проп (стул/костёр) — в чистом поле применяется
  at-position fallback, но поза может не сесть; для чистого теста use
  `WORLD_HUMAN_SMOKE_CIGARETTE`/`WORLD_HUMAN_STAND_IMPATIENT`.

**Тест E3a (fake host → игровой гость):**
```powershell
# 2 сценарных (курит/стоит) далеко друг от друга + 3 ходят; разные spawn-позиции
python tools\fake_actor_client.py --mode host --name TestHost `
  --scenario 2 --scenario-spacing 12 --pattern circle `
  --despawn-after 0 --duration 300

# вариант со стулом (нужен проп chair):
python tools\fake_actor_client.py --mode host --name TestHost `
  --scenario 2 --scenario-names "PROP_HUMAN_SEAT_CHAIR,WORLD_HUMAN_CAMP_FIRE_SIT" `
  --pattern circle --despawn-after 0 --duration 300

# сценарии + смерть последнего через 20 с и возрождение через 10 с
python tools\fake_actor_client.py --mode host --name TestHost `
  --scenario 2 --pattern circle --dead-after 20 --revive-after 10 `
  --despawn-after 0 --respawn-after 0 --duration 300
```
Ожидаемые строки host:
```
fakehost: scenario=2/5
fakehost: scenario hashes=0xE25A61E5 (WORLD_HUMAN_SMOKE_CIGARETTE), 0xBBFC4627 (WORLD_HUMAN_STAND_IMPATIENT)
fakehost: scenario positions distance_min=24.00 (n=2, spacing=12.0)
fakehost: spawn epoch=1 n=5
fakehost: death id=5
fakehost: revive id=5
```
В игре-госте: 2 NPC курят/стоят на разных точках (≥12 м друг от друга),
3 идут по кругу вокруг своих точек; через 20 с последний падает (труп,
health=0), через 10 с снова живой.

**Проверка полей в сети (игровой хост → fake guest):**
```powershell
python tools\fake_actor_client.py --mode guest --name TestGuest --duration 120
```
Ожидаемые строки E3a:
```
fakeguest: scenarios=N dead_frames=M
fakeguest: scenario_frames=K
```
`scenarios=N` — число уникальных actorId с `scenarioHash!=0`;
`scenario_frames=K` — кадры от сценарных актёров (флаг scenario);
`dead_frames=M` — сколько кадров пришло с dead-флагом (в начале 0, после
смерти растёт; после revive перестаёт).

### E3b: бой/оружие

E3b сдаёт `ActorSpawn.weaponHash` для акторов и боевые флаги в
`ActorFrame.flags` (биты):
`Drawn=4`, `Changing=8`, `Combat=16`, `Reloading=64`, `Aiming=128`
(+ `mounted=1`, `dead=2`, `scenario=32`).

**Проверенные оружейные хэши** (сверено с
`third_party/rdr2-hashdb/CatalogItems.json` и ScriptHook `WeaponHashes.cs`):

| Оружие | Hash |
|---|---|
| `WEAPON_REVOLVER_CATTLEMAN` | **0x169F59F7** (default) |
| `WEAPON_SHOTGUN_DOUBLEBARREL` | 0x6DFA071B |
| `WEAPON_REPEATER_CARBINE` | 0xF5175BA1 |
| `WEAPON_PISTOL_M1899` | 0x5B78B8DD |
| `WEAPON_RIFLE_SPRINGFIELD` | 0x63F46DE6 |

**Важно:** joaat имени оружия **не подходит** — `GIVE_WEAPON_TO_PED` его
отклоняет (joaat(`weapon_revolver_cattleman`) = 0x8B264946 невалиден).
`0x67DC3FDE` — это `WEAPON_MELEE_TORCH` (факел), не револьвер.
Default `--weapon` = **0x169F59F7** (револьвер); `--weapon-list` без
значения использует все 5 валидных хэшей по кругу акторам.

Таймлайн (от первого спавна):
- `--combat`: флаг Combat (16) постоянно (и в spawn, и в кадрах);
- `--drawn`: первые 3 с оружие в кобуре, затем Drawn (4); лог
  `fakehost: weapon drawn`;
- `--aim-after SEC`: через SEC — Aiming (128) + Drawn на 8 с; **x,y и
  heading фиксируются на момент начала окна** (актор стоит, heading не
  дрейфует от circle-паттерна, клиент не перевыдаёт aim); anim=0, speed=0;
- `--shoot-after SEC`: через SEC — Drawn|Aiming на 3 с (shoot), в кадрах
  `targetActorId=1`; вне окна `targetActorId=0`;
- `--reload-after SEC`: через SEC — Reloading (64) импульсами **1 с каждые
  8 с**; переходы логируются `fakehost: reload on/off`;
- `--combat-cycle SEC` (**default 20**, 0 = одноразовые окна): повторяет
  боевые окна каждый цикл: `+0..2` — Drawn, `+2..7` — Aiming (idle поза:
  x,y,heading статичны), `+7..10` — Shoot (Drawn|Aiming + targetActorId=1,
  idle), `+12..13` — Reload 1 с; повторяется до `--duration`. В лог:
  `fakehost: cycle N aim/shoot/reload`. Одноразовые `--aim-after`/
  `--shoot-after`/`--reload-after` действуют только при `--combat-cycle 0`;
- `--weapon HASH` — явный реальный хэш оружия (0 = без оружия);
- `--weapon-list [HASH,...]` — разные оружия по кругу акторам (без значения
  берётся 5 валидных хэшей); лог `fakehost: weapons=...`.

**Тест E3b (fake host → игровой гость):**
```powershell
# повторяющиеся окна aim/shoot/reload каждые 20 с (default)
python tools\fake_actor_client.py --mode host --name TestHost `
  --combat --drawn --combat-cycle 20 `
  --despawn-after 0 --duration 120

# разные валидные оружия по кругу (5 моделей):
python tools\fake_actor_client.py --mode host --name TestHost `
  --weapon-list --combat --drawn --combat-cycle 20 `
  --despawn-after 0 --duration 120
```
Ожидаемые строки host:
```
fakehost: weapons=0x169F59F7, 0x6DFA071B, 0xF5175BA1, 0x5B78B8DD, 0x63F46DE6
fakehost: combat flags drawn=1 aim_after=0.0 shoot_after=0.0 reload_after=0.0 combat=1 cycle=20.0 weapon=0x169F59F7
fakehost: cycle 1 aim
fakehost: cycle 1 shoot
fakehost: cycle 1 reload
fakehost: cycle 2 aim
fakehost: spawn epoch=1 n=5
```
В игре-госте: NPC с валидным оружием; каждые 20 с достают револьвер,
целятся 5 с (неподвижно), стреляют 3 с (Drawn|Aiming + targetActorId=1),
перезаряжаются 1 с — и так пока идёт `--duration`.

**Проверка полей в сети (игровой хост → fake guest):**
```powershell
python tools\fake_actor_client.py --mode guest --name TestGuest --duration 120
```
Ожидаемые строки E3b:
```
fakeguest: weapon_spawns=N drawn_frames=... aim_frames=... reload_frames=...
fakeguest: combat_frames=...
fakeguest: target_frames=N
```
`weapon_spawns=N` — уникальные actorId с `weaponHash!=0`; остальные — число
кадров с соответствующим флагом (`drawn=4`, `aim=128`, `reload=64`,
`combat=16`); `target_frames=N` — кадры с `targetActorId!=0` (shoot-окно).

### E3c: животные и свободные лошади

`ActorSpawn.kind` = **0 human, 1 animal, 2 horse** (поле есть с v15/v16;
текущий E5-протокол v17 его не меняет). Host-флаги добавляют акторов сверх
`--num` (людей):

- `--animals N` — N наземных животных kind=1; модели берутся из
  `Peds.json` (A_C_DEER_01, A_C_BOAR_01, A_C_WOLF, A_C_BUFFALO_01,
  A_C_PRONGHORN_01, A_C_ELK_01 и т.п.; **без рыб/птиц**), двигаются по
  `--pattern`;
- `--horses N` — N свободных лошадей kind=2, первые N `A_C_HORSE_*` из
  `Peds.json`, двигаются по `--pattern`;
- всего акторов `--num + --animals + --horses ≤ 12`; у животных/лошадей
  `metaOutfit=0`, `weaponHash=0`, `scenarioHash=0`, без mounted-флагов.
  Лог: `fakehost: animals=... horses=... models=...`.

**Тест E3c (fake host → игра-гость):**
```powershell
# 5 людей (default) + 3 животных + 2 лошади, все двигаются
python tools\fake_actor_client.py --mode host --name TestHost `
  --animals 3 --horses 2 --pattern mixed `
  --despawn-after 0 --duration 120

# только животные/лошади (без людей): --num 0
python tools\fake_actor_client.py --mode host --name TestHost `
  --num 0 --animals 3 --horses 2 --pattern mixed `
  --despawn-after 0 --duration 120
```
Ожидаемые строки host:
```
fakehost: animals=3 horses=2 models=animal:[0x... (A_C_DEER_01), ...] horse:[...]
fakehost: spawn epoch=1 n=10
```
Fake guest:
```powershell
python tools\fake_actor_client.py --mode guest --name TestGuest --duration 120
```
```
fakeguest: kinds human=5 animal=3 horse=2
fakeguest: animal_frames=... animal_dead_frames=... horse_frames=... horse_dead_frames=...
```
`kinds` — уникальные actorId по `kind` из spawn-записей; `*_frames` — кадры
этого kind; `*_dead_frames` — кадры с dead-флагом. Ожидаемо `errors=0`;
животные/лошади двигаются по паттерну без телепортов. Для визуального теста
запустить fake host **до** игры-гостя; гость увидит животных/лошадей рядом с
собой (fake guest не нужен).

**Трупы (E3c corpse demo):**
```powershell
python tools\fake_actor_client.py --mode host --name TestHost `
  --num 0 --animals 3 --horses 2 --pattern mixed `
  --kill-after 15 --despawn-after 0 --duration 120
```
Через 15 с после первого спавна хост убивает **одно животное и одну лошадь**
(dead-флаг + health=0; остальные не трогаются); `--kill-all` убивает всех
animals/horses разом. `--revive-after` относится только к людям (kind=0):
животные/лошади остаются трупами. Логи:
```
fakehost: kill animal id=1
fakehost: kill horse id=4
fakeguest: corpses animal_died=1 horse_died=1
```
`fakeguest: corpses animal_died=A horse_died=R` — уникальные actorId животных/
лошадей, у которых были dead-кадры; `*_dead_frames` — число таких кадров.
Для визуального теста fake host запускается первым, затем игра-гостем.

### E5: host-authoritative урон (ActorDamage 0x1F)

Fake host принимает `ActorDamage` (0x1F, 16 B: `epoch/actorId/damage u16/pad/
weaponHash`) от игрового гостя (или fake guest) и **сам применяет** урон:
human HP=200, animal/horse HP=100. При hp≤0 актор получает dead-флаг,
`health=0`, хост шлёт труп; логи:
```
fakehost: damage id=1 dmg=60 hp_left=140
...
fakehost: damage id=1 dmg=60 hp_left=0
fakehost: killed id=1 by damage
```
Некорректный 0x1F (размер/epoch/actorId/damage) игнорируется с WARN.

**Smoke без игры (fake host + fake guest):**
```powershell
# 1) fake host: 2 human + 1 animal, actor id=1 — human (HP 200)
python tools\fake_actor_client.py --mode host --name DamageHost `
  --num 2 --animals 1 --pattern mixed `
  --despawn-after 0 --duration 20

# 2) fake guest: после 2 с бьёт actor id=1 по 60 каждые 0.5 с
python tools\fake_actor_client.py --mode guest --name DamageGuest `
  --damage-after 2 --damage-actor 1 --damage 60 --duration 8
```
Ожидаемо: host hp 140→80→20→0 и `killed id=1 by damage`; guest получает
последний spawn с health=0 и dead-кадры (`fakeguest: ... dead_frames>0`),
`errors=0`.
Для животного (HP 100) хватает двух пакетов по 60; actorId животных идут
после людей, лошадей — после животных.

**Реальный E5-тест (игра-гость стреляет в прокси):**
```powershell
python tools\fake_actor_client.py --mode host --name TestHost `
  --num 2 --animals 2 --horses 1 --pattern mixed `
  --despawn-after 0 --duration 300
# затем запустить RDR2 и подключиться ГОСТЕМ, стрелять в прокси
```
Игровой гость шлёт 0x1F; fake host логирует damage/killed, присылает dead;
гость видит падение/труп. В guest-логе `actor: ... death` (human/animal).

### E2c: поздние outfit-обновления (metaOutfit resolver)

E2c: игровой хост подбирает `metaOutfit` для NPC, у которых он был 0, и
присылает его в **heartbeat-spawn** для уже существующего `actorId`
(metaOutfit меняется `0 → hash`). Fake guest отслеживает это по actorId.

**Проверка (игра-хост первой, fake guest вторым):**
```powershell
# игра подключена ПЕРВОЙ (хост) с npc_actor=1; E2c resolver на хосте
python tools\fake_actor_client.py --mode guest --name TestGuest `
  --outfit-wait 60 --duration 120
```
`--outfit-wait SEC` — после первого spawn продолжать слушать ещё SEC секунд
(даже если `--duration` короче), чтобы увидеть поздние heartbeat-обновления.

Ожидаемые строки:
```
fakeguest: outfit update id=1 0x00000000 -> 0x12345678
fakeguest: outfits nonzero=N outfit_updates=K resolved_late=Q
```
- `nonzero=N` — уникальные actorId с `metaOutfit != 0`;
- `outfit_updates=K` — spawn-записи, где у существующего actorId изменился
  metaOutfit (`0→hash` или `hash→другой`);
- `resolved_late=Q` — actorId, получившие ненулевой outfit не на первом
  спавне (позже, в heartbeat).
Exit 0/PASS — как обычно (≥1 spawn-batch и ≥10 frames).

### E2c-b (v16): ActorAppearance (0x1E)

`ActorAppearance` (0x1E) — asset/tint внешность NPC, хост → сервер → гость;
**реализовано и задеплоено (protocol v16)**. Fake guest валидирует: header
12 B (`epoch:u32, actorId:u32, count:u8, pad3`), `count` в 1..16, точный
размер `12 + count*28` (max 460 B), `epoch!=0`, `actorId!=0`, `reserved==0`
в каждой 28-байтной записи; предупреждает, если appearance пришёл раньше
ActorSpawn. Финальные строки:
```
fakeguest: appearance_entries avg=8.00 max=8
fakeguest: appearance_msgs=N appearance_actors=U entry_max=M errors=E
```
`appearance_msgs` — получено сообщений 0x1E; `appearance_actors` — уникальные
actorId; `entry_max` — максимум записей в сообщении; `errors` — нарушения
формата/размера/count/reserved (не считая предупреждений).

**Проверка E2c-b (игра-хост первой, fake guest вторым):**
```powershell
python tools\fake_actor_client.py --mode guest --name TestGuest `
  --outfit-wait 60 --duration 300
```
Реальный хост шлёт 0x1E сам: host log `asset capture count=8-10
valid=8-10`; fake guest — `appearance_msgs=504 appearance_actors=8
entry_max=10 errors=0`. Визуальная двух-ПК приёмка (ARINA v16 задеплоена) —
pending. Для дымового теста без игры fake host `--appearance` шлёт
синтетические записи (drawable=model hash, tints=255), fake guest показывает
`appearance_msgs=5 appearance_actors=5 entry_max=8 errors=0` на 5 акторах.

### E4: замеры производительности (`perf_report.py`)

`tools/perf_report.py` парсит `perf: npc host ...` / `perf: npc guest ...`
(формат v16 E4, `npc_debug=1`) и legacy `perf: actor ...` (старые логи).
Считает p50/p95/max по tick/discover/capture/build/spawn/appearance/motion/
weapons, counters (`actors/spawns/frames/despawns/bytes/s`,
`proxies/spawns/applied/failed/timeouts`), warn/errors/timeouts, сравнивает
`--baseline` vs `--log` и выдаёт PASS/FAIL по порогам E4.

```powershell
# один прогон
python tools\perf_report.py --log "E:\Games\Red Dead Redemption 2\scripts\rdr2-coop.log"

# дельты к baseline (например пустая локация vs город)
python tools\perf_report.py --log perf_city_npc1.log `
  --baseline perf_baseline_empty.log

# JSON
python tools\perf_report.py --log perf_city_npc1.log --json > perf.json
```
Exit: 0 PASS, 1 FAIL, 2 нет `perf:` данных. Пороги: Tick p95 ≤2.0 мс
(хост/гость; с baseline — добавленный p95), discover/capture/apply (и
guest spawn/appearance/motion/weapons/build) p95 ≤1.5 мс, warn=0,
timeouts ≤0.5%, appearance failed ≤1%, delete failed=0.
Методика/приёмка — `docs/PERF.md`.

### Режим B: fake GUEST (игра — хост, эмулируется гость)

**Порядок: сервер → игра-хост → fake guest.**

1. Запустить сервер v16.
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
| `--outfit-wait` | 0 | guest: после первого spawn слушать ещё SEC секунд для поздних metaOutfit-обновлений (E2c) |
| `--model 0x...` | `0x54D8BE8E` | модель ped'а для JOIN |
| `--num` | 5 | число human NPC (host, 0..12; 0 = только animals/horses); сумма `num+animals+horses` ≤12 |
| `--animals` | 0 | E3c: N животных kind=1 (наземные, без рыб/птиц), модели из Peds.json |
| `--horses` | 0 | E3c: N свободных лошадей kind=2 (первые A_C_HORSE_*), двигаются |
| `--pattern` | `circle` | `circle` / `line` (4 м назад-вперёд) / `mixed` (2 круга, 2 линии, остальные стоят) |
| `--gait` | 1.0 | E2a.2 gait движущихся актёров: `1.0` walk, `2.0` run/галоп (уходит в `ActorFrame.pad=gait*1000`; idle=0) |
| `--mounted` | 0 | E2b: сколько из `--num` акторов верхом (первые N; 0..num); flags bit0 + `ActorSpawn.reserved=mount model` |
| `--mount-model` | — | E2b: явный хэш лошади для всех всадников (иначе `A_C_HORSE_*` из Peds.json) |
| `--scenario` | 0 | E3a: сколько из `--num` акторов в сценарии (первые N; idle + flags bit5 + scenarioHash) |
| `--scenario-names` | `WORLD_HUMAN_SMOKE_CIGARETTE,WORLD_HUMAN_STAND_IMPATIENT` | E3a: валидные имена сценариев; хэш = joaat(lowercase); `none` — fallback в probe-лог |
| `--scenario-from-log` | `E:\Games\Red Dead Redemption 2\scripts\rdr2-coop.log` | E3a: откуда брать `dynScenario=0x...` fallback (пустая строка — встроенный fallback) |
| `--scenario-hash` | — | E3a: явный scenarioHash для всех сценарных акторов |
| `--scenario-spacing` | 12.0 | E3a.4: радиус широкого кольца сценарных акторов (м), чтобы не накладывались на одну scenario point |
| `--weapon` | `0x169F59F7` | E3b: реальный RDR2 weapon hash (cattleman; joaat имени НЕ подходит); 0 = без оружия |
| `--weapon-list` | — (const: 5 валидных) | E3b: реальные хэши оружия через запятую round-robin; без значения — 0x169F59F7,0x6DFA071B,0xF5175BA1,0x5B78B8DD,0x63F46DE6 |
| `--drawn` | off | E3b: через 3 с после спавна флаг Drawn (4) |
| `--aim-after` | 0 (off) | E3b: через SEC — Aiming+Drawn на 8 с, актор стоит |
| `--shoot-after` | 0 (off) | E3b: через SEC — Drawn\|Aiming на 3 с (shoot) |
| `--reload-after` | 0 (off) | E3b: через SEC — Reloading (64) импульсами 1 с каждые 8 с; логи on/off (только при `--combat-cycle 0`) |
| `--combat-cycle` | 20 | E3b.3: повторять aim(+2..7)/shoot(+7..10)/reload(+12) каждый SEC; 0 = одноразовые окна |
| `--combat` | off | E3b: флаг Combat (16) |
| `--appearance` | off | E2c-b host: синтетические ActorAppearance (0x1E) по акторам для дымового/визуального теста |
| `--dead-after` | 0 | E3a: через сколько секунд после первого спавна убить последний HUMAN actorId (0 = off) |
| `--revive-after` | 10 | E3a: через сколько после смерти возродить человека (kind=0; animals/horses не оживляются) |
| `--kill-after` | 0 | E3c: через SEC после первого спавна убить одно животное kind=1 и одну лошадь kind=2 (0 = off) |
| `--kill-all` | off | E3c: убить всех animals/horses (с `--kill-after` — по таймеру, без него — сразу) |
| `--outfits-from-log` | `E:\Games\Red Dead Redemption 2\scripts\rdr2-coop.log` | пары `model+metaOutfit` из probe-лога; пустая строка — выкл. |
| `--circle-radius` | 3.0 | радиус круга движения NPC, м |
| `--speed` | 1.0 | базовая скорость движения, м/с (фактическая = speed*gait; в кадр идёт speed/8*255) |
| `--despawn-after` | 30 | через сколько секунд удалить actorId=5 (0 = выкл.; для E3-теста смерти ставьте 0) |
| `--respawn-after` | 10 | через сколько после despawn вернуть actorId=5 (0 = выкл.) |
| `--peds-json` | `third_party/rdr2-hashdb/Peds.json` | откуда брать хэши моделей |
| `--models` | — | свои модели (hex через запятую, ≥`--num`) |
| `--log-file` | `tools/fake_actor_client.log` | лог-файл (append) |
| `--protocol-version` | auto | override JOIN protocol version (например `16` для старого сервера; по умолчанию — из `net/protocol.h`) |
| `--damage-after` | 0 | guest E5 smoke: через SEC повторять ActorDamage (0x1F) каждые 0.5 с |
| `--damage-actor` | 0 | guest E5 smoke: actorId цели для ActorDamage |
| `--damage` | 60 | guest E5 smoke: урон в одном 0x1F (1..1000) |

### Важно / troubleshooting

- **Первый подключившийся — HOST.** Если fake host запустить после игры, он
  получит `is_host=0` и выйдет с кодом 3 (`fakehost: ERROR: this tool is not
  HOST`). Аналогично fake guest, запущенный до игры, станет HOST — exit 3.
- Сервер и игра должны быть **protocol v16** (`kProtocolVersion=16`, инструмент читает версию из `net/protocol.h`); при
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
