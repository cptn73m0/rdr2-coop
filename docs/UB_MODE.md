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
| `scripts/ci.ps1` | proof gate: build ASI + build server + integration 41/41 + C++ тесты; quality gate strict по умолчанию (`-NoStrictQuality`/`DSH_CI_STRICT_QUALITY=0` — report-only) |
| `tools/snapshot_build.ps1` | сохранить сборку в `dist/<tag>/` для отката |
| `tools/perf_report.py`, server `net:` heartbeat | измеримые перф/трафик-гейты (G2/G3) |

Компаньоны ub-mode (`crapper`, `dryer`, `mutator`) требуют отдельной установки и покрытия;
в этом репозитории роль crapper/dryer выполняет `quality_scan.py` (CRAP-прокси и
дублирование), роль mutator — тесты `tests/` + CI (там, где честный тест возможен; для
игровых нативов — smoke/2-ПК сценарии). Не пишем «плохие» тесты ради покрытия.

## Метрики: baseline → после 5799ea7 (npc-e15)

Источник baseline — `tools/quality_baseline.json` (снимок из
`docs/UB_MODE_BASELINE.md`); финальный скан —
`python tools\quality_scan.py --json build\quality_final_e15.json --top 20`.
Baseline зафиксирован до шлифовки и **не перезаписывается** автоматически.

| метрика | baseline | после 0a4cc48/c3dc018/6295e6b/5799ea7 | delta |
|---|---:|---:|---:|
| файлов | 45 | 45 | 0 |
| функций | 334 | 387 | +53 |
| функций с CRAP-прокси >5000 | **4** | **1** | **-3** |
| dup-групп `count>=4` | **7** | **3** | **-4** |
| TODO/FIXME | **0** | **0** | 0 |
| warning-disable | 0 | 0 | 0 |

Рост числа функций — выделенные helpers при разбиении `RunOnce`/`Tick`/
`ApplyRemoteFrame`/`ApplyLine` + behavior-тесты; CRAP>5000 снизился,
`world_sync` host-netId dedupe убрал 4 dup-блока (7→3).

Топ-5 CRAP:

| # | baseline | после |
|---|---|---|
| 1 | `coop.cpp Tick` **140250** (1712) | `coop.cpp Tick` **120756** (1597) |
| 2 | `net/client.cpp RunOnce` 24492 (574) | `tests/frame_reader_fuzz.cpp main` 3192 (312) |
| 3 | `coop.cpp ApplyRemoteFrame` 8010 (386) | `coop.cpp ApplyActionToGhost` 1892 (168) |
| 4 | `config.cpp ApplyLine` 7656 (129) | `net/client.cpp RunOnce` 1482 (211) |
| 5 | `tests/frame_reader_fuzz.cpp main` 2550 (280) | `coop.cpp ApplyPedStateToGhost` 1332 (185) |

`ApplyRemoteFrame` 8010→**20** (386→22 строк), `ApplyLine` 7656→506,
`RunOnce` 24492→1482, `UpdateLocalShotEvents` 181→132 строк (CRAP 2352→1260)
— из top-5 вышли.

**Остаточные hotspots:** `coop.cpp Tick` — единственная функция CRAP>5000
(120756, 1597 строк); 3 dup-группы `count>=4` — оставшиеся нативные обёртки
`src/actor_sync.cpp:2689/2691`, `src/world_sync.cpp:1751`; следующие helper'ы —
`ApplyActionToGhost` 1892 (168), `ApplyPedStateToGhost` 1332 (185),
`ApplyOutfitToGhost` 1122 (165); dead code/native dedupe — backlog.
Baseline оставлен прежним, чтобы strict-гейт продолжал ловить регрессии.

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
- **proof** — build + `scripts/ci.ps1` (41/41 + C++) + повторный `quality_scan` с дельтой.

| # | Единица | Зона | Статус |
|---|---|---|---|
| 0 | Установка навыка и директива «always» | DSH + AGENTS.md | done |
| 1 | `quality_scan.py` + baseline | tools/docs | done |
| 2 | Шлифовка `net/` и `server/` | net-review | done — **0a4cc48** |
| 3 | Шлифовка `src/` (кооп-логика, actor_sync, world_sync) | client-review | done — **c3dc018 + 6295e6b + 5799ea7** |
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


## Правила безопасного рефакторинга

1. Поведение не меняется: только структура/имена/удаление мёртвого кода. Любое изменение
   поведения — отдельная задача с тестом/приёмкой.
2. Один коммит — одна единица; после каждой: build + CI. Протокол не трогаем.
3. Не «улучшаем» код, который уже ясен и локален (правило ub-mode «leave it»).
4. Спорные/рискованные места — в backlog с file:line, а не «на глазок».
