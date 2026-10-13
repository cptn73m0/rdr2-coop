#pragma once

// Общее для сервера: состояние игрока и служебные функции.
// Серверная часть — отдельное (кроссплатформенное) приложение.

#include <cstdint>
#include <string>

#include "net/protocol.h"

namespace srv {

// Подключённый игрок = ник + последний принятый кадр.
struct Player {
    std::string name;
    uint64_t    sessionId = 0;
    net::FramePayload last;
    bool        hasFrame = false;
};

// Текущее состояние сервера (для логов и вывода).
} // namespace srv