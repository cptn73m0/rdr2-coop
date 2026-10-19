#pragma once

#include "native_call.h"
#include "net/protocol.h"

#include <cstdint>
#include <vector>

// Phase-1 local prototype: spawn a "ghost" ped alongside the player and
// drive its transform by the player transform, proving the engine will
// create and animate a second entity that we read/write each frame.
// No network yet — this is the foundation the network layer will feed later.

namespace coop {

// Called every script tick; idempotent against repeated entry into the
// world (Player 0 ped may be re-created).
void Tick();

// P1 (модель A): отправить снапшот мира от хоста (вызывается world_sync).
void SendWorldSnapshot(const std::vector<uint8_t>& payload);

// R&D: отправить TrainSnapshot от хоста (вызывается world_sync).
void SendTrainSnapshot(const std::vector<uint8_t>& payload);

// Crew: отправить seat-привязку водителя/пассажира (хост).
void SendVehicleCrew(const net::VehicleCrewPayload& crew);

// P1: служебные сущности мода (ghost/mount), которые нельзя реплицировать
// как NPC мира.
bool IsWorldSyncExcluded(unsigned int handle);

} // namespace coop