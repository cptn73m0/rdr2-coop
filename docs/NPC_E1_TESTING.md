# NPC actor replication (E1–E3b) — in-game test plan and acceptance metrics

**Статус (23.10.2026):** E1–E3b реализованы в клиенте (`src/actor_sync.*`,
флаг `npc_actor=0` по умолчанию) и **проверены на одном ПК** fake host/guest
(`tools/fake_actor_client.py`): E1 human-only, E2a meta-outfit (5/5 applied,
failed=0), E2b всадники (mount spawn ×9, failed=0), E3a сценарии/смерть,
E3b оружие/бой (shoot=1450, reload=385, weapon fail=0). E2c-a resolver
механически работает (resolved=0/failed=11 для random ambient NPC);
**E2c-b asset/tint — протокол v16 0x1E в разработке**. Двух-ПК приёмка ещё
впереди: второй ПК ARINA офлайн/старая сборка.

E1 — human-only NPC: хост находит до `npc_cap` (default 8, max 12) ближайших
human NPC в радиусе `npc_radius` (default 60 м) и шлёт spawn/frames/despawn
(0x18/0x19/0x1A) + heartbeat; гость держит прокси-педов. E2/E3 добавляют
`metaOutfit`, всадников, сценарии/смерть и оружие через поля/флаги тех же
сообщений. Mission NPC, животные, транспорт и трупы (кроме human NPC) не
реплицируются; 0x1B/0x1C зарезервированы; resync 0x1D реализован (≤1/с).
E2c-b (asset/tint) ждёт v16 0x1E.

Автоматизированная часть — wire/relay/validation: `tests/integration_server.py`
(27 сценариев, actor-тесты) + 7 C++ тестов; клиентская логика проверяется
в игре/fake-клиентами.

## 0. Флаги и предусловия

- Оба ПК: одна и та же сборка (protocol v15: `net/protocol.h`,
  `kProtocolVersion=15`), один `rdr2_coop_server.exe` v15. **ARINA сейчас
  офлайн/старая сборка — перед двух-ПК тестом дозалить `.asi`/INI.**
- INI на обоих ПК (кроме указанных):
  - `npc_actor = 1` — включить NPC-репликацию;
  - `npc_cap = 12` — для проверки 8–12 NPC в городе (default 8; допустимо 1..12);
  - `npc_radius = 60` — default; despawn-гистерезис +10 м (70 м);
  - `npc_appearance = 1` — meta-outfit внешность (E2a, default 1);
  - `npc_debug = 1` — probe-лог `probe: scan humans=...` (только хост);
    после прогона выключить;
  - остальные экспериментальные флаги — **0** (`world_sync=0`, `action_sync=0`,
    `vehicle_sync=0`, `mission_sync=0`, `world_trains=0`, `train_debug=0`,
    `mission_credit=0`).
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

## 3. Игровой чек-лист (E1, два ПК)

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
| **S14** | E2c-a outfit resolver (опционально) | Host с resolver; fake guest (`--mode guest --outfit-wait 60`) | `fakeguest: outfits nonzero=… outfit_updates=… resolved_late=…`; для random ambient ожидаемо resolved=0/failed=11 — E2c-b после v16 | fake guest лог | [ ] |

Дополнительно: сценарные позы курящих/стоящих на разных точках (E3a),
ночь/дождь (LOD/видимость), E2c-b asset/tint — после протокола v16 0x1E.
Транспорт/животные/лошадиные трупы по-прежнему вне NPC-репликации.

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
  asset/tint — отдельный v16 0x1E.
- **Только human NPC**: животные, транспорт и лошадиные трупы не
  реплицируются; mission NPC исключены; мёртвые human NPC — E3a (dead flag +
  health=0), труп/возрождение подтверждены.
- **Ambient гостя не подавляется** (отдельный world_sync-пайплайн): гость
  видит своих ambient NPC и прокси хоста; «нет дублей» = нет двух прокси
  одного actorId, а не идентичная картинка A/B.
- **E2c-a resolver**: механически работает (890 моделей/4568 кандидатов,
  бюджет 24 натива/тик), но random ambient NPC без именованного outfit —
  `resolved=0/failed=11`; **E2c-b asset/tint ждёт v16 0x1E**, тогда проверять
  `fakeguest: outfit_updates/resolved_late` и визуал.
- **Двух-ПК приёмка не пройдена**: ARINA офлайн/старая сборка (нужен дозалив
  `.asi`/INI); текущие E1–E3b — однопК-проверки fake host/guest.
- **Tick budget** не инструментирован (нет `perf:` лога): точная p95-оценка
  только внешним профайлером/OBS; FPS-метрики из `scan: fps=` — основной
  автоматический прокси.
- Discovery/кап зависят от позиции **обоих** игроков и от `npc_cap/radius`;
  хост должен быть рядом с толпой, иначе 8–12 не наберётся.
- Cap-кламп: `npc_cap` 1..12, `npc_radius` 20..100; некорректные значения
  игнорируются (остаются defaults 8/60).
- `npc_actor=0` — обязательный gating: ни клиент, ни сервер не должны писать
  `actor:`/`probe:` строки; проверять на обоих ПК и сервере (S0/S9).
