#pragma once

#include <cstdint>
#include <vector>

#include "net/protocol.h"

// P1 (модель A): репликация мира/NPC между хостом и гостем.
//
// Хост раз в ~100 мс сканирует пул педов вокруг игроков, присваивает им
// стабильные netId и отправляет WorldSnapshot. Гость создаёт визуальные
// прокси-педы, обновляет их позиции/состояние и подавляет собственную
// ambient-популяцию, чтобы мир хоста не дублировался.
//
// Все нативы выполняются только из Tick() (игровой поток). OnSnapshot()
// вызывается из сетевого потока и лишь копирует payload под мьютексом.

namespace coop {
namespace world_sync {

void Tick(unsigned int localPlayerPed, unsigned int tickCount,
          bool amHost, bool enabled, bool includeTrains,
          bool vehiclesOnly = false, int vehicleRadius = 80,
          float partnerX = 0.f, float partnerY = 0.f, float partnerZ = 0.f,
          bool partnerValid = false);

// Сетевой поток: пришёл WorldSnapshot от хоста (payload уже проверен client).
void OnSnapshot(const std::vector<uint8_t>& payload);

// E3d-c (v19): сетевой поток — occupants транспорта от хоста (копия в inbox).
void OnVehicleOccupants(
    uint32_t vehicleNetId,
    const std::vector<net::VehicleOccupantEntry>& entries);

// E3d-b (v19): сетевой поток — TrainSnapshotV2 (копия в inbox).
void OnTrainSnapshotV2(const net::TrainSnapshotV2Payload& payload);

// R&D: TrainSnapshot от хоста (payload уже проверен client).
void OnTrainSnapshot(const std::vector<uint8_t>& payload);

// v20 P1: сетевой поток -> inbox интерактивности транспорта. Только копия под
// mutex (нативы/логи не вызываются). 0x23 — хосту, 0x24/0x26 — гостю.
void OnVehicleInteraction(const net::VehicleInteractionPayload& intent);
void OnVehicleInteractionResult(
    const net::VehicleInteractionResultPayload& result);
void OnVehicleState(const net::VehicleStatePayload& state);

// v20 P1 (гость): handle vehicle-прокси -> netId (false — не vehicle-прокси).
bool GetVehicleProxyNetId(unsigned int handle, uint32_t& netId);
// v20 P1 (гость): локальный игрок сейчас в vehicle-прокси? (для capture C2).
bool GetLocalProxyVehicle(unsigned int localPed, uint32_t& netId,
                          unsigned int& handle, int& seat);
// v20 P1 (гость): водитель-NPC из последнего 0x21 для vehicle (actorId;
// 0 = водителя нет/неизвестен) — выбор Enter vs Hijack.
bool GetProxyOccupantDriver(uint32_t netId, uint32_t& actorId);
// v20 P1 (гость): ближайший интерактивный vehicle-прокси в радиусе (capture
// дверей controls, §4.4 C3). false — рядом нет.
bool GetNearestVehicleProxy(unsigned int localPed, float radius,
                            uint32_t& netId, unsigned int& handle);
// v20 P1 (гость): есть near-интерактивный vehicle-прокси (дешёвый gate для
// control-poll: не дёргать IS_CONTROL_* вне зоны).
bool HasInteractiveVehicleProxy();

// v21/e20.4: allowlist handles occupants (водители/пассажиры/draft-лошади),
// собранный host-веткой в этом тике. actor discovery читает напрямую, чтобы не
// зависеть от порядка вызовов world_sync::Tick -> actor_sync::Tick.
const std::vector<unsigned int>& HostOccHandles();

// Убрать прокси/сбросить состояние (выход из сессии, смена роли, disable).
void Reset();

// E7.1 visitor mode: погасить ambient-плотности у гостя (THIS_FRAME-нативы —
// вызывать каждый кадр). suppressVehicles=false — оставить локальный трафик.
// Вызывается из coop::Tick при npc_visitor=1 и роли гостя.
void SuppressAmbientForVisitor(bool suppressVehicles);

// P1→P2: сопоставление сущности и стабильного netId (только игровой поток).
// Хост: handle → netId (для ActionState.targetNetId).
// Гость: netId → handle (чтобы применить переноску/лутание к прокси).
bool GetNetIdForHandle(unsigned int handle, uint64_t& netId);
bool GetHandleForNetId(uint64_t netId, unsigned int& handle);

} // namespace world_sync
} // namespace coop
