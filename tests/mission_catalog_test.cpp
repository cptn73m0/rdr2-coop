// Headless validation of the MIT-derived mission catalog and reward plan.
#include "mission_catalog.h"
#include "mission_rewards.h"

#include <cstdio>
#include <cstring>
#include <set>
#include <string>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        ++g_failures;
    }
}

bool IsKnownBinding(coop::missions::RewardBinding b)
{
    const int v = static_cast<int>(b);
    return v >= 1 && v <= 6;
}

} // namespace

int main()
{
    using namespace coop::missions;

    // Catalog size and uniqueness.
    Check(kMissionCatalogSize == 80, "catalog must contain 80 missions");
    std::set<std::uint32_t> ids;
    for (int i = 0; i < kMissionCatalogSize; ++i)
    {
        const MissionInfo& m = kMissionCatalog[i];
        Check(m.code && m.code[0], "mission code non-empty");
        Check(m.runtime && m.runtime[0], "mission runtime non-empty");
        Check(m.title && m.title[0], "mission title non-empty");
        const size_t codeLen = m.code ? std::strlen(m.code) : 0;
        Check(codeLen == 4 || codeLen == 5, "mission code length 4/5");
        Check(m.id == HashMissionId(m.code), "mission id matches joaat(code)");
        Check(ids.insert(m.id).second, "mission ids unique");
        Check(FindMission(m.id) == &m, "FindMission round-trip");
    }

    // Hash helper is case-insensitive.
    Check(HashMissionId("FUD1") == HashMissionId("fud1"),
          "hash is case-insensitive");

    // Specific anchors from the source-reviewed catalog.
    {
        const MissionInfo* fud = FindMission(HashMissionId("FUD1"));
        Check(fud != nullptr, "FUD1 present");
        Check(fud && std::strcmp(fud->runtime, "feud1") == 0, "FUD1 runtime");
        const MissionInfo* hnt = FindMission(HashMissionId("HNT1"));
        Check(hnt != nullptr, "HNT1 present");
        Check(hnt && std::strcmp(hnt->runtime, "hunting1") == 0, "HNT1 runtime");
        const MissionInfo* wnt4 = FindMission(HashMissionId("WNT4"));
        Check(wnt4 != nullptr, "WNT4 present");
    }

    // Rewards: 23 records over 17 missions, all bound to catalog entries.
    Check(kMissionRewardsSize == 23, "reward records count");
    std::set<std::uint32_t> rewardedMissions;
    int weaponOrInventory = 0;
    for (int i = 0; i < kMissionRewardsSize; ++i)
    {
        const MissionRewardPlan& r = kMissionRewards[i];
        Check(FindMission(r.missionId) != nullptr,
              "reward mission exists in catalog");
        Check(IsKnownBinding(r.binding), "reward binding in 1..6");
        rewardedMissions.insert(r.missionId);
        if (r.binding == RewardBinding::InventoryItem)
        {
            ++weaponOrInventory;
            Check(r.amount > 0, "inventory reward amount > 0");
        }
        else if (r.binding == RewardBinding::WeaponOwnership)
        {
            ++weaponOrInventory;
            // Some weapon records (e.g. binoculars) are ownership flags and
            // intentionally carry amount 0.
        }
        else
        {
            Check(r.amount == 0, "unlock/shop reward amount == 0");
        }
    }
    Check(rewardedMissions.size() == 17, "17 missions have rewards");
    Check(weaponOrInventory > 0, "there is at least one item/weapon reward");

    // No duplicate reward records.
    for (int i = 0; i < kMissionRewardsSize; ++i)
    {
        for (int j = i + 1; j < kMissionRewardsSize; ++j)
        {
            const auto& a = kMissionRewards[i];
            const auto& b = kMissionRewards[j];
            Check(!(a.missionId == b.missionId && a.binding == b.binding &&
                    a.recordHash == b.recordHash),
                  "duplicate reward record");
        }
    }

    if (g_failures == 0)
    {
        std::printf("mission_catalog_test: PASS (80 missions, 23 rewards/17)\n");
        return 0;
    }
    std::printf("mission_catalog_test: %d failure(s)\n", g_failures);
    return 1;
}
