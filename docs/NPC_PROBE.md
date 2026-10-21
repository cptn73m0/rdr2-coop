# E-1: probe-контракт и чек-лист анализа NPC (read-only)

**Статус:** E-1 (research/gate) — контракт лога, анализатор, чек-лист. Код
probe-режима реализует `client-review` (task-34) в `src/actor_sync.*` +
`src/config.*` + `src/natives.h`. Этот документ фиксирует контракт и порядок
проверки; визуальную репликацию NPC он **не** включает.

**Связанные файлы/задачи:**
`docs/TESTING.md` (двух-ПК прогоны), `docs/PROTOCOL.md` (v14 → v15),
`research/rdr2-existing-coop-mods.md`, `third_party/rdr3-discoveries/`
(данные по метапедам/сценариям/relationship), `third_party/rdr2-hashdb/Peds.json`
(имена моделей). Прошлый research-отчёт по матрице реализуемости NPC отправлен
Lead отдельно; этот документ — его E-1-срез.

**Цель E-1 (на что отвечаем до кода E1):**
1. Читается ли у NPC текущий meta outfit hash (`metaOutfit != 0`) или нужен
   fallback через перебор outfit-хэшей / захват компонентов.
2. Согласуется ли `_IS_META_PED_OUTFIT_EQUIPPED` (`equipped`) с найденным хэшем.
3. Что реально приходит в `dynScenario`/`pointType` для сценарных NPC и
   достаточна ли детекция (полная/частичная/нулевая).
4. Наблюдаема ли доля human vs animal в пуле (и не фильтрует ли probe только
   людей — это меняет план E1/E2).
5. Есть ли в пробе кони/оружие (для приоритета mount/combat-репликации).

**Ничего не собирать и не запускать игру для E-1-документа.** Для самого
probe-прогона нужен собранный `.asi` с probe-режимом (его готовит
`client-review`), игра запускается пользователем/Lead по чек-листу ниже.

---

## 1. Как включить probe

1. Собрать/задеплоить `.asi` с probe-режимом (E0-каркас task-34) на **хост**
   (на госте probe не нужен; гость может быть на той же сборке).
2. В `rdr2-coop.ini` рядом с `.asi` на хосте:

   ```ini
   npc_actor = 1
   npc_debug = 1
   ```

   - `npc_actor = 1` включает фасад `actor_sync`; в E0 это **только probe** и
     заглушки (сетевых сообщений/спавна нет), поэтому probe-прогон остаётся
     read-only.
   - `npc_debug = 1` включает сам probe (чтение мира + лог `probe:`).
   - **Важно — расхождение с постановкой E-1:** в текущей реализации task-34
     probe гейтится `npc_actor=1` (`src/actor_sync.cpp:289-302`), поэтому
     `npc_actor=0` даёт **ноль** `probe:`-строк. Постановка Lead предполагала
     `npc_debug=1, npc_actor=0` (probe без actor-пайплайна). До мержа
     `client-review` прогон делается с `npc_actor=1` (E0-заглушка поведение не
     меняет); после мержа проверить, развязан ли probe от `npc_actor`, и
     зафиксировать окончательный вариант в этом разделе. Решение — за Lead.
   - `npc_debug = 0` (default) или `npc_actor = 0` — **ноль** строк `probe:`
     и ноль новых нативов.
   - `world_sync = 0` (и прочие experimental-флаги = 0) — чтобы лог был чистым.
   - Текущий probe фильтрует **только людей** (`!IsHuman`), исключает мёртвых,
     педов-игроков и локальную лошадь: животные/кони в `probe:` не появятся
     (см. H4/§5); это ограничение task-34, а не контракта.
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
| `metaOutfit` | `0x%X` | `_GET_PED_META_OUTFIT_HASH` `0x30569F348D126A5A`(1) [nh] | текущий мета-наряд | **главная гипотеза H1**: у NPC ≠ 0; у педов игрока = 0 (проверено в репо) |
| `equipped` | `0 / 1` | `_IS_META_PED_OUTFIT_EQUIPPED(ped, metaOutfit)` `0x98082246107A6ACF`(2) [nd] | «наряд считается надетым» | `1` при `metaOutfit != 0` (H2); если всегда 0 — проверить арность/семантику |
| `mount` | `0x…` (0 = пешком) | `GET_MOUNT` `0xE7E11B8DCBED1058`(1) [nh]; канон — **model hash лошади** `GET_ENTITY_MODEL(GET_MOUNT(ped))` | на чём едет | ≠0 только у всадников; в пробе у дороги/ранчо должно быть заметно |
| `weapon` | `0x…` (0 = без оружия) | `GET_CURRENT_PED_WEAPON` `0x3A87E44BB9A01D54`(5) [nh] (out-hash) или `_GET_PED_CURRENT_HELD_WEAPON` `0x8425C5F057012DAB`(1) [nh] | оружие в руках/выбранное | ≠0 у бандитов/закона/охотников; у мирян чаще 0 |
| `scenarioAny` | `0 / 1` | `IS_PED_USING_ANY_SCENARIO` `0x57AB4A3080F85143`(1) [nh] | пед вообще в сценарии | 1 у сидящих/курящих/работающих; 0 у идущих/стоящих/в бою |
| `dynScenario` | `0x%X` | `_GET_ACTIVE_DYNAMIC_SCENARIO` `0x569F1E1237508DEB`(1) [nh] | динамический сценарий | комментарий nativedb: **только kneel/sit/squat/sleep**; у остальных сценариев может быть 0 (H3) |
| `pointType` | `0x%X` | `_GET_SCENARIO_POINT_TYPE_PED_IS_USING` `0x2D0571BB55879DA2`(1) [nd] | тип scenario point, который использует пед | ≠0 у педов на сценарных точках (стоячие сценарии, работа, охрана) — второй канал H3 |
| `pedType` | `N` | `GET_PED_TYPE` `0xFF059E1E4C01E63C`(1) [nd] | ePedType (значения уточняются) | распределение в логе; human=1 должен коррелировать с людьми |
| `human` | `0 / 1` | `IS_PED_HUMAN` `0xB980061DA992779D`(1) [nd] | человек/животное | 1 — люди, 0 — животные/кони (H4, H6) |
| `mission` | `0 / 1` | `IS_ENTITY_A_MISSION_ENTITY` `0x138190F64DB4BBD1`(1) [nh] | mission-entity | 0 у ambient; 1 у миссионных акторов (их в E1 не реплицируем) |

Пометки: `[nh]` — хэш уже есть в `src/natives.h` и проверен в проекте;
`[nd]` — из `third_party/rdr2-runtime-scripthook/.../NativeDecl.cs` /
`research/rdr3-nativedb-data/natives.json`, перед реализацией сверить по
правилу проекта (DEV-NOTES: Disquse/rdr3noclip, не alloc8or).

**Служебные нативы сканера (не в строке):** `IS_PED_A_PLAYER`
`0x12534C348C6CB68B`(1) [nd], `IS_PED_ON_MOUNT` `0x460BC76A0E10655E`(1) [nh],
`IS_PED_DEAD_OR_DYING` `0x3317DEDB88C95038`(2) [nh], `GET_ENTITY_COORDS`
`0xA86D5F069399F44D`(3) [nh], отсев локального игрока/ghost/лошади, радиус 60 м,
cap 20 педов, частота ~120 тиков (2 с).

**Троттлинг:** строка пишется раз в ~2 с на **уникальную комбинацию
model+metaOutfit** (иначе throttle). Клиент дополнительно пишет **summary**
строку на каждое окно скана:

```
probe: scan humans=H logged=L newCombos=C unique=U
```

`humans` — людей в радиусе 60 м, `logged` — сколько попало в top-20,
`newCombos` — новых комбинаций в этом окне, `unique` — всего уникальных
комбинаций за сессию. Анализатор парсит summary отдельно (это не запись педа)
и выводит таблицу `Probe scan windows`. Поэтому:
- `records` ≠ числу NPC в мире; «unique signatures» в анализаторе — оценка
  разнообразия (стабильного `ped id` в контракте нет);
- распределения смещены к первым встреченным комбинациям; для процентов
  двигайтесь по городу/лагерю, а не стойте на месте;
- если `humans=0` во всех окнах — probe стоит в безлюдном месте;
  если `humans>0`, а записей мало — сработал throttle (комбинации уже видели).

**Открытые вопросы к реализации (сверить с `client-review`):**
1. `equipped` — именно `_IS_META_PED_OUTFIT_EQUIPPED(ped, metaOutfit)`, или
   проверка другого кандидата? Контракт — первый вариант.
2. `mount` — handle из `GET_MOUNT` или model hash? Канон для E1 — **model
   hash**; если сейчас handle, `client-review` меняет лог (иначе E1 не сможет
   спавнить лошадь по модели).
3. `human`-фильтр: task-34 говорит «20 ближайших **human** NPC», и текущая
   реализация действительно фильтрует людей (`!IsHuman`), т.е. в логе
   `human=1` у всех записей, а животные не появляются. Для E-1 это значит
   **H4 = WARN (animal-доля неизмерима)**; чтобы получить долю human/animal,
   нужен отдельный all-peds режим probe (согласовать с Lead/client-review).

---

## 3. Что искать в логе (ручная проверка до анализатора)

```powershell
# сколько probe-строк и как они выглядят
Select-String -Path "E:\Games\Red Dead Redemption 2\scripts\rdr2-coop.log" -Pattern 'probe:' | Select-Object -First 20
(Select-String -Path "...\rdr2-coop.log" -Pattern 'probe:').Count
```

Ожидаемо:
- при `npc_actor=1, npc_debug=1` на **хосте** — есть строки `probe:` с 11
  полями и summary-строки `probe: scan humans=… logged=… newCombos=… unique=…`
  (раз в окно ~2 с);
- при `npc_actor=0` **или** `npc_debug=0` — **ноль** `probe:`; при
  `npc_actor=0` также ноль `actor:`-строк (probe/actor не активны);
- на **госте** при `npc_debug=1` — ноль `probe:` (probe host-only); иначе
  сообщить Lead (нарушение контракта/реализации);
- в строках нет `NaN`, отрицательных «хэшей», пустых `model=`/`human=`;
- `probe:` не чаще ~1 строки на 2 с на комбинацию; нет потока тысяч строк/с
  (throttle работает, не спамит Tick-бюджет).

---

## 4. Какие прогоны нужны (3 файла лога)

| Прогон | Где | Что проверяем | Длительность |
|---|---|---|---|
| `probe_town.log` | Saint-Denis / Valentine / Rhodes, людная улица | H1 (outfit hash у `a_m_*`/`a_f_*`/`u_*`), H2, H4 human-доля, H6 | 2–3 мин |
| `probe_road.log` | дорога/ранчо/кемпинг, всадники, караваны | H5 mount/weapon, животные `a_c_*`, мета-аутфиты лошадей | 2–3 мин |
| `probe_scenario.log` | салун/скамейки/кровать/костёр/рабочие точки, лагерь | H3 dynScenario/pointType, позы (sit/smoke/kneel/sleep) | 2–3 мин |
| (опционально) `probe_mission.log` | рядом с миссией/лагерем банды | `mission=1`, `cs_*`/`mp_*`/`msp_*`, бой (`weapon`, `scenarioAny`) | 1–2 мин |

Рекомендации: хост стоит/медленно идёт, не бежит; время суток/погода не важны;
`npc_actor=1`, `npc_debug=1` (см. §1), `world_sync=0`; после каждого прогона —
отдельный файл лога (иначе смешаются локации и гипотезы).

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
всадники. Из текущего тестового лога (`scan:`-строки): ближайшие модели были
`0x0D7114C9 PLAYER_ZERO`, `0x28F9976A A_C_HORSE_DUTCHWARMBLOOD_CHOCOLATEROAN`,
`0x0348B323 A_C_HORSE_AMERICANSTANDARDBRED_PALOMINODAPPLE`,
`0x1E6ABEAD A_C_WOODPECKER_01`, `0x05C70C99 A_C_HORSE_MORGAN_PALOMINO`;
часть хэшей в `Peds.json` отсутствует — это нормально, анализатор помечает их
как `?`/unknown class.

Важно для H4/H6: в текущей реализации task-34 probe **human-only** (`!IsHuman`
в `src/actor_sync.cpp:225`), поэтому `human=1` у всех записей, животные в лог не
попадают и H4 будет `WARN` (animal-доля неизмерима). Для полноценного E-1
нужен all-peds режим probe (или отдельный прогон животных) — согласовать с
Lead/client-review; анализатор уже поддерживает оба варианта (`human=` +
hashdb-классы) и предупреждает (`H4 WARN`). H6 при этом работает: он проверяет
`human` против имени модели для людей (и животных, если они появятся).

---

## 6. Гипотезы и критерии (реализованы в `tests/npc_probe_analyze.py`)

| ID | Гипотеза | PASS | PARTIAL/WARN | FAIL/UNKNOWN | Что значит для E1 |
|---|---|---|---|---|---|
| **H1** | `metaOutfit != 0` читается у human NPC | ≥90% уникальных human-педов | 10–90% → `PARTIAL` | <10% → `FAIL`; нет human → `INSUFFICIENT` | PASS → E1-A (request/apply outfit); PARTIAL → E1-A + E1-B; FAIL → E1-B/E1-C |
| **H2** | `equipped=1` согласуется с `metaOutfit != 0` | ≥95% записей с хэшем | >0 но <95% → `WARN` | 0% → `FAIL`; нет поля → `UNKNOWN` | FAIL → не полагаться на `equipped`: проверить хэш/арность `_IS_META_PED_OUTFIT_EQUIPPED`, искать другой детект |
| **H3** | Сценарные позы детектируются (`dynScenario` или `pointType`) | ≥80% записей с `scenarioAny=1` | 30–80% → `PARTIAL` | <30% → `FAIL`; нет `scenarioAny` → `INSUFFICIENT` | PASS → E1-D по dyn/point-цепочке; PARTIAL → только детектируемое подмножество; FAIL → расширить probe/boolean-probe |
| **H4** | Доля human vs animal наблюдаема | после all-peds режима: `human+animal` ≥95% записей **и** есть animal | animal=0 → `WARN`; в текущем human-only probe это **ожидаемый** статус | нет `human=` → `INSUFFICIENT` | WARN → согласовать all-peds probe до E1/E2 (животные); H4 не блокирует H1/H3 |
| **H5** | mount/weapon заполнены | — (INFO) | — | — | низкий mount → отдельный road/camp probe; weapon → приоритет combat-репликации |
| **H6** | `human` согласуется с именем модели из hashdb | ≤5% несовпадений | ≤20% → `WARN` | >20% → `FAIL`; нет hashdb → `UNKNOWN` | FAIL → баг контракта/реализации (перепутаны human/pedType/поле), чинить до E1 |

Дополнительно анализатор печатает: распределение `pedType`, топ моделей,
топ `metaOutfit`, топ `dynScenario`, топ `pointType`, топ mount/weapon,
классы моделей и per-class проценты `outfit!=0`/mount/weapon/scenarioAny.
Gating для `--strict`: только **FAIL** у H1/H2/H3/H6; `PARTIAL/WARN/UNKNOWN/
INSUFFICIENT` не валят gate (это исследовательские статусы, Lead решает по
отчёту).

---

## 7. Как читать вывод анализатора

1. `log lines / parsed / invalid / scan summaries` — если `invalid > 0`, часть
   строк битая: сравнить с контрактом (§2), сообщить `client-review` точный
   пример. `scan summaries` — число summary-строк `probe: scan …`.
2. `Probe scan windows` — плотность мира: `humans_max/avg` в радиусе 60 м,
   `logged_max/avg`, `newCombos`/`unique`; если `humans=0` — локация пустая
   (не ошибка контракта).
3. `Pool` — доля human/animal. В текущем probe human-only `animal=0` —
   ожидаемо (H4 WARN); если появится all-peds режим — проверить animal-долю.
4. `Outfits` — **H1**: доля `metaOutfit != 0` по всем/human/animal/уникальным.
5. `Equipped` — **H2**: `equipped=1` среди записей с хэшем; `outfit==0` с
   `equipped=1` — шум/баг семантики.
6. `Scenarios` — **H3**: `dynScenario != 0`, `pointType != 0` и их объединение
   среди `scenarioAny=1`; отдельно «both zero» — сколько поз E1 не увидит.
7. `Mount/weapon/mission` — H5 + доля mission-entity.
8. `pedType` — накопить реальные значения; сверить с `human` (H6) и позже
   заменить/дополнить маппингом ePedType, когда он подтвердится.
9. `Top models/outfits/scenarios` — рабочий список для E1: какие модели
   массовые, какие outfit-хэши повторяются, какие scenario-хэши реально
   приходят (их и реплеить).
10. `Verdicts` + `E1 plan hint` — итог для Lead; в отчёт копируются строки
    `[STATUS] H…` и `E1 plan hint`.

Пример (синтетический, для проверки формата):

```
[PASS] H1: meta outfit hash readable for human NPCs
      2/2 unique human peds have metaOutfit != 0
[PASS] H3: scenario detection coverage
      2/2 scenario peds detected (dyn=50%, point=50%); preferred chain: dynamic-scenario chain
```

---

## 8. Приёмка E-1 (чек-лист)

- [ ] `npc_debug=0` (default) на хосте: в логе **0** строк `probe:`; поведение
      и производительность не отличаются от текущей сборки.
- [ ] `npc_actor=0` на хосте (при любом `npc_debug`): **0** строк `probe:` и
      **0** строк `actor:` — каркас E0 неактивен (текущая реализация task-34
      гейтит probe через `npc_actor=1`, см. §1).
- [ ] `npc_actor=1, npc_debug=1` на хосте: есть `probe:`-строки с 11 полями,
      регулярно в людной локации (>= 1 строка/2 с на новые комбинации), плюс
      summary `probe: scan humans=… logged=… newCombos=… unique=…`;
      нет потока/спама, нет крашей.
- [ ] `npc_actor=1, npc_debug=1` на госте: **0** `probe:`-строк (probe
      host-only); если нет — зафиксировать и решить с Lead.
- [ ] Три прогона (§4) сохранены отдельными логами; анализатор на каждом
      завершается exit 0 и печатает verdicts.
- [ ] `python tests\test_npc_probe_analyze.py` — `ALL N TESTS PASSED`.
- [ ] H1/H2/H3/H4/H6 имеют статус (PASS/PARTIAL/FAIL/WARN/INSUFFICIENT), H5
      INFO; отчёт Lead с топ-моделями/outfit/scenario и `E1 plan hint`.
- [ ] Проверено отсутствие `actor:`-строк при `npc_actor=0` (каркас E0 не
      активен) и отсутствие утечек/новых нативов при `npc_debug=0`.
- [ ] Зафиксировано фактическое поведение probe: human-only + гейт
      `npc_actor=1`; H4 = `WARN` (animal-доля не измеряется) — решение об
      all-peds режиме/escalation к Lead записано в отчёте.

---

## 9. План E1 по результатам probe

Матрица «результат H1 → путь внешности»:

| Результат | Путь E1 | Работы | Критерий готовности |
|---|---|---|---|
| H1 PASS (≥90%, hash!=0) | **E1-A: meta outfit** | На спавне актора: `_REQUEST_META_PED_OUTFIT(model, metaOutfit)` → ждать `_HAS_META_PED_OUTFIT_LOADED` → `_APPLY_PED_META_PED_OUTFIT(req, ped, true, true)` → `_RELEASE_...`; применять до `SET_ENTITY_VISIBLE`; hash уже в `ActorSpawn` (v15, поле `metaOutfit`, 0x18) | side-by-side 20 NPC на хосте/госте: outfit hash совпал, нет «дефолтного» вида, apply latency ≤ 3 кадра; при APPLY=false — fallback E1-B |
| H1 PARTIAL (10–90%) | **E1-A + E1-B** | E1-A для классов/моделей с hash!=0; E1-B — перебор outfit-хэшей для остальных | те же критерии + для каждого класса свой путь; логировать `outfit fallback model=… candidates=… match=…` |
| H1 FAIL (<10%) / hash всегда 0 | **E1-B → E1-C** | E1-B: на хосте для модели перебрать outfit-хэши из `rdr3-discoveries/clothes/metaped_outfits.lua` (890 моделей) через `_IS_META_PED_OUTFIT_EQUIPPED(ped, cand)`; нужен model→candidates список (генерируемый C++ header из Lua, не руками); кэш по модели, cap кандидатов (8–16). Если матча нет — E1-C | E1-B: найден кандидат для ≥90% проверенных моделей, cost ≤ N нативов/пед (bounded, не каждый тик). E1-C: компонентный захват `GET_NUM_COMPONENTS_IN_PED` + `GET_META_PED_ASSET_GUIDS/TINT` → `_SET_META_PED_TAG`; нужен протокол (ActorOutfit 0x1C — это shop-набор 784B, для asset GUID/тинтов нужен новый/расширенный формат — согласовать с net-review) |
| H2 FAIL/UNKNOWN | не блокирует внешность | E1-A/B не используют `equipped`; проверить арность/порядок `_IS_META_PED_OUTFIT_EQUIPPED`, при необходимости заменить детект на component-compare | документировать, что `equipped` не используется до выяснения |

Матрица «H3 → путь сценариев»:

| Результат | Путь E1-D | Работы | Критерий |
|---|---|---|---|
| H3 PASS, dyn>point | dynamic scenario | в фрейм/ивент актора: `scenarioHash = dynScenario`; гость `TASK_START_SCENARIO_IN_PLACE_HASH(ped, hash, -1, 1, 0, heading, 0)` + `TASK_STAND_STILL` при выходе | позы sit/smoke/kneel/sleep совпадают на 80–90% NPC; props могут отличаться |
| H3 PASS, point>dyn | scenario point | хост шлёт pointType + coords/heading; гость `CREATE_SCENARIO_POINT_HASH(pointType, x,y,z,heading,…)` + `TASK_USE_SCENARIO_POINT`, удалять point при выходе | совпадение позы и пропа (где есть) лучше dyn-пути; cap на количество point-ов |
| H3 PARTIAL | только детектируемое подмножество | как выше для детектируемых; остальные — idle и лог `scenario-undetected` | не хуже текущего (стоят/идут), без «танцев»; список непокрытых поз для расширения probe |
| H3 FAIL/INSUFFICIENT | расширить probe до E1-D | добавить в probe `PED_HAS_USE_SCENARIO_TASK`, `_GET_SCENARIO_POINT_PED_IS_USING`, `_IS_PED_IN_SCENARIO_BASE`, пробовать boolean-перебор известных scenario-хэшей (ограниченно) | после probe повторно решить; до этого сценарии в E1 не включать |

Прочие решения:
- **H4 WARN (фактически human-only probe):** в task-34 probe фильтрует
  `!IsHuman`, поэтому H4 гарантированно `WARN`; до E1/E2 добавить
  `npc_debug_all=1` (или снять фильтр), иначе животные/кони в E1 не покрыты.
  H4 не блокирует E1-A (люди) — это отдельный скоуп.
- **H5**: mount/weapon — приоритет слоёв E1: если mount высокий — добавлять
  pair rider↔mount (ActorSpawn уже содержит `mountActorId`), если weapon
  высокий — combat-репликацию (ActorEvent/ActorFrame flags) раньше сценариев.
- **H2 FAIL:** не использовать `equipped` в E1; детект смены наряда — по
  `metaOutfit` (если он читается) или component-compare.
- **Mission-entity:** в E1 по умолчанию не реплицировать (`mission=1` —
  исключение из discovery), иначе двойники со своей копией миссии гостя;
  probe нужен, чтобы понять долю mission-акторов в сценах.
- **H6 FAIL:** сначала починить контракт (human/pedType/поле), потом E1.

---

## 10. Ограничения и риски

1. **Троттлинг/уникальность:** нет `ped id` в контракте → «unique» —
   приближение; если нужен трекинг одной особи во времени, предложить
   добавить `ped=0x…`/`netId` в контракт отдельной задачей.
2. **`equipped` семантика (подтверждена кодом):** `src/actor_sync.cpp`
   вызывает `_IS_META_PED_OUTFIT_EQUIPPED(handle, metaOutfit)` — контракт
   соблюдён, H2 валиден. При переписывании probe в E1 сохранить именно эту
   пару аргументов (ped, текущий hash), не проверять «0»/другой кандидат.
3. **`mount` — model hash (подтверждено):** `GetMountModel()` в
   `src/actor_sync.cpp` пишет `GET_ENTITY_MODEL(GET_MOUNT(ped))`; для E1 этого
   достаточно. Если понадобится handle — добавить отдельное поле, не менять
   существующее.
4. **Probe human-only (факт):** `src/actor_sync.cpp:225` фильтрует `!IsHuman`,
   а также исключает мёртвых, player-педов и local mount → H4 = `WARN`,
   животные/кони не покрыты. Нужен all-peds режим (новая задача
   `client-review`) или отдельный animal-probe; H4 не блокирует E1-A (люди).
5. **`_GET_ACTIVE_DYNAMIC_SCENARIO` ограничен** (nativedb: только
   kneel/sit/squat/sleep) — H3 это и проверяет; не ожидать 100% по стоячим
   сценариям.
6. **`dynScenario`/`pointType` — 32-bit hashes**, коллизии/мусор возможны от
   невалидных аргументов; проверять `0` vs `!=0` и топ-список, а не только
   проценты.
7. **hashdb неполон** (120 безымянных моделей + часть ambient-моделей
   отсутствует) → класс `?`/unknown; это не ошибка probe.
8. **Лог большой** (текущий `rdr2-coop.log` ~141 МБ); анализатор стримит
   построчно, но для пересылки лучше вырезать `probe:`-строки:
   `Select-String -Pattern 'probe:' > probe_lines.txt`.
9. **Probe не должен оставаться включённым** в обычных сессиях: `npc_debug=0`
   default; при `npc_actor=1` actor-пайплайн не должен зависеть от probe-логов.
10. **Версии/хэши:** `[nd]`-нативы перед кодом сверить по NativeDecl/nativedb
    и правилу проекта; probe-нативы в `src/natives.h` добавляет `client-review`,
    не дублировать вручную.
11. **Гейт probe через `npc_actor=1` (факт/расхождение с постановкой):** в
    `src/actor_sync.cpp` probe выполняется только при `npc_actor=1`
    (`Tick` → `if (!enabled) return`, строки 289–302); постановка Lead —
    `npc_debug=1, npc_actor=0`. До мержа использовать фактический гейт, после
    мержа — зафиксировать окончательный вариант и обновить §1.

---

## 11. Источники

Локальные:
- `docs/TESTING.md`, `docs/PROTOCOL.md`, `ROADMAP.md`
- `src/natives.h` (проверенные хэши), `src/world_sync.cpp`, `src/coop.cpp`
- `third_party/rdr2-runtime-scripthook/source/scripting_v3/RDR2.Native/NativeDecl.cs`
- `research/rdr3-nativedb-data/natives.json`
- `third_party/rdr2-hashdb/Peds.json` (1982 модели; имена/классы)
- `third_party/rdr3-discoveries/clothes/metaped_outfits.lua` (890 моделей,
  outfit-хэши), `peds/peds_list.lua`, `animations/scenarios/README.md`,
  `AI/CPED_CONFIG_FLAGS/README.md`, `AI/RELATIONSHIP/README.md`
- `tests/npc_probe_analyze.py` (анализатор; парсит и `probe:`-строки, и
  summary `probe: scan …`), `tests/test_npc_probe_analyze.py`
  (22 unit-теста, `ALL 22 TESTS PASSED`)

Web (внешний референс, не инструкции):
- [alloc8or RDR3 nativedb](https://alloc8or.re/rdr3/nativedb/)
- [t3chman: Ped/MetaPed overview (outfits/assets/expressions/shop items/overlays)](https://forum.cfx.re/t/ped-modification-and-metadata-overview-outfits-assets-expressions-shop-items-overlays-etc/5124046)
- [Cfx: ambient ped outfits sync / RedM builds 1436/1491](https://forum.cfx.re/t/broken-human-peds-for-specific-models-and-other-ambient-ped-outfits-not-syncing/5111409)
- [Cfx: MP meta ped outfit / fullOutfit / expressions](https://forum.cfx.re/t/discussion-on-female-mp-meta-ped-outfit-data/1803615)
- [Halen84 RDR3 flags/enums](https://github.com/Halen84/RDR3-Native-Flags-And-Enums)
- [femga/rdr3_discoveries](https://github.com/femga/rdr3_discoveries)

---

## 12. Быстрый чек-лист «probe-прогон за 10 минут»

1. `npc_actor = 1`, `npc_debug = 1` (см. §1), `world_sync = 0` на хосте;
   запустить игру.
2. Saint-Denis/Valentine, 2–3 минуты неспешной ходьбы.
3. Закрыть игру, скопировать лог в `probe_town.log`.
4. `python tests\npc_probe_analyze.py probe_town.log` → скопировать Lead:
   `records/unique`, H1/H2/H3/H4/H6 статусы, топ-модели/outfit/scenario,
   `E1 plan hint`.
5. Повторить для `probe_road.log` (лошади/животные) и
   `probe_scenario.log` (салун/скамейки/кровать); помнить, что текущий probe
   human-only и animals появятся только после all-peds режима.
6. Проверить `npc_actor=0` или `npc_debug=0` → 0 строк `probe:`; запустить
   `python tests\test_npc_probe_analyze.py` → `ALL 22 TESTS PASSED`.
