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

## Метрики: baseline → текущий скан

Baseline (снимок `tools/quality_baseline.json`, из `docs/UB_MODE_BASELINE.md`):
45 файлов, 334 функции, **4** функции с CRAP-прокси >5000, **30** групп
дублирования (в отчёте top-25; **7** групп с `count>=4`), **0** TODO/FIXME,
warning-disable = 0. Топ CRAP baseline: `coop.cpp Tick` 140250 (1712 строк),
`net/client.cpp RunOnce` 24492 (574), `coop.cpp ApplyRemoteFrame` 8010 (386),
`config.cpp ApplyLine` 7656 (129).

Текущий скан после задачи-166 (на фоне параллельных рефакторингов
`net/client.cpp` / `src/config.cpp` / `src/coop.cpp`): 45 файлов,
**381** функция, **2** функции CRAP>5000, **7** dup-групп `count>=4`,
**0** TODO; топ: `Tick` 123552, `ApplyRemoteFrame` 8010,
`main` frame_reader_fuzz 3192, `UpdateLocalShotEvents` 2352,
`ApplyActionToGhost` 1892. Цифры меняются при каждом скане — их источник
`build/quality_latest.json` и summary CI (baseline не перезаписывается
автоматически).

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
| 2 | Шлифовка `net/` и `server/` | net-review | planned |
| 3 | Шлифовка `src/` (кооп-логика, actor_sync, world_sync) | client-review | planned |
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


## Правила безопасного рефакторинга

1. Поведение не меняется: только структура/имена/удаление мёртвого кода. Любое изменение
   поведения — отдельная задача с тестом/приёмкой.
2. Один коммит — одна единица; после каждой: build + CI. Протокол не трогаем.
3. Не «улучшаем» код, который уже ясен и локален (правило ub-mode «leave it»).
4. Спорные/рискованные места — в backlog с file:line, а не «на глазок».
