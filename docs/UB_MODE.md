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
| `scripts/ci.ps1` | proof gate: build ASI + build server + integration 41/41 + C++ тесты |
| `tools/snapshot_build.ps1` | сохранить сборку в `dist/<tag>/` для отката |
| `tools/perf_report.py`, server `net:` heartbeat | измеримые перф/трафик-гейты (G2/G3) |

Компаньоны ub-mode (`crapper`, `dryer`, `mutator`) требуют отдельной установки и покрытия;
в этом репозитории роль crapper/dryer выполняет `quality_scan.py` (CRAP-прокси и
дублирование), роль mutator — тесты `tests/` + CI (там, где честный тест возможен; для
игровых нативов — smoke/2-ПК сценарии). Не пишем «плохие» тесты ради покрытия.

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
| 4 | Шлифовка тестов/скриптов/доков + quality-гейт в CI | qa-tests-review | planned |
| 5 | Независимое ревью шлифовки по чек-листу ub-mode | worldsync-architect | planned |

## Правила безопасного рефакторинга

1. Поведение не меняется: только структура/имена/удаление мёртвого кода. Любое изменение
   поведения — отдельная задача с тестом/приёмкой.
2. Один коммит — одна единица; после каждой: build + CI. Протокол не трогаем.
3. Не «улучшаем» код, который уже ясен и локален (правило ub-mode «leave it»).
4. Спорные/рискованные места — в backlog с file:line, а не «на глазок».
