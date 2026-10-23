# E4 — производительность NPC-репликации: методика и приёмка

Документ описывает, как снимать и проверять стоимость NPC-подсистемы
(`src/actor_sync.cpp`) на v16. Анализ делает `tools/perf_report.py`
(stdlib, без внешних зависимостей). E4 принимается по цифрам, а не «на глаз».

## 1. Что нужно

- Сервер v16 (`build-server-ci\Release\rdr2_coop_server.exe`).
- v16-клиент с E4-инструментацией: ASI
  `5FD7AEB1D7B06FB5C71F604CFECF3280EA88256C025967C9B578A3D4A4E251F0`.
- `npc_debug = 1` — включает `perf: npc ...` строки (при `npc_debug=0` или
  `npc_actor=0` — **ноль** `perf:` строк, overhead ≈ 0).
- Для городского прогона: `npc_actor = 1`, `npc_cap = 8..12`,
  `npc_radius = 60`, `npc_appearance = 1`; игра-хост + fake guest
  (`tools/fake_actor_client.py --mode guest`, fake guest следует за позицией
  хоста), либо два ПК.
- Логи: `scripts\rdr2-coop.log` (игровой клиент; там `perf: npc`) и при
  необходимости `server_console.log` (ошибки/таймауты сервера).

## 2. Как снимать

1. Запустить сервер v16.
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

## 4. Формат строк (v16 E4)

`npc_debug=1`, окно ~10 с:

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

## 5. Пороги приёмки

| Метрика | Порог |
|---|---|
| Tick p95, хост | ≤ **2.0 мс** (с baseline — добавленный p95 ≤2.0 мс) |
| Tick p95, гость | ≤ **2.0 мс** |
| discover/capture/apply p95 | ≤ **1.5 мс** |
| Guest spawn/appearance/motion/weapons p95 | ≤ **1.5 мс** (в отчёте проверяются) |
| `perf: npc warn` | **0** строк |
| timeouts | ≤ **0.5%** (timeouts / (guest spawns+timeouts); для legacy — appliedFrames) |
| appearance failed | ≤ **1%** (failed / (applied+failed)) |
| delete failed | **0** |
| mount/scenario failed, generic ERROR | информационно; разбирать отдельно |

p50 в отчёте — медиана per-window avg по логу; p95 — худший p95, который
прислало окно (консервативно); max — худший max.

## 6. Пример отчёта (synthetic, формат v16)

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

## 8. Ограничения

- **1 ПК:** игра может быть хостом **или** гостем, но не обоими; host и guest
  метрики снимаются отдельными прогонами (host — с fake guest; guest — со
  вторым ПК). Двух-ПК приёмка E2c-b/E4 pending.
- Fake guest не даёт guest-`perf:` строк: это Python-инструмент, не игровой
  клиент; guest-метрики есть только у игры-гостя.
- Baseline `npc_actor=0` не содержит `perf: npc` (нулевой overhead); для
  дельт используйте `npc_actor=1` в пустой локации или лог предыдущей сборки.
- Замеры зависят от города/толпы, `npc_cap`, фоновых программ; для приёмки
  фиксируйте локацию/время и повторяйте 2–3 прогона.
- `npc_debug=1` сам добавляет QPC/логи (≈1 строка/10 с на хост/гость) — при
  `npc_debug=0` overhead практически нулевой, это отдельно проверяется
  baseline-прогоном.
