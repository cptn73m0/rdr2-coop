#pragma once

#include <cstdint>

#include "net/protocol.h"

namespace coop {
namespace missionsbarrier {

enum class Role : uint8_t
{
    Host = 0,
    Guest = 1,
};

bool IsKnownPhase(uint8_t phase);

// Host шлёт Offer/StartWindow/Abort; guest — Ready/Started.
bool IsPhaseAllowedForRole(Role role, uint8_t phase);

// Чистые проверки соответствия события активному offer (без state/natives).
bool MatchesOffer(uint32_t offerMissionId, uint32_t offerEpoch,
                  const net::MissionBarrierPayload& event);

// Host может обработать только Ready/Started по своему offer.
bool IsHostEventAllowed(uint32_t offerMissionId, uint32_t offerEpoch,
                        const net::MissionBarrierPayload& event);

// Guest принимает Offer всегда; StartWindow/Abort — только по своему offer.
bool IsGuestEventAllowed(uint32_t offerMissionId, uint32_t offerEpoch,
                         const net::MissionBarrierPayload& event);

} // namespace missionsbarrier
} // namespace coop
