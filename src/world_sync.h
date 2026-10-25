#pragma once

#include <cstdint>
#include <vector>

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
          bool amHost, bool enabled, bool includeTrains);

// Сетевой поток: пришёл WorldSnapshot от хоста (payload уже проверен client).
void OnSnapshot(const std::vector<uint8_t>& payload);

// R&D: TrainSnapshot от хоста (payload уже проверен client).
void OnTrainSnapshot(const std::vector<uint8_t>& payload);

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
