#pragma once

// R&D E0: каркас синхронизации NPC-акторов (actor sync).
//
// Пока это ТОЛЬКО фасад: probe-режим E-1 (npc_debug=1) и заглушки для E1.
// При npc_actor=0 actor_sync::Tick() немедленно выходит — ни нативов, ни
// логов, поведение клиента не меняется. Сетевые сообщения появятся в E1;
// точные сигнатуры хуков зафиксированы ниже, чтобы E1 не менял API.

#include <cstdint>
#include <vector>

namespace coop {
namespace actor_sync {

// Вызывается из coop::Tick() в конце, после базовой обработки (перед persist).
// enabled = coopcfg::Get().npcActor; amHost — только хост собирает снапшот и
// гоняет probe. При enabled=false — немедленный return без нативов/логов.
void Tick(unsigned int localPed, unsigned int tickCount, bool amHost,
          bool enabled);

// Сброс состояния (leave/reconnect/смена роли/disable). Пока чистит probe-кэш;
// в E1 — реестр акторов.
void Reset();

// ---------------- E1 TODO hooks (точные сигнатуры) ----------------
// Сетевой поток: копирует payload NpcSnapshot в inbox под mutex (нативы НЕ
// вызывает). Игровой поток забирает его в ApplyPendingSnapshot().
void OnSnapshot(const std::vector<uint8_t>& payload);

// Игровой поток: применить pending-снапшот к реестру акторов (spawn/move/
// outfit/weapon/mount/death/despawn) в рамках бюджета тика.
void ApplyPendingSnapshot(unsigned int tickCount);

// Игровой поток (только хост): собрать NpcSnapshot из ближайших акторов и
// отправить через coop::SendNpcSnapshot() (сеть появится в E1).
void SendSnapshot(unsigned int localPed, unsigned int tickCount);

// Реестр: actorId -> локальный handle (нужно для target/маппинга в E1).
bool GetHandleForActorId(uint32_t actorId, unsigned int& handle);

} // namespace actor_sync
} // namespace coop
