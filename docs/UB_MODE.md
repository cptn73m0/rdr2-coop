# ub-mode: как применяем навык к rdr2-coop

Навык: [unclebob/ub-mode](https://github.com/unclebob/ub-mode) — «First Make it Work → Then
Make it Right → Practice». Установлен как обязательный:
- user-skill: `C:\Users\frola\.dsh\skills\ub-mode\SKILL.md` (виден в каталоге скиллов сессии);
- project-copy: `.dsh/skills/ub-mode/SKILL.md` (workspace) и `.agents/skills/ub-mode/SKILL.md` (репозиторий);
- директива «всегда использовать при любом коде» — `AGENTS.md` (workspace и репозиторий).

## Инструменты-рычаги (Build the Lever)

| Инструмент | Назначение |
|---|---|
| `tools/quality_scan.py` | read-only аудит: длина/сложность функций (CRAP-прокси), дублирование (6-строчные шинглы), magic numbers, дубликаты в `natives.h`, TODO/FIXME/warning-disable |
| `scripts/ci.ps1` | proof gate: build ASI + build server + integration (**npc-e19: 42/42**) + C++ тесты; quality gate strict по умолчанию (`-NoStrictQuality`/`DSH_CI_STRICT_QUALITY=0` — report-only) |
| `tools/snapshot_build.ps1` | сохранить сборку в `dist/<tag>/` для отката |
| `tools/perf_report.py`, server `net:` heartbeat | измеримые перф/трафик-гейты (G2/G3) |

Компаньоны ub-mode (`crapper`, `dryer`, `mutator`) требуют отдельной установки и покрытия;
в этом репозитории роль crapper/dryer выполняет `quality_scan.py` (CRAP-прокси и
дублирование), роль mutator — тесты `tests/` + CI (там, где честный тест возможен; для
игровых нативов — smoke/2-ПК сценарии). Не пишем «плохие» тесты ради покрытия.

## Метрики: initial baseline → parser-fixed scan → e19 (HEAD ef49ac8)

Старый сканер не видел функции с многострочной сигнатурой (например
`TickPerfWindow`); P3-фикс `tools/quality_scan.py` вскрыл **скрытые функции**
(`HandleClient`, `SendHostSnapshot`, `ProcessGuestHorseOutfits` и др.).
Это **не регрессия кода**, а артефакт прежнего сканера. Baseline
перегенерирован после P3-фикса: `tools/quality_baseline.json` =
45/**489**/**4**/**3**/0 (снимок чистого `2a58ee8`, база `npc-e16`).

Скан **npc-e19** (чистый архив `ef49ac8`, `python tools\quality_scan.py
--root <clean-ef49ac8> --json build\quality_scan_qa_e19.json --top 20`):
**45 файлов / 505 функций / CRAP>5000 = 3 / dup≥4 = 3 / TODO = 0**. Рост
функций 489→505: **e18 +13** (phase-хелперы `server.cpp`, HandleClient
782→25 строк; loading-heartbeat/vendor-dedupe хелперы) и **e19 +3**
(`MakeLiteralString`, `SetBlipName`, `UpdateBlipName`), а не дублирование
логики; CRAP>5000 **4→3** (HandleClient перестал быть hotspot'ом).

| метрика | initial baseline (old scanner) | parser-fixed baseline (`2a58ee8`) | скан e19 (`ef49ac8`) | delta e19 |
|---|---:|---:|---:|---:|
| файлов | 45 | 45 | 45 | 0 |
| функций | 334 | 489 | **505** | +16 (e18 +13 phase/helper; e19 +3 blip-name) |
| функций с CRAP-прокси >5000 | **4** | **4** | **3** | **−1** (`HandleClient`) |
| dup-групп `count>=4` | **7** | **3** | **3** | 0 |
| TODO/FIXME | **0** | **0** | **0** | 0 |
| warning-disable | 0 | 0 | 0 | 0 |

Топ-5 CRAP (e19):

| # | hotspot | значение |
|---|---|---|
| 1 | `coop.cpp Tick` | **66306** (1222 строки) |
| 2 | `world_sync.cpp SendHostSnapshot` | **15750** (578) |
| 3 | `actor_sync.cpp ProcessGuestHorseOutfits` | 7140 (263) |
| 4 | `tests/frame_reader_fuzz.cpp main` | 3192 (312) |
| 5 | `coop.cpp ApplyRemoteFramePositionPhase` | 2256 (201) |

**P3 note:** `TickPerfWindow` (52 строки) и другие многострочные заголовки
теперь учитываются; `--self-check` проверяет многострочную функцию и
отсутствие ложных `if`/call-функций.

**Остаточные hotspot'ы (следующие цели):** `Tick` — главный (66306, 1222
строки); `SendHostSnapshot` 15750 (578; вырос в e19 из-за wagon/wheel-кода —
тот же класс CRAP, что и раньше); `ProcessGuestHorseOutfits` 7140 (263);
дальше `ApplyRemoteFramePositionPhase` 2256, `SendLocalPlayerFrame` 1806,
`ApplyActionToGhost` 1560, `TickHostTrains` 1560. 3 dup-группы `count>=4` —
нативные обёртки `actor_sync`/`world_sync`.

**Baseline note:** baseline **обновлён на e16** (45/388/1/3/0) и затем
перегенерирован после P3-фикса парсера (45/489/4/3/0 на `2a58ee8`).
**e18-скан (45/502/3/3/0) и e19-скан (45/505/3/3/0) в baseline НЕ залиты** —
обновятся осознанно только после сигнала Lead (строгий гейт и так не падает:
CRAP>5000/dup>=4/TODO не растут).

## Quality gate в CI

**Strict-гейт включён по умолчанию** в `scripts\ci.ps1`: CI падает при росте
`CRAP>5000`, dup-групп `count>=4` или TODO/FIXME относительно
`tools\quality_baseline.json`. Для диагностики strict можно явно отключить.

```powershell
# обычный CI: strict quality gate (default)
powershell -File scripts\ci.ps1

# report-only: без fail на регрессии (явное отключение)
powershell -File scripts\ci.ps1 -NoStrictQuality
$env:DSH_CI_STRICT_QUALITY = "0"; powershell -File scripts\ci.ps1
```

CI всегда запускает `python tools\quality_scan.py --json build\quality_latest.json --top 20`,
пишет артефакт `build/quality_latest.json` и печатает summary (files/functions/
dups/CRAP>5000/TODO + top-5). Strict-режим сравнивает только **регрессии**:
`CRAP>5000`, группы дублей `count>=4`, число TODO/FIXME. Delta в логе означает
`latest - baseline`; отрицательная дельта — улучшение. Baseline обновляется
осознанно после доказанной шлифовки (build + CI + повторный scan):
`python tools\quality_scan.py --json tools\quality_baseline.json --top 20`.

## P2/P3 — independent proof fixes (closed)

- **P3 (scanner):** `tools/quality_scan.py` теперь детектит многострочные
  заголовки (накопление до `)`+`{`, окно 10 строк) и имеет honest
  `--self-check`: многострочная функция находится, `if`/вызовы не
  превращаются в функции. Исправление вскрыло скрытые функции; baseline
  перегенерирован (**45/489/4/3/0** на `2a58ee8`), это не регрессия кода.
- **P2 (integration flake):** `tests/integration_server.py` сериализует
  процессы через `IntegrationRunLock` (lock-файл в temp, OS-release на
  выходе) — `_pick_port` больше не гоняется с параллельным прогоном.
  Параллельные запуски ждут, а не получают WinError 10054/10061.
  Проверка: 3 последовательных прогона + параллельный — 0 флейков.

## Глоссарий домена (Model the Domain)

| Термин | Значение |
|---|---|
| actor / roster | реплицируемый NPC/животное/лошадь и серверный список активных акторов на хосте |
| epoch | идентификатор сессии/раунда хоста; устаревшие сообщения с чужим epoch отбрасываются |
| proxy | гостевая визуальная копия сущности хоста (ped/vehicle/train) |
| host / guest | игрок-авторитет мира и второй игрок («visitor») |
| visitor mode | у гостя подавляется локальная ambient-толпа; продавцы/квестовые/событийные NPC остаются своими |
| presence | UI-статус игрока (карта/пауза/сатчель/магазин) в имени над головой |
| occupant / draft | пассажир/водитель в транспорте; тягловая лошадь в упряжке |
| snapshot | состояние хоста: ActorFrames (движение), WorldSnapshot (транспорт), TrainSnapshotV2 (поезд) |
| warp / teleport | аварийная синхронизация позиции при большом рассинхроне (>25 м / >8 м для машин) |
| gait / MBR | desired move blend ratio: 0..3 (idle/walk/run/sprint), упакован в кадр |
| appearance assets / tint | компоненты внешности (drawable/albedo/normal/material/palette/tint) |
| metaOutfit | hash мета-наряда (в spawn/heartbeat актора) |
| corpse hold | удержание трупа у гостя, пока он существует на хосте (`npc_corpse_max_sec`) |
| mission barrier | синхронизация «все готовы/миссия стартует» между игроками |
| damage / headshot | host-authoritative урон от гостя; хедшот = 250 dmg (ваншот) |

## Журнал шлифовки (Keep a Log)

Базовая линия: `docs/UB_MODE_BASELINE.md` (генерируется `python tools/quality_scan.py --md
docs/UB_MODE_BASELINE.md`). Каждая единица шлифовки оформляется отдельным коммитом:
- **why** — какое нарушение ub-mode закрывает (CRAP/длина/дублирование/мёртвый код/границы);
- **what changed** — структурные изменения без изменения поведения;
- **proof** — build + `scripts/ci.ps1` (**npc-e19: 42/42**) + C++ + повторный `quality_scan` с дельтой.

| # | Единица | Зона | Статус |
|---|---|---|---|
| 0 | Установка навыка и директива «always» | DSH + AGENTS.md | done |
| 1 | `quality_scan.py` + baseline | tools/docs | done |
| 2 | Шлифовка `net/` и `server/` | net-review | done — **0a4cc48** |
| 3 | Шлифовка `src/` (кооп-логика, actor_sync, world_sync) | client-review | done — **c3dc018 + 6295e6b + 5799ea7 + 0e64a22 + cd1da72** |
| 4 | Шлифовка тестов/скриптов/доков + quality-гейт в CI | qa-tests-review | done |
| 5 | Независимое ревью шлифовки по чек-листу ub-mode | worldsync-architect | planned |

### Action log task-166

| шаг | зачем | что | proof |
|---|---|---|---|
| 1 | Build the Lever: сделать качество измеримым в proof gate | `scripts/ci.ps1`: quality scan + summary + strict by default (`-NoStrictQuality`/env0 off) | quality-only CI PASS; strict FAIL-симуляция |
| 2 | Зафиксировать текущую точку | `tools/quality_baseline.json` = снимок 45/334/4/7/0 | baseline JSON совпадает со сканом |
| 3 | Честные behavior-тесты вместо «покрытия» | расширены `mission_barrier_rules_fuzz`, `world_entity_budget_test`, `world_snapshot_parse_test`, `config_test`, `frame_reader_fuzz` | C++ tests PASS (см. `tests/README.md`) |
| 4 | Не допустить регрессии | strict-гейт: CRAP>5000 не растёт, dup>=4 не растёт, TODO не растёт | CI quality PASS; baseline delta |
| 5 | Документировать | `docs/UB_MODE.md`, `tests/README.md`, `tools/README.md` | `git diff --check`, CI |
| 6 | Финальный quality-delta после 0a4cc48/c3dc018 | `quality_scan.py --json build\quality_final.json --top 20`; обновлены числа «до/после» | 45/334/4/7/0 → 45/384/1/7/0; top-5 таблица; canonical CI 41/41 strict PASS |
| 7 | Финальный e14-delta после 6295e6b (npc-e14) | `quality_scan.py --json build\quality_final_e14.json --top 20`; docs/tools обновлены | 45/334/4/7/0 → 45/386/1/7/0; Tick 120756; e14 deployed local+ARINA |
| 8 | Финальный e15-delta после 5799ea7 (npc-e15) | `quality_scan.py --json build\quality_final_e15.json --top 20`; docs/tools/README обновлены | 45/334/4/7/0 → 45/387/1/3/0; Tick 120756; e15 local+ARINA, сервер PID 10636 |
| 9 | e16-delta после 0e64a22 (npc-e16) + **lowered baseline** | скан чистого архива 0e64a22; `tools/quality_baseline.json` = 45/388/1/3/0; docs/tools/README | 45/388/1/3/0 → baseline; strict CI PASS; e16 local+ARINA, ASI C4E4CF70 |
| 10 | P2/P3 independent-proof fixes (task-173) | P3: multiline parser + `--self-check`; P2: `IntegrationRunLock`; baseline regenerated at `2a58ee8` | scan 45/489/4/3/0; 3 integration runs + parallel PASS; strict CI 41/41 |
| 11 | e18: native research + server soft timeout + client heartbeat/vendor + parser/run-lock + v20 docs (task-174/175/176/180) | `server/`, `src/`, `tests/`, `tools/`, `docs/` | e18 scan 45/**502**/**3**/**3**/0; canonical CI 41/41 + C++7; деплой local+ARINA (ASI `E3FAB30F`, server `DD9BD9D8`); baseline после 341164e = 45/489/4/3/0, e18-скан в baseline НЕ залит (ждём сигнал Lead) |
| 12 | e19: blip name + occupant-first roster/full allowlist + opt-in wheel spin + SET_ENTITY_COORDS arity (df02bec); docs/test e18+e19, join-grace 42/42, quality journal (ef49ac8) | `src/`, `docs/`, `tests/` | e19 scan 45/**505**/**3**/**3**/0 (+3 функции: `MakeLiteralString`/`SetBlipName`/`UpdateBlipName`; Tick 66306, SendHostSnapshot **15750**); canonical CI **42/42** + C++7 + quality strict PASS; деплой local+ARINA (ASI `DD0EE210`, server `DD9BD9D8`), snapshot `dist/npc-e19`, тег npc-e19 |
| 13 | **v20 P1 (guest influence), ветка `feature/v20-p1`:** U1 wire (0x23/0x24/0x26), U2 server relay/rate 8/20/10, U3 host apply, U4 guest capture/mirror + 4 INI-ключа, U5 tools/tests/docs | `net/`, `server/`, `src/`, `tests/`, `tools/`, `docs/` (не `main`) | `9703943`/`8a34e7d`/`77055fc`; скан ветки **46/573/3/2/0**; full `scripts\ci.ps1` = **CI OK**: build client+server 0 warnings, C++ **8/8**, integration **50/50**, quality strict PASS (delta crap 4→3, dup 3→2), baseline не менялся; **P1 default-off** (`vehicle_interaction=0`), deploy `npc-e20` pending |

### Action log npc-e18 (task-174/175/176/180)

| шаг | зачем | что | proof |
|---|---|---|---|
| 1 | убрать скрытый CRAP-hotspot и подготовить soft-миграцию | **a3e6df9** `server/server.cpp`: `HandleClient` 782→25 строк, phase-хелперы (`ReadSocketPhase`, `CheckTimeoutPhase`, `CheckHostGameTimeoutPhase`, `JoinRequestPhase`, `LifecycleMessagesPhase`, `FrameMessagesPhase`, `WorldRelayMessagesPhase`, `ActorRelayMessagesPhase`, `MiscMessagesPhase`, `DispatchMessagesPhase`, `CleanupClient`); **soft host game-timeout** (демоция без закрытия сокета, join grace 15 с, presence=liveness) | integration **41/41** (host-timeout тест переписан под soft-семантику), C++7 |
| 2 | не терять хоста на загрузке и не дублировать продавцов | **5736885** `src/coop.cpp`/`coop.h`/`actor_sync.cpp`: loading presence heartbeat ~2 с, пока playerPed отсутствует; visitor mode skip host-прокси продавца (`actor: visitor vendor skip`, throttled) | build PASS; логика — in-game/fake-тест (S31/S20) |
| 3 | честные тесты + безопасные параллельные прогоны + корректный парсер | **341164e** `tests/integration_server.py`: `IntegrationRunLock` + soft-семантика host-timeout (demote/presence/grace); `tools/quality_scan.py`: многострочные сигнатуры + `--self-check`; baseline regenerated (489/4/3/0) | 3 последовательных + параллельный прогон PASS; scan 45/489/4/3/0 |
| 4 | native-разведка для e19/v20 без изменения кода мода | **50c7fd9** `docs/NATIVE_RESEARCH_INTERACT.md` (blip name, wheel spin ABSENT/SIGNATURE-ONLY, guest→host vehicle interactivity) | read-only research; хэши/арности сверены с NativeDecl/nativedb |
| 5 | зафиксировать дизайн v20 и журнал/метрики | **6772c8a** `docs/INTERACTIVITY.md` (P0–P4), журнал/числа в `docs/UB_MODE.md`, README-ссылки | docs-only; CI не затрагивается |
| 6 | приёмка e18 (эта задача) | `docs/ACCEPTANCE_2PC.md` → npc-e18 (soft migration, F1–F4, INI 27, хеши), `docs/NPC_E1_TESTING.md` S31, README/ROADMAP/tools/tests README, +1 honest grace-тест | `tests/integration_server.py` на `build-server-ci`: **42/42** (41 e18 + grace-тест, 0 failed); `scripts\ci.ps1 -NoClient -NoServer`: quality strict PASS (crap>5000 4→3, dup 3, TODO 0) + C++7 + restore + 42/42; e18-скан 45/502/3/3/0; e18-пара сверена на local+ARINA |


### Action log npc-e19

| шаг | зачем | что | proof |
|---|---|---|---|
| 1 | закрыть игровые жалобы: «Спутник» на карте, кучер/лошади повозки у гостя, статичные колёса | **df02bec** `src/`: имя игрока на карте (`MakeLiteralString` + `_SET_BLIP_NAME` 0x9CB1A1623062F402, `SetBlipName`/`UpdateBlipName`, лог `coop: blip name '%s'`, `BLIP_REMOVE_MODIFIER` — резерв); occupant-first roster + полный allowlist occupants всех vehicle в occ-радиусе (кучер/draft-лошади повозки); opt-in `npc_vehicle_wheels=0` (A/B, `SET_VEHICLE_FORWARD_SPEED` SIGNATURE-ONLY, лог `world: vehicle wheels speed=`); `SET_ENTITY_COORDS` арность 8 (clearArea=0 — поведение не менялось) | build 0 warnings; canonical CI **42/42** + C++7; in-game A/B — S31/S32/ACCEPTANCE G5 |
| 2 | снапшот/деплой/доки под e19 | **ef49ac8**: acceptance A–H (e18/e19), S31, e19 wheel INI note, join-grace тест (42/42), журнал; снапшот `dist/npc-e19`, тег npc-e19, деплой local+ARINA (ASI `DD0EE210`, server `DD9BD9D8`); независимая проверка `architect-verify2`: fresh build 0 warnings, 42/42, C++7, restore PASS, quality strict PASS, low/info — blip label/плотный трафик/wheel A/B | хеши local+ARINA совпали; docs scan **45/505/3/3/0** |
| 3 | number-блоки e19 (этот unit) | `docs/UB_MODE.md`, `tools/README.md`, `README.md`, `ROADMAP.md` → e19-числа; `docs/ACCEPTANCE_2PC.md` → e19-пара/28-й INI `npc_vehicle_wheels`, S31+occupants/vendors/blip/wheel A/B в чек-лист | scan 45/505/3/3/0, Tick 66306/1222, SendHostSnapshot 15750/578; baseline не меняется (45/489/4/3/0) |

## Правила безопасного рефакторинга

1. Поведение не меняется: только структура/имена/удаление мёртвого кода. Любое изменение
   поведения — отдельная задача с тестом/приёмкой.
2. Один коммит — одна единица; после каждой: build + CI. Протокол не трогаем.
3. Не «улучшаем» код, который уже ясен и локален (правило ub-mode «leave it»).
4. Спорные/рискованные места — в backlog с file:line, а не «на глазок».
