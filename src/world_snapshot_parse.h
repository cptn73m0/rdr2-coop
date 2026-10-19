#pragma once

#include <cstdint>
#include <vector>

#include "net/protocol.h"

namespace coop {
namespace world_sync {

// Чистый (без нативов) разбор WorldSnapshot. Используется игровым путём
// (ApplyPendingSnapshot) и headless stress-тестом.
struct ParsedWorldSnapshot
{
    uint16_t count = 0;
    std::vector<net::WorldEntity> entities;
};

// Строгая проверка: header, count <= kMaxWorldEntities, ровно
// header + count*sizeof(WorldEntity) байт (без хвоста).
bool ParseWorldSnapshot(const std::vector<uint8_t>& payload,
                        ParsedWorldSnapshot& out);

} // namespace world_sync
} // namespace coop
