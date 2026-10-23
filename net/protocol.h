#pragma once

// Общий протокол связи RDR2 CO-OP: константы команд и форматы пакетов.
// Используется и сервером-релеем, и сетевым клиентом в ASI-плагине.

#include <cstddef>
#include <cstdint>

namespace net {

// Версия протокола. Должна совпадать у клиента и сервера; при несовпадении
// сервер отклоняет JOIN (Msg::JoinRejected), чтобы не разбирать чужие байты.
// v14: OutfitState + wearable states (открытая/закрытая куртка, рукава и т.п.).
// v15 (E0): actor-репликация NPC — ActorSpawnBatch/ActorFrames/ActorDespawn/
//           ActorEvent/ActorOutfit (хост) и ActorResync (гость -> хост).
// v16 (E2c-b): ActorAppearance — точная внешность NPC (asset/tint), хост -> гость.
constexpr uint16_t kProtocolVersion = 16;

// Максимум игроков в сессии (Spike 1: хост + один гость).
constexpr int kMaxPlayers = 2;

// Причины отказа в JOIN (payload Msg::JoinRejected: reason:u8).
enum class JoinRejectReason : uint8_t {
    VersionMismatch = 1,
    ServerFull      = 2,
    NameTaken       = 3,
    BadName         = 4,
};

// Команды (type в кадре).
enum class Msg : uint8_t {
    Join        = 0x01, // клиент -> сервер: подключиться
    JoinOk      = 0x02, // сервер -> клиент: ответ, вкл. сохранённую позицию
    PlayerJoined= 0x03, // сервер -> остальные: кто-то подключился
    Frame       = 0x04, // клиент -> сервер -> остальные: состояние игрока
    Leave       = 0x05, // клиент -> сервер: выход
    PlayerLeft  = 0x06, // сервер -> остальные: кто-то вышел
    Died        = 0x07, // клиент -> сервер: локальный игрок умер (лог в консоли)
    WorldState  = 0x08, // клиент-хост -> сервер -> остальные: время и погода
    Outfit      = 0x09, // клиент -> сервер -> остальные: компоненты одежды
    Respawned   = 0x0A, // клиент -> сервер -> остальные: игрок ожил после смерти
    Shot        = 0x0B, // клиент -> сервер -> остальные: событие выстрела
    Ping        = 0x0C, // клиент -> сервер: замер задержки (метка времени)
    Pong        = 0x0D, // сервер -> клиент: ответ с той же меткой (RTT)
    HorseOutfit = 0x0E, // клиент -> сервер -> остальные: снаряжение лошади
    JoinRejected= 0x0F, // сервер -> клиент: отказ в JOIN (версия/полный/ник)
    WorldSnapshot = 0x10, // хост -> сервер -> гость: сущности мира (P1)
    ActionState = 0x11,   // клиент -> сервер -> остальные: действие/сценарий (P2)
    PlayerVehicle = 0x12, // клиент -> сервер -> остальные: посадка в транспорт
    MissionEvent = 0x13,  // хост -> сервер -> гость: миссия завершена/рейтинг
    MissionState = 0x14,  // хост -> сервер -> гость: активная Story-миссия
    MissionBarrier = 0x15,// P3: offer/ready/start-window/started/abort
    HostChanged = 0x16,   // сервер -> клиент: новый хост / смена роли
    TrainSnapshot = 0x17, // R&D: поезд/состав (engine + вагоны по порядку)
    // E0 (v15): actor-репликация NPC. 0x18-0x1C — только хост -> сервер ->
    // остальные; 0x1D — только гость -> сервер -> хосту.
    ActorSpawnBatch = 0x18, // хост -> сервер -> гость: спавн/респавн актёров
    ActorFrames     = 0x19, // хост -> сервер -> гость: кадры движения актёров
    ActorDespawn    = 0x1A, // хост -> сервер -> гость: удаление актёра
    ActorEvent      = 0x1B, // хост -> сервер -> гость: событие актёра
    ActorOutfit     = 0x1C, // хост -> сервер -> гость: outfit актёра
    ActorResync     = 0x1D, // гость -> сервер -> хосту: запрос resync
    // E2c-b (v16): точная внешность NPC — только хост -> сервер -> гость.
    ActorAppearance = 0x1E, // хост -> сервер -> гость: asset/tint внешности
};

// 1 байт: 1 = «ты теперь хост», 0 = «ты гость».
struct HostChangedPayload {
    uint8_t isHost = 0;
    uint8_t pad[3] = {0, 0, 0};
};
static_assert(sizeof(HostChangedPayload) == 4,
              "HostChangedPayload must be 4 bytes");

// Компоненты одежды игрока (наряд): список хэшей одетых shop-предметов.
// Лимит 96 (а не 32): GET_NUM_COMPONENT_CATEGORIES_IN_PED на реальном педе
// возвращает 47–80 категорий; при лимите 32 хвост обрезался, часть одежды
// не передавалась — у призрака были видны случайные слои/кожа.
constexpr int kMaxOutfitComponents = 96;
struct OutfitState {
    unsigned long long model = 0;            // модель педа (для проверки)
    uint32_t count = 0;                      // сколько компонентов одето
    uint32_t components[kMaxOutfitComponents] = {0}; // хэши компонентов
    // Wearable state для каждого компонента (0 = нет состояния).
    // Без него у гостя дефолтное состояние слоя (например, куртка "закрыта"),
    // и рубашка просвечивает/пропадает не так, как у оригинала.
    uint32_t wearable[kMaxOutfitComponents] = {0};
};
// 8 + 4 + 96*4 + 96*4 = 780, выравнивание до 8 → 784. Формат уходит в сеть
// как есть; менять только вместе с kProtocolVersion.
static_assert(sizeof(OutfitState) == 784, "OutfitState must be 784 bytes");

// Состояние мира, транслируемое хостом всем игрокам (время + погода).
// Хостом считается ПЕРВЫЙ подключившийся игрок; остальные применяют его
// время и погоду у себя (клиентские часы/погода не расходятся с хостом).
struct WorldState {
    uint8_t  hour = 0;        // 0..23
    uint8_t  minute = 0;      // 0..59
    uint8_t  second = 0;      // 0..59
    uint8_t  pad = 0;         // выравнивание
    uint32_t weatherType1 = 0; // текущий тип погоды (хэш)
    uint32_t weatherType2 = 0; // следующий тип (для плавного перехода)
    float    weatherPct = 0.f; // доля weatherType2 в текущем состоянии (0..1)
};
static_assert(sizeof(WorldState) == 16, "WorldState must be 16 bytes");

// Длина имени игрока (как строка). Строки в пакетах: 1 байт длины + байты.
using NameLen = uint8_t;
constexpr std::size_t kMaxName = 255;

// Доп. состояние педа (state в кадре). move описывает локомоцию,
// state — особое положение/поведение (приоритет над move).
enum class PedState : uint8_t {
    Normal            = 0,
    Ragdoll           = 1,
    InCover           = 2,
    Swimming          = 3,
    SwimUnderwater    = 4,
    Climbing          = 5,
    Jumping           = 6,
    Falling           = 7,
};

// Битовые флаги (flags в кадре).
constexpr uint8_t kFlagSprinting   = 1u << 0;
constexpr uint8_t kFlagWeaponReady = 1u << 1;
// Оружие у отправителя: drawn = в руках (не в кобуре), changing = идёт
// анимация доставания/убирания. Нужны приёмнику для draw/holster анимаций.
constexpr uint8_t kFlagWeaponDrawn    = 1u << 2;
constexpr uint8_t kFlagWeaponChanging = 1u << 3;

// Действие (action в кадре): кратковременное/контекстное поведение поверх
// движения. Идёт в отдельном байте кадра (pad0). Приоритет ниже state
// (ragdoll и т.п. обрабатываются первыми). None = обычное движение/стойка.
enum class Action : uint8_t {
    None      = 0,
    Shooting  = 1,   // стреляет (IS_PED_SHOOTING)
    Reloading = 2,   // перезаряжается (IS_PED_RELOADING)
    Sitting   = 3,   // сидит (IS_PED_SITTING)
    InScenario= 4,   // в сценарии (IS_PED_USING_ANY_SCENARIO: ест/пьёт/курит…)
    Aiming    = 5,   // целится (IS_PED_AIMING_FROM_COVER / оружие наготове)
    Drunk     = 6,   // пьян (_IS_PED_DRUNK)
};

// Размер полезной нагрузки (без len/type). 88 байт.
struct FramePayload {
    float    x = 0.f;          // world X
    float    y = 0.f;          // world Y
    float    z = 0.f;          // world Z
    float    heading = 0.f;    // поворот
    uint8_t  move = 0;         // MoveState (Idle/Walk/Run/RideHorse)
    uint8_t  state = 0;        // PedState (доп. поведение)
    uint8_t  flags = 0;        // битовые флаги (sprint/weapon)
    uint8_t  action = 0;       // Action (стрельба/сидение/сценарий…)
    float    health = 0.f;     // здоровье 0..1
    // Точка прицела: куда игрок целится/стреляет (мировые координаты).
    // Получатель разворачивает педа и целится/стреляет именно туда.
    float    aimX = 0.f;
    float    aimY = 0.f;
    float    aimZ = 0.f;
    float    aimDist = 0.f;    // дистанция до точки прицела (0 = нет данных)
    uint64_t weaponHash = 0;   // хэш оружия в руке (0 = нет)
    uint64_t seq = 0;
    // Резерв:
    // [0] — хэш модели лошади (если верхом);
    // [1] — хэш мета-наряда игрока (_GET_PED_META_OUTFIT_HASH);
    // [2] — модель персонажа игрока (GET_ENTITY_MODEL(playerPed)).
    uint64_t reserved[3] = {0, 0, 0};
    // Мета-наряд (снаряжение: седло/одеяло/поклажа) лошади — применяется к
    // лошади педа-призрака тем же механизмом, что и наряд игрока.
    uint64_t horseOutfitHash = 0;
};
static_assert(sizeof(FramePayload) == 88, "FramePayload must be 88 bytes");

// Событие выстрела: точка попадания/прицеливания. Шлётся отдельным пакетом,
// чтобы НИ ОДИН выстрел не терялся (опрос состояния в кадре — ненадёжен).
struct ShotPayload {
    float x = 0.f;
    float y = 0.f;
    float z = 0.f;
    float pad = 0.f;
};
static_assert(sizeof(ShotPayload) == 16, "ShotPayload must be 16 bytes");

// ---------------- P1: репликация мира (NPC) ----------------
// Хост 10 Гц шлёт гостю снапшот сущностей вокруг игроков. Пока только Ped;
// Vehicle/Object будут добавлены отдельными kind. Позиция — авторитетная,
// гость держит визуальные прокси.
constexpr uint8_t kEntityKindPed     = 1;
constexpr uint8_t kEntityKindVehicle = 2;
constexpr uint8_t kEntityKindObject  = 3;

constexpr uint8_t kEntityFlagMission = 1u << 0; // mission entity (важнее)
constexpr uint8_t kEntityFlagDead    = 1u << 1; // мёртв/умирает
constexpr uint8_t kEntityFlagMounted = 1u << 2; // сидит на лошади

// Locomotion-состояние прокси: 0 — стоять, 1 — шаг, 2 — бег/галоп.
constexpr uint8_t kEntityStateIdle = 0;
constexpr uint8_t kEntityStateWalk = 1;
constexpr uint8_t kEntityStateRun  = 2;

// Максимум сущностей в WorldSnapshot: 32 Ped + 16 Vehicle + 16 mission Object.
constexpr int kMaxWorldEntities = 64;

struct WorldEntity {
    uint64_t netId = 0;   // стабильный id (не handle)
    uint32_t model = 0;   // хэш модели
    float    x = 0.f;
    float    y = 0.f;
    float    z = 0.f;
    float    heading = 0.f;
    uint8_t  kind = kEntityKindPed;
    uint8_t  flags = 0;
    uint8_t  state = kEntityStateIdle;
    uint8_t  pad = 0;
};
static_assert(sizeof(WorldEntity) == 32, "WorldEntity must be 32 bytes");

struct WorldSnapshotHeader {
    uint16_t count = 0;
    uint16_t pad = 0;
};
static_assert(sizeof(WorldSnapshotHeader) == 4, "WorldSnapshotHeader must be 4 bytes");

// Полный размер сообщения WorldSnapshot: header + count * WorldEntity.
constexpr std::size_t kWorldSnapshotMaxSize =
    sizeof(WorldSnapshotHeader) + kMaxWorldEntities * sizeof(WorldEntity);

// ---------------- R&D: поезд/состав ----------------
// payload = TrainSnapshotHeader + carCount * WorldEntity (kind=Vehicle).
// В R&D-режиме гость пока только логирует; визуально вагоны идут через
// обычный WorldSnapshot, если попали в его бюджет.
constexpr int kMaxTrainCars = 16;

struct TrainSnapshotHeader {
    uint32_t engineNetId = 0;   // netId локомотива из WorldSnapshot
    uint32_t engineModel = 0;
    float speed = 0.f;          // GET_ENTITY_SPEED(engine)
    uint8_t direction = 0;      // _GET_TRAIN_DIRECTION(engine)
    uint8_t carCount = 0;       // 0..kMaxTrainCars
    uint16_t pad = 0;
};
static_assert(sizeof(TrainSnapshotHeader) == 16,
              "TrainSnapshotHeader must be 16 bytes");

constexpr std::size_t kTrainSnapshotMaxSize =
    sizeof(TrainSnapshotHeader) + kMaxTrainCars * sizeof(WorldEntity);

// ---------------- P2: Action v2 (действия/сценарии) ----------------
// Каждый клиент шлёт состояние СВОЕГО персонажа; сервер ретранслирует.
// Клипы/фазы AnimGraph публичным SDK не читаются — передаём наблюдаемые
// высокоуровневые состояния (сценарий/укрытие/переноска/…) и хэш сценария.
enum class ActionMode : uint8_t {
    None     = 0,
    Scenario = 1,   // сидение/курение/еда/питьё и т.п. (scenarioHash)
    Cover    = 2,   // в укрытии
    Carry    = 3,   // несёт тело/предмет
    Drag     = 4,   // тащит
    Hogtie   = 5,   // связан
    Loot     = 6,   // лутает
    Fishing  = 7,   // рыбачит
};

constexpr uint16_t kActionFlagAiming   = 1u << 0;
constexpr uint16_t kActionFlagShooting = 1u << 1;
constexpr uint16_t kActionFlagReloading= 1u << 2;
constexpr uint16_t kActionFlagInCover  = 1u << 3;
constexpr uint16_t kActionFlagCarrying = 1u << 4;
constexpr uint16_t kActionFlagDragging = 1u << 5;
constexpr uint16_t kActionFlagHogtied  = 1u << 6;
constexpr uint16_t kActionFlagFishing  = 1u << 7;

struct ActionStatePayload {
    uint8_t  mode = 0;         // ActionMode
    uint8_t  flags = 0;        // kActionFlag*
    uint16_t pad = 0;
    uint32_t scenarioHash = 0; // _GET_ACTIVE_DYNAMIC_SCENARIO
    uint32_t animDictHash = 0; // резерв: точный клип, если появится
    uint32_t animNameHash = 0; // резерв
    float    phase = 0.f;      // резерв: 0..1
    uint32_t targetNetId = 0;  // цель переноски/лутания (позже, из P1)
};
static_assert(sizeof(ActionStatePayload) == 24, "ActionStatePayload must be 24 bytes");

// ---------------- P2: посадка игрока в транспорт ----------------
// Каждый клиент шлёт состояние СВОЕГО персонажа; сервер ретранслирует.
// Гость по этому сообщению создаёт/двигает транспорт и сажает в него призрака.
constexpr uint8_t kPlayerVehicleFlagValid  = 1u << 0;
constexpr uint8_t kPlayerVehicleFlagDriver = 1u << 1;

struct PlayerVehicleState {
    uint32_t vehicleModel = 0; // 0 = не в транспорте
    uint32_t netId = 0;        // резерв: мировой netId (P1)
    float x = 0.f, y = 0.f, z = 0.f;
    float heading = 0.f;
    int8_t seat = -1;          // -1 = водитель (как в RDR2/GTA)
    uint8_t flags = 0;         // kPlayerVehicleFlag*
    uint16_t pad = 0;
    uint32_t reserved = 0;     // выравнивание/резерв (итого 32 Б)
};
static_assert(sizeof(PlayerVehicleState) == 32,
              "PlayerVehicleState must be 32 bytes");

// ---------------- P3: MissionData-событие ----------------
// Хост сравнивает каталог MissionData со своим baseline и при новом завершении
// шлёт missionId + рейтинг. Гость пишет зачёт в СВОЙ сейв (без копирования мира).
constexpr uint8_t kMissionFlagCompleted     = 1u << 0;
constexpr uint8_t kMissionFlagRatingUpdate  = 1u << 1;

struct MissionEventPayload {
    uint32_t missionId = 0; // joaat 4-char MissionData ID
    uint8_t rating = 0;     // MISSION_RATING_* (0..5)
    uint8_t flags = 0;      // kMissionFlag*
    uint16_t pad = 0;
};
static_assert(sizeof(MissionEventPayload) == 8,
              "MissionEventPayload must be 8 bytes");

// ---------------- P3: активная миссия ----------------
// Хост находит активный runtime-скрипт миссии через
// GET_NUMBER_OF_THREADS_RUNNING_THE_SCRIPT_WITH_THIS_HASH и сообщает ID/состояние.
constexpr uint8_t kMissionStateIdle   = 0;
constexpr uint8_t kMissionStateActive = 1;

struct MissionStatePayload {
    uint32_t missionId = 0;   // MissionData joaat
    uint32_t runtimeHash = 0; // joaat runtime script
    uint8_t state = 0;        // kMissionState*
    uint8_t flags = 0;
    uint16_t pad = 0;
};
static_assert(sizeof(MissionStatePayload) == 12,
              "MissionStatePayload must be 12 bytes");

// ---------------- P3: барьер совместного старта миссии ----------------
// Host: Offer -> (guest Ready) -> StartWindow -> (guest Started) -> done.
// Гость сам запускает свою копию миссии; барьер только координирует окно.
constexpr uint8_t kMissionBarrierOffer       = 1;
constexpr uint8_t kMissionBarrierReady       = 2;
constexpr uint8_t kMissionBarrierStartWindow = 3;
constexpr uint8_t kMissionBarrierStarted     = 4;
constexpr uint8_t kMissionBarrierAbort       = 5;

constexpr uint8_t kMissionBarrierFlagEligible = 1u << 0;
constexpr uint8_t kMissionBarrierFlagTimeout  = 1u << 1;

struct MissionBarrierPayload {
    uint32_t missionId = 0;
    uint32_t epoch = 0;   // эпоха барьера у хоста
    uint8_t phase = 0;    // kMissionBarrier*
    uint8_t flags = 0;
    uint16_t pad = 0;
};
static_assert(sizeof(MissionBarrierPayload) == 12,
              "MissionBarrierPayload must be 12 bytes");

// ---------------- E0: actor-репликация NPC (протокол v15) ----------------
// Хост реплицирует до kMaxActors "актёров" (управляемых NPC/игровых педов,
// кроме собственно игроков): батч спавна, батч кадров движения, despawn,
// события и outfit. Сервер принимает 0x18-0x1C только от хоста и релеит
// байт-в-байт; 0x1D (resync) — только от гостя и только хосту.
constexpr int kMaxActors = 12;

// Флаги ActorSpawn::flags.
constexpr uint8_t kActorFlagMounted   = 1u << 0; // сидит на лошади
constexpr uint8_t kActorFlagDead      = 1u << 1; // мёртв/умирает
constexpr uint8_t kActorFlagDrawn     = 1u << 2; // оружие в руках
constexpr uint8_t kActorFlagChanging  = 1u << 3; // анимация draw/holster
constexpr uint8_t kActorFlagCombat    = 1u << 4; // в бою
constexpr uint8_t kActorFlagScenario  = 1u << 5; // в сценарии
constexpr uint8_t kActorFlagReloading = 1u << 6; // перезаряжается
constexpr uint8_t kActorFlagAiming    = 1u << 7; // целится

// kind: 0 = human.
constexpr uint8_t kActorKindHuman = 0;

// Один актёр для батча спавна (64 байта).
struct ActorSpawn {
    uint32_t actorId = 0;      // стабильный id актора в рамках эпохи
    uint16_t epoch = 0;        // эпоха/сессия хоста
    uint8_t  kind = kActorKindHuman;
    uint8_t  flags = 0;        // kActorFlag*
    uint32_t model = 0;        // хэш модели педа
    uint32_t metaOutfit = 0;   // _GET_PED_META_OUTFIT_HASH (0 = нет)
    uint32_t mountActorId = 0; // actorId лошади (0 = нет)
    uint32_t weaponHash = 0;   // оружие в руке (0 = нет)
    uint32_t scenarioHash = 0; // сценарий (0 = нет)
    float    x = 0.f;
    float    y = 0.f;
    float    z = 0.f;
    float    heading = 0.f;
    uint8_t  health = 0;       // 0..255 -> 0..1 у получателя
    uint8_t  moveStyle = 0;    // стиль движения
    uint8_t  phase = 0;        // фаза клипа/сценария 0..255
    uint8_t  pad = 0;
    int16_t  vx = 0;           // скорость, фикс. точка (см. игровой слой)
    int16_t  vy = 0;
    int16_t  vz = 0;
    uint16_t pad2 = 0;
    uint32_t reserved = 0;
    uint32_t reserved2 = 0;
};
static_assert(sizeof(ActorSpawn) == 64, "ActorSpawn must be 64 bytes");

struct ActorSpawnBatchHeader {
    uint32_t epoch = 0;
    uint16_t count = 0; // 0..kMaxActors
    uint16_t pad = 0;
};
static_assert(sizeof(ActorSpawnBatchHeader) == 8,
              "ActorSpawnBatchHeader must be 8 bytes");
// payload = header + count * ActorSpawn; максимум 8 + 12*64 = 776.
constexpr std::size_t kActorSpawnBatchMaxSize =
    sizeof(ActorSpawnBatchHeader) + kMaxActors * sizeof(ActorSpawn);
static_assert(kActorSpawnBatchMaxSize == 776,
              "ActorSpawnBatch max payload must be 776 bytes");

// Один кадр движения актёра (32 байта).
struct ActorFrame {
    uint32_t actorId = 0;
    uint16_t flags = 0;
    uint8_t  animState = 0;
    uint8_t  speed = 0;        // 0..255 (доля максимальной скорости)
    float    x = 0.f;
    float    y = 0.f;
    float    z = 0.f;
    float    heading = 0.f;
    uint32_t targetActorId = 0; // цель атаки/взаимодействия (0 = нет)
    uint16_t seq = 0;           // порядковый номер кадра
    uint16_t pad = 0;
};
static_assert(sizeof(ActorFrame) == 32, "ActorFrame must be 32 bytes");

struct ActorFramesHeader {
    uint32_t epoch = 0;
    uint16_t seq = 0;
    uint16_t count = 0; // 0..kMaxActors
};
static_assert(sizeof(ActorFramesHeader) == 8,
              "ActorFramesHeader must be 8 bytes");
// payload = header + count * ActorFrame; максимум 8 + 12*32 = 392.
constexpr std::size_t kActorFramesMaxSize =
    sizeof(ActorFramesHeader) + kMaxActors * sizeof(ActorFrame);
static_assert(kActorFramesMaxSize == 392,
              "ActorFrames max payload must be 392 bytes");

// Удаление актёра (12 байт).
struct ActorDespawnPayload {
    uint32_t epoch = 0;
    uint32_t actorId = 0;
    uint8_t  reason = 0;
    uint8_t  pad[3] = {0, 0, 0};
};
static_assert(sizeof(ActorDespawnPayload) == 12,
              "ActorDespawnPayload must be 12 bytes");

// Событие актёра (32 байта). x/y/z — u32-поля (формат задаёт игровой слой).
struct ActorEventPayload {
    uint32_t epoch = 0;
    uint32_t actorId = 0;
    uint8_t  type = 0;
    uint8_t  flags = 0;
    uint16_t pad = 0;
    uint32_t a = 0;
    uint32_t b = 0;
    uint32_t x = 0;
    uint32_t y = 0;
    uint32_t z = 0;
};
static_assert(sizeof(ActorEventPayload) == 32,
              "ActorEventPayload must be 32 bytes");

// Outfit актёра (792 байта): epoch + actorId + net::OutfitState (784).
struct ActorOutfitPayload {
    uint32_t epoch = 0;
    uint32_t actorId = 0;
    OutfitState outfit{};
};
static_assert(sizeof(ActorOutfitPayload) == 792,
              "ActorOutfitPayload must be 792 bytes");

// Запрос resync от гостя хосту (12 байт).
struct ActorResyncPayload {
    uint32_t epoch = 0;
    uint32_t lastSeq = 0;
    uint8_t  reason = 0;
    uint8_t  pad[3] = {0, 0, 0};
};
static_assert(sizeof(ActorResyncPayload) == 12,
              "ActorResyncPayload must be 12 bytes");

// ---------------- E2c-b (v16): точная внешность NPC ----------------
// Хост присылает полный набор (snapshot) валидных компонентов внешности
// одного актёра: до kActorAppearanceMaxComponents=16 записей (у человека
// обычно 8-16 слоёв; меньшее ограничение обрезало бы одежду).
// Запись описывает компонент внешности: drawable/albedo/normal/material/
// palette (хэши метаданных; 0 допустим для необязательных guid/слотов)
// и три tint-индекса. reserved = 0 (зарезервирован под будущее;
// сервер/клиент отбрасывают ненулевой reserved).
constexpr std::size_t kActorAppearanceMaxComponents = 16;

struct ActorAppearanceEntry {
    uint32_t drawable = 0;
    uint32_t albedo = 0;
    uint32_t normal = 0;
    uint32_t material = 0;
    uint32_t palette = 0;
    int16_t  tint0 = 0;
    int16_t  tint1 = 0;
    int16_t  tint2 = 0;
    uint16_t reserved = 0;
};
static_assert(sizeof(ActorAppearanceEntry) == 28,
              "ActorAppearanceEntry must be 28 bytes");

struct ActorAppearanceHeader {
    uint32_t epoch = 0;
    uint32_t actorId = 0;
    uint8_t  count = 0; // 1..kActorAppearanceMaxComponents (0 не шлём)
    uint8_t  pad[3] = {0, 0, 0};
};
static_assert(sizeof(ActorAppearanceHeader) == 12,
              "ActorAppearanceHeader must be 12 bytes");
// payload = header + count * ActorAppearanceEntry; максимум 12 + 16*28 = 460.
constexpr std::size_t kActorAppearanceMaxSize =
    sizeof(ActorAppearanceHeader) +
    kActorAppearanceMaxComponents * sizeof(ActorAppearanceEntry);
static_assert(kActorAppearanceMaxSize == 460,
              "ActorAppearance max payload must be 460 bytes");

// Размеры полей кадра.
constexpr std::size_t kLenSize   = 4; // u32
constexpr std::size_t kTypeSize  = 1; // u8
constexpr std::size_t kHeaderSize = kLenSize + kTypeSize;

// Максимальный размер сообщения (защита от битых кадров).
constexpr uint32_t kMaxPayload = 64 * 1024;

} // namespace net