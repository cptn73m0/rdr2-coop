# Дорожная карта — RDR2 CO-OP

Кооператив для одиночного сюжета RDR2. Дорожная карта показывает этапы,
порядок работ и текущий прогресс. Отмечаем галочками по мере выполнения.

**Актуально на:** 23.10.2026
**Целевой билд:** RDR2 1.0.1491.50, ScriptHookRDR2 1.0.1491.17
**Текущий протокол:** **v16** (actor-репликация NPC E1–E3b + E2c-b,
0x18–0x1E). v15↔v16 несовместимы; откат v15-пары — `dist\npc-e2c-v15\`.
Рабочая база/игровой стабильный тег — **stable-v14** (пед-призрак, лошадь,
одежда shop+wearable, выстрелы, время/погода). NPC-репликация реализована и
проверена на **одном ПК**: E1 human-only, E2a meta-outfit, E2b всадники,
E3a сценарии/смерть, E3b оружие/бой, E2c-b asset/tint — по данным реального
хоста (capture 8-10 valid, fake guest errors=0). Теги этапов: `npc-e0`,
`npc-e1`, `npc-e2`, `npc-e2b`, `npc-e3`, `npc-e3b`, `npc-e2c-b`.
`npc_actor=0` по умолчанию; ARINA v16 задеплоена (ASI `63588AD3`, server
`4EDAE4B6`), но двух-ПК визуальная приёмка ещё **pending**.

---

## Фаза 0 — Каркас и окружение — готово

- [x] Структура проекта `E:\rdr2-coop` (CMake, C++ ASI-плагин)
- [x] Резолв экспортов `ScriptHookRDR2.dll` без проприетарного SDK
- [x] Сборка `.asi`, совместимость с билдом 1.0.1491.50
- [x] Развёртывание в папку игры + базовый лог
- [x] Резолв `worldGetAll*`, `getGlobalPtr`, `getStaticPtr` (Spike 1)

## Фаза 1 — Локальный «призрак» — готово

- [x] Создание второй сущности в мире и чтение/запись трансформа
- [x] Рендер педа (mission + variation + visible)
- [x] Метка на карте/мини-карте, имя над головой (500 м)
- [x] Смерть/возрождение, блип/имя скрываются

## Фаза 2 — Персистентность и сессия — готово

- [x] Последняя позиция игрока на сервере (`players.txt`)
- [x] Спавн повторного игрока на сохранённой позиции
- [x] Обработка дисконнекта: удаление педа и метки
- [x] Явные роли HOST/GUEST, лимит 2 игрока, `JoinRejected`
- [x] Версия протокола в JOIN (v16), отказ при несовпадении; v15-откат в
      `dist\npc-e2c-v15\`

## Фаза 3 — Сеть — готово (база)

- [x] Протокол команд, бинарные кадры, little-endian
- [x] Кадр состояния 88 Б, отправка каждый тик (~60 Гц)
- [x] TCP_NODELAY, неблокирующие сокеты, own send queue
- [x] Ping/Pong, RTT/джиттер, HUD
- [x] WorldState (время/погода от хоста)
- [ ] UDP-канал для высокочастотных снапшотов (отложено)

## Фаза 4 — Игрок: движение и внешность — готово

- [x] Move/State/Action/aim/weapon в кадре
- [x] Движение задачей по навмешу, интерполяция, спецсостояния
- [x] Ragdoll/укрытие/плавание/лезть/прыжок/падение (грубо)
- [x] Действия: сценарий/сидение/пьян (грубо), прицел/перезарядка/выстрел
- [x] Оружие: модель, доставание, прицел, выстрелы, перезарядка
- [x] События выстрелов: аммо-детект + очередь 64 + независимый таймер
- [x] Лошадь: модель, посадка/спешивание, движение, коллизии, снаряжение
- [x] одежда: shop-компоненты, 96 категорий, протокол v14
- [x] Wearable states одежды (открытая/закрытая куртка, рукава и т.п.):
      `OutfitState` 784 B (+`wearable[96]`), протокол v14 — реализовано
- [ ] Игровая проверка wearable-синхронизации на двух ПК
- [x] Покой без «танцев»: гистерезис 1.5/2.5 м, порог скорости 0.35 м/с
- [x] Труп и снаряжение остаются, `RESURRECT_PED`
- [ ] Верховая стрельба/сложные mounted-анимации

## Фаза 5 — P1: репликация мира/NPC

### NPC-репликация — E1–E3b + E2c-b реализованы; 2-ПК приёмка pending
- [x] E-1: probe-режим `npc_debug=1` + анализатор probe-логов (22% NPC имеют
      metaOutfit hash; сценарии/модели/лошади/оружие)
- [x] E0: протокол actor-сообщения 0x18–0x1E, `kMaxActors=12`,
      static_assert размеров
- [x] E0: серверный host-only relay + валидация (exact size, count≤12,
      epoch/actorId!=0, finite coords, duplicate actorId), resync guest→host
- [x] E0: integration-тесты v16 (host-only, размеры/лимиты, epoch/actorId,
      NaN/Inf, resync-направление, gating `actor:`=0, appearance 16comp/460B);
      **29/29** на build-server-ci
- [x] E1: human-only discovery 10 Гц вокруг обоих игроков, cap 12, roster
      actorId/epoch, spawn heartbeat 2.5 с, frames 10 Гц, despawn
      gone/dead/evicted, resync 0x1D; guest-прокси spawn≤2/тик + model load,
      trail-навигация/in-place task update, gait с хоста — **подтверждено в игре**
- [x] E2a: meta-outfit внешность NPC (probe hash; 5/5 applied, failed=0),
      heartbeat-пересылка и guest reapply (`npc_appearance=1`)
- [x] E2b: всадники rider+horse (`ActorSpawn.reserved` = mount model), seat
      watchdog, dismount/despawn cleanup; подтверждено mount spawn ×9, failed=0;
      плавное движение gait_restart=0, err_p95≈0
- [x] E3a: сценарии (optimistic confirm, анти-наложение skip 8 м + warp revert
      3 м — визуально «двое курят на разных местах»), death/revive
      (SET_ENTITY_HEALTH 0 / RESURRECT_PED, труп остаётся)
- [x] E3b: оружие (валидные hash через GIVE_WEAPON_TO_PED), боевая стойка,
      aim, серии выстрелов (TASK_SHOOT_AT_COORD + переприцеливание),
      reload с отключением infinite clip; state=shoot=1450, reload=385,
      weapon fail=0
- [x] E2c-a: host outfit brute-force resolver (`metaped_outfits.lua` →
      `gen_metaped_outfits.h`, 890 моделей / 4568 кандидатов,
      `_IS_META_PED_OUTFIT_EQUIPPED`, бюджет 24 натива/тик, heartbeat) —
      механически работает; для random ambient NPC resolved=0/failed=11
- [x] **E2c-b:** protocol v16 **ActorAppearance (0x1E)**: header 12B + entry
      28B (count 1..16, max 460B), host-only relay, точный размер; host
      capture (`_GET_NUM_COMPONENTS_IN_PED`/`GET_META_PED_ASSET_GUIDS/TINT`,
      fingerprint, budget 1 актор/тик период 2с) + guest apply
      (reset+`_SET_META_PED_TAG`+commit+`UPDATE_PED_VARIATION`, readback,
      fallback; asset > metaOutfit). Валидация: host `capture count=8-10
      valid=8-10`, fake guest `appearance_msgs=504 appearance_actors=8
      entry_max=10 errors=0`; визуал — 2-ПК pending
- [ ] E3c backlog: животные/лошадиные трупы, транспорт/NPC-водители,
      mission-акторы (вне текущего human-only E1–E3b)
- [ ] E4 backlog: **двух-ПК визуальная приёмка E2c-b** (ARINA v16 задеплоена,
      прогон не начат), host migration/resync под нагрузкой, Tick p95/FPS
      бюджеты и игровой чек-лист «неотличимости»; `npc_actor` не включать в
      рабочую сборку до неё
- [ ] Известные ограничения: лицо/волосы/борода, точные анимации/катсцены,
      ambient-мир гостя не подавляется (возможны «двойники»)

- [x] `WorldSnapshot` (0x10), хост-авторитет
- [x] Сканер NPC 60 м, до 64 сущностей (32 Ped + 16 Vehicle + 16 Object),
      10 Гц, приоритет mission
- [x] Гость: прокси-педы (model request/mission/variation/visible, без коллизии)
- [x] Подавление ambient-популяции гостя density-множителями
- [x] Таймаут снапшотов 3 с → удаление прокси
- [ ] **Тест на двух ПК** (`world_sync = 1` на обоих) — ждём прогона
- [x] Транспорт в снапшоте (Vehicle до 16: дилижансы/повозки/лодки/трамваи;
      mission-прокси без коллизии; поезда пока пропускаются)
- [x] Объекты/предметы в снапшоте: mission Object до 16 (амбиентные пропсы
      не дублируются — они уже есть в мире гостя)
- [ ] Поезда (состав), произвольные/динамические объекты, миссионные акторы
- [x] Двусторонний маппинг целей handle↔netId (carry/loot host→guest и guest→host)
- [ ] Очистка старых ambient-педов у гостя при входе в сессию
- [x] Интерполяция/задачи анимации для прокси-NPC (walk/run/idle,
      navmesh-задачи, heading; без точных клипов сценариев/боя)
- [ ] Реконсиляция и tombstones при массовых изменениях мира

## Фаза 6 — P2: Action v2 — действия и анимации

**Цель:** синхронизировать не «все клипы» (публичный SDK их не отдаёт), а
наблюдаемые действия через state/task/scenario-replication. Полный
клип-в-клип AnimGraph невозможен без версионного реверса/хуков — см. ниже.

- [x] Базовые Action: None/Shooting/Reloading/Sitting/InScenario/Aiming/Drunk
- [x] Синхронизация перезарядки и выстрелов
- [x] **Action v5:** сообщение `ActionState` (mode, flags, scenarioHash)
- [ ] Сценарии: сидение/курение/еда/питьё через `_GET_ACTIVE_DYNAMIC_SCENARIO`
      + `TASK_START_SCENARIO_IN_PLACE_HASH`
- [x] Укрытие: флаги `IS_PED_IN_COVER` / `IS_PED_AIMING_FROM_COVER`,
      `TASK_ENTER_COVER`/`TASK_EXIT_COVER` (cover-point приблизительный)
- [x] Переноска тела: `IS_PED_CARRYING_SOMETHING` + target netId (host→guest)
      (guest→host потребует обратного world_sync)
- [ ] Таскание/хогтай: `_IS_PED_DRAGGING`, `IS_PED_HOGTIED` (флаги есть)
- [x] Лутание: `GET_LOOTING_PICKUP_TARGET_ENTITY` + `TASK_LOOT_ENTITY`
      (host→guest, через netId из P1)
- [ ] Рыбалка: `_GET_TASK_FISHING` (флаг есть, задача позже)
- [ ] Скиннинг/лагерные действия: сценарии/задачи по таблице
- [x] Таблица «детект → задача», fallback в Idle (без «танцев»)
- [x] Посадка в транспорт удалённого игрока (PlayerVehicle 0x12:
      прокси-транспорт + место; приблизительная анимация посадки)
- [ ] Диагностика неизвестных Actions в лог для расширения таблицы

**Ограничения (зафиксировано):**
- Публичный SDK не умеет получить текущий клип/фазу/бленды AnimGraph;
  `rdr2-coop-story` подтверждает это и оставляет `VersionedMemoryReader`
  как будущий версионный реверс.
- Нет API записи позы скелета → стримить «позу» нельзя, только задачи.
- AnimScene можно ловить/реплеить инлайн-хуками по RVA (как в coop-story),
  но это версионно, только хосту и не «все анимации».

## Фаза 7 — Миссии, прогресс, сейвы

- [x] Каталог 80 Story MissionData ID (MIT-атрибуция rdr2-coop-story)
- [x] Детект новых завершений хоста: baseline `WAS_COMPLETED` + rating
- [x] `MissionEvent` (v9): хост → гость, только хост; гость логирует
- [x] Зачёт в свой сейв гостя по флагу `mission_credit`
      (`_UILOG_MARK_MISSION_COMPLETED` + rating; без копирования сейва)
- [x] Определение текущей миссии: runtime-скрипт через
      `GET_NUMBER_OF_THREADS_RUNNING_THE_SCRIPT_WITH_THIS_HASH` + каталог
- [x] MissionState (v10): гость видит активную миссию хоста (лог/субтитр)
- [x] MissionBarrier v11: offer → ready → start-window → started/abort
      (протокол + mock-тесты сервера; in-game handshake не тестирован)
- [ ] Барьер совместного старта: удержание миссии до готовности гостя
      (сейчас окно открывается уже после старта хоста)
- [ ] Награды/allow-list 17 миссий, деньги/Honor по умолчанию не трогать
- [x] Journal + backup сейва гостя перед зачётом (`save_backup=1`, без restore)
- [ ] Restore/сравнение сейва и откат зачёта при ошибке
- [ ] Миссионные AnimScene/катсцены — capture/replay (R&D, версионно)
- [ ] Тест: одна миссия (FUD1/HNT1) на двух ПК, save/reload

## Фаза 8 — Надёжность, сервер, мультиплеер

- [x] Сервер: keepalive/timeouts, лимиты очередей, атомарный players.txt,
      защита от пустых/повторных JOIN, host-only сообщения
- [x] Headless-тесты протокола/сервера (20 integration tests, без игры)
- [x] Лимит одновременных соединений сервера (32) + flood-тест
- [x] Mission catalog/rewards validation test (80 миссий, 23 записи/17)
- [x] Idle timeout сервера (настраивается 2-м аргументом, по умолчанию 30 с)
- [x] Атомарная запись players.txt (tmp + MoveFileEx/rename)
- [x] scripts\restore_saves.ps1: восстановление сейва из бэкапа (dry-run/-Force)
- [x] Проверка и фикс: probe-подключения больше не затирают players.txt
- [x] Reconnect с backoff (1→10 с), очистка педа, повторный JOIN
- [x] Host migration: сервер повышает гостя, шлёт HostChanged (v12)
- [x] Headless-тест host migration: новый хост принимается для WorldSnapshot
- [x] Fuzz-тест: 300 случайных кадров/размеров + C++ FrameReader fuzz
- [x] C++ тест client reconnect с фейковым сервером
- [x] WorldSnapshot parser stress: 4 потока × 20k, строгий размер/count,
      вынесен в src/world_snapshot_parse.cpp
- [x] GitHub Actions badge + build artifacts (asi/server)
- [x] GitHub Actions workflow: сборка клиента/сервера + все тесты
- [x] Mission rewards dry-run: каталог 23 записей/17 миссий в логе, без нативов
- [x] Config-тесты: LoadFromFile, флаги/hex/битые строки/defaults
- [x] players.txt recovery-тест: битые строки и stale .tmp игнорируются
- [x] restore_saves.ps1 автотест (restore + pre-restore snapshot)
- [ ] TrainSnapshot v13 + attach вагонов к локомотиву на госте —
      R&D, в игре нестабильно, по умолчанию выключено (`world_trains=0`)
- [x] C++ stress candidate budget: 4×10k против reference-выборки
- [x] Mission barrier rules fuzz: 4×100k фаз/ролей/offer/epoch
- [x] Per-IP rate limit (12 новых TCP/сек) + flood-тест
- [x] Graceful shutdown: Ctrl+C/nonblocking accept + SaveAll; тест-хук
- [x] Метрики сервера в статусной таблице (active/peak/total/rate/cap/uniqueIPs)
- [x] GitHub Actions cache для build/server/tests + artifacts
- [ ] Применение наград allow-list после игрового теста
- [x] CI-скрипт `scripts/ci.ps1`: сборка клиента+сервера и тесты
- [x] Убрано предупреждение C4005 WIN32_LEAN_AND_MEAN
- [ ] Проверка reconnect/host migration на двух ПК (нужна игра)
- [ ] 3+ игроков: `map<sessionId, RemotePeer>`, player id в кадрах
- [ ] Anti-desync: метрики, resync по запросу, tombstones
- [ ] Логи: ротация, уровни, без спама outfit categories
- [ ] Документация: PROTOCOL/README под текущую версию, ROADMAP

## Идеи на перспективу

- Озвучка появления/ухода игрока
- Метка с настраиваемым цветом, настройка дистанции имени
- Чат/меню/список игроков (по мотивам RDR2Coop)
- Host-authoritative ambient encounters (по мотивам coop-story)
- WireGuard/туннель для интернета (без публичного сервера)

## Архитектурная заметка

- Модель A: два процесса, два мира; хост авторитетен, гость — ведомый.
  Видимость «одного мира» достигается репликацией сущностей и действий,
  а не единой симуляцией движка.
- Сюжетные миссии не синхронизируются «из коробки»: каждый проходит свою
  копию; совместный зачёт — через MissionData-нативы и барьер старта.
- Связь по интернету — через WireGuard/туннель; публичный сервер не
  предполагается.
- Юридически: Story Mode only, non-commercial, без бандла ScriptHook/dinput8
  и без изменения R*-скриптов/ассетов для публичной раздачи.
