#pragma once

// Story mission reward plan catalog (dry-run only).
// Source: MIT-licensed rdr2-coop-story (JuliPodie), CampaignMissionCatalog.hpp.
// Only IDs/bindings/amounts are copied; no native calls are made by this file.
// The plan is logged for visibility; actual reward application is intentionally
// not implemented until it can be validated on two PCs.

#include <cstdint>

#include "mission_catalog.h"

namespace coop {
namespace missions {

enum class RewardBinding : uint8_t {
    WeaponOwnership = 1,
    UnlockVisible = 2,
    InventoryItem = 3,
    RecipeUnlock = 4,
    UnlockEntitlement = 5,
    WeaponShopEligibility = 6,
};

struct MissionRewardPlan
{
    std::uint32_t missionId;
    RewardBinding binding;
    std::uint32_t recordHash;
    std::uint32_t amount;
};

inline constexpr MissionRewardPlan kMissionRewards[] = {
    {HashMissionId("WNT4"), RewardBinding::WeaponOwnership, HashMissionId("WEAPON_REPEATER_CARBINE"), 99U},
    {HashMissionId("WNT4"), RewardBinding::WeaponOwnership, HashMissionId("WEAPON_LASSO"), 1U},
    {HashMissionId("MUD1"), RewardBinding::WeaponShopEligibility, HashMissionId("WEAPON_THROWN_TOMAHAWK"), 0U},
    {HashMissionId("MUD4"), RewardBinding::WeaponOwnership, HashMissionId("WEAPON_SNIPERRIFLE_ROLLINGBLOCK"), 100U},
    {HashMissionId("UTP2"), RewardBinding::WeaponOwnership, HashMissionId("WEAPON_REPEATER_LANCASTER"), 60U},
    {HashMissionId("SEN1"), RewardBinding::WeaponOwnership, HashMissionId("WEAPON_THROWN_TOMAHAWK"), 1U},
    {HashMissionId("MUD6"), RewardBinding::WeaponShopEligibility, HashMissionId("WEAPON_SHOTGUN_PUMP"), 0U},
    {HashMissionId("HNT1"), RewardBinding::InventoryItem, HashMissionId("DOCUMENT_MAP_LEGENDARY_ANIMALS"), 1U},
    {HashMissionId("HNT1"), RewardBinding::UnlockVisible, HashMissionId("SP_CHAL_HUNT_ROOT"), 0U},
    {HashMissionId("FUD1"), RewardBinding::WeaponOwnership, HashMissionId("WEAPON_FISHINGROD"), 1U},
    {HashMissionId("GRY1"), RewardBinding::WeaponShopEligibility, HashMissionId("WEAPON_REPEATER_EVANS"), 0U},
    {HashMissionId("TRE1"), RewardBinding::WeaponOwnership, HashMissionId("WEAPON_SNIPERRIFLE_ROLLINGBLOCK_EXOTIC"), 100U},
    {HashMissionId("DST1"), RewardBinding::WeaponOwnership, HashMissionId("WEAPON_SHOTGUN_DOUBLEBARREL"), 12U},
    {HashMissionId("DST1"), RewardBinding::WeaponOwnership, HashMissionId("WEAPON_THROWN_THROWING_KNIVES"), 1U},
    {HashMissionId("DST5"), RewardBinding::WeaponOwnership, HashMissionId("WEAPON_SNIPERRIFLE_CARCANO"), 60U},
    {HashMissionId("DST5"), RewardBinding::WeaponShopEligibility, HashMissionId("WEAPON_REPEATER_LITCHFIELD"), 0U},
    {HashMissionId("IND3"), RewardBinding::WeaponShopEligibility, HashMissionId("WEAPON_SHOTGUN_SEMIAUTO"), 0U},
    {HashMissionId("IND3"), RewardBinding::WeaponShopEligibility, HashMissionId("WEAPON_REVOLVER_DOUBLEACTION_GAMBLER"), 0U},
    {HashMissionId("IND3"), RewardBinding::InventoryItem, HashMissionId("PROVISION_POCKET_WATCH_REUTLINGE"), 1U},
    {HashMissionId("GNG3"), RewardBinding::WeaponShopEligibility, HashMissionId("WEAPON_SHOTGUN_REPEATING"), 0U},
    {HashMissionId("MAR8"), RewardBinding::WeaponOwnership, HashMissionId("WEAPON_KIT_BINOCULARS"), 0U},
    {HashMissionId("AB21"), RewardBinding::InventoryItem, HashMissionId("DOCUMENT_LETTER_SADIE_TELEGRAM"), 1U},
    {HashMissionId("SAD3"), RewardBinding::WeaponOwnership, HashMissionId("WEAPON_SNIPERRIFLE_CARCANO"), 50U},
};

inline constexpr int kMissionRewardsSize =
    static_cast<int>(sizeof(kMissionRewards) / sizeof(kMissionRewards[0]));

} // namespace missions
} // namespace coop
