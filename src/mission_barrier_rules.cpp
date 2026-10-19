#include "mission_barrier_rules.h"

namespace coop {
namespace missionsbarrier {

bool IsKnownPhase(uint8_t phase)
{
    return phase == net::kMissionBarrierOffer ||
           phase == net::kMissionBarrierReady ||
           phase == net::kMissionBarrierStartWindow ||
           phase == net::kMissionBarrierStarted ||
           phase == net::kMissionBarrierAbort;
}

bool IsPhaseAllowedForRole(Role role, uint8_t phase)
{
    if (role == Role::Host)
    {
        return phase == net::kMissionBarrierOffer ||
               phase == net::kMissionBarrierStartWindow ||
               phase == net::kMissionBarrierAbort;
    }
    return phase == net::kMissionBarrierReady ||
           phase == net::kMissionBarrierStarted;
}

bool MatchesOffer(uint32_t offerMissionId, uint32_t offerEpoch,
                  const net::MissionBarrierPayload& event)
{
    return offerMissionId != 0 &&
           event.missionId == offerMissionId &&
           event.epoch == offerEpoch;
}

bool IsHostEventAllowed(uint32_t offerMissionId, uint32_t offerEpoch,
                        const net::MissionBarrierPayload& event)
{
    // Host принимает только ответы гостя: Ready/Started.
    if (!IsPhaseAllowedForRole(Role::Guest, event.phase))
        return false;
    return MatchesOffer(offerMissionId, offerEpoch, event);
}

bool IsGuestEventAllowed(uint32_t offerMissionId, uint32_t offerEpoch,
                         const net::MissionBarrierPayload& event)
{
    // Guest принимает фазы хоста: Offer/StartWindow/Abort.
    if (!IsPhaseAllowedForRole(Role::Host, event.phase))
        return false;
    if (event.phase == net::kMissionBarrierOffer)
        return true;
    return MatchesOffer(offerMissionId, offerEpoch, event);
}

} // namespace missionsbarrier
} // namespace coop
