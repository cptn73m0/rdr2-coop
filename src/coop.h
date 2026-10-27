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

// P1: служебные сущности мода (ghost/mount), которые нельзя реплицировать
// как NPC мира.
bool IsWorldSyncExcluded(unsigned int handle);
// task-180: модель продавца (visitor mode не спавнит host-прокси).
bool IsVisitorVendorModel(uint32_t model);

// E1: отправка actor-сообщений от хоста (вызывается actor_sync).
void SendActorSpawnBatch(uint32_t epoch,
                         const std::vector<net::ActorSpawn>& actors);
void SendActorFrames(uint32_t epoch, uint16_t seq,
                     const std::vector<net::ActorFrame>& frames);
void SendActorDespawn(const net::ActorDespawnPayload& msg);
// E2c-b (v16): точная внешность NPC (asset/tint) — хост -> сервер -> гости.
void SendActorAppearance(uint32_t epoch, uint32_t actorId,
                         const std::vector<net::ActorAppearanceEntry>& entries);
// E9 (task-142): horse tack 0x1C и сбор outfit с произвольного педа.
void SendActorOutfit(uint32_t epoch, uint32_t actorId,
                     const net::OutfitState& outfit);
void CollectLocalOutfitForActor(unsigned int ped, net::OutfitState& out);
// E5 (v17): гость -> хост запрос урона по NPC (0x1F, только guest).
bool SendActorDamage(uint32_t epoch, uint32_t actorId, uint16_t damage,
                     uint32_t weaponHash);
// E6 (v18): сообщить свой UI-статус (карта/пауза/сатчель/магазин) другому.
bool SendPlayerPresence(uint32_t epoch, uint16_t flags);
// E3d-c (v19): occupants транспорта (хост -> сервер -> гость).
void SendVehicleOccupants(
    uint32_t vehicleNetId,
    const std::vector<net::VehicleOccupantEntry>& entries);
// E3d-b (v19): снапшот поезда/состава (хост -> сервер -> гость).
void SendTrainSnapshotV2(const net::TrainSnapshotV2Payload& payload);
// E1: запрос resync от гостя. ВАЖНО: в net/client пока есть только приём
// ActorResync, но НЕ отправка (Client::SendActorResync отсутствует) — заглушка
// до правки net-слоя (net-review), игровой слой вызывает безопасно.
void SendActorResync(uint32_t epoch, uint32_t lastSeq, uint8_t reason);

} // namespace coop