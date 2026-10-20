#pragma once

#include <cstdint>

#include "net/protocol.h"

namespace coop {
namespace npc_sync {

// Репликация NPC как «призраков» (тот же механизм, что у призраков игроков).
// Хост регистрирует пеших/конных NPC (модель + meta-outfit + лошадь + оружие)
// и шлёт кадры движения ~10 Гц. Гость ведёт список призраков; визуализация
// подключается поэтапно. Вызывается из игрового потока.
void Tick(unsigned int localPlayerPed, unsigned int tickCount,
          bool amHost, bool enabled);

// Сетевой поток: пришла регистрация NPC-призрака (payload уже проверен).
void OnSpawn(const net::NpcSpawnPayload& spawn);

// Сетевой поток: пришёл кадр движения NPC-призрака.
void OnFrame(const net::NpcFramePayload& frame);

// Сброс состояния (выход из сессии/смена роли/disable).
void Reset();

} // namespace npc_sync
} // namespace coop
