# Финальная приёмка 2-ПК — RDR2 CO-OP v19 / npc-e10

**Назначение:** пошаговый приёмочный сценарий для пользователя + ARINA (второй
ПК) после деплоя v19. Проверяем не отдельные unit-тесты, а связку
«игра-хост + игра-гость» в реальном мире: NPC, visitor, транспорт, поезда,
внешность, миграцию хоста и 30–40 минут стабильной игры.

**Версия:** protocol **v19**, сборка **npc-e10**: ASI
`B8BA1B93C5F6B68BFFE612F4408C9626E8E669294446E385D27C880081D1C88B`,
server `E26C715A...`; оба ПК — **одна и та же** сборка/снапшот
`dist\npc-e10`. **E11** (точные пассажиры поездов) в работе и в приёмку
v19/npc-e10 не входит; S26 проверяет текущий mission-train без E11.

**Текущий статус:** ARINA offline — сначала задеплоить на неё
`dist\npc-e10` и сверить хеши. Локальная пара ASI `B8BA1B93` + server
`E26C715A`. Без одинаковой пары на обоих ПК приёмка не начинается.

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
| ASI | npc-e10, `B8BA1B93…` (SHA256 как в `dist\npc-e10\README.md`) | оба ПК: `...\scripts\rdr2_coop.asi` |
| Server | v19, `E26C715A39E2CA4F590DE625C561D8B025A905A215FBC44E4F713348D2EB3551` | ПК-хост: `E:\rdr2-coop\build-server\Release\rdr2_coop_server.exe` |
| Protocol | **v19** (`kProtocolVersion=19`) | `net/protocol.h` / JOIN в логе |
| INI | `dist\npc-e10\rdr2-coop.ini.local` как эталон | оба ПК |

Если хеш/протокол не совпал — **стоп**, откатиться на `dist\npc-e10`
(см. блок H, §8) и не начинать прогон.

### 0.2. INI: 25 ключей приёмки

Ключи подключения/идентичности (задаются отдельно, **не входят** в 25
игровых):

```ini
server_host = 192.168.1.103   ; ПК с сервером
server_port = 7777
player_name = Player1         ; на втором ПК — Player2 (ники разные)
ped_model   = 0xCF7E73BE
```

**25 ключей игровых настроек — выставить на обоих ПК одинаково:**

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

Дополнительно (не входят в 25): `action_sync=0`, `vehicle_sync=0`,
`mission_sync=0`, `mission_credit=0`, `save_backup=1`,
`mission_reward_log=1` — как в снапшоте.

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
   В консоли должно быть `Host game-timeout: 4 s (idle timeout: 30 s)`.
3. Запустить RDR2 → Story Mode на **хосте первым**, зайти в сессию
   (`is_host=1`); дождаться JOIN_OK/подключения к серверу.
4. Запустить RDR2 на **госте вторым**, проверить `is_host=0` и отсутствие
   `VersionMismatch`.
5. На сервере — ровно два игрока; в `players.txt` появляются записи; в
   обоих логах есть JOIN/FRAME и `npc_actor=1`.
6. Записать время старта, FPS baseline на обоих ПК; сохранить копии
   `scripts\rdr2-coop.log` и `server_console.log` как «старт».

---

## 1. Блок A — базовый мир/NPC (S1–S21 кратко)

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

---

## 4. Блок D — поезда (S26)

Условия: `world_trains=0` → **`1` на обоих ПК**, рестарт; `npc_actor=1`,
`world_vehicles=1`, `world_sync=0`, `npc_debug=1`. Это тестовый opt-in.

| # | Действие | Ожидаемый результат | Логи/проверка | При fail прислать |
|---|---|---|---|---|
| D1 | Хост у станции/на поезде; гость смотрит | Гость видит **один** mission-train на рельсах; вагоны в правильном порядке, локального второго поезда нет | `world: train send config=… cars=…`, guest `world: train spawn … ok=1` | оба лога, видео |
| D2 | Хост едет, останавливается у станции, разворачивается | Совпадают скорость/направление/station/reversing; нет скачков >25 м/|dz|>15 | `world: train send … speed=… state=…` | guest/host log, видео |
| D3 | E11 (позже) — точные пассажиры поезда | Не входит в v19/npc-e10; отметить «E11 not yet», не fail | — | — |
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

## 6. Блок F — host migration (S23)

Условия: активны транспорт/occupants/поезд (если проверяли) и visitor;
`npc_debug=1`.

| # | Действие | Ожидаемый результат | Логи/проверка | При fail прислать |
|---|---|---|---|---|
| F1 | Хост Alt+F4/quit (обычный выход) | Гость становится HOST; старые прокси/транспорт/поезда/occupants очищены; role-change reset; старый хост rejoin — как гость | guest `coop: role change -> HOST reset complete`; server PlayerLeft+HostChanged; нет дублей | оба лога, server console, видео |
| F2 | Хост «зависает», шлёт только Ping | Через ~4 с сервер: `[!] host game-timeout (N ms), promoting remaining`; гость становится HOST; Ping не продлевает таймаут | server log, guest `coop: role change -> HOST` | server log, guest log |
| F3 | Rejoin старого хоста после миграции | Заходит как гость, без старых прокси/транспорта; новый хост отдаёт свой мир | JOIN_OK `is_host=0`; `actor: reset`/`resync`; нет дублей | оба лога, видео |

---

## 7. Блок G — производительность (30–40 мин)

Непрерывная игра 30–40 минут: город → транспорт → поезд (если D) →
внешность; хотя бы один раз проверить host migration. Записывать FPS на
обоих ПК каждые 5–10 минут.

| # | Действие | Ожидаемый результат | Логи/проверка | При fail прислать |
|---|---|---|---|---|
| G1 | 30–40 мин игры без рестарта | FPS host/guest стабилен относительно baseline; без длинных фризов/утечек | FPS-скрины/лог; `perf: npc host tick=… p95=… max=…`; `perf: npc guest tick=… p95=…` | FPS-замеры, оба лога |
| G2 | Проверить бюджет | host/guest tick p95 **≤ 2.0 мс**, `perf: npc warn` = 0; guest timeouts ≤0.5%, appearance failed ≤1% | `perf: npc` строки, `perf: npc warn` | host/guest `perf:` выдержки |
| G3 | Проверить трафик/прокси | Суммарный трафик ~≤15 КБ/с на клиента (штатно), `bytes=` не растёт бесконечно; `proxies`/`actors` в пределах cap | `perf: npc host actors=… bytes=…/s`, guest `proxies=… timeouts=…` | 2–3 `perf:` строки, FPS |
| G4 | Ошибки/десинк | Нет `ERROR`, `warn`, `VersionMismatch`, «вечных» `ok=0`; трупы/прокси не копятся | grep `actor:|world:|coop:` | проблемные строки + видео |

Ориентир: `perf: npc host tick=0.42 p95=0.55…` — норма; warning вида
`perf: npc warn host tick=3.40 > 2.0 ms` — FAIL по G.

---

## 8. Блок H — откат при проблемах

Если блок A–G даёт FAIL, не «лечить» на ходу:

1. Закрыть игры на обоих ПК и остановить сервер.
2. Открыть `dist\npc-e10\README.md` (или `dist\npc-e9\README.md`, если
   откат на предыдущую) — там SHA256, INI и порядок восстановления.
3. ASI: `dist\npc-e10\rdr2_coop.asi` → `E:\Games\Red Dead Redemption 2\scripts\rdr2_coop.asi`.
4. Server: `dist\npc-e10\rdr2_coop_server.exe` → `E:\rdr2-coop\build-server\Release\rdr2_coop_server.exe`.
5. INI: привести к разделу INI снапшота; протокол должен быть v19 на обоих
   ПК; v18↔v19 несовместимы (`VersionMismatch`).
6. Запустить сервер в видимом окне, проверить `Host game-timeout: 4 s`, затем
   зайти хостом/гость и убедиться, что JOIN/FRAME идут.
7. Зафиксировать в отчёте: label снапшота, оба хеша, причину отката.

---

## 9. Что прислать при FAIL (общий пакет)

Для каждого FAIL-пункта — отдельная папка `accept-<block>-<item>-<time>/`:

- `rdr2-coop.log` **host** и **guest** (полные или участок ±2 минуты);
- `server_console.log` (окно сервера; особенно game-timeout/PlayerLeft);
- оба `rdr2-coop.ini` (можно замазать IP/ник, но ключи сохранить);
- `Get-FileHash` ASI/server на обоих ПК и `git rev-parse HEAD`;
- FPS/скриншоты или OBS-видео 20–30 с (что именно видно);
- точное время, локация, блок/пункт (A1…H), ожидаемое/фактическое;
- `Select-String` выдержки: `actor:`, `world:`, `perf: npc`, `coop:`;
- если откатывались — `dist\<label>` и хеши восстановленной пары.

---

## 10. Итоговая таблица (заполняет пользователь)

| Блок | Пункты | PASS/FAIL | Комментарий/время |
|---|---|---|---|
| A — базовый NPC | A1–A10 |  |  |
| B — visitor | B1–B4 |  |  |
| C — транспорт/occupants | C1–C5 |  |  |
| D — поезда | D1–D4 |  |  |
| E — внешность E9/E10 | E1–E4 |  |  |
| F — host migration | F1–F3 |  |  |
| G — производительность | G1–G4 |  |  |
| H — откат (если был) | — |  |  |

**Критерий общего PASS:** все блоки PASS, нет FAIL по G2/G3 и F; E11
точные пассажиры поездов отмечены как «deferred» (не FAIL для npc-e10).
Если ARINA offline — приёмка не начата, таблица не заполняется.

---

## 11. Связанные документы

- [NPC_E1_TESTING.md](NPC_E1_TESTING.md) — детальные S0–S28 и методики.
- [PERF.md](PERF.md) — пороги/разбор `perf: npc`.
- [TRAINS.md](TRAINS.md) — mission-train S26.
- [../README.md](../README.md#откат-на-предыдущую-сборку) — снапшоты/откат.
- [../tools/README.md](../tools/README.md#снапшоты-сборокоткат) — `snapshot_build.ps1`.
