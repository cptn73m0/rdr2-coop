#pragma once

// Общий протокол связи RDR2 CO-OP: константы команд и форматы пакетов.
// Используется и сервером-релеем, и сетевым клиентом в ASI-плагине.

#include <cstddef>
#include <cstdint>

namespace net {

// Версия протокола. Должна совпадать у клиента и сервера; при несовпадении
// сервер отклоняет JOIN (Msg::JoinRejected), чтобы не разбирать чужие байты.
// v4: OutfitState расширен до 96 компонентов (реальные категории педа 47–80).
constexpr uint16_t kProtocolVersion = 4;

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
};

// Компоненты одежды игрока (наряд): список хэшей одетых shop-предметов.
// Лимит 96 (а не 32): GET_NUM_COMPONENT_CATEGORIES_IN_PED на реальном педе
// возвращает 47–80 категорий; при лимите 32 хвост обрезался, часть одежды
// не передавалась — у призрака были видны случайные слои/кожа.
constexpr int kMaxOutfitComponents = 96;
struct OutfitState {
    unsigned long long model = 0;            // модель педа (для проверки)
    uint32_t count = 0;                      // сколько компонентов одето
    uint32_t components[kMaxOutfitComponents] = {0}; // хэши компонентов
};
// 8 + 4 + 96*4 = 396, выравнивание до 8 → 400. Формат уходит в сеть как есть;
// менять только вместе с kProtocolVersion.
static_assert(sizeof(OutfitState) == 400, "OutfitState must be 400 bytes");

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

constexpr int kMaxWorldEntities = 32;

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

// Размеры полей кадра.
constexpr std::size_t kLenSize   = 4; // u32
constexpr std::size_t kTypeSize  = 1; // u8
constexpr std::size_t kHeaderSize = kLenSize + kTypeSize;

// Максимальный размер сообщения (защита от битых кадров).
constexpr uint32_t kMaxPayload = 64 * 1024;

} // namespace net