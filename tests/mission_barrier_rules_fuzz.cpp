// Fuzz for mission-barrier role/phase/state-transition rules.
#include "mission_barrier_rules.h"

#include <atomic>
#include <cstdio>
#include <random>
#include <thread>
#include <vector>

namespace {

using coop::missionsbarrier::Role;

int g_failures = 0;

void Check(bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

void Worker(unsigned seed, std::atomic<int>& failures)
{
    std::mt19937 rng(seed);
    for (int i = 0; i < 100000; ++i)
    {
        const uint8_t phase = static_cast<uint8_t>(rng() % 8);
        const Role role = (rng() % 2) ? Role::Host : Role::Guest;

        const bool known =
            phase >= net::kMissionBarrierOffer && phase <= net::kMissionBarrierAbort;
        if (coop::missionsbarrier::IsKnownPhase(phase) != known)
        {
            ++failures;
            return;
        }

        const bool allowed =
            coop::missionsbarrier::IsPhaseAllowedForRole(role, phase);
        const bool expected =
            (role == Role::Host)
                ? (phase == net::kMissionBarrierOffer ||
                   phase == net::kMissionBarrierStartWindow ||
                   phase == net::kMissionBarrierAbort)
                : (phase == net::kMissionBarrierReady ||
                   phase == net::kMissionBarrierStarted);
        if (allowed != expected)
        {
            ++failures;
            return;
        }

        net::MissionBarrierPayload ev{};
        ev.phase = phase;
        ev.missionId = rng();
        ev.epoch = rng();
        ev.flags = static_cast<uint8_t>(rng());
        const uint32_t offerId = (rng() % 2) ? ev.missionId : (ev.missionId ^ 1u);
        const uint32_t offerEpoch = (rng() % 2) ? ev.epoch : (ev.epoch ^ 1u);

        const bool hostOk =
            coop::missionsbarrier::IsHostEventAllowed(offerId, offerEpoch, ev);
        const bool guestOk =
            coop::missionsbarrier::IsGuestEventAllowed(offerId, offerEpoch, ev);
        const bool match = offerId != 0 && ev.missionId == offerId &&
                           ev.epoch == offerEpoch;

        // Host принимает Ready/Started только по совпадающему offer.
        const bool hostExpected =
            (phase == net::kMissionBarrierReady ||
             phase == net::kMissionBarrierStarted) && match;
        if (hostOk != hostExpected)
        {
            ++failures;
            return;
        }

        // Guest принимает Offer всегда; StartWindow/Abort — только по offer.
        const bool guestExpected = (phase == net::kMissionBarrierOffer) ||
            (phase == net::kMissionBarrierStartWindow ||
             phase == net::kMissionBarrierAbort) && match;
        if (guestOk != guestExpected)
        {
            ++failures;
            return;
        }

        // Role sets are disjoint.
        if (coop::missionsbarrier::IsPhaseAllowedForRole(Role::Host, phase) &&
            coop::missionsbarrier::IsPhaseAllowedForRole(Role::Guest, phase))
        {
            ++failures;
            return;
        }
    }
}

} // namespace

int main()
{
    // Edge cases.
    Check(coop::missionsbarrier::IsKnownPhase(net::kMissionBarrierOffer),
          "offer known");
    Check(!coop::missionsbarrier::IsKnownPhase(0), "phase 0 unknown");
    Check(!coop::missionsbarrier::IsKnownPhase(6), "phase 6 unknown");
    Check(coop::missionsbarrier::IsPhaseAllowedForRole(
              Role::Host, net::kMissionBarrierOffer), "host offer");
    Check(!coop::missionsbarrier::IsPhaseAllowedForRole(
              Role::Host, net::kMissionBarrierReady), "host ready denied");
    Check(coop::missionsbarrier::IsPhaseAllowedForRole(
              Role::Guest, net::kMissionBarrierStarted), "guest started");

    // Multithreaded fuzz: 4 x 100k random events/roles.
    {
        std::atomic<int> threadFailures{0};
        std::vector<std::thread> threads;
        for (unsigned i = 0; i < 4; ++i)
            threads.emplace_back(Worker, 3000u + i, std::ref(threadFailures));
        for (auto& t : threads) t.join();
        g_failures += threadFailures.load();
        std::printf("mission_barrier_rules_fuzz: multithreaded pass\n");
    }

    if (g_failures == 0)
    {
        std::printf("mission_barrier_rules_fuzz: PASS\n");
        return 0;
    }
    std::printf("mission_barrier_rules_fuzz: %d failure(s)\n", g_failures);
    return 1;
}
