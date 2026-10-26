# E4 — производительность NPC-репликации: методика и приёмка

Документ описывает, как снимать и проверять стоимость NPC-подсистемы
(`src/actor_sync.cpp`) на v19/npc-e13. Анализ делает `tools/perf_report.py`
(stdlib, без внешних зависимостей). E4 принимается по цифрам, а не «на глаз».
С npc-e12 добавлены **`perf: frame`** (полный кадр), клиентский **`perf: net`**
и серверный **`net: bytes_in/out`**; с npc-e13 добавлен **`tick_hz`** и
wall-clock интервалы (FPS-independent).

## 1. Что нужно

- Сервер v19 (`build-server-ci\Release\rdr2_coop_server.exe`; локальная пара
  npc-e13 — ASI `B43B7EE0`, server `F3EBD6D1`; ARINA offline).
- v19-клиент npc-e13 (ASI `B43B7EE0`, локальная пара npc-e13).
- `npc_debug = 1` — включает `perf: npc`/`perf: frame`/`perf: net` строки (при
  `npc_debug=0` или `npc_actor=0` — **ноль** `perf:` строк, overhead ≈ 0).
- Для городского прогона: `npc_actor = 1`, `npc_cap = 8..12`,
  `npc_radius = 60`, `npc_animal_cap = 4`, `npc_appearance = 1`,
  `npc_damage = 1`, `npc_presence = 1`, `npc_visitor = 1`,
  `npc_visitor_pause_scenes = 1`, `world_vehicles = 1`,
  `npc_vehicle_radius = 80`, `npc_occupants = 1`, `world_trains = 0`,
  `npc_animal_appearance = 0`, `npc_motion_update = 1`,
  `player_frame_hz = 20`, `player_frame_change_only = 1` (E6/E7/E9/E3d не
  выключать, если проверяем их вклад); игра-хост + fake guest
  (`tools/fake_actor_client.py --mode guest`, fake guest следует за позицией
  хоста), либо два ПК.
- Логи: `scripts\rdr2-coop.log` (игровой клиент; там `perf: npc`) и при
  необходимости `server_console.log` (ошибки/таймауты сервера).

## 2. Как снимать

1. Запустить сервер v19.
2. **Baseline `npc_actor=0`:** игра-хост в том же городе 3–5 минут с
   `npc_actor=0`, `npc_debug=0`; скопировать `rdr2-coop.log` как
   `perf_baseline_off.log`. В этом режиме `perf: npc` строк нет — это
   подтверждает нулевой debug-overhead; дельты по такому логу не считаются.
3. **E4 run (`npc_actor=1`):** в INI `npc_actor=1`, `npc_debug=1`; город,
   3–5 минут активной ходьбы среди NPC; fake guest подключён (или второй
   игрок). Скопировать лог как `perf_city_npc1.log`.
4. **Baseline для дельт:** тот же E4-билд и `npc_debug=1` в пустой/малолюдной
   локации (0–2 NPC) 2–3 минуты — `perf_baseline_empty.log`. Для сравнения
   «npc_actor=0 vs 1» нужен лог, где были `perf: npc` строки; у `npc_actor=0`
   их нет by design.
5. (Опционально, только при наличии второго игрока) отдельный прогон с
   **игровым гостем** даёт `perf: npc guest ...`; fake guest — Python, его
   guest-метрики в игре не измеряются.

Минимально для E4-хоста: `--log perf_city_npc1.log --baseline
perf_baseline_empty.log`. Логи можно фильтровать (`Select-String "perf: npc"`),
но скрипт стримит файл и нормально читает полный лог.

## 3. Запуск отчёта

```powershell
# один прогон
python tools\perf_report.py --log "E:\Games\Red Dead Redemption 2\scripts\rdr2-coop.log"

# сравнение с baseline (дельты p95)
python tools\perf_report.py --log perf_city_npc1.log `
  --baseline perf_baseline_empty.log

# JSON для CI/сводки
python tools\perf_report.py --log perf_city_npc1.log --json > perf.json
```

Exit codes: **0** PASS, **1** FAIL (порог/ошибки), **2** нет `perf:` данных /
файл не читается. `--label NAME` задаёт имя прогона в отчёте.

## 4. Формат строк (v19 E4 / npc-e12/e13)

`npc_debug=1`. **Новый полный кадр (главная метрика G2), окно ~5 с:**

```text
perf: frame host tick=0.42 p95=0.55 max=1.10 | world=0.10 p95=0.15 | actor=0.30 p95=0.40 | coop=0.02 p95=0.03 frames_hz=20.0 tick_hz=144.0
perf: frame guest tick=0.35 p95=0.48 max=0.90 | world=0.00 p95=0.00 | actor=0.28 p95=0.35 | coop=0.02 p95=0.03 frames_hz=20.0 tick_hz=60.0
```

`tick` — полный кадр (мс), `world` — WorldSnapshot/vehicle-scan,
`actor` — actor_sync (NPC), `coop` — остальная coop-логика; `frames_hz` —
фактическая частота Player Frame за окно (target ≤`player_frame_hz`, покой
heartbeat 5 Гц при `player_frame_change_only=1`); `tick_hz` — реальный FPS
игры за окно (npc-e13), нужен для проверки FPS-independence: при 60 и 144 FPS
`tick`/p95/трафик не должны масштабироваться с FPS.

Старые строки `perf: npc` сохранены для детализации actor/world:

```text
perf: npc host tick=0.42 p95=0.55 max=1.10 | discover=0.20 p95=0.30 | capture=0.05 p95=0.08 | build=0.10 p95=0.15
perf: npc host actors=7 spawns=7 frames=70 despawns=0 bytes=18432/s
perf: npc guest tick=0.35 p95=0.48 max=0.90 | spawn=0.10 p95=0.12 | appearance=0.05 p95=0.07 | motion=0.04 p95=0.06 | weapons=0.10 p95=0.14
perf: npc guest proxies=7 spawns=7 applied=6 failed=0 timeouts=0 despawns=0
perf: npc warn host tick=3.40 > 2.0 ms
```

- `tick` — полный `actor_sync::Tick` (мс): avg, p95, max.
- `discover`/`capture`/`build` — хост: сканирование мира, capture appearance
  (E2c-b), сборка/отправка spawn/frames.
- `spawn`/`appearance`/`motion`/`weapons` — гость: применение spawn,
  appearance, движение, оружие.
- counters: `actors/spawns/frames/despawns/bytes/s` (хост); `proxies/spawns/
  applied/failed/timeouts/despawns` (гость; applied/failed — appearance).
- `perf: npc warn ...` — самостоятельная строка, когда tick p95 > 2.0 мс.
- Legacy `perf: actor scan/spawn/apply` (старые логи/старые сборки) парсится
  отдельной веткой; в новом E4-билде он **не пишется**.

**Реальный трафик (npc-e12/e13):**

```text
# server_console.log / видимое окно сервера, heartbeat ~10 с
net: bytes_in=123456 bytes_out=234567 | frame=... actor=... world=... outfit=... presence=... damage=... trains=... occupants=... other=...
# rdr2-coop.log, клиент, ≥10 с
perf: net tx=1.2 KB/s rx=1.4 KB/s msgs_tx=24 msgs_rx=28
```

`bytes_in/out` — накопленные байты на сервере; для КБ/с снимать **приращение**
между двумя heartbeat и делить на интервал. Per-type поля — **счётчики
сообщений** по категориям (frame/actor/world/outfit/presence/damage/trains/
occupants/other), не байты; их доля — в % от сообщений.
Клиентский `perf: net` — tx/rx KB/s за 10-секундное окно, msgs_tx/rx — число
сообщений.

## 5. Пороги приёмки

| Метрика | Порог |
|---|---|
| `perf: frame host` p95 / max | **≤4.0 мс / ≤10.0 мс** полного кадра (рекомендуемый бюджет, согласовать с Lead) |
| `perf: frame guest` p95 / max | **≤4.0 мс / ≤10.0 мс** |
| `perf: frame ... actor` p95 | **≤2.0 мс** (actor_sync остаётся ключевым) |
| `perf: frame ... world`/`coop` p95 | информационно; аномалии разбирать отдельно |
| `perf: npc` host/guest tick p95 | ≤ **2.0 мс** (старый actor-бюджет, сохранён) |
| discover/capture/apply p95 | ≤ **1.5 мс** |
| Guest spawn/appearance/motion/weapons p95 | ≤ **1.5 мс** (в отчёте проверяются) |
| `perf: npc warn` | **0** строк |
| Traffic, типично | **≤10–12 КБ/с** на клиента (tx+rx) |
| Traffic, worst | **≤15 КБ/с** на клиента; выше — FAIL G3 |
| timeouts | ≤ **0.5%** (timeouts / (guest spawns+timeouts); для legacy — appliedFrames) |
| appearance failed | ≤ **1%** (failed / (applied+failed)) |
| delete failed | **0** |
| mount/scenario failed, generic ERROR | информационно; разбирать отдельно |

p50 в отчёте — медиана per-window avg по логу; p95 — худший p95, который
прислало окно (консервативно); max — худший max.

## 6. Пример отчёта (synthetic, формат v19)

```text
E4 NPC perf report: perf_city_npc1.log
[HOST]
  metric          n      p50      p95      max
  build           2     0.11     0.18     0.18
  capture         2     0.06     0.09     0.09
  discover        2     0.21     0.31     0.31
  tick            2     0.46     0.70     1.20
  deltas vs baseline (p95): build +0.08, capture +0.09, discover +0.11, tick +0.35
  counters: actors=7 spawns=7 frames=70 despawns=0 bytes/s=18432
[GUEST]
  metric          n      p50      p95      max
  appearance      2     0.06     0.09     0.09
  motion          2     0.04     0.08     0.08
  spawn           2     0.11     0.15     0.15
  tick            2     0.38     0.60     1.00
  weapons         2     0.11     0.16     0.16
  counters: proxies=7 spawns=7 appearance_applied=6 appearance_failed=0 timeouts=0
VERDICT: PASS
```

При `failed=1` на 6 applied rate=14% → FAIL по appearance; при tick p95
2.8/3.4 → FAIL по tick и warn. Именно это и должен ловить E4.

## 7. Исторический лог (старый `perf: actor`)

На старом 163-МБ логе (`E:\Games\Red Dead Redemption 2\scripts\rdr2-coop.log`,
до E4-билда) есть только `perf: actor`:

```text
[LEGACY] apply p50=0.01 p95=0.12 max=0.51; discover p50=0.10 p95=0.52 max=1.16
VERDICT: FAIL — tick_metrics_present: no host/guest tick
```

Это ожидаемо: старый билд не даёт `tick`; скрипт подсказывает, что нужен
E4-билд с `npc_debug=1`. Для старых логов отчёт всё равно показывает
scan/spawn/apply и ошибки.

## 8. E5/E5.1 — урон в perf-бюджете

E5 не добавляет отдельных `perf:` полей: обработка `ActorDamage` 0x1F идёт
внутри `actor_sync::Tick` (хост: ≤8 применений/тик, ≤15/с; гость: детект+
отправка ≤10/с) и уже входит в `tick`/`p95`. Для приёмки E5 вместе с E4:

- `perf: npc host tick` p95 ≤2.0 мс и `perf: npc warn` = 0 при активной
  стрельбе по прокси (гость стреляет, хост применяет урон);
- в логе есть `headshot=1` для E5.1, `damage id=… hp_left=…` →
  `killed id=… by damage`, dead-кадры/труп; `errors=0` в fake guest;
- `appearance_failed`/`delete_failed`/timeouts не растут от damage-нагрузки.

Ограничение: damage-метрики не разложены по отдельным p50/p95 — они
покрываются общим tick-бюджетом; при регрессии сначала смотреть
`perf: npc warn` и host tick p95.

## 9. E6/E7.2 — presence и плавность

E6 (`PlayerPresence` 0x20) почти не влияет на tick: 0.5 Гц heartbeat/изменение,
rate-limit 10/с; в `perf: npc` отдельных полей нет — смотреть общий tick и
`actor:`-логи. Проверка presence — по `ui: pause=…`/суффиксу, не по perf.

E7.2 диагностируется отдельными строками; `tools/perf_report.py` их пока не
парсит, поэтому смотреть вручную:

```text
actor: motion id=%u reissue=%.2f/s snap=%.2f/s target_err_p95=%.2f status=…
actor: motion warn update-native A/B id=…
actor: teleport id=… dist=…
```

Приёмка E7.2 baseline: **reissue <0.05/s, snap=0**, target_err_p95 низкий,
`warn update-native A/B` отсутствует; телепорт только на respawn/стрим-прыжке
(>25 м/|dz|>15/нет кадров >2 с). При `npc_motion_marker=1` — учитывать A2
fallback отдельно.

E7.1 visitor mode выполняется в coop/actor tick: cleanup ≤8/тик раз в ~10 с,
подавление density — нативы; отдельных `perf:`-полей нет, вклад виден в общем
tick и `actor: visitor cleanup removed=`. E5.2 corpse retention увеличивает
число прокси (трупы) — учитывать при `npc_cap`/tick; отдельных метрик нет.

## 10. E3d-a — трафик транспорта и host migration

E3d-a (`world_sync=0`, `npc_actor=1`, `world_vehicles=1`) не добавляет
`perf:`-полей: `WorldSnapshot` vehicles-only шлётся раз в **6 тиков**
(≈10 Гц при 60 Гц), до 16 Vehicle × 32 B + header 4 B → **≤516 B/6 тиков**
(≈5.2 КБ/с на гостя, worst case); пустой snapshot — 4 B и удаляет прокси.
Проверять host tick p95/warn и `world: vehicles send n=`; guest создаёт
`CREATE_VEHICLE` (mission/invincible/no-collision) и дедупит `engineNetId`,
timeout 3 с.

Host migration hardening — серверный таймер 4 с (по game-thread сообщениям,
не Ping); CPU-эффект negligible, отдельных perf-метрик нет. Client
role-change reset (world_sync/visitor/presence/actor/боевые) — разово; при
частых миграциях смотреть tick p95/`actor:` timeouts. При `npc_vehicle_radius`
80 м и 16 машинах вклад в трафик детерминирован; в пустой локации
`world: vehicles send n=0` (header 4 B) — норма.

## 10a. v19 — трафик occupants/train

E3d-c (`npc_occupants=1`) не добавляет `perf:`-полей: `VehicleOccupants` 0x21
шлётся при изменении occupants + heartbeat **1/с**, payload ≤ **72 B**
(8+8×8), серверный rate-limit **20/с**, host throttle ≤18/с; при нескольких
машинах трафик ≤72 B/c на vehicle с occupants. `count=0` — 8 B clear.
Проверять host tick p95/warn, `world: occupants vehicle=`, `actor: seat/draft
attach … ok=`, `actor: occupants errors=`.

E3d-b (`world_trains=1`, opt-in) — `TrainSnapshotV2` 0x22 фиксированные
**48 B**, ≤**4/с** → ≤192 B/с; поезда исключены из vehicle-бюджета;
host только при `npc_actor=1 && world_vehicles=1 && world_trains=1` и
`npc_debug=1` логирует `world: train send`. Отдельных perf-метрик нет;
смотреть tick p95/warn и время создания mission-train на госте (cleanup
timeout 2 с). При `world_trains=0` трафик 0x22 отсутствует.

## 10b. E9 — animal/horse appearance

E9 (`npc_animal_appearance=1`, host+guest синхронно) не добавляет
`perf:`-полей. Host capture идёт с бюджетом **1 animal/tick** (~2 с на
актора), payload `ActorAppearance` 0x1E ≤ **460 B** при изменении + heartbeat;
horse tack (kind=2) — `ActorOutfit` 0x1C **792 B** on change (бюджет
1 horse/tick ~2 с). При `npc_animal_appearance=0` — 0x1E/0x1C для животных
не шлются (random). Отдельных guest-метрик нет: смотреть tick p95/warn,
`actor: animal appearance apply … ok=`, `actor: horse tack apply … ok=`,
`actor: animal appearance probe …` (1/модель ≤1/10 с) и 5-сек счётчики.

## 10c. npc-e12 — P0/P1 перф-фиксы и traffic counters

Что изменилось (сборка npc-e12, ASI `D47B91A5`, server `F3EBD6D1`):

- **Player Frame throttle:** `player_frame_hz=20` (clamp 10..60),
  `player_frame_change_only=1` — Frame 88 B не каждый тик: покой heartbeat
  ~5 Гц (~0.44 КБ/с), движение до 20 Гц (~1.76 КБ/с) вместо 5.3/12.7 КБ/с
  при 60/144 FPS. `perf: frame ... frames_hz` показывает фактическую частоту.
- **P0/P1:** `CollectLocalOutfit` игрока/лошади ≤10 Гц (было каждый тик
  ~47–96 нативов); единый appearance-capture бюджет 2 захвата/тик
  (animal/horse/mount период ~5–5.75 с); общий vehicle-скан
  (`SendHostSnapshot`+`TickHostTrains`, TTL 6 тиков), occupants 1/с;
  TTL host-кэшей occupants >300 тиков; resync сбрасывает
  `animalAppearanceSent`/`horseTackSent`/`mountOutfitSent`; `world_scan`
  только при `npc_debug=1` или раз в 5 с.
- **Метрики:** `perf: frame host/guest` (полный кадр и world/actor/coop),
  `perf: npc` (детализация), server `net: bytes_in/out` + per-type,
  client `perf: net tx/rx`. Старые `perf: npc` не удалены.
  `tools/perf_report.py` (обновлён под npc-e12) разбирает `perf: frame`,
  `perf: net` и server `net:`; см. `tools/README.md`.

## 10d. npc-e13 — wall-clock нормализация (FPS-independent)

E13 (ASI `B43B7EE0`, server без изменений `F3EBD6D1`) переводит интервалы с
тиков на `GetTickCount64()` (ms), поэтому при высоком FPS больше нет
ускорения: раньше `npc_frame_hz=15` при 144 FPS давал фактически ~36 Гц и
трафик ×2.4. Основные окна:

- actor frame send — `1000/npc_frame_hz` мс (adaptive 10/15 Гц);
  discovery — 100 мс; human appearance capture — 2 с, animal/horse/mount —
  5 с + stagger по actorId;
- horse tack/mount heartbeat — 5–6 с; spawn heartbeat — 2.5 с;
  train snapshot — ≤4/с (250 мс); train occupants heartbeat — 1 с;
  vehicle occupants сбор/heartbeat — 1 с + окно rate-limit 1 с;
  presence — 2 с; player frame — `1000/player_frame_hz` + heartbeat 200 мс;
  `world_scan` — 5 с; outfit collect — 10 Гц.
- В `perf: frame` добавлен `tick_hz` (реальный FPS). Проверка FPS-independence:
  сравнить run при 60 и 144 FPS (или limit FPS) — `frames_hz`/`tick_hz`
  меняются, а `tick/p95/max` и KB/s остаются в тех же пределах; пороги G2/G3
  не меняются.
- **Остаточные тиковые таймеры (известное ограничение):** corpse hold,
  action/presence poll (5 тиков), motion/scenario task timers, attach/dead
  timeout windows, stop-hysteresis 0.25 с, vehicle-scan TTL 6 тиков, train
  engine cache/scan TTL; при 144 FPS их реальные окна короче. Это не влияет
  на основные интервалы E13, но учитывать при сравнении логов на разном FPS.

## 11. Ограничения

- **1 ПК:** игра может быть хостом **или** гостем, но не обоими; host и guest
  метрики снимаются отдельными прогонами (host — с fake guest; guest — со
  вторым ПК). Двух-ПК приёмка E2c-b/E4/E6/E7/E3d-a/E3d-b/E3d-c/E9/E10/E11
  pending; локальная пара npc-e13 — ASI `B43B7EE0`/server `F3EBD6D1`, ARINA
  offline (деплой pending).
- Fake guest не даёт guest-`perf:` строк: это Python-инструмент, не игровой
  клиент; guest-метрики есть только у игры-гостя.
- Baseline `npc_actor=0` не содержит `perf: npc` (нулевой overhead); для
  дельт используйте `npc_actor=1` в пустой локации или лог предыдущей сборки.
- Замеры зависят от города/толпы, `npc_cap`, фоновых программ; для приёмки
  фиксируйте локацию/время и повторяйте 2–3 прогона.
- `npc_debug=1` сам добавляет QPC/логи (≈1 строка/10 с на хост/гость) — при
  `npc_debug=0` overhead практически нулевой, это отдельно проверяется
  baseline-прогоном.
