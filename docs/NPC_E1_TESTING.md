# NPC actor replication (E1–E20.3) — in-game test plan and acceptance metrics

**Статус (27.10.2026):** E1–E20.3 реализованы; текущий протокол — **v21**
(`kMaxActors = 50`).
E1 human-only, E2a meta-outfit, E2b всадники, E2c-b asset/tint, E3a
сценарии/смерть, E3b оружие/бой, E3c животные/свободные лошади, E4 perf,
E5 урон/E5.1 хедшоты, E5.2/E5.3 трупы/mounted, E6 presence, E7.1 visitor,
E7.2 плавное движение, E7.3 E3d-a vehicles/host migration/mission-aware,
E8 — E3d-c occupants/draft (0x21) + E3d-b mission-train (0x22),
E9 — animal/horse appearance (0x1E assets + 0x1C tack),
E10 — mounted NPC-horse appearance через 0x1C (семантика rider actorId),
**E11 — точные пассажиры поездов через 0x21 carIndex**,
**E12 — полный `perf: frame`, `perf: net`, Player Frame throttle + P0/P1**,
**E13 — wall-clock интервалы (ms, FPS-independent), `tick_hz`, config warnings**,
**E14–E17 — ub-mode refactor-only (протокол/поведение не менялись)**,
**E18 — soft host migration (grace 15 с, presence-liveness, демоция без
закрытия сокета), клиентский loading presence heartbeat, vendor dedupe у
гостя, server HandleClient phases, integration run lock; провод v19 без
изменений**,
**E19 — имя игрока на карте/blip, occupant-first кучер/draft-лошади повозки,
opt-in wheel A/B `npc_vehicle_wheels=0`, SET_ENTITY_COORDS арность 8**,
**v20 P1 — guest influence 0x23/0x24/0x26 (default-off, блок I)**,
**E20.1/E20.2 — hotfix-диагностика roster/occupants/колёс**,
**E20.3/v21 — actor-cap 12→50 (S33): животные/лошади/кучер дилижанса больше
не вытесняются людьми из roster**.
CI **50/50** integration (protocol v21, `actors=on(50)`) + **8/8** C++ +
quality strict PASS. Задеплоенная пара — **npc-e20.2/v20** (ASI `26BB6FDD`,
server `0710D2F7`); **v21 (npc-e20.3) — сборка/деплой Lead pending** (кандидат:
ASI `D95A0F2F`/server `B225BE7C`). E2c-b/E1–E5.1 проверены на одном ПК +
реальный хост; двух-ПК приёмка pending. Финальный чек-лист 2-ПК:
[ACCEPTANCE_2PC.md](ACCEPTANCE_2PC.md) (блок A13/S33 + блок I).

E1 — human-only NPC; E2/E3 добавляют внешность, всадников, сценарии/смерть,
оружие/бой, животных/лошадей. **E5** — урон применяет только хост:
гость детектит free-aim target и шлёт `ActorDamage` 0x1F; **E5.1** —
хедшот `SKEL_Head`/impact ≤0.30 м → `damage=250` (ваншот). **E6** —
presence 0x20. **E7.1** — visitor mode; **E7.2** — плавное движение 60 Гц;
**E7.3** — транспорт E3d-a, mission-aware visitor и host migration hardening.
**E8** — E3d-c водители/пассажиры/draft (0x21) и E3d-b mission-train (0x22).
**E9** — assets животных/лошадей (0x1E) и horse tack (0x1C).
**E10** — внешность лошади под NPC-всадником через 0x1C.
**E11** — точные пассажиры поездов (0x21 carIndex).
**E12** — полный перф-кадр/трафик и Player Frame throttle.
**E13** — wall-clock нормализация (FPS-independent), `tick_hz`,
`config: warn` для опасных комбинаций. **E14–E17** — refactor-only.
**E18** — мягкая host-migration и клиентские фиксы (S31).
**E19** — blip name, кучер/draft повозки, wheel A/B (S32).
**E20.3/v21** — actor-cap 50: животные/лошади/кучер дилижанса видны у гостя,
всадник+конь парой (S33). Mission NPC не
входят; 0x1B зарезервирован; 0x1C используется E9/E10; resync 0x1D
реализован (≤1/с).

Автоматизированная часть — wire/relay/validation:
`tests/integration_server.py` (**50 сценариев**, protocol v21, appearance +
ActorDamage + PlayerPresence + VehicleOccupants + TrainSnapshotV2 + rate-limit
+ host migration/timeout incl. e18 soft/join-grace) + 8 C++ тестов;
клиентская логика проверяется в игре/fake-клиентами.

## 0. Флаги и предусловия

- Оба ПК: одна и та же сборка (protocol **v21**: `net/protocol.h`,
  `kProtocolVersion=21`, `kMaxActors=50`), один `rdr2_coop_server.exe` v21.
  **Текущая задеплоенная пара — npc-e20.2/v20** (ASI `26BB6FDD`, server
  `0710D2F7`, снапшот `dist\npc-e20.2`); **v21/npc-e20.3 — сборка/деплой Lead
  pending** (кандидат: ASI `D95A0F2F`, server `B225BE7C`; снапшот
  `dist\npc-e20.3`). Откат — e20.2 (v20) полной парой; глубже e19 —
  ASI `DD0EE210`/server `DD9BD9D8`; v18 — `C9BFA565`/`BEC3428B`;
  v17 — ASI `35163CC2`/server `B7AAABAE`; v16 — `63388AD3`/`4EDAE4B6`;
  v15 — `dist\npc-e2c-v15\`.
- INI на обоих ПК (кроме указанных):
  - `npc_actor = 1` — включить NPC-репликацию;
  - `npc_cap = 50` — **v21:** для S33 30–50 NPC (default 8; clamp 1..50;
    для базового S1 достаточно 12);
  - `npc_radius = 60` — default; despawn-гистерезис +10 м (70 м);
  - `npc_appearance = 1` — meta-outfit E2a + asset/tint E2c-b (default 1);
  - `npc_animal_cap = 4` — 0..12, животные/свободные лошади (E3c);
  - `npc_damage = 1` — рубильник E5 host-authoritative урона;
  - `npc_corpse_max_sec = 300` — E5.2, удержание host-трупа (0 = старое);
  - `npc_presence = 1` — E6, суффикс UI-статуса в имени;
  - `npc_visitor = 1`, `npc_visitor_vehicles = 1`,
    `npc_visitor_preserve_events = 1`, `npc_visitor_pause_scenes = 1` — E7.1/E7.3;
  - `npc_motion_update = 1`, `npc_motion_marker = 0`,
    `npc_motion_status_p2 = 1` — E7.2;
  - `world_vehicles = 1`, `npc_vehicle_radius = 80` — E3d-a транспорт;
  - `npc_occupants = 1` — E3d-c водители/пассажиры/draft;
  - `world_trains = 0` — E3d-b mission-train (тестовый opt-in; включать
    только для S26);
  - `npc_animal_appearance = 0` — E9; для S27 включить **1 на обоих ПК**;
  - `npc_mount_appearance = 0` — E10; для S28 включить **1 на обоих ПК**;
  - `player_frame_hz = 20`, `player_frame_change_only = 1` — P0 Player Frame;
  - `npc_debug = 1` — probe/perf/visitor/motion/world-логи (host и guest);
    после прогона выключить;
  - остальные экспериментальные флаги — **0** (`world_sync=0`, `action_sync=0`,
    `vehicle_sync=0`, `mission_sync=0`, `train_debug=0`, `mission_credit=0`).
- E13–E18 **не добавляют новых INI-ключей** (27 приёмочных, 37 строк);
  **e19 добавляет 28-й** `npc_vehicle_wheels = 0`; **v20 P1 добавляет 29–32-й**
  (`vehicle_interaction = 0` default-off, `vehicle_interaction_mission = 0`,
  `vehicle_interaction_host_vehicle = 0`, `vehicle_interaction_debug = 1`) →
  **32 приёмочных / 42 строки** = 32 + 6 служебных P2/P3 + 4 подключения.
  **v21 ключи не меняет** (только clamp `npc_cap` 1..50). Эталон —
  `dist\npc-e20.3\rdr2-coop.ini.local/.arina` (после деплоя v21); текущая
  задеплоенная e20.2 — те же 32/42. `npc_motion_status_p2` (default 1) в INI
  не выписан, но поддержан config.
- Хост — первый подключившийся; гость — второй. Оба игрока стоят/ходят рядом
  с одной и той же толпой NPC (discovery учитывает позиции обоих игроков).
- Бэкап INI/ASI и сейвов (см. `docs/TESTING.md` §7); лог-файл
  `scripts\rdr2-coop.log` на каждом ПК и `server_console.log` (рабочая папка
  сервера) копируются в папку прогона.

**Порядок проверки:** сначала baseline `npc_actor=0` (эталон FPS/поведения и
«ноль actor:»), затем `npc_actor=1`; на одном ПК допустим fake host/guest
(`tools/fake_actor_client.py`), полная двух-ПК приёмка — после деплоя на
ARINA. Результат — таблица pass/fail на сценарий.

## 1. Сбор артефактов

Папка на прогон: `rdr2-coop-npc-e1-YYYYMMDD-HHMM/`
```
pc-host/rdr2-coop.log, pc-guest/rdr2-coop.log, server/server_console.log,
both INI, screenshots/ (или OBS-видео 60 fps), summary.txt (таблица ниже)
```
Полезные проверки (PowerShell, выполнять на соответствующем ПК):
```powershell
# количество/типы actor-строк (E1 использует: spawn/despawn/snapshot/proxies/resync/reset)
Select-String -Path .\rdr2-coop.log -Pattern 'actor:' | Measure-Object
Select-String -Path .\rdr2-coop.log -Pattern 'actor: (spawn|despawn|snapshot|proxies|resync|reset)'
# gating: при npc_actor=0 должно быть 0 строк actor: и 0 строк probe:
(Select-String -Path .\rdr2-coop.log -Pattern 'actor:').Count
(Select-String -Path .\rdr2-coop.log -Pattern 'probe:').Count
# FPS-серия (world_scan пишет раз в секунду)
Select-String -Path .\rdr2-coop.log -Pattern 'scan: fps='
# сервер: actor-релея в логе нет; gating-проверка — 0 строк actor:
(Select-String -Path .\server_console.log -Pattern 'actor:').Count
```

## 2. Метрики и пороги приёмки

Все `actor:`-строки пишутся клиентом (`sh_log`, без timestamp — см. риски),
часть строк rate-limited (1/с), сводка `actor: proxies=...` — раз в 5 с
(300 тиков). Поэтому latency-пороги 300 мс/1 с измеряются по OBS-видео с
таймкодом/покадрово, а потоковые метрики — по счётчикам и логам.

| Метрика | Источник (лог-маркер) | Порог приёмки | Провал |
|---|---|---|---|
| **Spawn latency** (host NPC появился → guest proxy виден) | host `actor: spawn id=… n=…`, guest `actor: spawn id=…`; визуально/OBS | p95 ≤ **300 мс**, max ≤1 с, на ≥20 спавнах | p95 >300 мс или пропуск спавна |
| **Despawn latency** (host NPC ушёл/умер → proxy удалён) | host `actor: despawn id=… reason=…`; guest `actor: despawn`/визуально | p95 ≤ **1 с**, max ≤2 с, на ≥10 despawn | proxy висит >2 с или пропал «не тот» |
| **Proxies count** (город, `npc_cap=12`) | guest `actor: proxies=N`, host `npc_debug` → `probe: scan humans=…` | **8–12** стабильно, отклонение ±1 за 5 с | <8 при ≥12 рядом / >12 / скачки |
| **Snapshot rate/count/bytes** | guest `actor: snapshot epoch=E count=C bytes=B` (1/с, rate-limited) | C == числу proxies (±1), B == C*32 и ≤392, epoch неизменен в сессии | несоответствие count/bytes, epoch «прыгает» |
| **appliedFrames** | guest `actor: proxies=N appliedFrames=F timeouts=T` (раз в 5 с) | F ≥ 0.99 × (50×N) (≤1% потерь кадров); рост F стабилен | <99% ожидаемых кадров |
| **Timeout rate** | guest `actor: proxies=… timeouts=T`; `actor: resync needed id=…` | T ≤ **0.5%** ожидаемых кадров (≤0.25×N за 5 с), в steady T=0; resync ≤1 на актора за idle-стрик | T >0.5% или повторные resync |
| **Resync request/recovery** | guest `actor: resync needed id=…` + `actor: resync request epoch=…`; host повторный `actor: spawn … n=` (0x18) | в steady 0 запросов; при индуцированной потере кадров ≤1 запроса/с (троттлинг), roster восстановлен ≤2 с после запроса | запросы без восстановления/шторм >1/с |
| **Teleports** | визуально/OBS (отдельного маркера в E1 нет; код snaps при >12 м) | **0** в steady state; нет прыжков >2 м за кадр; snap допустим только при spawn/resync/epoch | любой «поп» NPC в кадре |
| **Spawn/despawn churn** | host `actor: spawn/despawn` за 10 с; guest `proxies` | ≤10 событий/10 с при ходьбе по городу; один и тот же actorId не переспавнивается чаще 1/30 с | циклы spawn↔despawn/«мигание» |
| **No duplicates** | guest `proxies` vs host `spawn n=`; визуально | нет двух proxy с одним `actorId`; при `npc_cap=12` proxies ≤12; после reset старые прокси исчезли | дубли моделей на одном месте/залипшие прокси |
| **Reconnect/leave cleanup** | `actor: reset (session=S)`, `actor: proxies=0`, затем 8–12 | после leave/disconnect/reconnect/host-migration: старые прокси удалены, session+1, новые прокси ≤cap, без дублей | старые NPC остались/двойные |
| **Gating `npc_actor=0`** | оба `rdr2-coop.log`: `actor:` = 0; `probe:` = 0; server: `actor:` = 0 | **ровно 0 строк** `actor:` на обоих ПК и сервере; FPS = baseline | хоть одна строка actor:/probe: |
| **FPS** | `scan: fps=` оба ПК (раз в 1 с) | на той же сцене/маршруте vs baseline: avg drop ≤ **5%**, 1% low drop ≤ **10%**; нет стабильных <30 FPS в городе | превышение drop/FPS-провал |
| **Tick budget** | инструментация E1 отсутствует (нет `perf:` лога); OBS frame timing / внешний профайлер (RTSS) | NPC-пайплайн (discovery+spawn/frames+hooks) p95 ≤ **2 мс** при 8–12 акторах; общий Tick ≤3 мс; нет фризов >100 мс | фризы/просадки выше порога |

Пояснения к измерениям:
- `actor: spawn` rate-limited 1/с, поэтому в 10-секундном окне видно не более
  ~10 событий; для подсчёта churn используйте пары host+guest логов и `proxies`.
- `appliedFrames` накапливается за 5 с (сбрасывается после сводки); ожидаемое
  число = 5 с × 10 Гц × N proxies. Для N=12 ожидание 600 кадров за 5 с.
- `timeouts` — resync-триггеры + удаление proxy по 10-секундному idle;
  в стабильной сессии должен быть 0; единичные значения допустимы только после
  сетевого сбоя/телепорта хоста.
- При `npc_cap=8` (default) ожидаемые proxies 8, а не 8–12; для приёмки E1
  городского сценария используйте `npc_cap=12`.

## 3. Игровой чек-лист (E1–E19, два ПК)

| # | Сценарий / условия | Шаги | Ожидаемо (визуал + лог) | Артефакт | Pass |
|---|---|---|---|---|---|
| **S0** | Baseline gating: `npc_actor=0`, `npc_debug=0`, город 2–3 мин | Оба подключены, стоят в толпе/ходят; затем повтор с `npc_actor=1` | `npc_actor=0`: оба лога — **0** `actor:`, **0** `probe:`; сервер — 0 `actor:`; FPS-эталон | оба лога, `scan: fps` | [ ] |
| **S1** | Город (Valentine / Saint Denis), толпа, `npc_cap=12`, `npc_radius=60`, `npc_debug=1` | 2–3 мин ходить по людным улицам рядом с гостем | host `probe: scan humans=…` ≥12; guest `actor: proxies=8..12`; `actor: snapshot count` соответствует: NPC появляются в пределах ~300 мс, без «попов»; 8–12 прокси | логи, скрины/видео, таблица метрик | [ ] |
| **S2** | Движение без телепортов | Ходить/бегать среди NPC, менять направление; гость смотрит на прокси | Прокси идут по навмешу, heading доворачивается плавно (deadzone 5°), нет прыжков >2 м, нет застреваний/слайд-шоу (10 Гц интерполяция) | OBS 10–20 с, лог `actor: snapshot` | [ ] |
| **S3** | Despawn при уходе | Хост уезжает/уходит так, чтобы NPC остались за 70 м; повтор для 2–3 NPC | host `actor: despawn id=… reason=1`; guest proxy исчезает ≤1 с; `proxies` уменьшается; нет «пустых» залипших прокси | лог, видео | [ ] |
| **S4** | Despawn при смерти | Хост убивает NPC; повторить для 2–3 NPC (в т.ч. в бою) | host `actor: despawn reason=2`; guest убирает proxy ≤1 с; труп/лут не реплицируются (E1) и не висят | лог, видео | [ ] |
| **S5** | Нет дублей | Пройти через толпу; сравнить host/guest; проверить уникальность моделей рядом | Нет двух proxy с одним `actorId`; число proxy ≤ `npc_cap`; каждый host-спавн имеет один guest-proxy (E1 не подавляет ambient гостя — см. риски, «дубли» = дубли прокси, не похожие ambient NPC) | скрины, guest `proxies`, host `spawn n=` | [ ] |
| **S6** | Reconnect гостя | Гость Alt+F4/обрыв сети → reconnect → JOIN_OK; хост остаётся | guest `actor: reset (session=S+1)`, старые прокси удалены (`proxies=0`), затем снова 8–12; нет дублей/старых NPC; после reconnect нет залипших | оба лога, видео | [ ] |
| **S7** | Leave/миграция хоста | Хост выходит; гость становится HOST; затем второй игрок заходит к нему | guest `actor: reset`, старые прокси удалены; у нового хоста своя epoch/roster; при втором игроке — новый spawn batch, без старых NPC | логи обоих, сервер | [ ] |
| **S8** | Resync (0x1D реализован) | Пауза/задержка сети у гостя (lag switch/сетевой шейпер) 2–5 с; после восстановления | guest `actor: resync needed id=…` + `actor: resync request epoch=…` (≤1/с); хост шлёт повторный roster (`actor: spawn … n=`), прокси восстановлены ≤2 с; `timeouts` ≤0.5%, нет залипших/дублей | логи, видео | [ ] |
| **S9** | S0 повтор (gating) после E1-прогона | Вернуть `npc_actor=0` на обоих, рестарт игры; город 1–2 мин | **0** строк `actor:`/`probe:` в обоих логах и сервере; FPS возвращается к baseline | логи, `scan: fps` | [ ] |
| **S10** | E2a meta-outfit внешность | `npc_appearance=1`; подойти к NPC с outfit hash; сравнить host/guest | Гость видит ту же meta-одежду; «5/5 applied, failed=0»; без мерцания/пересоздания | скрины host/guest, лог | [ ] |
| **S11** | E2b всадники | `npc_cap=12`; найти всадников; спровоцировать dismount/despawn | rider+mount — одна пара; mount spawn ×9, failed=0; при dismount/despawn лошадь и всадник убираются без залипаний | скрины/видео, лог | [ ] |
| **S12** | E3a сценарии + смерть/возрождение | 2 сценарных NPC (`--scenario-names` smoking/standing), двое курят; убить NPC | Сценарии подтверждены («двое курят на разных местах»); анти-наложение: skip 8 м / warp revert 3 м; труп остаётся; revive работает | скрины/видео, лог | [ ] |
| **S13** | E3b оружие/бой | NPC с валидным weapon hash; draw → aim → shoot → reload | Оружие достаётся, прицел без дёргания, серии выстрелов + переприцеливание, reload; weapon fail=0; state=shoot=1450/reload=385 (по логу) | видео, host/guest лог | [ ] |
| **S14** | E2c-a/E2c-b внешность | Host с E2c-b capture; fake guest (`--mode guest --outfit-wait 60`) | `fakeguest: appearance_msgs>0 appearance_actors>0 entry_max` 8..16, `errors=0`; host log `asset capture count=8-10 valid=8-10`; для E2c-a `outfit_updates/resolved_late`; визуал на 2 ПК — pending | fake guest log, host log, скрины | [ ] |
| **S15** | E3c животные/лошади | fake host `--num 0 --animals 3 --horses 2 --pattern mixed`; гость смотрит | Гость видит 3 животных + 2 лошади, двигаются без телепортов; `fakeguest: kinds human=0 animal=3 horse=2`; ошибок 0; при убийстве — `corpses`/труп | fake guest/host log, видео | [ ] |
| **S16** | E5 host-authoritative урон | `npc_damage=1`; игра-гость стреляет в прокси (human и animal); fake host или реальный хост | Гость шлёт 0x1F, урон применяет только хост: `damage id=… hp_left=…` → `killed id=… by damage`; гость видит dead/труп; прокси до смерти invincible | host/guest log, видео | [ ] |
| **S17** | E5.1 хедшот | игра-гость стреляет в голову прокси; human и animal | `damage=250`, `headshot=1` → ваншот; body-урон остаётся по классу оружия (35/60/80/30); пользователь подтвердил «хэдшот ваншотит» | host/guest log, видео | [ ] |
| **S18** | E5.2 трупы | `npc_corpse_max_sec=300`; убить human/animal/horse на хосте, смотреть гостя | Труп-прокси живёт, пока существует host-труп; `actor: corpse hold` → `corpse despawn reason=dead after=…` при исчезновении/лимите; локальный труп гостя не пропадает раньше | host/guest log, видео | [ ] |
| **S19** | E6 presence | гость открывает карту/паузу/сатчель/магазин, затем закрывает; смотреть имя над головой у хоста | У имени суффикс `in map`, `pause menu`, `in satchel`, `in shop` (ASCII `Name - …`); после закрытия/12 с без обновлений суффикс исчезает; `ui: pause=…` в debug | host/guest log, скрины | [ ] |
| **S20** | E7.1 visitor mode | у гостя `npc_visitor=1`, `npc_visitor_vehicles=1`; город; сравнить до/после | Локальная ambient-толпа гостя подавлена/уменьшена (`actor: visitor mode on`, `visitor cleanup removed=N`); продавцы/квестовые/событийные NPC остаются; локального трафика нет, машины — из реплик хоста (E3d-a) | guest/host log, видео | [ ] |
| **S21** | E7.2 плавность | host fake `--pattern circle --num 4 --gait 3 --duration 120`, гость смотрит | `actor: motion id=… reissue=<0.05/s snap=0 target_err_p95` низкий; нет телепортов (кроме `>25 м/|dz|>15/нет кадров>2 с`); стоп/старт без дёргания; `warn update-native A/B` отсутствует | fake host/guest log, OBS | [ ] |
| **S22** | E3d-a транспорт | `world_vehicles=1`, `npc_vehicle_radius=80`, `npc_visitor_vehicles=1`, `npc_occupants=1`; хост садится за руль/пассажиром и едет; повозка с draft-лошадью | Гость видит тот же транспорт в пределах ~80 м; водитель/пассажиры seated (0x21) без дублей/T-pose, draft-лошадь в упряжке; `world: vehicles send n=`/`vehicle spawn`, `world: occupants vehicle=`, `actor: seat attach … ok=`/`draft attach … ok=`; локального трафика гостя нет; при уходе из радиуса/пустом snapshot прокси удаляются; до 16 машин. При `npc_occupants=0` — старое E3d-a (пустые машины) | host/guest log, видео | [ ] |
| **S23** | Host migration | хост Alt+F4/quit/Ping-only зависание; гость остаётся; затем старый хост rejoin | Гость становится HOST (HostChanged), реплики очищены/reset, чек `coop: role change -> HOST reset complete`; для Ping-only с e18 — **мягкая** миграция: game-timeout 4 с (`[!] host game-timeout`+`promoting remaining`), сокет старого хоста не закрыт, `PlayerLeft` нет, Ping/Pong жив (детально — S31); старый хост rejoin как гость без дублей | оба лога, server log, видео | [ ] |
| **S24** | Mission-aware visitor | гость в миссии/катсцене (`GET_MISSION_FLAG`/`IS_PLAYER_READY_FOR_CUTSCENE`) в городе | Толпа гостя **не удаляется** во время миссии/катсцены (`actor: visitor scene pause on mission=1/cutscene=1`); после выхода cleanup возобновляется; density/road suppression продолжаются | guest log, видео | [ ] |
| **S25** | E3d-c occupants/draft | `npc_occupants=1`; хост за рулём, затем пассажиром; повозка с draft-лошадью; сравнить host/guest | Гость видит водителя/пассажиров **seated** в тех же местах, draft-лошадь прицеплена; без дублей human-прокси (occupant не спавнится как отдельный human), без T-pose/провала в текстуры; log `world: occupants vehicle=…`, `actor: seat attach … ok=`, `actor: draft attach … ok=`, `occupants errors=0`; `npc_occupants=0` откат на пустые машины | host/guest log, видео | [ ] |
| **S26** | E3d-b mission-train + E11 passengers | `world_trains=1` (при `npc_actor=1 && world_vehicles=1 && world_sync=0`); хост у станции/едет, останавливается, разворачивается; затем `world_trains=0` | Гость видит **один** mission-train на рельсах с правильным числом вагонов, станцией/направлением/разворотом; **E11:** пассажиры поезда в carIndex seated/совпадают, без T-pose/дублей; локального второго поезда нет; состав не скачет >25 м/|dz|>15; при выключении/leave/role change mission-train удаляется и random trains восстанавливаются (`SET_RANDOM_TRAINS(true)`); риски: config matching 87 хэшей, direction/reversing семантика | host/guest log, видео | [ ] |
| **S27** | E9 animal/horse appearance | `npc_animal_appearance=1` на **обоих** ПК; хост в конюшне/на улице с лошадьми и животными; гость смотрит; затем флаг 0 и рестарт | Лошади: масть/грива/хвост/седло совпадают с хостом; животные — окрас/мех; нет invisible/T-pose/заморозки; host log `actor: animal appearance probe …`; `npc_animal_appearance=0` → random как раньше; без дублей/randomize после tack; выключение флага на одном ПК не допускается (INI host+guest совпадают) | host/guest log, видео, probe | [ ] |
| **S28** | E10 mounted horse appearance | `npc_mount_appearance=1` на **обоих** ПК; хост садится на NPC-лошадь/всадника (NPC mounted pair); гость смотрит; затем флаг 0 и рестарт | Лошадь под NPC-всадником совпадает с хостом (масть/грива/хвост/седло); нет invisible/T-pose/заморозки/дублей; host log `actor: mount outfit send actor=… model=… items=…`; guest `actor: mount outfit apply … ok=1`/`guest mount outfit applied/failed`; при 0 → random; mounted-horse повторно применяется после позднего mount (heartbeat/retry) | host/guest log, видео | [ ] |
| **S29** | E12 перф/трафик | 5–10 мин активной игры (NPC + транспорт); `npc_debug=1`, `player_frame_hz=20`, `player_frame_change_only=1`; снять `perf: frame`, `perf: npc`, `perf: net` на обоих ПК и server `net: bytes_in/out` | `perf: frame host/guest` p95 ≤**4 мс**, max ≤**10 мс**; actor p95 ≤**2 мс**; `perf: npc warn`=0; traffic типично **≤10–12 КБ/с**, worst **≤15 КБ/с**; `frames_hz` соответствует движению/покою (≤20/5), bytes не растут бесконечно | host/guest log, server console, FPS | [ ] |
| **S30** | E13 wall-clock + config warnings | Обычный прогон с 28 ключами (e19); затем отдельный scratch-прогон с опасной комбинацией (например `world_sync=1 && npc_actor=1`), вернуть INI; смотреть `perf: frame host/guest ... tick_hz=` | В обычном прогоне **нет** `config: warn`; в опасной комбинации ровно соответствующая строка `config: warn world_sync=1 && npc_actor=1 …`, игра не падает; `tick_hz` ≈ реальный FPS, `frames_hz` — целевой Player Frame; интервалы/трафик не масштабируются с FPS (60 vs 144) | host log, `config: warn`, `perf: frame` выдержки | [ ] |
| **S31** | **E18 soft host migration + загрузка + vendor dedupe** | (а) хост Alt+F4/quit — обычная миграция; (б) хост «зависает» (только Ping) после 5–10 с игры; (в) хост перезапускает игру и 30–60 с висит на загрузке (playerPed нет); (г) rejoin старого хоста; (д) гость в visitor mode у продавца | **(а)** гость HOST, `PlayerLeft`+`HostChanged`, `reset complete`; **(б)** через ~4 с server `[!] host game-timeout … promoting remaining` + `[HOST] … повышен до хоста`, guest HOST, старый хост GUEST (`HostChanged(0)`, `GUEST reset complete`), **сокет старого хоста не закрыт**, `PlayerLeft` нет, Ping→Pong работает, reconnect-шторма/флапа ролей нет; **(в)** демоции **нет** — grace 15 с + presence heartbeat ~2 с, в server log нет `host game-timeout`, после загрузки идут Frame; **(г)** старый хост заходит гостем (`is_host=0`), без старых прокси, роль не забирает; **(д)** у продавца нет дубля: guest throttled `actor: visitor vendor skip id=… model=0x…`, host-прокси продавца не спавнится | server `server_console.log` (game-timeout/promoting/PlayerLeft), оба `coop: role change`, guest `actor: visitor vendor skip`, видео загрузки/миграции | [ ] |
| **S32** | **E19 blip name + кучер/draft повозки + wheel A/B** | (а) открыть карту/легенду — подпись второго игрока; (б) повозка с кучером и тягловыми лошадьми, плотный трафик; (в) отдельный прогон `npc_vehicle_wheels=1` на обоих ПК → вернуть `0` | **(а)** подпись = `player_name`, не «Спутник»/«Companion»; лог `coop: blip name '<имя>'`; после rejoin/respawn не пропадает; **(б)** у гостя кучер seated и draft-лошади в упряжке, без T-pose/дублей; на плотном трафике нет висящего «driver без машины»; логи `world: occupants vehicle=…`, `actor: seat/draft attach … ok=`; **(в)** при 1 — `world: vehicle wheels speed=…`, колёса крутятся (дрейф/рывки допустимы); при 0 после рестарта — статичные колёса, регрессий нет | скрины карты host/guest, `coop: blip name`, `world: occupants`, `world: vehicle wheels`, видео повозки/плотной улицы | [ ] |
| **S33** | **E20.3/v21 actor-cap 50: животные/лошади/кучер дилижанса/всадник+конь/55+ NPC** | (а) плотный город/улица при `npc_cap=50`: 30–50 NPC, животные и свободные лошади, NPC-всадник, дилижанс с кучером и draft-лошадьми; 3–5 мин; (б) сравнить host/guest; (в) отдельный взгляд на конюшню/ранчо (много лошадей) | **(а/б)** у гостя **animals>0 и horses>0** (животные/лошади больше не вытесняются людьми из roster); у дилижанса видны **кучер и тягловые лошади** (draft в упряжке, без T-pose/дублей); NPC-**всадник+конь** реплицируются парой; при наличии животных на хосте `animalsSelected>0`, `animals=0` у гостя — FAIL; host `cap=50` достижим, `actor: discovery humans=… animals=…`/`world: occ dbg` заполнены; **(в)** лошади конюшни видны у гостя (нет cap-starvation); перф-пороги S29/G2 не нарушены (трафик 50×15 Гц×32 B ≈ 24 КБ/с — для теста допустимо, зафиксировать в отчёте) | оба лога (3–5 мин): `actor: discovery`, `world: occ dbg`, `actor: proxies=`, `animal appearance`, `mounted`; видео животных/дилижанса/всадника; скрины host/guest | [ ] |

Дополнительно: визуальная проверка E2c-b/E5/E6/E7/E3d-a/E3d-b/E3d-c/E9/E10/E19/
E20.3 (S33) на **двух ПК** (сейчас задеплоена npc-e20.2/v20; v21/e20.3 —
деплой Lead pending; полный прогон — после деплоя), сценарные позы (E3a),
ночь/дождь.
Ambient-поезда не чистим v1, точные пассажиры поездов — P1, seat=-1/driver
и direction/reversing требуют in-game проверки.

### E8 — occupants и mission-train (уточнения)

- **E3d-c (0x21):** host discovery снова включает педов в транспорте
  (mission-occupants приоритетно, draft-лошади через animal pass); host
  собирает driver/passengers seats 0..max-1/draft harness 0..5, шлёт при
  изменении + heartbeat 1/с (0x21 ≤72 B, 20/с сервер). Guest ждёт
  vehicle+actor, invisible→`SET_PED_INTO_VEHICLE` warp→check→visible;
  seated/draft skip MoveProxy; vehicle-прокси 60 Гц; cleanup.
- **E3d-b (0x22):** mission-train host→guest; 87 TrainConfigs,
  forward/reverse match, бюджет 5/тик/80 model; guest `SET_RANDOM_TRAINS(false)`
  + `_CREATE_MISSION_TRAIN`, speed/coords/warp/halt/leave/reverse, cleanup
  timeout 2 с и restore; `world_trains=0` opt-in; логи
  `world: train send/spawn/despawn`.

### E9 — animal/horse appearance (S27)

- Включить `npc_animal_appearance = 1` **на обоих ПК** (INI должны
  совпадать); `npc_debug=1` для probe. Хост держит лошадей (конюшня/улица)
  и животных рядом с обоими.
- Host: MPT_ANIMAL (`_GET_META_PED_TYPE==3`, kind1/2, cap16) → assets/tint
  в `ActorAppearance` 0x1E (≤460 B, budget 1 animal/tick ~2 с); horse tack
  (kind2) → `ActorOutfit` 0x1C 792 B on change (budget 1 horse/tick ~2 с).
  Probe `actor: animal appearance probe model=0x… type=… comps=… cats=…
  valid=… metaOutfit=… shop=…` (1/модель ≤1/10 с).
- Guest: staged apply/readback, fallback random; horse tack apply; без
  randomize после tack. Проверять `actor: animal appearance apply … ok=`,
  `actor: horse tack apply … ok=`, 5-сек счётчики.
- Выключить `npc_animal_appearance=0` → животные/лошади снова random (не
  залипают в предыдущем варианте); INI host+guest должны совпадать.
- **Риски:** точные пассажиры поездов — E11 (0x21 carIndex, проверять S26);
  ambient-поезда не чистим v1; seat=-1 (driver) требует in-game проверки;
  direction/reversing семантика приближённая; config matching (87 хэшей)
  может давать skip/ambiguity.

### E10 — mounted NPC-horse appearance (S28)

- Включить `npc_mount_appearance = 1` **на обоих ПК** (INI совпадают);
  `npc_debug=1`. Хост — NPC-всадник (rider + mount) в кадре.
- Host: для NPC mounted pair шлёт внешность лошади через `ActorOutfit` 0x1C
  с actorId **всадника** (`actor: mount outfit send actor=… model=0x…
  items=…`, бюджет 1 mounted horse/тик ~2 с).
- Guest: ждёт mounted+mount handle, применяет `ActorOutfit` к лошади
  всадника (`actor: mount outfit apply actor=… items=… ok=1`), retry ≤3/2 с
  после reseat/recreate; счётчик `actor: guest mount outfit applied=u failed=u`.
- Ожидаемо: лошадь NPC-всадника совпадает с хостом, без invisible/T-pose/
  дублей; `npc_mount_appearance=0` → random. Флаг host+guest синхронно.
- Ограничения E10: лошадь может получить краску с задержкой/не примениться
  при позднем mount (heartbeat/retry повторит); старые shop items не
  снимаются.

### E11/E12/E13 — passengers/performance/wall-clock (S26/S29/S30)

- **E11:** точные пассажиры поездов через `VehicleOccupants` 0x21
  `carIndex`; guest seated в вагонах, без T-pose/дублей; проверять
  `world: occupants vehicle=`, `actor: seat attach … ok=`,
  `world: train send … cars=…`.
- **E12:** `perf: frame host/guest tick/p95/max | world/actor/coop |
  frames_hz` каждые ~5 с; server `net: bytes_in/out` + per-type (~10 с);
  client `perf: net tx/rx KB/s msgs_tx/rx` (≥10 с); `player_frame_hz=20`,
  `player_frame_change_only=1`.
- S29: 5–10 мин активной игры; пороги `perf: frame` p95 ≤4 мс, max ≤10 мс;
  actor p95 ≤2 мс; traffic типично ≤10–12, worst ≤15 КБ/с; warn=0.
  P0/P1-фиксы: outfit ≤10 Гц, capture budget 2/тик, общий vehicle-скан+TTL,
  occupants 1/с, resync appearance, world_scan debug.
- **E13:** интервалы wall-clock (`GetTickCount64`, ms), FPS-independent;
  `perf: frame` содержит `tick_hz=` (реальный FPS) рядом с `frames_hz`;
  `config: warn …` пишется один раз при старте для опасных комбинаций
  (world_sync+npc_actor/world_trains; !world_vehicles+occupants/visitor;
  !npc_actor+vehicles/visitor), не падает.
- S30: обычный INI (28 ключей, e19) — **ноль** `config: warn`; scratch-прогон с
  опасной комбинацией — соответствующая warning-строка и игра жива; `tick_hz`
  ≈ FPS, p95/max/трафик не масштабируются с 60→144 FPS. Остаточные тиковые
  таймеры (corpse hold, action/presence poll, motion/scenario, attach/dead
  windows, vehicle-scan TTL, train engine cache) — известное ограничение.

### E18 — soft host migration, загрузка и vendor dedupe (S31)

- **Семантика сервера** (task-175, `server/server.cpp`
  `CheckHostGameTimeoutPhase`): host game-timeout 4 с считается только по
  **игровым** сообщениям (Ping/Pong — нет). До **первого** game-thread
  сообщения хоста действует **join grace 15 с** от JOIN/промоушена — загрузка/
  меню роли не меняют. По таймауту — **мягкая миграция**: `isHost` снимается
  со старого хоста, второй повышается, обоим уходит `HostChanged`; **сокет
  старого хоста не закрывается**, `PlayerLeft` не рассылается; старый хост
  остаётся обычным клиентом (Ping/Pong живёт) до idle/connection cleanup.
  Логи: `[!] host game-timeout (N ms), promoting remaining`,
  `[HOST] <имя> повышен до хоста`; клиенты — `coop: role change -> HOST/GUEST
  reset complete`. Новому хосту даётся тот же grace 15 с.
- **Клиент на загрузке** (task-176): пока `playerPed` отсутствует (загрузка/
  респавн), клиент шлёт `PlayerPresence` flags=0 каждые ~2 с; presence —
  game-thread сообщение, поэтому сервер не демотирует загружающегося хоста.
  После появления педа обычная presence-логика (2 Гц/изменение) берёт верх;
  дублей нет. Лог `perf: net`/серверная статистика показывают ~1 сообщение/2 с.
- **Vendor dedupe** (task-180): при `npc_visitor=1` гость не спавнит
  host-прокси продавца (`IsVisitorVendorModel`) — продавец остаётся локальным,
  без дубля. Guest-лог throttled: `actor: visitor vendor skip id=… model=0x…`.
- **Как проверять (S31):** см. F1–F4 в [ACCEPTANCE_2PC.md](ACCEPTANCE_2PC.md)
  §6; серверный лог — `Select-String -Pattern "host game-timeout|promoting
  remaining|\[HOST\]|PlayerLeft"`; наличие `PlayerLeft` **вместе** с
  `promoting remaining` для одного и того же хоста — FAIL (hard-миграция
  вместо soft). Reconnect-шторм = серия JOIN/reconnect/WinError в логах
  клиента после такого таймаута — FAIL.
- **Автотесты:** `tests/integration_server.py` —
  `test_host_game_timeout_ping_only` (demote/no-socket-close/no-PlayerLeft,
  Ping→Pong; presence-only не демотируется),
  `test_host_game_join_grace` (хост без game-сообщений не демотируется внутри
  grace), `test_host_game_timeout_frame_activity` (Frame продлевает).

### E19 — blip name, кучер/draft повозки и wheel A/B (S32)

- **Имя игрока на карте/blip:** клиент готовит VAR_STRING
  (`MakeLiteralString`, `MISC_VAR_STRING`) и зовёт `_SET_BLIP_NAME`
  (0x9CB1A1623062F402) для blip партнёра; `SetBlipName` ставит подпись,
  `UpdateBlipName` обновляет её в Tick/после rejoin/respawn (`s_blipName`
  сравнивается с `s_pedName`). Лог `coop: blip name '%s'`. Если на карте
  осталась дефолтная «Спутник»/«Companion» — FAIL; резерв —
  `BLIP_REMOVE_MODIFIER` (0xB059D7BD3D78C16F) перед установкой имени (с
  возвратом friendly-модификатора).
- **Кучер/тягловые лошади повозки:** в e19 host-скан — occupant-first
  (педы в транспорте и draft-лошади имеют приоритет в roster) + полный
  allowlist occupants всех vehicle в occ-радиусе. Ожидаемо: у гостя кучер
  seated и draft-лошади в упряжке, без T-pose/дублей; логи
  `world: occupants vehicle=…`, `actor: seat attach … ok=`,
  `actor: draft attach … ok=`, `actor: occupants errors=0`.
  **Low/info:** на плотном трафике возможен гость-«driver без машины»
  (occupant привязан к отсутствующему/ещё не созданному vehicle) — проверить,
  что водитель не висит в воздухе/без транспорта; зафиксировать в отчёте.
- **Wheel A/B (`npc_vehicle_wheels`):** default `0` — колёса guest
  vehicle-прокси статичны (исходное поведение); отдельный прогон `1` на
  **обоих** ПК — `ApplyGuestVehicleWheelSpeed` задаёт прокси скорость
  `SET_VEHICLE_FORWARD_SPEED` (0xF9F92AF49F12F6E7, SIGNATURE-ONLY), лог
  `world: vehicle wheels speed=%.1f`; возможен конфликт с интерполяцией
  (дрейф/рывки — допустимо для A/B). После A/B вернуть `0` + рестарт игры
  на обоих ПК. 28-й INI-ключ, default 0 (см. §0).
- **SET_ENTITY_COORDS:** подтверждена арность 8 (`clearArea=0`) — поведение
  не менялось; отдельной проверки не требует.
- **Как проверять (S32):** см. A12/B5/C6/G5 в
  [ACCEPTANCE_2PC.md](ACCEPTANCE_2PC.md); быстрый фильтр:
  `Select-String -Pattern "coop: blip name|world: occupants|draft attach|world: vehicle wheels"`.

### E20.3/v21 — actor-cap 50 (S33)

- **Проблема:** при `kMaxActors = 12` люди заполняли весь roster и животные/
  лошади/тягловые дилижанса не выбирались (`animalsSelected=0`, у гостя
  animals=0/horses=0, у дилижанса нет draft). e20.2 nearest-first roster не
  устранил starvation полностью.
- **Фикс:** `kProtocolVersion 21`, `kMaxActors = 50` (`net/protocol.h`);
  `ActorSpawnBatch` max 8+50*64 = **3208 B**, `ActorFrames` max
  8+50*32 = **1608 B** (было 776/392); серверные валидаторы/клиент — через
  `net::kMaxActors` (хардкодов 12 нет). `npc_cap` clamp **1..50** (default 8;
  `npc_cap=51` → keep default). v21 — жёсткий bump: v20↔v21 несовместимы,
  deploy только парой ASI+server из `dist\npc-e20.3` (откат — e20.2/v20).
- **Приёмка (S33):** см. A13 в [ACCEPTANCE_2PC.md](ACCEPTANCE_2PC.md); host
  `actor: discovery humans=… animals=… selected=…`/`world: occ dbg`; guest
  `actor: proxies=`, `animal appearance`, `mounted`. FAIL, если у гостя
  animals=0/horses=0 при наличии животных на хосте или у дилижанса нет
  кучера/draft. **Трафик-риск:** 50 акторов × 15 Гц × 32 B ≈ 24 КБ/с только
  `ActorFrames` — выше G3 (≤15 КБ/с); для 2-ПК теста допустимо, зафиксировать
  в отчёте (при регрессии — `npc_cap` ниже или adaptive 15 Гц).

### E5.2/E6/E7 — уточнения методики

- **E5.2:** труп-прокси держится до `npc_corpse_max_sec` (300 с) или пока
  существует host-труп; проверять `corpse hold/despawn` и визуально, что
  труп не исчезает раньше/не висит после.
- **E6:** presence шлётся при изменении UI и heartbeat ~2 с; host видит
  суффикс у имени гостя. Проверять карту, pause menu, satchel, shop,
  cutscene; таймаут 12 с и `npc_presence=0` → суффикса нет.
- **E7.1:** подавление ambient на госте — только визуальное (другие игроки/
  mission NPC не должны пропадать); продавцы/квестовые/событийные NPC —
  сохраняются (`npc_visitor_preserve_events=1`); при `npc_visitor=0` —
  прежняя толпа. `npc_visitor_vehicles=1` подавляет локальный трафик, машины
  едут из реплик хоста (E3d-a); при `world_vehicles=0` дороги гостя пустые.
- **E7.2:** baseline-порог плавности `reissue <0.05/s`, `snap=0`;
  телепорт-лог ожидается только на respawn/стрим-прыжках; `npc_motion_marker`
  включать только для A/B fallback.
- **E7.3:** E3d-a — см. S22; host migration — S23 (+ soft e18 — S31);
  mission-aware visitor —
  S24; новых сообщений протокола нет (v18 без bump), v19 — VehicleOccupants
  0x21/TrainSnapshotV2 0x22.

### E5/E5.1 — методика урона (для приёмки)

- Гость: детект реального выстрела + free-aim target
  (`GET_ENTITY_PLAYER_IS_FREE_AIMING_AT`); урон по оружию: пистолет/револьвер
  35, винтовка/repeater/sniper 60, дробовик 80, иначе 30; лимиты ≤10/с и
  100 мс/actor. Хедшот: `SKEL_Head` + impact ≤0.30 м (fallback — луч камеры),
  `damage=250`.
- Хост: применяет `APPLY_DAMAGE_TO_PED` (fallback `SET_ENTITY_HEALTH`),
  лимиты ≤8/тик, ≤15/с, per-actor 100 мс, дистанция ≤150 м, только live
  actor из roster/epoch; при hp≤0 — dead flag/health=0 → труп.
- Ожидаемые строки: `actor: damage send … headshot=1`,
  `actor: damage apply …`, host `fakehost: damage … hp_left=…`,
  `fakehost: killed … by damage`; в игре — падение/труп прокси.

## 4. Откат и аварийные действия

1. `npc_actor=0`, `npc_debug=0` на **обоих** ПК → перезапустить игру (E1
   hot-reload не поддерживается).
2. При вылете/краше: сохранить `rdr2-coop.log` и `server_console.log`,
   зафиксировать сцену/место; ASI/INI восстановить из
   `scripts\rdr2_coop.asi.bak-*`, `rdr2-coop.ini.bak-*`; сервер — старый exe.
3. E1 не пишет в сейвы (только чтение мира/сетевые сообщения); при сомнениях —
   бэкап сейвов по `docs/TESTING.md` §7 и restore через
   `scripts\restore_saves.ps1` (dry-run, затем `-Force`).
4. Не оставлять `npc_actor=1` в публичной/рабочей сборке до полного прохождения
   чек-листа и метрик.

## 5. Риски и известные ограничения E1 (учитывать при приёмке)

- **Нет timestamp в `sh_log`**: `actor:`-строки без времени, поэтому spawn
  p95 ≤300 мс / despawn ≤1 с измеряются по OBS-видео покадрово (либо нужен
  будущий `t=<ms>` в логе). Логовые производные метрики — rate/count (см. §2).
- **Нет маркера телепорта** в E1 (снап при >12 м или |dz|>8 делается молча):
  «0 телепортов в steady» проверяется визуально/по видео; предложение — добавить
  `actor: teleport id=… dist=…` в E1.1.
- **Resync-запрос**: реализован (`Client::SendActorResync` +
  `coop::SendActorResync`, троттлинг ≤1/с, лог `actor: resync request`);
  хост по `OnActorResyncRequest` повторно шлёт roster (0x18). При проверке S8
  убедиться, что запрос уходит и roster восстанавливается; при отсутствии
  ответа/восстановления — это E1-дефект, а не ограничение.
- **0x1B ActorEvent / 0x1C ActorOutfit** зарезервированы: принимаются, но не
  применяются; E2a использует `metaOutfit` в heartbeat-spawn (0x18), E2c-b
  — отдельный `ActorAppearance` 0x1E (v16).
- **E2c-b**: реализован и провалидирован по данным (host capture 8-10
  valid, fake guest errors=0), но **визуальная двух-ПК приёмка pending** —
  проверять host/guest внешность по S14.
- **Люди + животные + лошади + транспорт**: E3c реплицирует животных/
  свободных лошадей, трупы до despawn (E5.2); E3d-a vehicles-only прокси,
  E3d-c occupants/draft (0x21), E3d-b mission-train (0x22, `world_trains=0`
  opt-in); mission NPC исключены; human death/revive — E3a, animal/horse
  death — E3c/E5 без revive.
- **E5/E5.1:** нет raycast — детект по free-aim target + impact; урон
  аппроксимирован по классу оружия/HP движка, хедшот-радиус 0.30 м; животные
  умирают быстро; труп до despawn; mission NPC не получают урон; прокси у
  гостя invincible до смерти (урон только через хоста).
- **E5.2/E5.3:** труп держится до host-трупа/`npc_corpse_max_sec`; mounted
  сглаживание — retarget/lead/stuck fallback (A/B).
- **E6:** суффикс presence — ASCII `Name - in map`/`pause menu`/`in satchel`/
  `in shop`; таймаут 12 с; `npc_presence=0` отключает. Карта/пауза — UI
  отправителя, второй игрок только отображает.
- **E7.1:** visitor mode подавляет ambient **только у гостя**; продавцы/
  квестовые/события сохраняются; `npc_visitor_vehicles=1` + `world_vehicles=1`
  — машины из реплик хоста, при `world_vehicles=0` дороги пустые; в миссии/
  катсцене cleanup паузится (`npc_visitor_pause_scenes=1`); Story Mode нельзя
  реально «подселить» второго игрока в один мир, RedM — другой стек.
- **E7.3/E3d-a/host migration:** E3d-a vehicles-only; E3d-c occupants/draft
  (0x21) — водитель/пассажиры seated, без дублей; при миграции мир не
  переносится (новый хост отдаёт свой), реплики очищаются; game-timeout 4 с
  не считается по Ping; **e18:** мягкая миграция без закрытия сокета/без
  `PlayerLeft`, join grace 15 с, presence-liveness (S31/F1–F4); пустой
  snapshot удаляет vehicle-прокси; 2-ПК прогон e19 pending (пользователь запускает).
- **E8/E3d-b/E3d-c:** mission-train (0x22) — 87 config-хэшей, direction/
  reversing семантика приближённая; E11 точные пассажиры поездов — 0x21
  `carIndex`, проверять S26; ambient-поезда не чистим v1; seat=-1 (driver)
  требует in-game проверки; `world_trains` default 0; occupancy/draft
  проверять по S25, поезд — по S26.
- **E9/E10 animal/horse appearance:** cap 16 компонентов; `RESET_PED_COMPONENTS`
  на животных не проверен в игре; horse tack/mount model-specific (старые shop
  items не снимаются); assets coverage `MPT_ANIMAL` может быть неполным →
  random fallback; MetaPedOutfit fallback только probe; `npc_animal_appearance`
  и `npc_mount_appearance` должны совпадать на host+guest. Проверять S27/S28.
  E10: mounted horse может получить внешность с задержкой/не примениться при
  позднем mount (heartbeat/retry повторит).
- **E12/E13/E18:** `perf: frame` p95/max — полный кадр (рекомендуемый бюджет
  p95≤4/ max≤10 мс), actor p95≤2 мс; traffic по server bytes и client
  `perf: net` — типично ≤10–12, worst ≤15 КБ/с; `player_frame_hz=20` может
  слегка сглаживать ghost (поднять до 60). E13: wall-clock/FPS-independent;
  `tick_hz` ≈ FPS, `config: warn` только для опасных комбинаций; часть
  таймеров осталась тиковой (corpse hold, action/presence poll, motion/
  scenario, attach/dead windows, vehicle-scan TTL, train engine cache) —
  при 144 FPS их окна короче; config-асимметрия host/guest не проверяется.
  E18: loading presence heartbeat добавляет ~1 сообщение/2 с только пока
  playerPed отсутствует; soft migration не закрывает сокет (трафик/Ping
  продолжаются, reconnect-шторма нет).
  **E19:** blip name + occupant-first кучер/draft + opt-in wheel A/B
  (`npc_vehicle_wheels=0` default; 28-й INI-ключ); при wheel=1 возможен
  дрейф/рывки guest-прокси (A/B, не fail); low/info — подпись blip и
  «driver без машины» на плотном трафике. **E20.3/v21:** cap 50 — животные/
  лошади/кучер дилижанса больше не вытесняются людьми (S33); трафик 50×15 Гц
  ×32 B ≈ 24 КБ/с выше G3 — для теста допустимо (см. S33).
- **E2c-a resolver**: механически работает (890 моделей/4568 кандидатов,
  бюджет 24 натива/тик), но random ambient NPC без именованного outfit —
  `resolved=0/failed=11`; это закрывает E2c-b asset/tint, проверять
  `appearance_msgs/appearance_actors/entry_max/errors` и визуал.
- **Двух-ПК приёмка не пройдена**: задеплоена пара npc-e20.2/v20 (ASI
  `26BB6FDD`, server `0710D2F7`), оба ПК (local + ARINA); v21/npc-e20.3 —
  сборка/деплой Lead pending (кандидат ASI `D95A0F2F`/server `B225BE7C`).
  Текущие E1–E20.2 — однопК-проверки fake host/guest + реальный хост,
  E5/E5.1 подтверждены игровым гостем. Полный прогон pending —
  пользователь запускает. Финальный чек-лист: [ACCEPTANCE_2PC.md](ACCEPTANCE_2PC.md).
- **Tick budget**: для E4 добавлена инструментация (`perf: npc host/guest ...`
  при `npc_debug=1`); анализ и пороги — `docs/PERF.md` +
  `tools/perf_report.py`. На старом логе (до E4-билда) `perf:` нет — это не
  дефект, а отсутствие инструментации.
- Discovery/кап зависят от позиции **обоих** игроков и от `npc_cap/radius`;
  хост должен быть рядом с толпой, иначе 8–12 не наберётся.
- Cap-кламп: `npc_cap` **1..50** (v21; default 8), `npc_radius` 20..100;
  некорректные значения игнорируются (остаются defaults 8/60), `npc_cap=51`
  → default.
- `npc_actor=0` — обязательный gating: ни клиент, ни сервер не должны писать
  `actor:`/`probe:` строки; проверять на обоих ПК и сервере (S0/S9).

## 6. E4 — производительность NPC-репликации

E4 принимается по `perf: npc host/guest` и полному `perf: frame host/guest`
(`npc_debug=1`) и порогам:

- `perf: frame` host/guest p95 ≤ **4.0 мс**, max ≤ **10.0 мс** (полный кадр);
- actor p95 ≤ **2.0 мс**; `perf: npc` tick p95 ≤2.0 мс;
- discover/capture/apply p95 ≤ **1.5 мс**; guest spawn/appearance/motion/
  weapons/build p95 ≤1.5 мс;
- `perf: npc warn` = **0**; timeouts ≤**0.5%**; appearance failed ≤**1%**;
  delete failed = **0**;
- traffic: server `net: bytes_in/out` + client `perf: net` — типично
  ≤**10–12 КБ/с**, worst ≤**15 КБ/с** (S29);
- E13: `tick_hz`/`frames_hz` в `perf: frame`, `config: warn` для опасных
  комбинаций (S30).

Команды и методика (baseline `npc_actor=0`, город 3–5 мин, fake guest,
`--baseline` vs `--log`, `--json`, exit 0/1/2) — в `docs/PERF.md`.
Ограничение 1 ПК: host и guest метрики снимаются отдельными прогонами;
fake guest не даёт guest-`perf:` (это Python). Двух-ПК приёмка
E2c-b/E4/E5/E6/E7/E3d-a/E3d-b/E3d-c/E9/E10/E18/E19/E20.3 — pending;
задеплоена npc-e20.2/v20 (ASI `26BB6FDD`/server `0710D2F7`, оба ПК);
v21/npc-e20.3 — деплой Lead pending; полный прогон ждём.
