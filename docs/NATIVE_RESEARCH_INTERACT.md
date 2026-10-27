# RDR2 native research: blip names, wheel spin, guest→host vehicle interactivity

**Статус:** R&D, read-only (код мода не изменялся), файл не закоммичен.
**Цель:** native-хэши/сигнатуры/рецепты для v20.
**Целевой билд:** RDR2 PC **1491.50**; сверка: `third_party/rdr2-runtime-scripthook/source/scripting_v3/RDR2.Native/NativeDecl.cs` (ScriptHook RDR2 runtime), `third_party/rdr2-runtime-scripthook/source/scripting_v3/RDR2/Blip.cs`, latest `alloc8or/rdr3-nativedb-data` master (`natives.json`), + текущий код `src/coop.cpp`, `src/world_sync.cpp`.

**Легенда:**
- **CONFIRMED** — hash и арность/порядок параметров присутствуют в NativeDecl (1491.50) и/или latest nativedb, и семантика подтверждена кодом/ScriptHook-обёрткой.
- **SIGNATURE-ONLY** — hash/параметры известны, но эффект/семантика не проверены в игре.
- **UNCERTAIN** — семантика/параметры не документированы; обязателен in-game тест.
- **ABSENT** — публичного натива нет (в 1491.50).

В C++ проекта все вызовы идут через `natives_api`: `Init(hash) → Push(arg1) … Push(argN) → Call()`; строки пушатся указателем (`reinterpret_cast<unsigned long long>(const_cast<char*>(str))`), а для «literal string» — через `MISC_VAR_STRING` (см. ниже).

---

## 1. Имя игрока на карте / blip label

### 1.1. Что сейчас в коде

- `src/coop.cpp:371-426`: `CreateFriendlyBlipAt` создаёт blip **по координатам**:
  - `MAP_BLIP_ADD_FOR_COORDS` (0x554D9D53F696D002) со sprite `BLIP_SPRITE_WHITE_DOT` = 0xB04092F8;
  - `MAP_BLIP_ADD_MODIFIER` (0x662D364ABF16DE2F) с `BLIP_MODIFIER_FRIENDLY` = 0x6414AA9A;
  - `MAP_SET_BLIP_SCALE` (0xD38744167B2FA257) = 0.9.
- Координаты обновляются `MAP_SET_BLIP_COORDS` (0x4FF674F5E23D49CE), blip создаётся при спавне ghost (`7553`) / fallback (`3916`), удаляется `MAP_REMOVE_BLIP` (0xF2C3C9DA47AAA54A) с `Blip*`.
- **Имя блипа нигде не задаётся.** Имя над головой (`s_pedName`, `DrawText`) — отдельный механизм; на карте/легенде при этом отображается дефолт/«Companion» (пользователь видит «Спутник»). Дефолтная подпись зависит от blip style/modifier и локализации; самый прямой фикс — задать имя после создания.

### 1.2. Ключевые нативы

| Задача | Натив | Hash | Сигнатура / арность | Статус | Рецепт/примечание |
|---|---|---|---|---|---|
| Создать blip по координатам | `BLIP_ADD_FOR_COORDS` | `0x554D9D53F696D002` | `(Hash blipHash, float x, y, z)` — 4 | CONFIRMED | уже используется; `blipHash` — sprite/hash (0xB04092F8 white dot) |
| Создать blip на entity | `BLIP_ADD_FOR_ENTITY` | `0x23F74C2FDA6E7C61` | `(Hash blipHash, Entity entity)` — 2 | CONFIRMED (сигнатура); в этом билде может вернуть 0 | код уже отмечал «BLIP_ADD_FOR_ENTITY returns 0 in this build»; fallback coords |
| Получить blip от entity | `MAP_GET_BLIP_FROM_ENTITY` | `0x6D2C41A8BD6D6FD0` | `(Entity entity)` — 1, ret Blip | CONFIRMED | если blip уже создан на entity; иначе 0 |
| **Задать имя строкой** | **`_SET_BLIP_NAME`** | **`0x9CB1A1623062F402`** | **`(Blip blip, const char* name)` — 2** | **CONFIRMED** (NativeDecl 1491.50; ScriptHook `Blip.Name` использует её) | имя готовить через `MISC::VAR_STRING(10, "LITERAL_STRING", name)`; см. рецепт |
| Имя из текстового GXT-файла | `SET_BLIP_NAME_FROM_TEXT_FILE` | `0x0A062D6D7C0B2C2C` | `(Blip blip, const char* textLabel)` — 2 | CONFIRMED | **только GXT label**, не произвольное имя игрока; динамические ники не подходят |
| Имя = имя локального Player | `SET_BLIP_NAME_TO_PLAYER_NAME` | `0x093DD5A31BC2B459` | `(Blip blip, Player player)` — 2 | CONFIRMED | только локальный player index; удалённый игрок в кооп-мире не является локальным Player |
| Стиль blip | `_BLIP_SET_STYLE` | `0xEDD964B7984AC291` | `(Blip blip, Hash styleHash)` — 2, ret BOOL | CONFIRMED | снимает существующие modifiers и ставит style; style friendly = `0xFAAB68A9`, MP player = `0x69DAFC52` |
| Добавить style без снятия | `_BLIP_ADD_STYLE` | `0xBD62D98799A3DAF0` | `(Blip blip, Hash styleHash)` — 2, ret BOOL | CONFIRMED | альтернатива `_BLIP_SET_STYLE` |
| Удалить modifier | `BLIP_REMOVE_MODIFIER` | `0xB059D7BD3D78C16F` | `(Blip blip, Hash modifierHash)` — 2, ret BOOL | CONFIRMED | modifier 0 = удалить все модификаторы |
| Friendly modifier | `BLIP_ADD_MODIFIER` | `0x662D364ABF16DE2F` | `(Blip blip, Hash modifierHash)` — 2, ret BOOL | CONFIRMED | friendly = 0x6414AA9A (уже используется) |
| Создать blip по style | `_BLIP_ADD_FOR_STYLE` | `0x3E593DF9C2962EC6` | `(Hash styleHash)` — 1, ret Blip | CONFIRMED | альтернатива: style-blip + `SET_BLIP_COORDS`; не обязательна |
| Подготовить literal string | `MISC_VAR_STRING` | `0xFA925AC00EB830B9` | `(int flags, args…)` — вариативно | CONFIRMED | `Push(10)`, `Push("LITERAL_STRING")`, `Push(name)`; результат — string handle |

Важно: `BLIP_AMBIENT_COMPANION`/`0xF4F30880` в ScriptHook — это **sprite/hash** (в `Blip.cs` это enum `eBlipSprite`, не `eBlipStyle`); в `src/natives.h` он назван `BLIP_STYLE_AMBIENT_COMPANION`, но это не style. Не использовать его как style без in-game проверки.

### 1.3. Минимальный рецепт (Push-порядок)

После создания blip (сразу, пока handle валиден):

```cpp
// 1) literal string handle: MISC::VAR_STRING(10, "LITERAL_STRING", name)
unsigned long long nameStr = 0;
{
    using namespace natives_api;
    Init(natives::MISC_VAR_STRING);              // 0xFA925AC00EB830B9
    Push(static_cast<unsigned long long>(10));   // flags
    Push(reinterpret_cast<unsigned long long>(const_cast<char*>("LITERAL_STRING")));
    Push(reinterpret_cast<unsigned long long>(const_cast<char*>(playerName)));
    nameStr = Call();
}

// 2) _SET_BLIP_NAME(blip, nameStr)
{
    using namespace natives_api;
    Init(natives::_SET_BLIP_NAME);                // 0x9CB1A1623062F402
    Push(blip);                                   // Blip
    Push(nameStr);                                // prepared string handle
    Call();
}
```

Минимальный вариант только с именем:

1. `BLIP_ADD_FOR_COORDS(sprite, x, y, z)` → `blip`.
2. `MISC_VAR_STRING(10, "LITERAL_STRING", name)` → `nameStr`.
3. `_SET_BLIP_NAME(blip, nameStr)`.
4. `MAP_SET_BLIP_COORDS(blip, ...)` как сейчас.

Если после этого на карте всё ещё «Companion» (зависит от style/legacy-подписи):

```cpp
// optional: убрать companion-подпись/модификатор, поставить friendly style
Init(natives::BLIP_REMOVE_MODIFIER);   // 0xB059D7BD3D78C16F
Push(blip); Push(0);                    // 0 = remove all modifiers
Call();

Init(natives::_BLIP_SET_STYLE);         // 0xEDD964B7984AC291
Push(blip); Push(0xFAAB68A9ull);        // BLIP_STYLE_FRIENDLY
Call();

// при желании снова добавить friendly modifier (цвет) и имя
Init(natives::BLIP_ADD_MODIFIER); Push(blip); Push(0x6414AA9Aull); Call();
// затем повторить VAR_STRING + _SET_BLIP_NAME
```

Порядок «style → modifier → name» безопаснее, чем «name → style»: `_BLIP_SET_STYLE` снимает модификаторы и может сбросить визуальную подпись; имя ставить последним.

### 1.4. Альтернативы и что не подходит

- **`SET_BLIP_NAME_FROM_TEXT_FILE`** — только GXT-entry (`textLabel` из текстовой базы), не произвольная строка. Для ников игроков не подходит.
- **`SET_BLIP_NAME_TO_PLAYER_NAME`** — только локальный `Player` index; удалённый игрок не является локальным Player. Для своего blip можно, для чужого — нет.
- **Свой blip** уже создаётся (`BLIP_ADD_FOR_COORDS`); менять модель создания не нужно — достаточно `_SET_BLIP_NAME`. `BLIP_ADD_FOR_ENTITY`/`GET_BLIP_FROM_ENTITY` — резерв, если в билде entity-blip заработает.
- Если ник меняется (rejoin/PlayerJoined) — повторить `MISC_VAR_STRING` + `_SET_BLIP_NAME`; handle blip сохранять в `s_blip` и переименовывать при обновлении `s_pedName`.
- `MAP_SET_BLIP_NAME_FROM_TEXT_STRING`/`_SET_BLIP_NAME_STRING` в 1491.50 **не найдены**; актуальное имя — `_SET_BLIP_NAME`.

### 1.5. Проверить в игре

- Создать blip, вызвать `_SET_BLIP_NAME` с ником, открыть карту/легенду: должен быть ник, не «Спутник».
- Проверить, не сбрасывается ли имя после `MAP_SET_BLIP_COORDS`/`CREATE`/`REMOVE`/reconnect.
- A/B: без `_BLIP_SET_STYLE` и с ним; проверить цвет/иконку/подпись.
- Проверить ник на обоих клиентах (у хоста — гость, у гостя — хост).

---

## 2. Спин колёс у guest vehicle-прокси

### 2.1. Главный вывод

Публичного натива **«задать вращение колёс»** в RDR2 1491.50 **нет**:

- поиск по latest nativedb/NativeDecl: **отсутствуют** `SET_VEHICLE_WHEEL_ROTATION`, `_SET_VEHICLE_WHEEL_ROTATION`, `_SET_VEHICLE_WHEEL_*` (кроме break/destroy), `SET_VEHICLE_WHEELS_*` кроме can-break, `SET_ENTITY_ANGULAR_VELOCITY`;
- найденные wheel-нативы — только поломка/проверка:
  - `SET_VEHICLE_WHEELS_CAN_BREAK` 0x839137C40275FB77(2),
  - `SET_VEHICLE_WHEELS_CAN_BREAK_OFF_WHEN_BLOW_UP` 0xC462C79379ABBCB1(2),
  - `_IS_VEHICLE_WHEEL_DESTROYED` 0xCB2CA620C48BC875(2),
  - `_ARE_ANY_VEHICLE_WHEELS_DESTROYED` 0x18714953CCED17D3(1),
  - `_BREAK_OFF_VEHICLE_WHEEL` 0xD4F5EFB55769D272(2, ret Entity),
  - `_BREAK_OFF_DRAFT_WHEEL` 0xC372B6A88F6E4AD8(3).
- `SET_ENTITY_ROTATION` вращает **весь vehicle**, не колёса; `_SET_ENTITY_ANIM_SPEED` — только для проигрываемых анимаций; колёсные bones публичного setter’а не имеют.

Следовательно, «крутить колёса» можно только косвенно — заставить физику/движок считать, что vehicle движется, либо принять статичные колёса. Guest-прокси сейчас — mission/no-collision, позиция задаётся интерполяцией `SET_ENTITY_COORDS` (`world_sync.cpp:2367-2415`), поэтому физическая скорость колёс = 0.

### 2.2. Кандидаты (SIGNATURE-ONLY / UNCERTAIN)

| Кандидат | Натив | Hash (arity) | Статус | Как использовать / риск |
|---|---|---|---|---|
| Скорость вперёд (главный кандидат) | `SET_VEHICLE_FORWARD_SPEED` | `0xF9F92AF49F12F6E7` (2: vehicle, float speed) | SIGNATURE-ONLY | каждый тик на guest-прокси задавать `speed` из host-frame (0..~8 м/с; для wagon — по `GET_ENTITY_SPEED`). Может заставить колёса/engine считать скорость; **может также двигать прокси**, конфликтуя с интерполяцией. Сначала тест «только скорость + текущая интерполяция»; если колёса не крутятся — тест с физикой. |
| Руль | `SET_VEHICLE_STEER_BIAS` | `0x84DAAE11E9EE4FC3` (2) | CONFIRMED (сигнатура/комментарий) | `-1..1`; **вызывать каждый тик** (комментарий: steering unlocked, когда перестали вызывать); для визуального поворота колёс/vehicle; на guest-прокси — только если физика активна. |
| Ручник | `SET_VEHICLE_HANDBRAKE` | `0x91BE51AEC4E99710` (2) | CONFIRMED (сигнатура) | при speed≈0; на host real — тормоз; на proxy — визуально/физически. |
| На землю | `SET_VEHICLE_ON_GROUND_PROPERLY` | `0x7263332501E07F52` (2, ret BOOL) | CONFIRMED (сигнатура) | после SetCoords/rotation/физики; p1 likely true; тест. |
| Динамика entity | `SET_ENTITY_DYNAMIC` | `0xFBFC4473F66CE344` (2) | CONFIRMED | `true` — физика (нужна для колёс); `false` — статичный прокси (колёса стоят). |
| Гравитация | `SET_ENTITY_HAS_GRAVITY` | `0x0CEDB728A1083FA7` (2) | CONFIRMED | для физического proxy; отключить, если не нужна. |
| Freeze | `FREEZE_ENTITY_POSITION` | `0x7D9EFB7AD6B19754` (2) | CONFIRMED | freeze, скорее всего, **убьёт** spin; не использовать, если колёса важны. |
| Velocity | `SET_ENTITY_VELOCITY` | `0x1C99BB7B6E96D16F` (3: entity, x,y,z) | CONFIRMED | альтернатива forward speed; тоже может дать wheel spin через физику; риск телепортов/коллизий. |
| Wheel bones (диагностика) | `GET_ENTITY_BONE_INDEX_BY_NAME` | `0xBACA8FE9C76C124E` (2: entity, const char*) | CONFIRMED | найти wheel bone; **setter’а нет**; только проверка существования/позиции через `GET_WORLD_POSITION_OF_ENTITY_BONE` 0x82CFA50E34681CA5(2). |
| Draft desired speed | `_SET_DRAFT_VEHICLE_DESIRED_SPEED` | `0x0C3F0F7F92CA847C` (2) | SIGNATURE-ONLY | для wagon/draft: задать скорость; возможно, заставит двигаться draft-лошадей/колёса; тест только на host/real или физическом proxy. |
| AI-действие | `TASK_VEHICLE_TEMP_ACTION` | `0xC429DCEEB339E129` (4: ped, vehicle, action, time) | CONFIRMED (сигнатура), UNCERTAIN (эффект) | требует **driver ped**; на host real vehicle/NPC coach — можно эмулировать газ/тормоз/поворот; на guest proxy без водителя не сработает. Известные action: 1 brake, 9 weak accel, 23 fast accel, 27 brake until stop; **16/17/18 — crash/опасные**, не использовать. |
| Diagnostic speed | `GET_ENTITY_SPEED` | `0xFB6BA510A533DF81` (1) | CONFIRMED | host speed для forward speed; также проверка, что proxy реально двигается. |
| Diagnostic velocity | `GET_ENTITY_VELOCITY` | `0x4805D2B1D8CF94A9` (2) | CONFIRMED | угол/скорость; p1 неизвестен — тест 0/1. |
| Max speed estimate | `_GET_VEHICLE_ESTIMATED_MAX_SPEED` | `0xFE52F34491529F0B` (1) | CONFIRMED | для clamp переднего хода. |

### 2.3. Рекомендуемый порядок экспериментов (A/B)

1. **A (минимальный):** оставить текущую интерполяцию позиции; каждый тик после интерполяции вызвать `SET_VEHICLE_FORWARD_SPEED(proxy, hostSpeed)`; проверить, крутятся ли колёса и не ломается ли позиция. Если колёса крутятся — больше ничего не нужно.
2. **B (физический proxy):** `SET_ENTITY_DYNAMIC(proxy,true)`, `SET_ENTITY_HAS_GRAVITY`, no-collision; `SET_VEHICLE_FORWARD_SPEED` + `SET_VEHICLE_STEER_BIAS` каждый тик; `SET_VEHICLE_ON_GROUND_PROPERLY`; корректировать позицию только при ошибке >1–2 м (иначе физика сама едет). Риск: уезжает/переворачивается/дёргается; для wagon — риск потери draft-лошадей.
3. **C (wagon/draft):** то же + `_SET_DRAFT_VEHICLE_DESIRED_SPEED(proxy, speed)`; проверить, крутятся ли колёса и не отцепляются ли draft-лошади; host real wagon — отдельно.
4. **D (без физики):** если A/B/C не дают визуального вращения — принять статичные колёса или скрыть wagon в отдалении; **не** делать prop-хак с подменой колёс (нет публичного bone setter’а, высокий риск).

**Host real vehicle:** `SET_VEHICLE_FORWARD_SPEED`/`SET_VEHICLE_STEER_BIAS`/`SET_VEHICLE_HANDBRAKE` — рабочий путь управления (колёса крутятся физикой), но конфликтует с вводом игрока/AI; для v20 remote control — host authority + арбитраж.

**Guest proxy:** пока host authority не включён, это только визуальная аппроксимация; признать «колёса статичны», если физический proxy небезопасен.

### 2.4. Риски

- `SET_VEHICLE_FORWARD_SPEED` на mission/no-collision/frozen vehicle может **не** дать wheel spin; нужен in-game тест, не обещать.
- Физический proxy может уезжать, переворачиваться, цеплять игроков (no-collision частично спасает), терять draft-лошадей/occupants.
- `SET_VEHICLE_STEER_BIAS` на proxy может поворачивать весь vehicle, не только колёса.
- `SET_ENTITY_VELOCITY`/`SET_ENTITY_COORDS` могут «бороться» с физикой; телепорты/рывки.
- `TASK_VEHICLE_TEMP_ACTION` перетирает задачи driver’а; на player-driven host vehicle — не использовать; action 16–18 опасны.
- Нет публичного wheel-bone setter’а; prop-хак не рекомендован.

---

## 3. Интерактивность транспорта для v20 (guest→host)

### 3.1. Authority-модель

- Хост-мир — авторитет: real vehicle, real driver/passengers, damage, doors, draft. Гость управляет **только запросами**; host валидирует и применяет.
- Guest-прокси в `world_sync` — mission/no-collision, позиция интерполируется (`world_sync.cpp:2307-2417`); его локальные нативы меняют только визуал гостя, не host-мир.
- Нужен новый guest→host канал (v20) с rate-limit; host-only relay уже есть в `server.cpp` (`RelayHostOnly`), а host→guest состояние — `VehicleOccupants 0x21`, `TrainSnapshotV2 0x22`, `WorldSnapshot`/`PlayerVehicle`.
- Ограничения: `kMaxPlayers=2`; mission entities (`SET_ENTITY_AS_MISSION_ENTITY`/`IS_ENTITY_A_MISSION_ENTITY`); draft vehicles (`IS_DRAFT_VEHICLE`); player-driven host vehicle (host input priority).
- «Сеть» здесь — TCP relay, не RDR2 network; `isNetwork=false` у локальных entity; напрямую гость не может вызвать натив на host entity.

### 3.2. Seats / enter / exit / hijack

| Назначение | Натив | Hash (arity) | Статус | Рецепт/ограничение |
|---|---|---|---|---|
| Кол-во мест модели | `GET_VEHICLE_MODEL_NUMBER_OF_SEATS` | `0x9A578736FF3A17C3` (1) | CONFIRMED | по model hash; для валидации seat |
| Макс. пассажиров | `GET_VEHICLE_MAX_NUMBER_OF_PASSENGERS` | `0xA9C55F1C15E62E06` (1) | CONFIRMED | последний seat = max-1 (комментарий DB) |
| Свободно ли место | `IS_VEHICLE_SEAT_FREE` | `0xE052C1B1CAA4ECE4` (2) | CONFIRMED | seat enum ниже |
| Есть ли свободные места | `ARE_ANY_VEHICLE_SEATS_FREE` | `0xA0A424505A1B6429` (1) | CONFIRMED | быстрый gate |
| Кто в seat | `GET_PED_IN_VEHICLE_SEAT` | `0xBB40DD2270B65366` (2) | CONFIRMED | seat enum |
| Последний в seat | `GET_LAST_PED_IN_VEHICLE_SEAT` | `0x74583B19FEEAFDA7` (2) | CONFIRMED | для истории |
| Водитель | `GET_DRIVER_OF_VEHICLE` | `0x2963B5C1637E8A27` (1) | CONFIRMED | driver = seat -1 |
| Посадить в seat (warp) | `SET_PED_INTO_VEHICLE` | `0xF75B0D629E1C063D` (3) | CONFIRMED | host: real ped; guest: local proxy/ghost; **не** анимированный вход |
| Создать ped в seat | `CREATE_PED_INSIDE_VEHICLE` | `0x7DD959874C1FD534` (6) | CONFIRMED | p3..p5 неизвестны (тест); appearance применять после |
| Анимированный вход | `TASK_ENTER_VEHICLE` | `0xC20E50AA46D09CA8` (7) | CONFIRMED (флаги — Halen84 eEnterExitVehicleFlags) | host real/NPC; для proxy не использовать; `ECF_JACK_ANYONE` (1<<3), `ECF_WARP_PED` (1<<4), `ECF_JUST_PULL_PED_OUT` (1<<19), `ECF_DONT_WAIT_FOR_VEHICLE_TO_STOP` (1<<6) |
| Выход | `TASK_LEAVE_VEHICLE` | `0xD3DBCE61A490BE02` (4) | CONFIRMED | flags see TASK_ENTER_VEHICLE; unkPed вероятно 0 |
| Board/hijack | `_TASK_BOARD_VEHICLE` | `0xE53D17AD837CBF7C` (6) | SIGNATURE-ONLY/UNCERTAIN | p2/p3/speed/boardingFlags; NativeDecl типы ulong; использовать только после TASK_ENTER |
| Board 2 | `_TASK_BOARD_VEHICLE_2` | `0xE41A09C8DDFF7AA4` (5) | UNCERTAIN | p1..p4; тест |
| Сломать замок двери | `TASK_BREAK_VEHICLE_DOOR_LOCK` | `0xBB28D1BC9EA8A6A5` (2) | CONFIRMED (сигнатура) | hijack/coach robbery; host real ped |
| В машине? | `IS_PED_IN_ANY_VEHICLE` | `0x997ABD671D25CA0B` (2) | CONFIRMED | p2 atGetIn |
| В конкретной? | `IS_PED_IN_VEHICLE` | `0xA3EE4A07279BB9DB` (3) | CONFIRMED | p3 atGetIn |
| В какой машине | `GET_VEHICLE_PED_IS_IN` | `0x9A9112A0FE9A4713` (2) | CONFIRMED | p2 lastVehicle |
| Использует | `GET_VEHICLE_PED_IS_USING` | `0x6094AD011A2EA87D` (1) | CONFIRMED | текущая |
| Садится | `GET_VEHICLE_PED_IS_ENTERING` | `0xF92691AED837A5FC` (1) | CONFIRMED | entering/exit state |
| Запретить вход в чужой | `SET_PLAYER_MAY_ONLY_ENTER_THIS_VEHICLE` | `0xDA35A134038557EC` (2) | CONFIRMED (сигнатура) | host player restrictions; к ghost не применимо |
| Запретить вход | `SET_PLAYER_MAY_NOT_ENTER_ANY_VEHICLE` | `0xBEC463B3A11C909E` (1) | CONFIRMED (сигнатура) | только Player; не remote ghost |
| Считать/не считать vehicle для игрока | `SET_VEHICLE_IS_CONSIDERED_BY_PLAYER` | `0x54800D386C5825E5` (2) | CONFIRMED (сигнатура) | false — player не пытается войти; полезно для proxy |
| Не угоняемый ambient | `SET_VEHICLE_NOT_STEALABLE_AMBIENTLY` | `0x09C970AE59ABF6B2` (2) | CONFIRMED (сигнатура) | theft prompt |
| Угнан | `SET_VEHICLE_IS_STOLEN` | `0x6C32FC81DFF25C9A` (2) | CONFIRMED (сигнатура) | theft state |
| Эксклюзивный водитель | `SET_VEHICLE_AI_CAN_USE_EXCLUSIVE_SEATS` | `0x0893DAFBFA67110E` (2) | CONFIRMED | не путать с native name; см. DB comment |
| Exclusive driver | `SET_VEHICLE_EXCLUSIVE_DRIVER` | `0xC6B9BF123B9463B6` (3) | CONFIRMED | index 0/1; optional |

**Seat enum (eVehicleSeat, confirmed из DB comment):**
`VS_ANY_PASSENGER=-2`, `VS_DRIVER=-1`, `VS_FRONT_RIGHT=0`, `VS_BACK_LEFT=1`, `VS_BACK_RIGHT=2`, `VS_EXTRA_LEFT_1=3`, `VS_EXTRA_RIGHT_1=4`, `VS_EXTRA_LEFT_2=5`, `VS_EXTRA_RIGHT_2=6`, `VS_EXTRA_LEFT_3=7`, `VS_EXTRA_RIGHT_3=8`, `VS_NUM_SEATS=9`.

**Hijack NPC driver (host):** host-side `TASK_ENTER_VEHICLE(ped,vehicle,timeout,seat=-1,speed,ECF_JACK_ANYONE|ECF_WARP_PED,0)` или `TASK_BREAK_VEHICLE_DOOR_LOCK` + `SET_PED_INTO_VEHICLE`. `SET_PED_INTO_VEHICLE` проще/детерминированнее, но «телепортирует» и игнорирует двери/замки; `TASK_ENTER_VEHICLE` анимирован, но зависит от AI/дверей. Для remote guest — host решает, кого посадить; guest-запрос только интент.

### 3.3. Doors / locks / damage / break parts

| Назначение | Натив | Hash (arity) | Статус | Рецепт/ограничение |
|---|---|---|---|---|
| Открыть дверь | `SET_VEHICLE_DOOR_OPEN` | `0x550CE392A4672412` (4: veh, doorId, loose, openInstantly) | CONFIRMED | работает и на trains/wagons (comment DB) |
| Закрыть дверь | `SET_VEHICLE_DOOR_SHUT` | `0x6A3C24B91FD0EA09` (3: veh, doorId, closeInstantly) | CONFIRMED | doorId enum ниже |
| Закрыть все | `SET_VEHICLE_DOORS_SHUT` | `0xA4FFCD645B11F25A` (2) | CONFIRMED | |
| Управлять углом/скоростью | `SET_VEHICLE_DOOR_CONTROL` | `0xD57F10EBBA814ECF` (4: veh, doorId, speed, angle) | CONFIRMED | для плавного remote open |
| Дверь открыта полностью? | `IS_VEHICLE_DOOR_FULLY_OPEN` | `0x7AE191143C7A9107` (2) | CONFIRMED | |
| Сломать дверь | `SET_VEHICLE_DOOR_BROKEN` | `0x9666CF20A1C6D780` (3: veh, doorId, deleteDoor) | CONFIRMED | deleteDoor=true удаляет; host authoritative |
| Дверь сломана? | `_IS_VEHICLE_DOOR_BROKEN` | `0xE979BB5602AD3402` (2) | CONFIRMED | |
| Разрешить ломать | `SET_DOOR_ALLOWED_TO_BE_BROKEN_OFF` | `0x081FB9D6422F804C` (3) | CONFIRMED | host/real; для proxy — визуал |
| Заблокировать все двери | `SET_VEHICLE_DOORS_LOCKED` | `0x96F78A6A075D55D9` (2: veh, doorLockStatus) | SIGNATURE-ONLY | **enum lock status не подтверждён**; тест 0/1/2; лучше individual + break |
| Заблокировать конкретную | `SET_VEHICLE_INDIVIDUAL_DOORS_LOCKED` | `0xA9F1D75195CC40F6` (3) | SIGNATURE-ONLY | doorId + lockStatus; enum неизвестен |
| Lock status | `GET_VEHICLE_DOOR_LOCK_STATUS` | `0xC867FD144F2469D3` (1) | SIGNATURE-ONLY | возвращает int; enum тестировать |
| Для всех игроков | `SET_VEHICLE_DOORS_LOCKED_FOR_ALL_PLAYERS` | `0x2381977DA948F8DC` (2) | CONFIRMED (сигнатура) | remote ghost не Player; на host только player |
| Для player | `SET_VEHICLE_DOORS_LOCKED_FOR_PLAYER` | `0x359A8EA1FB8D6F0F` (3) | CONFIRMED (сигнатура) | player index/local |
| Открыть замки/компартменты | `_BREAK_LOCKS_ON_VEHICLE` | `0x9D12796EF4BF9EA9` (1) | CONFIRMED (сигнатура/комментарий) | «pops/destroys breakable lock objects, swings open doors»; coach robbery |
| Замки unbreakable | `SET_BREAKABLE_VEHICLE_LOCKS_UNBREAKABLE` | `0xBC4735F48CD983EF` (2) | CONFIRMED | |
| Состояние lock objects | `_GET_BREAKABLE_VEHICLE_LOCKS_STATE` | `0xE015CF1F2C0959D8` (1) | SIGNATURE-ONLY | |
| Кол-во lock objects | `_GET_NUM_BREAKABLE_VEHICLE_LOCK_OBJECTS` | `0x2FA86833E3617E2D` (1) | CONFIRMED | |
| Lock object | `_GET_BREAKABLE_VEHICLE_LOCK_OBJECT` | `0x58F2244C1286D09A` (2) | CONFIRMED | |
| Двери открыты на любом расстоянии | `SET_VEHICLE_DOORS_TO_OPEN_AT_ANY_DISTANCE` | `0x362CEDD2A41E0747` (2) | CONFIRMED (сигнатура) | |
| Дверь latch | `SET_VEHICLE_DOOR_LATCHED` | `0x06F8A202EB312A3C` (5) | SIGNATURE-ONLY | p2..p4 неизвестны |
| Разбить окно | `REMOVE_VEHICLE_WINDOW` | `0x745F15A215F2DDF1` (2: veh, windowIndex) | CONFIRMED | windowIndex: 0 FR, 1 FL, 2 BR, 3 BL |
| Окно цело? | `IS_VEHICLE_WINDOW_INTACT` | `0x0E7910A63E05B12C` (2) | CONFIRMED | |
| Урон | `SET_VEHICLE_DAMAGE` | `0x1D7678F81452BB41` (7: veh, xOffset,yOffset,zOffset, damage, radius, BOOL p6) | CONFIRMED (сигнатура/comment) | offset относительно модели; p6 неизвестен — тест false |
| Body health | `SET_VEHICLE_BODY_HEALTH` | `0x55CCAAE4F28C67A0` (2) | CONFIRMED (сигнатура) | диапазон тестировать (вероятно 0..1000) |
| Engine health | `SET_VEHICLE_ENGINE_HEALTH` | `0x8BDC5B998B4654EF` (2) | CONFIRMED (comment: 1000 max, -4000 destroyed) | |
| Petrol tank | `SET_VEHICLE_PETROL_TANK_HEALTH` | `0x6AB2918EE3BEC94C` (2) | CONFIRMED (comment: 1000 max) | |
| Починить | `SET_VEHICLE_FIXED` | `0x79811282A9D1AE56` (1) | CONFIRMED | engine не всегда чинит (comment) |
| Можно ломать | `SET_VEHICLE_CAN_BREAK` | `0xC5ED9D59B4646611` (2) | CONFIRMED (сигнатура) | |
| Крепкий | `SET_VEHICLE_STRONG` | `0xAB315515C9F8803D` (2) | CONFIRMED (comment) | не crash damage, но bullets/explosions |
| Видимый урон | `SET_VEHICLE_CAN_BE_VISIBLY_DAMAGED` | `0x4BF8131AE811541C` (2) | CONFIRMED (сигнатура) | |
| Колёса ломаются | `SET_VEHICLE_WHEELS_CAN_BREAK` | `0x839137C40275FB77` (2) | CONFIRMED | |
| Шины burst | `SET_VEHICLE_TYRES_CAN_BURST` | `0xEBD0A4E935106FE5` (2) | CONFIRMED (comment) | |
| Колёса отрываются при взрыве | `SET_VEHICLE_WHEELS_CAN_BREAK_OFF_WHEN_BLOW_UP` | `0xC462C79379ABBCB1` (2) | CONFIRMED | |
| Отломать колесо | `_BREAK_OFF_VEHICLE_WHEEL` | `0xD4F5EFB55769D272` (2: veh, wheelIndex) ret Entity | CONFIRMED (сигнатура) | wheelIndex 0 left, 1 right, 4/5 unknown (DB comment) |
| Отломать draft-колесо | `_BREAK_OFF_DRAFT_WHEEL` | `0xC372B6A88F6E4AD8` (3: veh, wheelIndex, destroyingForce) | CONFIRMED | destroyingForce ~100 |
| Колесо уничтожено | `_IS_VEHICLE_WHEEL_DESTROYED` | `0xCB2CA620C48BC875` (2) | CONFIRMED | |
| Любое колесо уничтожено | `_ARE_ANY_VEHICLE_WHEELS_DESTROYED` | `0x18714953CCED17D3` (1) | CONFIRMED (comment) | |
| Порвать ремни/straps | `_BREAK_VEHICLE_STRAPS` | `0xD1EFA8D68BF5D63D` (4: veh,x,y,z) | CONFIRMED (comment: draft log straps, coords=vehicle) | |
| Сделать undriveable | `SET_VEHICLE_UNDRIVEABLE` | `0x6E884BAB713A2A94` (2) | CONFIRMED (сигнатура) | |

**Door IDs (eDoorId, CONFIRMED из NativeDecl/DB comment):**
`VEH_EXT_DOOR_INVALID_ID=-1`, `DSIDE_F=0`, `DSIDE_M=1`, `DSIDE_M1=2`, `DSIDE_M2=3`, `DSIDE_R=4`, `PSIDE_F=5`, `PSIDE_M=6`, `PSIDE_M1=7`, `PSIDE_M2=8`, `PSIDE_R=9`, `BONNET=10`, `BOOT=11`.

**Ограничения damage/doors:** host real vehicle — авторитет; guest-прокси mission/invincible/no-collision, локальные damage/health только визуальны; для отображения состояния нужен host→guest снапшот (health, broken parts bitmask, door/window states). Mission vehicle/NPC может быть защищён (`IS_ENTITY_A_MISSION_ENTITY`, proofs); не ломать mission-critical без проверки. `SET_ENTITY_INVINCIBLE`/`SET_VEHICLE_CAN_BREAK`/`SET_VEHICLE_STRONG` могут блокировать damage.

### 3.4. Управление и трансформация (host authority)

| Назначение | Натив | Hash (arity) | Статус | Рецепт/риск |
|---|---|---|---|---|
| Скорость вперёд | `SET_VEHICLE_FORWARD_SPEED` | `0xF9F92AF49F12F6E7` (2) | SIGNATURE-ONLY | host real/NPC vehicle: задаёт скорость; для player-driven может конфликтовать; для proxy — см. wheel test. |
| Руль | `SET_VEHICLE_STEER_BIAS` | `0x84DAAE11E9EE4FC3` (2) | CONFIRMED (comment) | каждый тик; -1..1; после остановки вызова steering unlocked; host remote control должен повторять last steer каждый тик. |
| Ручник | `SET_VEHICLE_HANDBRAKE` | `0x91BE51AEC4E99710` (2) | CONFIRMED | stop/hold. |
| Двигатель | `SET_VEHICLE_ENGINE_ON` | `0xB64CFA14CB9A2E78` (3) | CONFIRMED (comment) | value, instantly. |
| На землю | `SET_VEHICLE_ON_GROUND_PROPERLY` | `0x7263332501E07F52` (2) | CONFIRMED (сигнатура) | после transform/physics. |
| AI-газ/тормоз/поворот | `TASK_VEHICLE_TEMP_ACTION` | `0xC429DCEEB339E129` (4) | CONFIRMED (signature + action docs) | ТОЛЬКО driver ped; actions: 1 brake, 3 brake+reverse, 9 weak accel, 23 fast accel, 27 brake; **16/17/18 — crash/опасно**. Overrides NPC task; для player — нет. |
| AI drive to coord | `TASK_VEHICLE_DRIVE_TO_COORD` | `0xE2A2AA2F659D77A7` (11: ped,vehicle,x,y,z,speed,drivingStyle,vehicleModel,drivingMode,stopRange,straightLineDist) | CONFIRMED (signature/comment) | driver ped + vehicle; host NPC driver; drivingStyle/model/drivingMode — тест значений. |
| AI straight to point | `TASK_VEHICLE_DRIVE_STRAIGHT_TO_POINT` | `0x089FF2FB965F0A29` (8: driver,vehicle,x,y,z,p5,p6,flag) | CONFIRMED | driver + vehicle; p5/p6/flag unknown; R* uses flags 524419/0. |
| AI drive to destination | `TASK_VEHICLE_DRIVE_TO_DESTINATION` | `0x7F241A0D14354583` (11: driver,vehicle,x,y,z,speed,drivingFlags,p7,stoppingRange1,stoppingRange2,p10) | CONFIRMED (signature/comment) | driver + coords/speed/flags; p7=3/6, p10=false. |
| AI goto navmesh | `TASK_VEHICLE_GOTO_NAVMESH` | `0x195AEEB13CEFE2EE` (8: ped,vehicle,x,y,z,speed,behaviorFlag,stoppingRange) | CONFIRMED (signature) | driver + speed/behavior/stopping; no roads. |
| AI wander | `TASK_VEHICLE_DRIVE_WANDER` | `0x480142959D337D00` (4) | CONFIRMED (signature) | driver, speed, style. |
| AI mission | `TASK_VEHICLE_MISSION` | `0x659427E0EF36BCDE` (9) | CONFIRMED (signature) | driver, target vehicle, missionType, speeds/flags; semantics uncertain. |
| AI mission ped target | `TASK_VEHICLE_MISSION_PED_TARGET` | `0x9454528DF15D657A` (9) | CONFIRMED (signature) | same. |
| Draft desired speed | `_SET_DRAFT_VEHICLE_DESIRED_SPEED` | `0x0C3F0F7F92CA847C` (2) | SIGNATURE-ONLY | для wagon: возможно, двигает draft-лошадей/колёса; host real/wagon; тест. |
| Transform coords | `SET_ENTITY_COORDS` | `0x06843DA7060A026B` (8: entity,x,y,z, xAxis,yAxis,zAxis,clearArea) | CONFIRMED | host real vehicle; current project wrapper pushes only 7 args (bug!); использовать 8 bool=false; затем `SET_VEHICLE_ON_GROUND_PROPERLY`; риск: пассажиры/draft-лошади, коллизии. |
| Transform rotation | `SET_ENTITY_ROTATION` | `0x9CC8314DFEDE441E` (6: entity,pitch,roll,yaw,rotationOrder,p5) | CONFIRMED (signature) | rotationOrder enum неизвестен; p5 — тест; вместе с coords. |
| Velocity | `SET_ENTITY_VELOCITY` | `0x1C99BB7B6E96D16F` (3: entity,x,y,z) | CONFIRMED | host real; сброс/придание скорости; риск коллизий. |
| Freeze | `FREEZE_ENTITY_POSITION` | `0x7D9EFB7AD6B19754` (2) | CONFIRMED | host admin; не для wheel spin. |
| Dynamic | `SET_ENTITY_DYNAMIC` | `0xFBFC4473F66CE344` (2) | CONFIRMED | physics on/off. |
| Gravity | `SET_ENTITY_HAS_GRAVITY` | `0x0CEDB728A1083FA7` (2) | CONFIRMED | physics on/off. |

**Можно ли удалённо «водить» повозку хоста?**

- **Да, но только через host authority**: guest шлёт команду (throttle/steer/brake/enter/exit), host применяет нативы к real vehicle. Прямого `_SET_VEHICLE_CONTROL`/remote-input натива **нет**.
- Host real vehicle с **NPC driver**: можно `TASK_VEHICLE_TEMP_ACTION`/drive-to-coord (проще, AI сам крутит колёса), либо `SET_VEHICLE_FORWARD_SPEED`+`STEER_BIAS` (жёстче, но player input может перебить).
- Host real vehicle с **игроком-водителем**: remote control конфликтует с вводом хоста; v20 должен явно арбитрировать (кто владеет управлением), иначе рывки.
- **Mission entities**: могут быть защищены/заскриптованы; task/control может игнорироваться или ломать миссию; host должен проверять `IS_ENTITY_A_MISSION_ENTITY` и mission state.
- **Draft wagons**: `IS_DRAFT_VEHICLE`; прямое `SET_VEHICLE_FORWARD_SPEED` может не тянуть лошадей; тестировать `_SET_DRAFT_VEHICLE_DESIRED_SPEED`/AI driver/`TASK_HITCH_ANIMAL`; не рвать `_BREAK_VEHICLE_STRAPS`/harness без явной механики.
- **Guest local player физически не сядет в host vehicle** (разные процессы/миры). v20 архитектура должна решить: host сажает guest ghost в реальный vehicle (guest local player скрыт/локирован), либо guest остаётся в local proxy vehicle; нативы `SET_PED_INTO_VEHICLE` сами по себе это не решают.

### 3.5. Hitch / draft / loot / occupancy

| Назначение | Натив | Hash (arity) | Статус | Рецепт/ограничение |
|---|---|---|---|---|
| Hitch horse (world) | `_HITCH_HORSE` | `0x06AADE17334F7A40` (4: horse,x,y,z) | CONFIRMED (signature) | это hitch к world-точке, **не** к wagon; host real |
| Unhitch horse | `_UNHITCH_HORSE` | `0x0348469DAA17576C` (1) | CONFIRMED | host real |
| Hitch animal task | `TASK_HITCH_ANIMAL` | `0x9030AD4B6207BFE8` (3: ped,scenarioPoint,flag) | SIGNATURE-ONLY | AI/scenario; не для wagon harness |
| Attach draft horse | `_ATTACH_DRAFT_VEHICLE_HARNESS_PED` | `0x316CDB5B6E8F4110` (3: horse,veh,harnessId) ret BOOL | CONFIRMED | harness enum 0..5 (LR,RR,LM,RM,LF,RF) |
| Detach draft horse | `_DETACH_DRAFT_VEHICLE_HARNESS_PED` | `0xB36D3EC70963BE60` (2: veh,horse) | CONFIRMED | cleanup |
| Detach draft by index | `_DETACH_DRAFT_VEHICLE_HARNESS_FROM_INDEX` | `0x4402960666000E62` (2: veh,harnessId) | CONFIRMED (signature) | удобно при cleanup |
| Draft horse в harness | `_GET_PED_IN_DRAFT_HARNESS` | `0xA8BA0BAE0173457B` (2: veh,harnessId) | CONFIRMED | host capture/first check |
| Кол-во draft-животных | `GET_DRAFT_ANIMAL_COUNT` | `0xA19447D83294E29F` (3: veh,*expected,*actual) ret BOOL | CONFIRMED | verify attach |
| Кол-во harness slots модели | `_GET_NUM_DRAFT_VEHICLE_HARNESS_PED` | `0x5B1A26BB18E7D451` (1: modelHash) | CONFIRMED | iterate harness 0..N-1 |
| Draft horse → vehicle | `_GET_VEHICLE_DRAFT_HORSE_IS_ATTACHED_TO` | `0xE4770DA1B8FF4FD1` (1: horse) ret Vehicle | CONFIRMED | |
| Last draft vehicle | `_GET_LAST_VEHICLE_DRAFT_HORSE_WAS_ATTACHED_TO` | `0x5064DB5083C29921` (1: horse) ret Vehicle | CONFIRMED | |
| Draft? | `IS_DRAFT_VEHICLE` | `0xEA44E97849E9F3DD` (1) | CONFIRMED | |
| Animals can detach | `_SET_DRAFT_VEHICLE_ANIMALS_CAN_DETACH` | `0x6090A031C69F384E` (2) | CONFIRMED | false — запретить |
| Yoke can break | `_SET_DRAFT_VEHICLE_YOKE_CAN_BREAK` | `0x226C6A4E3346D288` (2) | CONFIRMED | false — запретить |
| Auto-create draft | `_SET_DRAFT_VEHICLE_ALLOW_DRAFT_ANIMAL_AUTO_CREATION` | `0x87344305778E5415` (2) | CONFIRMED | после manual attach false |
| Draft random seed | `_SET_DRAFT_ANIMAL_RANDOM_SEED` | `0x8C6D9A399126C194` (2) | CONFIRMED (signature) | deterministic auto-create |
| Draft desired speed | `_SET_DRAFT_VEHICLE_DESIRED_SPEED` | `0x0C3F0F7F92CA847C` (2) | SIGNATURE-ONLY | для AI/remote control; тест |
| Draft desired speed get | `_GET_DRAFT_VEHICLE_DESIRED_SPEED` | `0xC6D7DDC843176701` (1) | CONFIRMED | |
| Break draft wheel | `_BREAK_OFF_DRAFT_WHEEL` | `0xC372B6A88F6E4AD8` (3) | CONFIRMED | |
| Break straps | `_BREAK_VEHICLE_STRAPS` | `0xD1EFA8D68BF5D63D` (4) | CONFIRMED | |
| Coach robbery loot | `_SET_FORCE_COACH_ROBBERY_LOOT` | `0xF489F94BFEE12BB0` (2: vehicle, Hash coachrobberyLoot) | SIGNATURE-ONLY | hashes `COACH2_BOOT_LOOT_ITEMS_COACHROB*`; host real |
| Loot entity | `TASK_LOOT_ENTITY` | `0x48FAE038401A2888` (2: ped, entity) | CONFIRMED (signature) | host real; vehicle cargo depends on loot table |
| Pickup carriable | `TASK_PICKUP_CARRIABLE_ENTITY` | `0x502EC17B1BED4BFA` (2: ped, entity) | CONFIRMED (signature) | host real |
| Refresh loot state | `_REFRESH_LOOT_STATE_FOR_PED` | `0x5463C962BC7777C3` (5: ped, p1, Ped* target, p3, p4) | SIGNATURE-ONLY | |
| Request loot list | `_REQUEST_ENTITY_LOOT_LIST` | `0xA88E215CEB0435C0` (6: mount, Any* struct, hash requestType, flag, p4, p5) | SIGNATURE-ONLY | |
| Loot tables info | `_LOOT_TABLES_GET_INFO` | `0x48E4D50F87A96AA5` (6) | SIGNATURE-ONLY | |

**Loot from vehicle:** отдельного generic «vehicle inventory» натива в 1491.50 **не найдено**; груз повозки — это scenario/loot-table (coach robbery) + `TASK_LOOT_ENTITY`/carriable. Прямое «вынуть предмет из vehicle» зависит от конкретного vehicle/coach robbery сценария; MVP v20 — только coach robbery loot hash / loot tables, host authority.

**Occupancy checks** (см. также 3.2): `GET_DRIVER_OF_VEHICLE`, `GET_PED_IN_VEHICLE_SEAT`, `GET_LAST_PED_IN_VEHICLE_SEAT`, `IS_VEHICLE_SEAT_FREE`, `ARE_ANY_VEHICLE_SEATS_FREE`, `GET_VEHICLE_MAX_NUMBER_OF_PASSENGERS`, `IS_PED_IN_ANY_VEHICLE`, `IS_PED_IN_VEHICLE`, `GET_VEHICLE_PED_IS_IN/USING/ENTERING`; host должен валидировать seat перед посадкой/управлением; guest proxy seats приходят через `VehicleOccupants 0x21`.

### 3.6. Ограничения mission entities / network / draft (для дизайна v20)

- **Mission entity**: `SET_ENTITY_AS_MISSION_ENTITY` + `IS_ENTITY_A_MISSION_ENTITY`; не удалять без unmark; mission vehicles/NPC могут быть защищены от damage/task; remote control требует проверки mission state и владельца. `SET_ENTITY_INVINCIBLE`/`SET_VEHICLE_STRONG`/`SET_VEHICLE_CAN_BREAK` могут отменять урон.
- **Guest proxy**: mission/no-collision/invincible; локальные нативы визуальные; host authority обязательна. `SET_ENTITY_AS_MISSION_ENTITY` false перед удалением.
- **Draft**: wagon + отдельные лошади; harness attach/detach, `GET_DRAFT_ANIMAL_COUNT`; `_BREAK_VEHICLE_STRAPS`/`_BREAK_OFF_DRAFT_WHEEL` — только если механика поломки нужна; remote control может не «тянуть» лошадей через forward speed — тестировать `_SET_DRAFT_VEHICLE_DESIRED_SPEED`/AI task.
- **Player-driven host vehicle**: host input имеет приоритет; remote control надо либо отключать, либо явно забирать контроль (нет натива «передать контроль»); иначе конфликт/рывки.
- **Network**: это TCP relay, не RDR2 network; `CREATE_VEHICLE(isNetwork=false)`, `isNetwork` не даёт удалённого контроля. Все команды — через host-процесс.
- **kMaxPlayers=2**, `VehicleOccupants 0x21` max 8 entries; rate-limit/intent validation нужны.

### 3.7. Предлагаемая форма v20-команды (design-only, не реализовано)

`Guest→Host VehicleIntent` (host-only relay):
```
vehicleNetId u32, seq u16, action u8 (enter/exit/hijack/door/damage/control),
seat i8, doorId u8, windowId u8, wheelId u8,
steer f32, throttle f32, brake f32
```
Host:
1. Проверить vehicle netId, расстояние, `IS_ENTITY_A_MISSION_ENTITY`, жив ли vehicle/ped, права (driver/passenger), rate limit.
2. Для входа/выхода/hijack — `SET_PED_INTO_VEHICLE`/`TASK_ENTER_VEHICLE`/`TASK_LEAVE_VEHICLE`/`TASK_BREAK_VEHICLE_DOOR_LOCK` на host real.
3. Для дверей/урона — host-нативы из 3.3, затем разослать guest состояние (health/broken/door state).
4. Для управления — каждый тик `SET_VEHICLE_FORWARD_SPEED`/`SET_VEHICLE_STEER_BIAS`/`SET_VEHICLE_HANDBRAKE` (или AI task на host driver); guest proxy — визуал/интерполяция.
5. Guest local player не телепортировать в host entity; визуальная посадка — только через host ghost/seat или local proxy (архитектурное решение).

---

## 4. Итоговая сводка (для architect)

1. **Blip name:** `_SET_BLIP_NAME` 0x9CB1A1623062F402(2) + `MISC_VAR_STRING` 0xFA925AC00EB830B9; ставить имя после `BLIP_ADD_FOR_COORDS`; `SET_BLIP_NAME_FROM_TEXT_FILE` не подходит для ников, `SET_BLIP_NAME_TO_PLAYER_NAME` — только local Player. **CONFIRMED hash/signature; display — in-game verify.**
2. **Wheel spin:** публичного wheel-rotation натива **нет**. Тестировать `SET_VEHICLE_FORWARD_SPEED` (0xF9F92AF49F12F6E7) на dynamic/no-collision proxy ± `SET_VEHICLE_STEER_BIAS`/`SET_VEHICLE_HANDBRAKE`/`SET_VEHICLE_ON_GROUND_PROPERLY`; для wagon — `_SET_DRAFT_VEHICLE_DESIRED_SPEED`. Риск физики/дрейфа; **SIGNATURE-ONLY/UNCERTAIN**, не обещать до теста.
3. **Interactivity:** seats/doors/damage/controls/draft/loot хэши и сигнатуры в основном подтверждены; lock status enum и Task-параметры — uncertain. Host authority + новый v20 intent-канал; прямого remote-control натива нет; mission/draft/player-driven ограничения.
4. **Не реализовывать wheel-props/bone-хаки** без подтверждения; сначала A/B `SET_VEHICLE_FORWARD_SPEED`.

**Источники:** `third_party/rdr2-runtime-scripthook/source/scripting_v3/RDR2.Native/NativeDecl.cs` (1491.50), `.../RDR2/Blip.cs`, latest `alloc8or/rdr3-nativedb-data` (`natives.json`), Halen84 `eEnterExitVehicleFlags`, `eDoorId/README.md`, current code `src/coop.cpp:371-426,7553`, `src/world_sync.cpp:2307-2417`, `net/protocol.h` (v19 `VehicleOccupants 0x21`, `TrainSnapshotV2 0x22`).
