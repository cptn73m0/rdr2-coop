# Финальная приёмка 2-ПК — RDR2 CO-OP v19 / npc-e19

**Назначение:** пошаговый приёмочный сценарий для пользователя + ARINA (второй
ПК) после деплоя v19. Проверяем не отдельные unit-тесты, а связку
«игра-хост + игра-гость» в реальном мире: NPC, visitor, транспорт, поезда,
внешность, миграцию хоста и 30–40 минут стабильной игры.

**Версия:** protocol **v19**, **финальная сборка npc-e19** (HEAD `ef49ac8`,
тег `npc-e19`): ASI
`DD0EE210A06B0F4F61FF01EB1032D3E0C8DB8221702219FE4258D93C2233C5B9` (779 264 B),
server `DD9BD9D818CB5ADF9063A35CE8CDF7C0D54468D9711212A782411BD0D54781FC`
(410 112 B); оба ПК — **одна и та же** сборка/снапшот `dist\npc-e19`.
**28 INI-ключей приёмки** (было 27; e19 добавляет 28-й —
`npc_vehicle_wheels = 0`, A/B, default 0) — всего **38 строк** в INI
= 28 приёмочных + 6 служебных P2/P3 + 4 подключения (сверено с
`dist\npc-e19\rdr2-coop.ini.local/.arina`, живым INI ARINA и
`src/config.cpp` на `ef49ac8`).
E11 (точные пассажиры поездов через 0x21 carIndex) входит в базу и
проверяется в D3. E13 делает интервалы wall-clock (ms) **FPS-independent**;
E14–E17 — ub-mode refactor-only (протокол/поведение не менялись); пороги
G2/G3 не меняются.

**Что изменилось в npc-e18 (проверяется в блоке F/S31):**

- host game-timeout — теперь **мягкая миграция**: хост демотируется, второй
  повышается, **сокет старого хоста НЕ закрывается** (нет reconnect-шторма и
  флапа ролей); `PlayerLeft` не рассылается; Ping/Pong демотированного хоста
  продолжает работать;
- **join grace 15 с** до первого game-thread сообщения от хоста: загрузка/
  меню не дёргают роли; после первого game-сообщения действует обычный
  game-timeout 4 с;
- `PlayerPresence` считается game-thread liveness — presence-only хост не
  демотируется (интеграционный тест);
- клиент на загрузке/респавне, пока playerPed отсутствует, шлёт presence
  heartbeat ~2 с (с появлением педа обычная presence-логика берёт верх,
  дублей нет);
- гость в visitor mode **не спавнит host-прокси продавца** — продавцы больше
  не дублируются (throttled-лог `actor: visitor vendor skip`).

**Что изменилось в npc-e19 (проверяется в A12/B5/C6/G5):**

- **имя игрока на карте/blip:** подпись метки — `player_name`, а не дефолтная
  «Спутник»/«Companion» (`MakeLiteralString` + `_SET_BLIP_NAME` 0x9CB1A1623062F402
  + VAR_STRING, `SetBlipName`/`UpdateBlipName`, лог `coop: blip name '%s'`);
  если подпись осталась «Спутник» — FAIL (резерв `BLIP_REMOVE_MODIFIER`);
- **occupant-first roster + полный allowlist** occupants всех vehicle в
  occ-радиусе: у гостя видны **кучер и тягловые лошади повозки** (seated /
  в упряжке, без T-pose/дублей); low/info — на плотном трафике возможен
  «driver без машины» (проверить, что водитель не висит без транспорта);
- **opt-in wheel A/B:** `npc_vehicle_wheels = 0` по умолчанию (исходное
  поведение); отдельный прогон `npc_vehicle_wheels = 1` на **обоих** ПК —
  колёса guest vehicle-прокси крутятся (`SET_VEHICLE_FORWARD_SPEED`
  SIGNATURE-ONLY, лог `world: vehicle wheels speed=`; возможен дрейф/рывки —
  это A/B, не fail) → вернуть `0`;
- `SET_ENTITY_COORDS` арность 8 подтверждена (clearArea=0, поведение не
  менялось);
- серверный `HandleClient` фазы, `IntegrationRunLock` и grace-тест (e18)
  остаются; новый тест host-timeout — soft-семантика.
- **v20 P1 (guest influence) в npc-e19 не входит** и в прогоне A–H не
  проверяется. Wire v20 реализован в ветке `feature/v20-p1` (0x23/0x24/0x26,
  protocol bump v19→v20), серверный релей — U2; host apply/guest capture (U3/U4)
  ещё в работе. Отдельный **блок I1–I10** (раздел 9) выполняется **только
  после** U3/U4 и деплоя пары `dist\npc-e20`.

**Текущий статус:** **оба ПК задеплоены npc-e19** — локальный ПК и ARINA
(`\\ARINA\Red Dead Redemption 2\...`) имеют одинаковые хеши ASI/server,
сверено `Get-FileHash`. Canonical CI на момент снапшота: **42/42**
integration + C++ 7/7 + restore + quality strict PASS (`build-server-ci`);
независимая проверка `architect-verify2`: fresh build 0 warnings, хеши
canonical/`dist\npc-e19` совпали, вердикт «можно тестировать/деплоить».
Полный двух-ПК игровой прогон A–G ещё **не пройден** — пользователь запускает
его сейчас. Замечания low/info: подпись blip, плотный трафик, wheel A/B.
Без одинаковой пары на обоих ПК приёмка не начинается.

**v20 P1 (guest influence) — НЕ задеплоено:** ветка `feature/v20-p1`
(protocol **v20**, wire 0x23/0x24/0x26 + релизный релей; integration 50/50,
C++ 8/8). После U3/U4 будет собран `dist\npc-e20`; до этого деплой **не
делать** (v20 bump ломает v19 JOIN). INI v20 P1 станет **32 приёмочных
ключа / 42 строки** (+`vehicle_interaction`, `vehicle_interaction_mission`,
`vehicle_interaction_host_vehicle`, `vehicle_interaction_debug`). Чек-лист
этой пары — **блок I (раздел 9)**; откат — `dist\npc-e19` полной парой.

---

## 0. Подготовка

### 0.1. Версии, хеши, протокол

На **каждом** ПК:

```powershell
Get-FileHash "E:\Games\Red Dead Redemption 2\scripts\rdr2_coop.asi" -Algorithm SHA256
Get-FileHash "E:\rdr2-coop\build-server\Release\rdr2_coop_server.exe" -Algorithm SHA256
```

Ожидаемо:

| Компонент | Версия/хеш | Где |
|---|---|---|
| ASI | npc-e19, `DD0EE210…` (SHA256 как в `dist\npc-e19\README.md`) | оба ПК: `...\scripts\rdr2_coop.asi` |
| Server | v19, `DD9BD9D8…` (без изменений с e18: soft host migration, HandleClient phases) | ПК-хост: `E:\rdr2-coop\build-server\Release\rdr2_coop_server.exe` (ARINA: `coop-server\`) |
| Protocol | **v19** (`kProtocolVersion=19`) | `net/protocol.h` / JOIN в логе |
| INI | `dist\npc-e19\rdr2-coop.ini.local` как эталон; **28 ключей** приёмки (38 строк) | оба ПК |

Если хеш/протокол не совпал — **стоп**, откатиться на `dist\npc-e19`
(см. блок H, §8) и не начинать прогон.

**Для блока I (v20 P1)** — другая пара: `dist\npc-e20` (protocol **v20**,
ASI+server, оба ПК), INI **32/42** (28 ключей + 4 `vehicle_interaction*`).
До деплоя `npc-e20` блок I не выполняется; сейчас задеплоена пара npc-e19/v19
и прогон A–H идёт на ней.

### 0.2. INI: 28 ключей приёмки

Ключи подключения/идентичности (задаются отдельно, **не входят** в 28
игровых):

```ini
server_host = 192.168.1.103   ; ПК с сервером
server_port = 7777
player_name = Player1         ; на втором ПК — Player2 (ники разные)
ped_model   = 0xCF7E73BE
```

**28 ключей игровых настроек — выставить на обоих ПК одинаково:**

| # | Ключ | Значение | Зачем |
|---|---|---|---|
| 1 | `world_sync` | `0` | полный P1 не включаем; E3d-a живёт без него |
| 2 | `world_vehicles` | `1` | E3d-a транспорт хоста |
| 3 | `npc_vehicle_radius` | `80` | радиус транспорта (20..200) |
| 4 | `world_trains` | `0` → `1` только в D | E3d-b mission-train |
| 5 | `train_debug` | `0` | только диагностика поездов |
| 6 | `npc_actor` | `1` | включить NPC-репликацию |
| 7 | `npc_debug` | `1` | `actor:`/`probe:`/`perf:`/`world:` логи |
| 8 | `npc_cap` | `12` | 8–12 NPC в городе |
| 9 | `npc_radius` | `60` | радиус discovery |
| 10 | `npc_animal_cap` | `4` | животные/лошади |
| 11 | `npc_corpse_max_sec` | `300` | трупы E5.2 |
| 12 | `npc_appearance` | `1` | E2c-b внешность human |
| 13 | `npc_damage` | `1` | E5 host-authoritative урон |
| 14 | `npc_presence` | `1` | E6 presence-суффикс |
| 15 | `npc_visitor` | `1` | E7.1 visitor mode |
| 16 | `npc_visitor_vehicles` | `1` | подавлять локальный трафик гостя |
| 17 | `npc_visitor_preserve_events` | `1` | сохранять продавцов/квест/события |
| 18 | `npc_visitor_pause_scenes` | `1` | не чистить толпу в миссии/катсцене |
| 19 | `npc_occupants` | `1` | E3d-c водители/пассажиры/draft |
| 20 | `npc_animal_appearance` | `0` → `1` только в E | E9 animal appearance |
| 21 | `npc_mount_appearance` | `0` → `1` только в E | E10 mounted horse |
| 22 | `npc_frame_hz` | `15` | adaptive-частота кадров |
| 23 | `npc_frame_adaptive` | `1` | 10 Гц покой / 15 Гц движение |
| 24 | `npc_motion_update` | `1` | E7.2 in-place update |
| 25 | `npc_motion_marker` | `0` | E7.2 marker fallback (A2) |
| 26 | `player_frame_hz` | `20` | P0: Player Frame 10..60 Гц |
| 27 | `player_frame_change_only` | `1` | P0: покой heartbeat 5 Гц |
| 28 | `npc_vehicle_wheels` | `0` | **e19:** A/B спин колёс guest vehicle-прокси (SIGNATURE-ONLY); `1` — только отдельный прогон, потом вернуть `0` |

Дополнительно (не входят в 28): `action_sync=0`, `vehicle_sync=0`,
`mission_sync=0`, `mission_credit=0`, `save_backup=1`,
`mission_reward_log=1` — как в снапшоте.

**Проверка e19:** живые INI на **обоих** ПК — **38 строк** = 28 приёмочных +
6 служебных + 4 ключа подключения; новый ключ — `npc_vehicle_wheels = 0`
(default; `1` — только для A/B в G5/отдельном прогоне), проверено на local и
ARINA. **Оба эталона снапшота** — `dist\npc-e19\rdr2-coop.ini.local` и
`.ini.arina` — по 38 строк, включая `npc_vehicle_wheels` (снапшот переснят
после правки INI). При откате/передеплое **сверить 28-й ключ** (если
откатываетесь на e18 — убрать `npc_vehicle_wheels`, там 37 строк). Остальные
ключи не менялись; `npc_motion_status_p2` в файле не выписан, но поддержан
`src/config.cpp` (default 1) — это не новый ключ. Сверено с `src/config.cpp`
на `ef49ac8`: новых ключей, кроме wheel, нет.

После правки INI **перезапустить игру** на обоих ПК (конфиг читается при
старте). Если меняли `world_trains`/`npc_animal_appearance`/
`npc_mount_appearance` — рестарт обязателен.

### 0.3. Порядок запуска и сервер

1. Закрыть старые RDR2/сервер; проверить, что порт свободен:
   ```powershell
   Get-NetTCPConnection -LocalPort 7777 -State Listen -ErrorAction SilentlyContinue
   ```
2. Запустить сервер на ПК-хосте **в видимом окне**:
   ```powershell
   E:\rdr2-coop\build-server\Release\rdr2_coop_server.exe 7777
   ```
   В консоли должно быть `Host game-timeout: 4 s (idle timeout: 30 s)`;
   в логе при миграции — `[!] host game-timeout (N ms), promoting remaining`,
   затем `[HOST] <имя> повышен до хоста`.
   Grace 15 с до первого game-сообщения хоста в баннер не выводится, но
   действует (проверяется в F3): загрузка/меню до 15 с роли не дёргают.
3. Запустить RDR2 → Story Mode на **хосте первым**, зайти в сессию
   (`is_host=1`); дождаться JOIN_OK/подключения к серверу. Загрузка уровня/
   меню может занять >4 с — это нормально, миграции быть не должно
   (`host game-timeout` только после первого game-сообщения и ещё 4 с тишины).
4. Запустить RDR2 на **госте вторым**, проверить `is_host=0` и отсутствие
   `VersionMismatch`.
5. На сервере — ровно два игрока; в `players.txt` появляются записи; в
   обоих логах есть JOIN/FRAME и `npc_actor=1`.
6. Записать время старта, FPS baseline на обоих ПК; сохранить копии
   `scripts\rdr2-coop.log` и `server_console.log` как «старт».

---

## 1. Блок A — базовый мир/NPC (S1–S21 + e19 blip A12)

Общее: оба игрока в городе/у поселения, `npc_debug=1`. На каждую строку —
действие, ожидаемый результат, что смотреть, что прислать при fail.

| # | Действие | Ожидаемый результат | Логи/проверка | При fail прислать |
|---|---|---|---|---|
| A1 (S1) | Походить по толпе 2–3 мин; хост и гость рядом | Гость видит 8–12 NPC-прокси; спавн без «попов», actorId уникальны | `actor: spawn`, `actor: proxies=`, `actor: snapshot` | оба лога (участок 2–3 мин), видео |
| A2 (S2) | Движение/бег среди NPC, смена направления | NPC идут плавно, heading доворачивается, нет прыжков >2 м/телепортов | `actor: motion`, `actor: teleport` (только respawn) | guest/host log, видео 10–20 с |
| A3 (S10) | Подойти к NPC с E2c-b внешностью, сравнить A/B | Одежда/asset/tint совпадают; без мерцания | host `asset capture count=… valid=…`; guest `appearance apply` | скриншоты host/guest, `asset capture` |
| A4 (S12) | Сценарий (курение/стояние), затем убить NPC | Сценарий тот же; труп остаётся, без «двух курений» в одной точке | `actor: scenario`, `actor: death`/`despawn reason=2` | видео, `scenario`-лог |
| A5 (S11) | Найти NPC-всадника; спровоцировать dismount | rider+mount — пара, без дублей; после dismount без залипаний | `mount spawn`, `despawn`, `actor: motion mounted` | видео, оба лога |
| A6 (S18) | Убить human/animal/horse на хосте | Труп держится у гостя, пока host-труп существует/до 300 с; не висит вечно | `actor: corpse hold`, `actor: corpse despawn reason=dead` | оба лога, видео |
| A7 (S16) | Гость стреляет в human/animal-прокси | Урон применяет **только хост**: damage→hp_left→killed; прокси до смерти invincible | guest `damage send`, host `damage apply`, `killed … by damage` | оба лога, видео |
| A8 (S17) | Гость стреляет в голову прокси | `damage=250`, `headshot=1` → ваншот; body-урон 35/60/80/30 | `damage … headshot=1`, `killed` | оба лога, видео |
| A9 (S19) | Гость открывает карту/паузу/сатчель/магазин, затем закрывает | У имени хоста суффикс `in map`/`pause menu`/`in satchel`/`in shop`; после закрытия/12 с исчезает | host `ui: pause=…`, суффикс имени; presence heartbeat | скрины A/B, host log |
| A10 (S0/S9) | Baseline gating: `npc_actor=0` на обоих, рестарт; затем 1 | При 0 — **0** строк `actor:`/`probe:` и прежний FPS; при 1 — NPC снова есть | `npc_actor=0` логи/сервер | оба лога, `scan: fps` |
| A11 (S21) | 1–2 мин наблюдать движение NPC/прокси у хоста и гостя | Нет snap/телепортов (кроме respawn), движение плавное, стоп/старт без рывков | `actor: motion`, `actor: teleport` (только respawn) | видео 20–30 с, `actor:`-строки |
| A12 (e19) | Открыть карту/легенду: у второго игрока подпись метки | Подпись — **имя игрока** (`player_name`), а не «Спутник»/«Companion»; метка/цвет остаются friendly; после rejoin/respawn подпись не пропадает | guest/host `coop: blip name '<имя>'`; скрин карты/легенды; визуально | скрин host+guest карты, `coop: blip name` строки |

---

## 2. Блок B — visitor mode (S22)

Условия: `npc_visitor=1`, `npc_visitor_vehicles=1`,
`npc_visitor_preserve_events=1`, `world_vehicles=1`; гость в городе.

| # | Действие | Ожидаемый результат | Логи/проверка | При fail прислать |
|---|---|---|---|---|
| B1 | Гость стоит в городе; сравнить с хостом | У гостя нет **локальной** ambient-толпы; видны NPC хоста; нет спавна/удаления на глазах | `actor: visitor mode on`, `visitor cleanup removed=N`, `actor: proxies` | video 20–30 с, guest log |
| B2 | Подойти к продавцу/квестовому/событийному NPC | Эти NPC остаются, не удаляются cleanup | `visitor cleanup`, `npc_visitor_preserve_events` | guest log, видео |
| B3 | Войти в миссию/катсцену, затем выйти | Cleanup толпы **паузится** (`visitor scene pause on`), density/road suppression продолжаются; после выхода cleanup возобновляется | `actor: visitor scene pause on/off mission=… cutscene=…` | guest log, видео |
| B4 | `npc_visitor=0` на обоих, рестарт | Возвращается прежняя локальная толпа/трафик | guest `actor: visitor mode off` (или нет on) | guest log, видео |
| B5 (S20/e18) | Подойти к продавцу (vendor) в городе; сравнить host/guest | У продавца **одна** копия: гость не спавнит host-прокси продавца (e18 dedupe), локальный продавец сохранён | guest throttled `actor: visitor vendor skip id=… model=0x…` (не ошибка); визуально нет дубля/мерцания | guest/host log, видео продавца |

---

## 3. Блок C — транспорт, водители, тягловые (S25)

Условия: `world_vehicles=1`, `npc_vehicle_radius=80`, `npc_occupants=1`,
`npc_visitor_vehicles=1`; хост садится за руль/пассажиром; отдельно —
повозка с draft-лошадьми.

| # | Действие | Ожидаемый результат | Логи/проверка | При fail прислать |
|---|---|---|---|---|
| C1 | Хост едет на машине/повозке; гость смотрит | Гость видит тот же транспорт в радиусе ~80 м; до 16 машин; при уходе — proxy despawn | `world: vehicles send n=`, `world: vehicle spawn/despawn` | оба лога, видео |
| C2 | Хост за рулём, затем пассажиром | Водитель/пассажиры **seated** в тех же местах, без T-pose/провала/дублей human-прокси | `world: occupants vehicle=… n=… driver=…`, `actor: seat attach … ok=` | host/guest логи, видео |
| C3 | Повозка с тягловыми лошадьми | Draft-лошади прицеплены, не спавнятся отдельными прокси | `actor: draft attach … ok=`, `occupants errors=0` | логи, видео |
| C4 | Движение транспорта/повороты | Плавно (60 Гц interpolation), adaptive 15 Гц при движении; нет рывков/сквозного телепорта | `actor:`/`world:` без warnings; сравнить визуально | видео 20–30 с |
| C5 | `npc_occupants=0` на обоих, рестарт | Старое E3d-a: машины без водителей/пассажиров, без ошибок | `npc_occupants=0`, `world: occupants` отсутствует | guest log |
| C6 (S25/e19) | Повозка с кучером и тягловыми лошадьми; плотный трафик | У гостя **кучер** и **draft-лошади** видны seated/в упряжке, без T-pose/дублей; на плотном трафике нет висящего «driver без машины» | `world: occupants vehicle=…`, `actor: seat attach … ok=`, `actor: draft attach … ok=`, `actor: occupants errors=0` | host/guest log, видео повозки и плотной улицы |

---

## 4. Блок D — поезда (S26)

Условия: `world_trains=0` → **`1` на обоих ПК**, рестарт; `npc_actor=1`,
`world_vehicles=1`, `world_sync=0`, `npc_debug=1`. Это тестовый opt-in.

| # | Действие | Ожидаемый результат | Логи/проверка | При fail прислать |
|---|---|---|---|---|
| D1 | Хост у станции/на поезде; гость смотрит | Гость видит **один** mission-train на рельсах; вагоны в правильном порядке, локального второго поезда нет | `world: train send config=… cars=…`, guest `world: train spawn … ok=1` | оба лога, видео |
| D2 | Хост едет, останавливается у станции, разворачивается | Совпадают скорость/направление/station/reversing; нет скачков >25 м/|dz|>15 | `world: train send … speed=… state=…` | guest/host log, видео |
| D3 | E11 (в базе npc-e13): точные пассажиры поезда через 0x21 carIndex | Пассажиры поезда (driver/passenger в carIndex) видны seated в тех же вагонах/местах, без T-pose/дублей; если пассажиров нет — проверить, что их действительно нет (лог skip/пустой occupant list), молчаливый пропуск — fail | `world: occupants vehicle=…`, `actor: seat attach … ok=`, `world: train send … cars=…` | оба лога, видео |
| D4 | Выключить `world_trains=0` / выйти / role change | Mission-train удаляется; `SET_RANDOM_TRAINS(true)` восстанавливает random trains; второй поезд не остаётся | `world: train despawn reason=…`; в логе нет повторного spawn | guest log, видео |

---

## 5. Блок E — внешность E9/E10 (S27 + S28)

Базово оба флага `0` (random); для блока включить **`npc_animal_appearance=1`**
и **`npc_mount_appearance=1` на обоих ПК**, рестарт. `npc_debug=1`.

| # | Действие | Ожидаемый результат | Логи/проверка | При fail прислать |
|---|---|---|---|---|
| E1 (S27) | Хост в конюшне/на улице с лошадьми; гость сравнивает | Масть/грива/хвост/седло совпадают; животные — окрас/мех; нет invisible/T-pose/заморозки | host `actor: animal appearance probe model=… type=… comps=…`; guest `animal appearance apply … ok=`; `horse tack apply … ok=` | оба лога, видео, probe-строки |
| E2 (S27) | `npc_animal_appearance=0` на обоих, рестарт | Животные/лошади снова random, не залипают в старом варианте; без дублей после tack | guest log, визуал | guest log, видео |
| E3 (S28) | NPC-всадник (лошадь под NPC); гость смотрит | Лошадь под всадником совпадает с хостом; нет invisible/T-pose/дублей | host `actor: mount outfit send actor=… model=… items=…`; guest `mount outfit apply … ok=1`; `guest mount outfit applied/failed` | оба лога, видео |
| E4 (S28) | Поздний mount/reseat; затем `npc_mount_appearance=0` | Внешность применяется с retry/heartbeat; при 0 — random, без залипания | `mount outfit apply`, retry/ok=0 при отсутствии mount | guest log, видео |

Важно: флаги E9/E10 должны совпадать на host+guest; при расхождении
поведение несимметрично — это fail.

---

## 6. Блок F — host migration (S23) + soft migration e18/e19

Условия: активны транспорт/occupants/поезд (если проверяли) и visitor;
`npc_debug=1`. С e18 миграция по game-timeout **мягкая** (без закрытия
сокета старого хоста); обычный выход хоста (Alt+F4/quit) — по-прежнему
Leave → PlayerLeft → promote. Логика сервера в e19 **не менялась** (server
`DD9BD9D8` тот же, что в e18) — блок F проверяет её на e19-паре.

| # | Действие | Ожидаемый результат | Логи/проверка | При fail прислать |
|---|---|---|---|---|
| F1 | Хост Alt+F4/quit (обычный выход) | Гость становится HOST; старые прокси/транспорт/поезда/occupants очищены; role-change reset; старый хост rejoin — как гость | guest `coop: role change -> HOST reset complete`; server PlayerLeft+HostChanged; нет дублей | оба лога, server console, видео |
| F2 | Хост «зависает» (только Ping) после хотя бы одного game-сообщения | Через ~4 с сервер: `[!] host game-timeout (N ms), promoting remaining` + `[HOST] … повышен до хоста`; гость становится HOST (`HostChanged(1)`, `reset complete`), старый хост получает `HostChanged(0)` и `GUEST reset complete`; **сокет старого хоста не закрыт**, `PlayerLeft` нет; Ping старого хоста → Pong работает; нет reconnect-шторма/флапа ролей | server log; guest/host `HostChanged`; `Select-String -Pattern "host game-timeout|promoting remaining|PlayerLeft|reconnect"` | server log, guest+host логи, видео |
| F3 | Хост на загрузке/в меню: playerPed ещё нет, идут только presence heartbeat (~2 с) | **Демоции НЕТ**: join grace 15 с + presence считается game-thread liveness; в server log нет `host game-timeout`; после загрузки идут Frame, роли стабильны (гость остаётся гостем) | server log (нет `host game-timeout`); host `perf:`/FRAME после загрузки; отсутствие серии JOIN/reconnect | server log, оба лога, видео загрузки |
| F4 | Rejoin старого хоста после миграции | Заходит как гость, без старых прокси/транспорта; новый хост отдаёт свой мир; если старый хост после «зависания» ожил — он остаётся гостем (не забирает роль обратно) | JOIN_OK `is_host=0`; `actor: reset`/`resync`; нет дублей; в server log один `[HOST]` | оба лога, видео |

**Как проверить мягкую миграцию/загрузку (S31):**

1. Сервер в видимом окне, `server_console.log` копируется в папку прогона.
2. F2: хост заходит в игру, ходит 5–10 с (game-сообщения идут), затем
   «замораживается» (диспетчер задач → Suspend, или физически отключить
   игровой поток). Гость наблюдает: через ~4 с он HOST; в server log —
   `host game-timeout` и `promoting remaining`; **строки `PlayerLeft` для
   хоста нет**; `Get-NetTCPConnection` на сервере по-прежнему видит сокет
   старого хоста. После «оживления» хоста — в его логе `role change -> GUEST
   reset complete`.
3. F3: перезапустить RDR2 на хосте и наблюдать загрузку 30–60 с с сервером:
   до появления playerPed клиент шлёт presence каждые ~2 с; в логе сервера
   **нет** `host game-timeout`, роли не меняются. Проверить, что гость в это
   время не «прыгает» в HOST.
4. Ни в одном из сценариев F2/F3 не должно быть серии подряд
   `JOIN`/`reconnect` (reconnect-шторм) и повторных `HostChanged` туда-обратно
   (роль-флап); максимум одна миграция за сценарий.

---

## 7. Блок G — производительность (30–40 мин)

Непрерывная игра 30–40 минут: город → транспорт → поезд (если D) →
внешность; хотя бы один раз проверить host migration. Записывать FPS на
обоих ПК каждые 5–10 минут.

| # | Действие | Ожидаемый результат | Логи/проверка | При fail прислать |
|---|---|---|---|---|
| G1 | 30–40 мин игры без рестарта | FPS host/guest стабилен относительно baseline; без длинных фризов/утечек | FPS-скрины/лог; `perf: frame host tick=… p95=… max=…`; `perf: frame guest …` каждые ~5 с | FPS-замеры, оба лога |
| G2 | Проверить бюджет полного кадра | `perf: frame host` p95 **≤4.0 мс**, max **≤10.0 мс**; guest p95/max — те же; `actor` p95 ≤**2.0 мс**; `perf: npc warn` = 0; guest timeouts ≤0.5%, appearance failed ≤1% | grep `perf: frame`, `perf: npc`; снять 5–10 строк за 30–40 мин | host/guest `perf: frame` выдержки, `perf: npc warn` |
| G3 | Проверить трафик (server + client) | Типично **≤10–12 КБ/с** на клиента, worst **≤15 КБ/с**; нет роста `bytes_in/out` вне нормы; `bytes=`/`proxies` не копятся | server `net: bytes_in=… bytes_out=… | frame=… actor=… world=…` каждые ~10 с; client `perf: net tx=… KB/s rx=… KB/s msgs_tx=…` | 2–3 heartbeat server, 2–3 `perf: net`, FPS |
| G4 | Ошибки/десинк | Нет `ERROR`, `warn`, `VersionMismatch`, «вечных» `ok=0`; трупы/прокси не копятся. Допустимо/ожидаемо: одна пара `host game-timeout`/`promoting remaining` (только если F2 инъецировали), `actor: visitor vendor skip` (e18 dedupe, не ошибка), `coop: blip name` (e19), `world: vehicle wheels speed=` (только A/B с `npc_vehicle_wheels=1`), `world: occupants`/`seat attach`/`draft attach` (e19) | grep `actor:|world:|coop:|perf: net|host game-timeout|vendor skip|blip name|wheel` | проблемные строки + видео |
| G5 (e19 regression sweep) | В длинном прогоне (G1) прогнать по разу: **S31** (реконнект/загрузка хоста — свернуть/развернуть игру хоста, не должно быть reconnect-шторма), **S22** (`world: vehicles send`, vehicle-прокси в радиусе), **S25/C6** (occupants/кучер/draft), **S20/B5** (vendors без дублей), **A12** (blip label), а затем отдельный прогон **wheel A/B**: `npc_vehicle_wheels=1` на обоих ПК (колёса крутятся, дрейф допустим) → вернуть `0` (колёса статичны, без регрессий) | Соответствующие логи из G4 + скрины/видео; после A/B — чистое `npc_vehicle_wheels=0` на обоих и рестарт игры | video/S31-логи, `coop: blip name`, `world: occupants`, `world: vehicle wheels speed=` |

**Как снимать G2/G3:**

1. Включить `npc_debug=1` на обоих ПК и `player_frame_hz=20`,
   `player_frame_change_only=1`; прогреть 1–2 минуты.
2. G2: собрать `perf: frame host/guest` строки за 5–10 минут активной игры;
   взять **худшие p95/max** из выдачи (консервативно). `frame` содержит
   `frames_hz` и `tick_hz`; проверить FPS-independence: при 60 и 144 FPS
   (или FPS-limit) `tick_hz` меняется, а p95/max/KB/s — нет. `perf: npc` —
   для детализации actor; порог actor p95 остаётся 2 мс.
3. G3 server: запустить `Select-String` по `server_console.log` (или снять
   из видимого окна) две строки `net: bytes_in=… bytes_out=…` с разницей
   ~10 с; `(bytes_out2-bytes_out1)/1024/10` = КБ/с; проверить per-type
   `frame/actor/world/...`. E13: интервалы wall-clock, трафик не должен
   расти при переходе 60→144 FPS.
4. G3 client: в `rdr2-coop.log` взять 2–3 строки
   `perf: net tx=… rx=… msgs_tx=… msgs_rx=…`; сложить tx+rx как трафик
   клиента; сверить с server bytes_out.
5. FAIL, если устойчиво >15 КБ/с, `perf: npc warn` >0 или
   `perf: frame` p95/max выше бюджета; приложить все выдержки и участок лога.

```powershell
# G2: полный кадр + actor-детализация (оба ПК)
Select-String -Path "E:\Games\Red Dead Redemption 2\scripts\rdr2-coop.log" `
  -Pattern "perf: frame|perf: npc" | Select-Object -Last 20

# G3: клиентский tx/rx
Select-String -Path "E:\Games\Red Dead Redemption 2\scripts\rdr2-coop.log" `
  -Pattern "perf: net" | Select-Object -Last 5

# G3: серверные накопленные байты (взять две строки с разницей ~10 с)
Select-String -Path server_console.log -Pattern "net: bytes_in=" | Select-Object -Last 5
```

Ориентир: `perf: frame host tick=0.42 p95=0.55 max=1.10 …` — норма;
`perf: npc warn host tick=3.40 > 2.0 ms` — FAIL по G2.

---

## 8. Блок H — откат при проблемах

Если блок A–G даёт FAIL, не «лечить» на ходу:

1. Закрыть игры на обоих ПК и остановить сервер.
2. Открыть `dist\npc-e19\README.md` (или `dist\npc-e18\README.md`, если
   откат на предыдущую) — там SHA256, INI и порядок восстановления.
3. ASI: `dist\npc-e19\rdr2_coop.asi` → `E:\Games\Red Dead Redemption 2\scripts\rdr2_coop.asi`.
4. Server: `dist\npc-e19\rdr2_coop_server.exe` → `E:\rdr2-coop\build-server\Release\rdr2_coop_server.exe`
   (и на ARINA в `coop-server\`, если сервер поднимается там).
5. INI: привести к разделу INI снапшота (e19 — **28 ключей, включая
   `npc_vehicle_wheels`**); протокол должен быть v19 на обоих
   ПК; v18↔v19 несовместимы (`VersionMismatch`).
6. Запустить сервер в видимом окне, проверить `Host game-timeout: 4 s`, затем
   зайти хостом/гость и убедиться, что JOIN/FRAME идут.
7. Зафиксировать в отчёте: label снапшота, оба хеша, причину отката.

**Откат e19 → e18/e17/e16** (если e19-фиксы дали регрессию): сервер и ASI —
**только из одного снапшота**. У e19 server **тот же** (`DD9BD9D8`), менялся
только ASI (blip/occupants/wheels) и INI (28-й ключ) — поэтому откат e19→e18
безопасен по серверу, но INI нужно привести к 37 строкам e18 (убрать
`npc_vehicle_wheels`); ASI — из `dist\npc-e18` (`E3FAB30F`). `dist\npc-e17` —
refactor-only без серверных изменений; `dist\npc-e16` — пара
`C4E4CF70`/`1BE28084`. Смешивать пары нельзя.

---

## 9. Блок I — v20 P1 guest influence (I1–I10; только после U3/U4 + deploy `npc-e20`)

**Предусловия (не начинать раньше):** U3 (host apply) и U4 (guest capture)
влиты; собран и задеплоен **снапшот `dist\npc-e20`** (protocol **v20**,
wire 0x23/0x24/0x26) **парой ASI+server** на **оба** ПК (одинаковые хеши);
INI — **32 приёмочных ключа / 42 строки**: к 28 ключам e19 добавлены
`vehicle_interaction = 1`, `vehicle_interaction_mission = 0`,
`vehicle_interaction_host_vehicle = 0`, `vehicle_interaction_debug = 1`
(host+guest одинаково); `npc_actor = 1`; смешанная v19/v20-пара невозможна
(`VersionMismatch`). Server-лог содержит `net: … vehicle=` heartbeat
(новый трафик-бакет). Со стороны U2 доступен fake-клиент:
`python tools\fake_vehicle_client.py --mode scenario` → все wire-проверки
должны быть PASS до 2-ПК прогона (это precheck, а не замена I1–I10).

| # | Сценарий | Pass criteria | Логи/проверка |
|---|---|---|---|
| I1 | Гость стреляет по не-mission wagon (body/wheel) | host real vehicle health падает; у гостя `result=applied`; host видит `vehicle: interaction`; guest mirror body/wheels best-effort; `VehicleState` heartbeat сходится | оба лога: `vehicle: interaction`, `vehicle: state`, `perf: net`; host `GET_VEHICLE_*_HEALTH` |
| I2 | Двери через controls: guest у proxy жмёт attack/enter по двери | control-capture (`IS_CONTROL_*`) → host `DoorOpen/Close/Break`; host+guest дверь в одном состоянии; `doorBits` 0/1/2 (3 unknown); ajar не проверять; lock — `NotAllowed` до подтверждения enum (A/B) | `vehicle: interaction action=door_*`, `VehicleState doorBits` |
| I3 | Гость входит в припаркованный wagon (passenger) | host сажает guest ghost; guest local player в proxy; `VehicleState.guestSeat` совпадает; нет второго vehicle/дубля | `world: occupants`, `vehicle: state guest=…`, `vehicle: interaction action=enter` |
| I4 | Гость hijack не-mission NPC-водителя | NPC выходит/изгнан; guest в driver seat (host ghost); лог `driving disabled`; вождения нет | `vehicle: interaction action=hijack`, `vehicle: guest seat driver` |
| I5 | Mission wagon: damage/hijack | `result=not_allowed` (reason=mission_protected), мир хоста не изменён, mission не ломается | `vehicle: interaction result=not_allowed reason=mission_protected` |
| I6 | Host едет на своей wagon, guest пытается damage/hijack | `result=not_allowed` (reason=host_vehicle при default `vehicle_interaction_host_vehicle=0`), транспорт хоста не изменён | `result=not_allowed` + reason; host log |
| I7 | Loss/dup: отключить сеть на 2–3 с mid-intent (или fake burst) | нет двойного применения (idempotency по seq), state heartbeat чинит, нет дублей damage/seat | `vehicle: interaction seq=…` (один `applied` на seq), `vehicle: state` |
| I8 | Host migration во время P1 (Alt+F4/soft) | интенты/результаты/состояния старого epoch дропаются; новые интенты идут новому хосту; нет краша/утечек; proxy reset | `coop: role change`, `vehicle:`, server `[HOST]` |
| I9 | Rate/anti-grief: fake guest 30 интентов/с | сервер дропает >8/с; host не спайкает; `perf: frame host` p95/max в бюджете G2; при host-бюджете — `rate_limited` | `net: … vehicle=`, `perf: frame host`, fake-клиент счётчики |
| I10 | Collision/tangibility: гость упирается в wagon | локальный proxy solid, host state wins; при уходе/варпе collision off; игрок не застревает внутри | визуально/видео, `world: vehicle spawn/despawn`, guest log |

**Bump/deploy/rollback v20 (обязательно прочитать):**

- v20 — **жёсткий bump**: `kProtocolVersion 19→20`; v19-клиент на v20-сервере
  получает `VersionMismatch`. Deploy — **только парой** ASI+server из одного
  снапшота `dist\npc-e20`, оба ПК с одинаковыми хешами; промежуточные коммиты
  U1–U4 не деплоить.
- Задеплоенная сейчас рабочая пара — **npc-e19/v19** (ASI `DD0EE210`, server
  `DD9BD9D8`, INI 28/38). **Откат:** закрыть игры/сервер → ASI и server из
  `dist\npc-e19` → INI из `dist\npc-e19\README.md` (28 ключей; убрать 4
  `vehicle_interaction_*`) → протокол v19 на обоих ПК.
- В v20 INI **32/42**; при откате на e19 лишние ключи `vehicle_interaction_*`
  игнорируются старым клиентом, но лучше привести INI к снапшоту.
- Диагностика v20 (grep, оба лога + server): `vehicle: interaction`,
  `vehicle: intent`, `vehicle: state`, `rate_limited`, `config: warn vehicle`,
  server `net: … vehicle=`; отсутствие `ERROR`/`warn`, кроме ожидаемых
  policy-reject.

---

## 10. Что прислать при FAIL (общий пакет)

Для каждого FAIL-пункта — отдельная папка `accept-<block>-<item>-<time>/`:

- `rdr2-coop.log` **host** и **guest** (полные или участок ±2 минуты);
- `server_console.log` (окно сервера; особенно game-timeout/PlayerLeft/promoting
  remaining и `[HOST] … повышен до хоста`);
- оба `rdr2-coop.ini` (можно замазать IP/ник, но ключи сохранить);
- `Get-FileHash` ASI/server на обоих ПК и `git rev-parse HEAD`;
- FPS/скриншоты или OBS-видео 20–30 с (что именно видно);
- точное время, локация, блок/пункт (A1…I10), ожидаемое/фактическое;
- `Select-String` выдержки: `actor:`, `world:`, `perf: frame`, `perf: net`,
  `perf: npc`, `coop:`, `host game-timeout`, `visitor vendor skip`,
  `coop: blip name`, `world: occupants`, `world: vehicle wheels`; для v20
  P1 — `vehicle: interaction|intent|state`, `rate_limited`, `config: warn
  vehicle`; для server — строки `net: bytes_in=` (в v20 — с `vehicle=`);
  при проверке конфига — `config: warn` (в нормальном прогоне быть не должно);
- если откатывались — `dist\<label>` и хеши восстановленной пары.

---

## 11. Итоговая таблица (заполняет пользователь)

| Блок | Пункты | PASS/FAIL | Комментарий/время |
|---|---|---|---|
| A — базовый NPC (+blip e19) | A1–A12 |  |  |
| B — visitor (+vendors e18) | B1–B5 |  |  |
| C — транспорт/occupants (+wagon e19) | C1–C6 |  |  |
| D — поезда | D1–D4 |  |  |
| E — внешность E9/E10 | E1–E4 |  |  |
| F — host migration (soft e18/e19) | F1–F4 |  |  |
| G — производительность + e19 sweep | G1–G5 |  |  |
| H — откат (если был) | — |  |  |
| I — v20 P1 guest influence (после npc-e20) | I1–I10 |  |  |

**Критерий общего PASS:** все блоки PASS, нет FAIL по G2/G3 и F; E11
точные пассажиры поездов проверяются в D3 (входит в базу). Обязательное
условие PASS: F2/F3 подтверждают мягкую миграцию без reconnect-шторма и без
демоции хоста во время загрузки; G5 — blip label корректный, кучер/draft
повозки без дублей/T-pose, vendors без дублей, wheel A/B выполнен
(`npc_vehicle_wheels=1`→`0`, после — статичные колёса). **Блок I** —
только для пары v20 P1 (`dist\npc-e20`), после U3/U4; до её деплоя блок I
не заполняется. Пара уже задеплоена на оба ПК; если хеши на ARINA/local не
совпали — приёмка не начата, таблица не заполняется.

---

## 12. Связанные документы

- [NPC_E1_TESTING.md](NPC_E1_TESTING.md) — детальные S0–S32 и методики.
- [PERF.md](PERF.md) — пороги/разбор `perf: npc` (v20: серверный бакет
  `vehicle=`).
- [TRAINS.md](TRAINS.md) — mission-train S26.
- [INTERACTIVITY.md](INTERACTIVITY.md) — v20 design guest influence (P0–P4).
- [INTERACTIVITY_P1.md](INTERACTIVITY_P1.md) — v20 P1 spec/статус unit'ов
  U1–U5, блок I, fake-клиент, bump/deploy.
- [NATIVE_RESEARCH_INTERACT.md](NATIVE_RESEARCH_INTERACT.md) — native R&D
  (blip name и opt-in wheel spin реализованы в e19; vehicle interactivity — v20).
- [../README.md](../README.md#откат-на-предыдущую-сборку) — снапшоты/откат.
- [../tools/README.md](../tools/README.md#снапшоты-сборокоткат) — `snapshot_build.ps1`.
