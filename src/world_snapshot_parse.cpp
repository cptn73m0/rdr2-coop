#include "world_snapshot_parse.h"

#include <cstring>

namespace coop {
namespace world_sync {

bool ParseWorldSnapshot(const std::vector<uint8_t>& payload,
                        ParsedWorldSnapshot& out)
{
    out.count = 0;
    out.entities.clear();

    if (payload.size() < sizeof(net::WorldSnapshotHeader))
        return false;

    net::WorldSnapshotHeader header{};
    std::memcpy(&header, payload.data(), sizeof(header));
    if (header.count > net::kMaxWorldEntities)
        return false;

    const std::size_t need = sizeof(header) +
        static_cast<std::size_t>(header.count) * sizeof(net::WorldEntity);
    if (payload.size() != need)
        return false;

    out.count = header.count;
    out.entities.resize(header.count);
    for (uint16_t i = 0; i < header.count; ++i)
    {
        std::memcpy(&out.entities[i],
                    payload.data() + sizeof(header) +
                        static_cast<std::size_t>(i) * sizeof(net::WorldEntity),
                    sizeof(net::WorldEntity));
    }
    return true;
}

} // namespace world_sync
} // namespace coop
