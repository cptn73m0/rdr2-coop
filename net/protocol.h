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
};

// Длина имени игрока (как строка). Строки в пакетах: 1 байт длины + байты.
using NameLen = uint8_t;
constexpr std::size_t kMaxName = 255;

// Размер полезной нагрузки (без len/type).
struct FramePayload {
    float    x = 0.f;
    float    y = 0.f;
    float    z = 0.f;
    float    heading = 0.f;
    uint8_t  move = 0;   // MoveState
    uint64_t seq = 0;
};
static_assert(sizeof(FramePayload) == 32, "FramePayload must be 32 bytes");

// Размеры полей кадра.
constexpr std::size_t kLenSize   = 4; // u32
constexpr std::size_t kTypeSize  = 1; // u8
constexpr std::size_t kHeaderSize = kLenSize + kTypeSize;

// Максимальный размер сообщения (защита от битых кадров).
constexpr uint32_t kMaxPayload = 64 * 1024;

} // namespace net