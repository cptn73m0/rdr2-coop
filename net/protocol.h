#pragma once

// Общий протокол связи RDR2 CO-OP: константы команд и форматы пакетов.
// Используется и сервером-релеем, и сетевым клиентом в ASI-плагине.

#include <cstddef>
#include <cstdint>

namespace net {

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
};

// Компоненты одежды игрока (наряд): список хэшей одетых компонентов
// (_GET_NUM_COMPONENTS_IN_PED / _GET_PED_COMPONENT_CATEGORY_BY_INDEX) + модель.
// Отправляется при смене наряда (не в каждом кадре). Получатель применяет
// каждый компонент через _EQUIP_META_PED_OUTFIT + _UPDATE_PED_VARIATION.
constexpr int kMaxOutfitComponents = 8;
struct OutfitState {
    unsigned long long model = 0;            // модель педа (для проверки)
    uint32_t count = 0;                      // сколько компонентов одето
    uint32_t components[kMaxOutfitComponents] = {0}; // хэши компонентов
};

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
static_assert(sizeof(FramePayload) == 72, "FramePayload must be 72 bytes");

// Размеры полей кадра.
constexpr std::size_t kLenSize   = 4; // u32
constexpr std::size_t kTypeSize  = 1; // u8
constexpr std::size_t kHeaderSize = kLenSize + kTypeSize;

// Максимальный размер сообщения (защита от битых кадров).
constexpr uint32_t kMaxPayload = 64 * 1024;

} // namespace net