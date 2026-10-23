# E-1: probe-контракт, анализатор и результаты реального прогона NPC

**Статус (23.10.2026):** E-1 закрыт по данным probe-прогона 21.10.2026
(лог 157 МБ, 2 612 896 строк, 837 `probe:`-строк: 49 записей NPC + 788
scan-окон). По результатам реализованы **E1–E3b** actor-репликации NPC
(human-only, meta-outfit, всадники, сценарии/смерть, оружие/бой) и
**E2c-a** host outfit brute-force resolver; E1–E3b проверены на одном ПК
(fake host/guest, см. `docs/NPC_E1_TESTING.md`). E2c-a использует данные
probe (22% NPC имеют `metaOutfit`), но для random ambient NPC даёт
resolved=0/failed=11 → **E2c-b asset/tint ждёт протокол v16 (0x1E)**.
Этот документ — исторический E-1-срез; фактическая реализация описана в
`docs/PROTOCOL.md` и README.

**Связанные файлы/задачи:**
`docs/TESTING.md` (двух-ПК прогоны), `docs/PROTOCOL.md` (v14 → v15),
`research/rdr2-existing-coop-mods.md`, `third_party/rdr3-discoveries/`
(метапеды/сценарии/relationship), `third_party/rdr2-hashdb/Peds.json`
(имена моделей), `src/actor_sync.cpp` (probe), `tests/npc_probe_analyze.py`
(анализатор), `tests/test_npc_probe_analyze.py` (25 тестов). Прошлый
research-отчёт по матрице реализуемости NPC отправлен Lead отдельно; этот
документ — его E-1-срез.

**Цель E-1 (на что отвечаем до кода E1):**
1. Читается ли у NPC текущий meta outfit hash (`metaOutfit != 0`) или нужен
   fallback через перебор outfit-хэшей / захват компонентов.
2. Согласуется ли `_IS_META_PED_OUTFIT_EQUIPPED` (`equipped`) с найденным хэшем.
3. Что реально приходит в `dynScenario`/`pointType` для сценарных NPC и
   достаточна ли детекция (полная/частичная/нулевая).
4. Наблюдаема ли доля human vs animal в пуле (и как она зависит от локации).
5. Есть ли в пробе кони/оружие (для приоритета mount/combat-репликации).

**Ответы по данным §8:** H1 = PARTIAL (22.4% outfit hash), H2 = PASS (100%),
H3 = PASS (100% через pointType), H4 = PASS (pool counters: human 44.6% /
animal 55.4%, сильно зависит от локации), H5 = INFO (mount 10.2%, armed 10.2%),
H6 = PASS (0/25 несовпадений human vs имя модели).

---

## 1. Как включить probe

1. Собрать/задеплоить актуальный `.asi` (с actor-репликацией v15) на **хост**
   (на госте probe не нужен; гость может быть на той же сборке).
2. В `rdr2-coop.ini` рядом с `.asi` на хосте:

   ```ini
   npc_actor = 1
   npc_debug = 1
   ```

   - `npc_actor = 1` включает actor-репликацию NPC (E1–E3b) **и** probe;
     `npc_debug = 1` включает сам probe (чтение мира + лог `probe:`).
   - **Гейт (факт):** probe выполняется только при `npc_actor=1`
     (`src/actor_sync.cpp`, `Tick` → `if (!enabled) return`);
     `npc_actor=0` даёт **ноль** `probe:`-строк и ноль `actor:`-строк.
     Постановка Lead предполагала `npc_debug=1, npc_actor=0`; расхождение
     зафиксировано, решение — за Lead (развязать probe от `npc_actor` или
     оставить как есть).
   - `npc_debug = 0` (default) или `npc_actor = 0` — **ноль** строк `probe:`
     и ноль новых нативов.
   - `world_sync = 0` (и прочие experimental-флаги = 0) — чтобы лог был чистым.
   - **Per-ped строки human-only:** probe логирует top-20 **живых людей**
     (`!IsHuman`/dead отсекаются), животные в per-ped `probe: model=…` не
     попадают. Но summary-строка скана (task-38) содержит счётчики
     `humans=/animals=/total=` по **всем** педам в радиусе до human-фильтра —
     именно они дают pool-долю human/animal (H4).
3. Запустить RDR2, загрузить сейв хоста, встать в выбранной локации (см. §4)
   и дать probe поработать **2–3 минуты**.
4. Закрыть игру, скопировать `rdr2-coop.log` (рядом с `.asi`, на тестовой
   машине это `E:\Games\Red Dead Redemption 2\scripts\rdr2-coop.log`) как
   отдельный файл прогона (`probe_town.log`, `probe_road.log`, ...).
5. Прогнать анализатор:

   ```powershell
   python tests\npc_probe_analyze.py "E:\Games\Red Dead Redemption 2\scripts\probe_town.log"
   python tests\npc_probe_analyze.py probe_road.log --top 12
   python tests\npc_probe_analyze.py probe_town.log --strict   # gate: exit 1 при FAIL H1/H2/H3/H6
   python tests\npc_probe_analyze.py probe_town.log --json > probe_town.json
   ```

   Только stdlib, Python 3.8+, игру/сборку не требует. Exit codes:
   `0` — анализ выполнен; `1` — `--strict` и провалена gating-гипотеза
   (H1/H2/H3/H6 = FAIL); `2` — нет probe-строк/файл не прочитан.

---

## 2. Контракт строки `probe:`

Один пед = одна строка (порядок полей фиксированный, но анализатор терпим к
порядку/лишним пробелам/`true|false`):

```
probe: model=0x… metaOutfit=0x… equipped=N mount=0x… weapon=0x… scenarioAny=N dynScenario=0x… pointType=0x… pedType=N human=N mission=N
```

| Поле | Формат | Натив (hash, arity) | Смысл | Ожидаемо |
|---|---|---|---|---|
| `model` | `0x%X` | `GET_ENTITY_MODEL` `0xDA76A9F39210D365`(1) [nh] | модель педа | известные модели из `Peds.json`; `?`/unknown — модель отсутствует в hashdb |
| `metaOutfit` | `0x%X` | `_GET_PED_META_OUTFIT_HASH` `0x30569F348D126A5A`(1) [nh] | текущий мета-наряд | у NPC бывает ≠ 0, но не всегда: в прогоне 23:00 — 11/49 (22.4%), H1 = PARTIAL |
| `equipped` | `0 / 1` | `_IS_META_PED_OUTFIT_EQUIPPED(ped, metaOutfit)` `0x98082246107A6ACF`(2) [nd] | «наряд считается надетым» | `1` при `metaOutfit != 0` (H2); в прогоне 11/11 = 100% |
| `mount` | `0x…` (0 = пешком) | `GET_MOUNT` `0xE7E11B8DCBED1058`(1) [nh]; канон — **model hash лошади** `GET_ENTITY_MODEL(GET_MOUNT(ped))` | на чём едет | ≠0 только у всадников; в городе 5/49 = 10.2% |
| `weapon` | `0x…` | `GET_CURRENT_PED_WEAPON` `0x3A87E44BB9A01D54`(5) [nh] (out-hash) | оружие в руках/выбранное | **0xA2719263 = WEAPON_UNARMED — это «без оружия»**, не armed; в прогоне: raw ≠0 75.5%, из них Unarmed 32/49 (65.3%), armed 5/49 (10.2%) |
| `scenarioAny` | `0 / 1` | `IS_PED_USING_ANY_SCENARIO` `0x57AB4A3080F85143`(1) [nh] | пед вообще в сценарии | 1 у сидящих/курящих/работающих; в прогоне 26/49 |
| `dynScenario` | `0x%X` | `_GET_ACTIVE_DYNAMIC_SCENARIO` `0x569F1E1237508DEB`(1) [nh] | динамический сценарий | комментарий nativedb: **только kneel/sit/squat/sleep**; в прогоне 24/26 сценариев (92.3%) |
| `pointType` | `0x%X` | `_GET_SCENARIO_POINT_TYPE_PED_IS_USING` `0x2D0571BB55879DA2`(1) [nd] | тип scenario point, который использует пед | ≠0 у педов на сценарных точках; в прогоне **26/26 (100%)** — основной канал H3 |
| `pedType` | `N` | `GET_PED_TYPE` `0xFF059E1E4C01E63C`(1) [nd] | ePedType | в прогоне только 4 (42) и 5 (7); **гипотеза: 4 = human male, 5 = human female** (25/25 известных моделей сошлись, 24 unknown) |
| `human` | `0 / 1` | `IS_PED_HUMAN` `0xB980061DA992779D`(1) [nd] | человек/животное | 1 — люди; per-ped записи human-only, поэтому 1 у всех 49 |
| `mission` | `0 / 1` | `IS_ENTITY_A_MISSION_ENTITY` `0x138190F64DB4BBD1`(1) [nh] | mission-entity | 0 у ambient; в прогоне 14/49 (28.6%) — миссионные акторы |

Пометки: `[nh]` — хэш уже есть в `src/natives.h` и проверен в проекте;
`[nd]` — из `third_party/rdr2-runtime-scripthook/.../NativeDecl.cs` /
`research/rdr3-nativedb-data/natives.json`, перед реализацией сверить по
правилу проекта (DEV-NOTES: Disquse/rdr3noclip, не alloc8or).

**Служебные нативы сканера (не в строке):** `IS_PED_A_PLAYER`
`0x12534C348C6CB68B`(1) [nd], `IS_PED_ON_MOUNT` `0x460BC76A0E10655E`(1) [nh],
`IS_PED_DEAD_OR_DYING` `0x3317DEDB88C95038`(2) [nh], `GET_ENTITY_COORDS`
`0xA86D5F069399F44D`(3) [nh], отсев локального игрока/ghost/лошади, радиус 60 м,
cap 20 живых людей, частота ~120 тиков (2 с).

**Summary-строка (task-38), отдельно от per-ped:**

```
probe: scan humans=H animals=A total=T logged=L newCombos=C unique=U
```

- `total` — все педы в радиусе 60 м (кроме local player/mount/player-педов),
  `humans` — из них люди, `animals` — остальные (включая мёртвых и лошадей);
- `logged` — сколько попало в top-20, `newCombos` — новых комбинаций в окне,
  `unique` — всего уникальных комбинаций за сессию;
- анализатор парсит summary отдельно (это не запись педа), строит
  `Pool totals by scan windows`, `Pool by location heuristic` и
  `Longest pool segments`, и **H4 считает именно по этим счётчикам**.

**Троттлинг per-ped:** строка пишется раз в ~2 с на **уникальную комбинацию
model+metaOutfit**, поэтому `records` ≠ числу NPC в мире; «unique signatures»
в анализаторе — оценка разнообразия (стабильного `ped id` в контракте нет).
Для процентов внешности двигайтесь по городу/лагерю, а не стойте на месте.

**Открытые вопросы к реализации (остались после сверки кода):**
1. Гейт probe через `npc_actor=1` (см. §1) — расхождение с постановкой Lead.
2. `pointType` печатается как `(unsigned int)` от `int`: отрицательные значения
   выглядят как `0xFFFFFFxx`; анализатор считает любые `!= 0` — при разборе
   конкретных хэшей учитывать.
3. В прогоне 7 записей имеют `dynScenario != 0` при `scenarioAny=0` —
   семантика переходов/остаточного сценария требует отдельной проверки.

---

## 3. Что искать в логе (ручная проверка до анализатора)

```powershell
# сколько probe-строк и как они выглядят
Select-String -Path "E:\Games\Red Dead Redemption 2\scripts\rdr2-coop.log" -Pattern 'probe:' | Select-Object -First 20
(Select-String -Path "...\rdr2-coop.log" -Pattern 'probe:').Count
```

Ожидаемо:
- при `npc_actor=1, npc_debug=1` на **хосте** — есть строки `probe:` с 11
  полями и summary-строки `probe: scan humans=… animals=… total=… …`
  (раз в окно ~2 с);
- при `npc_actor=0` **или** `npc_debug=0` — **ноль** `probe:`; при
  `npc_actor=0` также ноль `actor:`-строк;
- на **госте** при `npc_debug=1` — ноль `probe:` (probe host-only); иначе
  сообщить Lead (нарушение контракта/реализации);
- в строках нет `NaN`, пустых `model=`/`human=`;
- `probe:` не чаще ~1 строки на 2 с на комбинацию; нет потока тысяч строк/с
  (throttle работает, не спамит Tick-бюджет).

---

## 4. Какие прогоны нужны (3+ файла лога)

| Прогон | Где | Что проверяем | Длительность |
|---|---|---|---|
| `probe_town.log` | Saint-Denis / Valentine / Rhodes, людная улица | H1 (outfit hash людей), H2, H4 pool, H6 | 2–3 мин |
| `probe_road.log` | дорога/ранчо/кемпинг, всадники, караваны | H5 mount/armed, pool animals, мета-аутфиты лошадей | 2–3 мин |
| `probe_scenario.log` | салун/скамейки/кровать/костёр/рабочие точки, лагерь | H3 dynScenario/pointType, позы (sit/smoke/kneel/sleep) | 2–3 мин |
| (опционально) `probe_mission.log` | рядом с миссией/лагерем банды | `mission=1`, `cs_*`/`mp_*`/`msp_*`, armed | 1–2 мин |

Рекомендации: хост стоит/медленно идёт, не бежит; время суток/погода не важны;
`npc_actor=1`, `npc_debug=1` (см. §1), `world_sync=0`; после каждого прогона —
отдельный файл лога (иначе смешаются локации и гипотезы). Прогон 23:00 был
смешанным (дикая природа + город) — поэтому animal-доля в нём location-biased,
см. §8.

---

## 5. Ожидаемые модели/классы

`third_party/rdr2-hashdb/Peds.json` — 1982 модели. Распределение по классам
(для интерпретации `model` и `human`):

| Класс (префикс) | Моделей | Доля | Где ожидать в probe |
|---|---:|---:|---|
| `mp_*` multiplayer | 499 | 25.2% | миссии/катсцены, не ambient |
| `cs_*` cutscene | 328 | 16.5% | миссии/катсцены |
| `u_*` unique | 234 | 11.8% | городские именные/уникальные ambient |
| `a_c_*` animal | 147 | 7.4% | дикая природа (олени, койоты, птицы) |
| `a_c_horse_*` horse | 144 | 7.3% | дороги, ранчо, всадники |
| `a_m_*` ambient male | 132 | 6.7% | города/дороги (основной ambient) |
| unnamed (`""`) | 120 | 6.1% | hashdb без имени; анализатор покажет `?` |
| `re_*`/`rc_*` event | 119 | 6.0% | случайные события/ограбления |
| `msp_/mes_/rcsp_/...` mission actor | 97 | 4.9% | миссии |
| `s_*` story | 62 | 3.1% | сюжетные сцены |
| `a_f_*` ambient female | 59 | 3.0% | города/дороги |
| `g_*` gang | 25 | 1.3% | лагеря банд, засады |
| прочее (`player_*`, `p_c_*`, сёдла, unnamed) | ~16 | ~0.8% | игрок/пропы; в probe игрока быть не должно |

Итого human-ish ≈ 79%, животные+кони ≈ 15%, unnamed ≈ 6% (по hashdb, не по
миру). В реальном probe распределение зависит от локации: в городе доминируют
`a_m_*`/`a_f_*`/`u_*`, в wilderness — `a_c_*`, на дороге — `a_c_horse_*` и
всадники. В прогоне 23:00 топ моделей: `G_M_M_UNIINBRED_01` (5),
`RE_MOONSHINECAMP_MALES_01` (2), `S_M_M_GENCONDUCTOR_01` (2), дальше ambient
`A_M_M_ASBTOWNFOLK_01`, `A_M_Y_ASBMINER_02` и unknown-модели; 24 из 49 записей
получили класс `unknown` (модели отсутствуют в `Peds.json` или безымянные) —
это нормально, анализатор помечает их `?`.

---

## 6. Гипотезы и критерии (реализованы в `tests/npc_probe_analyze.py`)

| ID | Гипотеза | PASS | PARTIAL/WARN | FAIL/UNKNOWN | Что значит для E1 |
|---|---|---|---|---|---|
| **H1** | `metaOutfit != 0` читается у human NPC | ≥90% уникальных human-педов | 10–90% → `PARTIAL` | <10% → `FAIL`; нет human → `INSUFFICIENT` | PASS → E1-A (request/apply outfit); PARTIAL → E1-A для меньшинства + E1-B/E1-C fallback; FAIL → E1-B/E1-C |
| **H2** | `equipped=1` согласуется с `metaOutfit != 0` | ≥95% записей с хэшем | >0 но <95% → `WARN` | 0% → `FAIL`; нет поля → `UNKNOWN` | FAIL → не полагаться на `equipped`; PASS → `equipped` годится для валидации outfit |
| **H3** | Сценарные позы детектируются (`dynScenario` или `pointType`) | ≥80% записей с `scenarioAny=1` | 30–80% → `PARTIAL` | <30% → `FAIL`; нет `scenarioAny` → `INSUFFICIENT` | PASS → E1-D; предпочитать цепочку с большим покрытием (dyn vs point) |
| **H4** | Доля human vs animal в пуле наблюдаема | есть scan-счётчики `humans=/animals=/total=` хотя бы в одном окне, `total>0` | — | нет счётчиков и нет animal-записей → `WARN`; нет данных → `INSUFFICIENT` | PASS → pool-доля известна; animal-доля зависит от локации → E1 human-only, animals/кони — E2 |
| **H5** | mount/armed заполнены | — (INFO) | — | — | mount% → приоритет rider↔mount; armed% (без `WEAPON_UNARMED`) → приоритет combat |
| **H6** | `human` согласуется с именем модели из hashdb | ≤5% несовпадений | ≤20% → `WARN` | >20% → `FAIL`; нет hashdb → `UNKNOWN` | FAIL → баг контракта/реализации (перепутаны human/pedType/поле), чинить до E1 |

Дополнительно анализатор печатает: pool totals/location/segments; распределение
`pedType` с метками-гипотезами; gender-check `pedType 4/5` против имён моделей;
топ моделей, `metaOutfit`, `dynScenario`, `pointType`, mount, armed weapons;
классы моделей и per-class проценты `outfit!=0`/mount/armed/scenarioAny.
Gating для `--strict`: только **FAIL** у H1/H2/H3/H6; `PARTIAL/WARN/UNKNOWN/
INSUFFICIENT` не валят gate (это исследовательские статусы, Lead решает по
отчёту).

---

## 7. Как читать вывод анализатора

1. `log lines / parsed / invalid / scan summaries` — если `invalid > 0`, часть
   строк битая: сравнить с контрактом (§2), сообщить `client-review` точный
   пример.
2. `Pool totals by scan windows` — **H4**: суммарные `humans/animals/total`,
   human/animal share (взвешенный по числу педов), min/avg/max window-share,
   число окон `humans=0`/пустых. Помните: «animal share (pool sum)» и
   «window animal share avg» — разные метрики (окна в wilderness малочисленны
   по людям, но их много; city-окна дают большой вклад в сумму).
3. `Pool by location heuristic` — разбивка окон по составу пула:
   `wild` (<20% humans), `mixed`, `town` (>=50% humans), `empty`; для каждой
   локации — окна, число сегментов, длительность, средние humans/animals и
   shares. Это **эвристика**: координат в scan-строке нет, локация выводится
   из состава пула.
4. `Longest pool segments` — самые длинные непрерывные сегменты (полезно
   увидеть «город»: в прогоне 23:00 — 87 окон/~174 с, humans_avg 64.0,
   animal_share 16.3%).
5. `Pool` (per-ped records) — у per-ped записей `animal=0` всегда (probe
   human-only), это ожидаемо; animal-долю смотреть в scan-таблицах.
6. `Outfits` — **H1**: доля `metaOutfit != 0` по всем/human/animal/уникальным.
7. `Equipped` — **H2**: `equipped=1` среди записей с хэшем; `outfit==0` с
   `equipped=1` — шум/баг семантики.
8. `Scenarios` — **H3**: `dynScenario != 0`, `pointType != 0` и их объединение
   среди `scenarioAny=1`; отдельно «both zero» — сколько поз E1 не увидит.
9. `Mount/weapon/mission` — H5: mount%, **armed (без Unarmed)**, unarmed,
   weapon==0, raw !=0, mission%; `WEAPON_UNARMED` (0xA2719263) — не оружие.
10. `pedType` — метки-гипотезы 4=male, 5=female + gender-check по именам
    моделей; если появятся другие значения — накапливать и добавлять метки
    только после подтверждения.
11. `Top ...` — рабочий список для E1: массовые модели, повторяющиеся
    outfit/scenario/pointType-хэши (их реплеить), armed weapons.
12. `Verdicts` + `E1 plan hint` — итог для Lead; в отчёт копируются строки
    `[STATUS] H…`, `value` и `E1 plan hint`.

---

## 8. Результаты реального прогона (23:00)

**Вход:** `E:\Games\Red Dead Redemption 2\scripts\rdr2-coop.log` (157 171 311 Б;
2 612 896 строк; последняя запись 21.10.2026 23:17). Probe: `npc_actor=1`,
`npc_debug=1`; смешанный маршрут (дикая природа + город), ~26 минут окон.

**Парсинг:** 837 `probe:`-строк = 49 per-ped записей + 788 scan-окон;
invalid = 0. Уникальных сигнатур 49, уникальных моделей 43, outfit-хэшей 11.

### 8.1 Внешность (H1/H2)

| Метрика | Значение |
|---|---:|
| per-ped записей / уникальных | 49 / 49 |
| `metaOutfit != 0` | **11/49 = 22.4%** |
| `metaOutfit == 0` | 38/49 = 77.6% |
| `equipped=1` среди `metaOutfit != 0` | **11/11 = 100%** |
| `equipped=1` при `metaOutfit == 0` | 0/38 |
| уникальных outfit-хэшей | 11 |

**H1 = PARTIAL (0.224)** → прямая репликация через meta outfit возможна только
для ~22% NPC; для ~78% нужен fallback.
**H2 = PASS (1.000)** → `_IS_META_PED_OUTFIT_EQUIPPED(ped, metaOutfit)` можно
использовать как валидатор применённого наряда (и как дешёвый детект смены,
если hash читается).

### 8.2 Сценарии (H3)

| Метрика | Значение |
|---|---:|
| `scenarioAny=1` | 26/49 = 53.1% |
| `dynScenario != 0` среди них | 24/26 = 92.3% |
| `pointType != 0` среди них | **26/26 = 100%** |
| `dyn or point` detected | 26/26 = 100% |
| both zero (undetected) | 0 |
| `dynScenario != 0` при `scenarioAny=0` | 7 записей (см. §11) |

**H3 = PASS (1.000)**, предпочтительная цепочка — **scenario point**
(100% против 92% у dynamic). Топ point-type хэшей: `0x2A0961F2` (2),
`0xAAF6DBE7` (2), `0x07CFB3C9` (2), далее по 1. Топ dynScenario: `0x8C510147`
(6 записей), `0x990BCA7A`, `0xB54455E1`, `0x59C715C7` (по 2).

### 8.3 Пул human/animal по scan-окнам (H4)

| Метрика | Значение |
|---|---:|
| scan-окон с pool-счётчиками | 788 |
| Σ humans / Σ animals / Σ total | 6485 / 8051 / **14536** |
| human share (взвешенный) | **44.6%** |
| animal share (взвешенный) | **55.4%** |
| window human share min/avg/max | 0.0% / 19.3% / 93.9% |
| window animal share min/avg/max | 6.1% / 80.7% / 100.0% |
| окон `humans=0` / пустых | 382 / 5 |
| logged peds max/avg | 20 / 2.83 |

Pool по локациям (эвристика по составу окон):

| Bucket | Окон | Сегментов | Длит., с | humans_avg | animals_avg | human_share | animal_share |
|---|---:|---:|---:|---:|---:|---:|---:|
| wild (<20% humans) | 526 | 12 | 1052 | 0.4 | 11.1 | 3.7% | **96.3%** |
| mixed | 127 | 15 | 254 | 3.5 | 7.5 | 31.8% | 68.2% |
| town (>=50% humans) | 130 | 7 | 260 | 44.8 | 9.7 | **82.1%** | **17.9%** |
| empty | 5 | 1 | 10 | 0.0 | 0.0 | n/a | n/a |

Самые длинные сегменты: wild 235 окон/470 с (humans 0.0, animal 99.8%),
wild 99/198 с (humans 0.6, animal 94.2%), **town 87/174 с (humans_avg 64.0,
animals_avg 12.4, human 83.7% / animal 16.3%)**, mixed 62/124 с, wild 48/96 с.

**H4 = PASS**: pool-доля измерена, но **animal share сильно зависит от
локации** — в городе ~18% животных, в дикой природе ~96–100%. Общие 55.4%
animal — следствие смешанного маршрута и множества wilderness-окон, а не
типичного города.

### 8.4 Остальные метрики

| Метрика | Значение |
|---|---:|
| mount != 0 | **5/49 = 10.2%** |
| armed (weapon ≠ 0, ≠ Unarmed) | **5/49 = 10.2%** |
| unarmed (`0xA2719263`) | 32/49 = 65.3% |
| weapon == 0 | 12/49 = 24.5% |
| weapon != 0 (raw, ошибочная метрика) | 37/49 = 75.5% |
| mission != 0 | **14/49 = 28.6%** |
| pedType 4 / 5 | 42 / 7 (85.7% / 14.3%) |
| gender-check 4=male/5=female (известные модели) | **25/25 match, 0 mismatch, 24 unknown** |

Топ armed weapons: `0x67DC3FDE` (2), `0x5C54EFD4` (2), `0x6DFA071B` (1).
Топ mount: `A_C_HORSE_KENTUCKYSADDLE_GREY`, `A_C_HORSE_MORGAN_PALOMINO` + 3
unknown-хэша. Модели: 24/49 класса `unknown` (нет в hashdb) — нормально;
outfit!=0 по классам: gang 57%, event 100%, story 50%, ambient 0% (маленькая
выборка), unknown 12.5%.

### 8.5 Вердикты H1–H6 по прогону

| ID | Статус | Кратко |
|---|---|---|
| H1 | **PARTIAL** 0.224 | 11/49 outfit hash; 78% требуют fallback |
| H2 | **PASS** 1.000 | 11/11 equipped согласуется |
| H3 | **PASS** 1.000 | 26/26 сценариев, pointType 100% (dyn 92%) |
| H4 | **PASS** | human 44.6% / animal 55.4% (взвеш.), town animal 17.9% vs wild 96.3% |
| H5 | **INFO** | mount 10.2%, armed 10.2% (Unarmed 65.3% исключён) |
| H6 | **PASS** 0.000 | 0/25 known-model несовпадений human vs имя |

### 8.6 Рекомендации E1/E2 (по данным прогона)

**E1 (human-only):**
1. **Внешность:** H1 PARTIAL → E1-A (request/apply meta outfit) только для
   ~22% педов; для остальных **E1-B**: на хосте перебрать outfit-хэши модели
   из `rdr3-discoveries/clothes/metaped_outfits.lua` (890 моделей) через
   `_IS_META_PED_OUTFIT_EQUIPPED(ped, cand)` (H2 это валидирует), кэш
   model→candidates, cap 8–16; если матча нет — **E1-C** asset/tint capture
   (`GET_NUM_COMPONENTS_IN_PED` + `GET_META_PED_ASSET_GUIDS/TINT` →
   `_SET_META_PED_TAG`, отдельное сообщение протокола, не `ActorOutfit` 784B).
2. **Сценарии:** H3 PASS → **scenario point цепочка**: хост шлёт
   pointType+coords/heading, гость `CREATE_SCENARIO_POINT_HASH` +
   `TASK_USE_SCENARIO_POINT` (сохраняет пропы), dynScenario — fallback
   (`TASK_START_SCENARIO_IN_PLACE_HASH`). Приоритет — pointType (100%).
3. **Бюджет/плотность:** в городе 55–64 человека в радиусе 60 м (сегмент
   town: humans_avg 64.0), а протокол v15 `kMaxActors=12` → покрытие ~19%
   городской толпы. Нужно решение Lead: увеличить cap E1 (напр. 32–64),
   либо явно принять разрежённую репликацию; animals/кони в тот же бюджет не
   брать.
4. **Mission-entity:** 28.6% записей — mission; в E1 **не реплицировать**
   (двойники со своей копией миссии гостя), оставить в discovery только как
   presentation при guest-idle (отдельная задача).
5. **Кони:** mount 10.2% → пара rider↔mount нужна для всадников; сама лошадь
   в E1 минимально (модель+снаряжение) или отложить в E2, но тогда всадник
   будет «пешком» — решить по объёму E1.
6. **Бой:** armed только 10.2% (Unarmed 65.3%) → combat-репликация ниже
   приоритета внешности/локомоции; replay выстрелов — после базового E1.

**E2 (animals/horses отдельно):**
1. Pool: overall animal 55.4%, но **town ~18%, wild ~96–100%** → нужны
   per-location бюджеты и отдельные probe-прогоны по локациям; животные
   per-ped не логируются (human-only) — для E2 нужен all-peds probe или
   отдельный animal-probe.
2. Horses: mount 10.2% в смешанном прогоне, топ-модели известны; horse
   appearance (outfit/shop items) проверять отдельно; тягловые лошади повозок —
   отдельный риск.
3. Animals: модель+outfit (metaped_outfits.lua для животных), локомоция
   задачами, поведение/AI не реплицировать; бюджет и LOD — по локациям.

---

## 9. Приёмка E-1 (чек-лист)

- [x] Реальный probe-прогон на хосте выполнен, лог сохранён
      (`rdr2-coop.log`, 837 `probe:`-строк: 49 per-ped + 788 scan).
- [x] Анализатор на реальном логе: exit 0, invalid=0, H1/H2/H3/H4/H5/H6
      посчитаны, цифры §8 воспроизводятся.
- [x] `python tests\test_npc_probe_analyze.py` → `ALL 25 TESTS PASSED`.
- [ ] `npc_debug=0` (default) на хосте: **0** строк `probe:`; поведение
      и производительность не отличаются от текущей сборки.
- [ ] `npc_actor=0` на хосте (при любом `npc_debug`): **0** строк `probe:` и
      **0** строк `actor:` — каркас E0 неактивен (probe гейтится
      `npc_actor=1`, см. §1).
- [ ] `npc_actor=1, npc_debug=1` на хосте: есть `probe:`-строки с 11 полями
      и summary `probe: scan humans=… animals=… total=… …`;
      нет спама/крашей.
- [ ] `npc_actor=1, npc_debug=1` на госте: **0** `probe:`-строк (probe
      host-only); если нет — зафиксировать и решить с Lead.
- [ ] Отдельные прогоны `probe_town.log` / `probe_road.log` /
      `probe_scenario.log` дают per-location разбивку animal-доли (нужно для
      E2; текущий прогон смешанный).

## 10. План E1 по результатам probe

Матрица «результат H1 → путь внешности»:

| Результат | Путь E1 | Работы | Критерий готовности |
|---|---|---|---|
| H1 PARTIAL (текущий 22.4%) | **E1-A + E1-B** | E1-A (meta outfit) для hash!=0; E1-B — перебор outfit-хэшей модели через `_IS_META_PED_OUTFIT_EQUIPPED` (H2 валидирует); E1-C asset/tint — если перебор не нашёл | E1-A: 11/49 моделей, latency ≤ 3 кадра; E1-B: ≥90% оставшихся моделей находят кандидата, cost bounded (cap 8–16/модель, кэш) |
| H1 PASS (<10% fallback) | **E1-A** | request/apply для всех | outfit hash совпал у >90% |
| H1 FAIL | **E1-B → E1-C** | перебор, затем asset/tint | ≥90% моделей покрыты, иначе E1-C протокол |

Матрица «H3 → путь сценариев»:

| Результат | Путь E1-D | Работы | Критерий |
|---|---|---|---|
| H3 PASS, point>dyn (текущий: point 100%, dyn 92%) | **scenario point** | хост: pointType + coords/heading; гость: `CREATE_SCENARIO_POINT_HASH(pointType, x,y,z,heading,…)` + `TASK_USE_SCENARIO_POINT`; удалять point при выходе | поза+проп совпадают; cap на число point-ов; dyn — fallback |
| H3 PARTIAL | только детектируемое | как выше для детектируемых, остальные idle | не хуже текущего, без «танцев» |
| H3 FAIL/INSUFFICIENT | расширить probe | `PED_HAS_USE_SCENARIO_TASK`, `_GET_SCENARIO_POINT_PED_IS_USING`, `_IS_PED_IN_SCENARIO_BASE` | после probe повторно решить |

Прочие решения:
- **H1 PARTIAL (факт):** 78% NPC — fallback; E1-B (перебор) дешёвый кандидат
  №1, E1-C — резерв; `ActorOutfit` 0x1C не подходит для asset GUID (это
  shop-набор 784B) — для E1-C нужен отдельный формат сообщения.
- **H3 PASS:** pointType-цепочка; `dynScenario` вне `scenarioAny` (7 записей)
  не считать ошибкой, но проверить переходы.
- **H4 PASS:** E1 = human-only; animals/кони — E2 по локациям
  (town animal ~18%, wild ~96–100%).
- **H5:** mount 10.2% → rider↔mount; armed 10.2% → combat не приоритет.
- **Mission-entity:** в E1 не реплицировать (28.6% записей mission), иначе
  двойники со своей копией миссии гостя.
- **H6 PASS:** контракт human/model в порядке; `pedType 4/5` — гипотеза,
  подтверждена 25/25 известных моделей.
- **Бюджет:** kMaxActors=12 (v15) vs 55–64 humans в городе — решение об
  увеличении cap/разрежении за Lead.

---

## 11. Ограничения и риски

1. **Троттлинг/уникальность:** нет `ped id` в контракте → «unique» —
   приближение; если нужен трекинг одной особи во времени, предложить
   добавить `ped=0x…`/`netId` в контракт отдельной задачей.
2. **`equipped` семантика (подтверждена кодом):** `src/actor_sync.cpp`
   вызывает `_IS_META_PED_OUTFIT_EQUIPPED(handle, metaOutfit)` — контракт
   соблюдён, H2 валиден и подтверждён 11/11.
3. **`mount` — model hash (подтверждено):** `GetMountModel()` в
   `src/actor_sync.cpp` пишет `GET_ENTITY_MODEL(GET_MOUNT(ped))`; для E1
   достаточно; handle отдельно не нужен.
4. **Per-ped records human-only (факт):** `src/actor_sync.cpp` фильтрует
   `!IsHuman`; animals/кони видны только в scan-счётчиках (`humans/animals/
   total`), не в per-ped строках → внешность животных в E1 не измеряется.
5. **`_GET_ACTIVE_DYNAMIC_SCENARIO` ограничен** (nativedb: только
   kneel/sit/squat/sleep); в прогоне dyn покрыл 92% сценариев, но
   предпочтительнее pointType (100%).
6. **`dynScenario`/`pointType` — 32-bit hashes**, коллизии/мусор возможны от
   невалидных аргументов (pointType печатается как unsigned int); проверять
   `0` vs `!=0` и топ-список, а не только проценты.
7. **hashdb неполон** (120 безымянных моделей + часть ambient-моделей
   отсутствует) → класс `?`/unknown (24/49 в прогоне); это не ошибка probe.
8. **Лог большой** (157 МБ в прогоне 23:00); анализатор стримит построчно,
   но для пересылки лучше вырезать агрегаты/`probe:`-строки:
   `Select-String -Pattern 'probe:' > probe_lines.txt`.
9. **Probe не должен оставаться включённым** в обычных сессиях: `npc_debug=0`
   default; при `npc_actor=1` actor-пайплайн не должен зависеть от probe-логов.
10. **Версии/хэши:** `[nd]`-нативы перед кодом сверить по NativeDecl/nativedb
    и правилу проекта; probe-нативы в `src/natives.h` добавляет `client-review`,
    не дублировать вручную.
11. **Гейт probe через `npc_actor=1` (факт/расхождение с постановкой):**
    `Tick` → `if (!enabled) return`; постановка Lead — `npc_debug=1,
    npc_actor=0`. До мержа использовать фактический гейт, после мержа —
    зафиксировать окончательный вариант и обновить §1.
12. **Location bias H4:** scan-строка без координат; `wild/mixed/town` —
    эвристика по составу пула. Общая animal share (55.4%) не равна
    «типичному городу» (~18%); для E2 нужны раздельные прогоны по локациям.
13. **`WEAPON_UNARMED` pitfall:** `0xA2719263` нельзя считать armed; raw
    метрика (75.5%) вводит в заблуждение, armed = 10.2%.
14. **pedType mapping — гипотеза:** 4/5 → male/female подтверждена 25/25
    известных моделей, но 24/49 моделей без гендерного имени; появление
    других значений pedType требует новых данных.
15. **`dynScenario != 0` при `scenarioAny=0` (7 записей):** семантика
    переходов/остаточного сценария неясна; не строить на этом детект без
    проверки.
16. **Бюджет E1:** в городе 55–64 humans в 60 м, `kMaxActors=12` → покрытие
    ~19%; нужно решение (cap/разрежение) до E1-приёмки.

---

## 12. Источники

Локальные:
- реальный лог probe-прогона:
  `E:\Games\Red Dead Redemption 2\scripts\rdr2-coop.log` (21.10.2026 ~23:00,
  157 МБ, 49 per-ped + 788 scan);
- `docs/TESTING.md`, `docs/PROTOCOL.md`, `ROADMAP.md`
- `src/natives.h` (проверенные хэши), `src/actor_sync.cpp` (probe-режим и
  summary-счётчики task-38), `src/config.cpp` (`npc_actor`/`npc_debug`),
  `src/world_sync.cpp`, `src/coop.cpp`
- `third_party/rdr2-runtime-scripthook/source/scripting_v3/RDR2.Native/NativeDecl.cs`
- `research/rdr3-nativedb-data/natives.json`
- `third_party/rdr2-hashdb/Peds.json` (1982 модели; имена/классы)
- `third_party/rdr3-discoveries/clothes/metaped_outfits.lua` (890 моделей,
  outfit-хэши), `peds/peds_list.lua`, `animations/scenarios/README.md`,
  `AI/CPED_CONFIG_FLAGS/README.md`, `AI/RELATIONSHIP/README.md`
- `tests/npc_probe_analyze.py` (анализатор: per-ped + scan pool/location/
  segments, unarmed, pedType labels/gender-check), `tests/test_npc_probe_analyze.py`
  (25 unit-тестов, `ALL 25 TESTS PASSED`)

Web (внешний референс, не инструкции):
- [alloc8or RDR3 nativedb](https://alloc8or.re/rdr3/nativedb/)
- [t3chman: Ped/MetaPed overview (outfits/assets/expressions/shop items/overlays)](https://forum.cfx.re/t/ped-modification-and-metadata-overview-outfits-assets-expressions-shop-items-overlays-etc/5124046)
- [Cfx: ambient ped outfits sync / RedM builds 1436/1491](https://forum.cfx.re/t/broken-human-peds-for-specific-models-and-other-ambient-ped-outfits-not-syncing/5111409)
- [Cfx: MP meta ped outfit / fullOutfit / expressions](https://forum.cfx.re/t/discussion-on-female-mp-meta-ped-outfit-data/1803615)
- [Halen84 RDR3 flags/enums](https://github.com/Halen84/RDR3-Native-Flags-And-Enums)
- [femga/rdr3_discoveries](https://github.com/femga/rdr3_discoveries)

---

## 13. Быстрый чек-лист «probe-прогон за 10 минут»

1. `npc_actor = 1`, `npc_debug = 1` (см. §1), `world_sync = 0` на хосте;
   запустить игру.
2. Saint-Denis/Valentine, 2–3 минуты неспешной ходьбы (для animal-доли —
   отдельный прогон в wilderness).
3. Закрыть игру, скопировать лог в `probe_town.log`.
4. `python tests\npc_probe_analyze.py probe_town.log` → скопировать Lead:
   `records/unique`, pool totals/location, H1/H2/H3/H4/H5/H6, топ-модели/
   outfit/scenario, `E1 plan hint`.
5. Повторить для `probe_road.log` (лошади/животные) и
   `probe_scenario.log` (салун/скамейки/кровать).
6. Проверить `npc_actor=0` или `npc_debug=0` → 0 строк `probe:`; запустить
   `python tests\test_npc_probe_analyze.py` → `ALL 25 TESTS PASSED`.
