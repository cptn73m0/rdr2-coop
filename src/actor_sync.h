#pragma once

// E1: репликация NPC-актёров (human-only) между хостом и гостем.
//
// Хост: discovery 10 Гц (sh::worldGetAllPeds, human, рядом с ОБОИМИ игроками),
// устойчивые actorId/epoch, ActorSpawnBatch на новые спавны/join/heartbeat,
// ActorFrames 10 Гц, ActorDespawn при уходе/смерти/дальности.
// Гость: inbox из callbacks net/client (копия под mutex), прокси-педы
// (REQUEST_MODEL -> CREATE_PED), движение (телепорт >12 м, навмеш-задачи,
// retarget 1 м, heading >5°), lifecycle (resync 2 с / delete 10 с, epoch
// mismatch -> удалить старые, leave/disable -> Reset).
//
// Гейт npc_actor=0: Tick сразу выходит (ноль нативов), callbacks не копят
// данные, ноль строк `actor:`. Базового педа-призрака/выстрелы/наряд/маунт
// модуль не трогает (никаких глобалов coop.cpp).

#include <cstdint>
#include <vector>

#include "net/protocol.h"

namespace coop {
namespace actor_sync {

// Контекст игрового тика (заполняется coop::Tick). Партнёрские позиции нужны
// для discovery «рядом с обоими игроками»; ghost/mount партнёра — для
// no-collision и исключения из скан-пула.
struct TickContext
{
    unsigned int localPed = 0;
    unsigned int tickCount = 0;
    bool amHost = false;
    bool enabled = false;
    float partnerX = 0.f;
    float partnerY = 0.f;
    float partnerZ = 0.f;
    bool partnerValid = false;
    unsigned int partnerGhost = 0; // s_ghost (пед партнёра)
    unsigned int partnerMount = 0; // s_mount (лошадь партнёра)
};

// Вызывается из coop::Tick() в конце, после базовой обработки (до persist).
void Tick(const TickContext& ctx);

// Полный сброс: прокси гостя удаляются, roster/очереди/epoch хоста чистятся.
// Вызывается при leave/reconnect/смене роли/disable.
void Reset();

// Реестр гостя: actorId -> локальный handle (для target/маппинга).
bool GetHandleForActorId(uint32_t actorId, unsigned int& handle);

// ---------------- callbacks net/client (сетевой поток) ----------------
// Только копируют данные в inbox под mutex; нативы/логи не вызывают.
void OnActorSpawnBatch(uint32_t epoch,
                       const std::vector<net::ActorSpawn>& actors);
void OnActorFrames(uint32_t epoch, uint16_t seq,
                   const std::vector<net::ActorFrame>& frames);
void OnActorDespawn(const net::ActorDespawnPayload& msg);
void OnActorEvent(const net::ActorEventPayload& ev);
void OnActorOutfit(uint32_t epoch, uint32_t actorId,
                   const net::OutfitState& outfit);
// Хост получает запрос resync от гостя: следующий тик перешлёт всех акторов.
void OnActorResyncRequest(const net::ActorResyncPayload& req);

} // namespace actor_sync
} // namespace coop
