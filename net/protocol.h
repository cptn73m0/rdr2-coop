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
};

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

// Размер полезной нагрузки (без len/type). 88 байт.
struct FramePayload {
    float    x = 0.f;          // world X
    float    y = 0.f;          // world Y
    float    z = 0.f;          // world Z
    float    heading = 0.f;    // поворот
    uint8_t  move = 0;         // MoveState (Idle/Walk/Run/RideHorse)
    uint8_t  state = 0;        // PedState (доп. поведение)
    uint8_t  flags = 0;        // битовые флаги (sprint/weapon)
    uint8_t  pad0 = 0;
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