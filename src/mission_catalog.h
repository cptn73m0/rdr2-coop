#pragma once

// Story MissionData catalog (80 four/five-character IDs) with runtime script names.
// Source: MIT-licensed rdr2-coop-story (JuliPodie), CampaignMissionCatalog.hpp;
// IDs/runtime names transcribed from init_all_sp func_282 registry. Attribution kept here.
// Only IDs/runtime names/titles are used; no third-party code/DLLs are bundled.

#include <cstdint>

namespace coop {
namespace missions {

// RAGE joaat hash for an ASCII script/MissionData name (case-insensitive).
constexpr std::uint32_t HashMissionId(const char* text)
{
    std::uint32_t hash = 0;
    for (const char* p = text; *p; ++p)
    {
        char c = *p;
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
        hash += static_cast<std::uint8_t>(c);
        hash += hash << 10;
        hash ^= hash >> 6;
    }
    hash += hash << 3;
    hash ^= hash >> 11;
    hash += hash << 15;
    return hash;
}

struct MissionInfo
{
    std::uint32_t id;       // MissionData joaat (upper 4/5 chars)
    const char* code;       // canonical MissionData code
    const char* runtime;    // RAGE runtime script name (init_all_sp)
    const char* title;
};

inline constexpr MissionInfo kMissionCatalog[] = {
    {HashMissionId("WNT1"), "WNT1", "winter1", "Outlaws from the West"},
    {HashMissionId("WNT2"), "WNT2", "winter2", "Enter, Pursued by a Memory"},
    {HashMissionId("WNT4"), "WNT4", "winter4", "Old Friends"},
    {HashMissionId("MUD1"), "MUD1", "mudtown1", "Americans at Rest"},
    {HashMissionId("MUD2"), "MUD2", "mudtown2", "Who Is Not without Sin"},
    {HashMissionId("MUD3"), "MUD3", "mudtown3", "Polite Society, Valentine Style"},
    {HashMissionId("MUD4"), "MUD4", "mudtown4", "The Sheep and the Goats"},
    {HashMissionId("MUD5"), "MUD5", "mudtown5", "Sodom? Back to Gomorrah"},
    {HashMissionId("MRY1"), "MRY1", "mary1", "We Loved Once and True I"},
    {HashMissionId("MRY3"), "MRY3", "mary3", "We Loved Once and True III"},
    {HashMissionId("SAL1"), "SAL1", "saloon1", "A Quiet Time"},
    {HashMissionId("UTP1"), "UTP1", "utopia1", "Blessed Are the Meek?"},
    {HashMissionId("UTP2"), "UTP2", "utopia2", "American Pastoral Scene"},
    {HashMissionId("SEN1"), "SEN1", "sean1", "The First Shall Be Last"},
    {HashMissionId("MUD6"), "MUD6", "mudtown3b", "Pouring Forth Oil"},
    {HashMissionId("BOU1"), "BOU1", "bounty1", "Good, Honest, Snake Oil"},
    {HashMissionId("RABI1"), "RABI1", "rcm_abigail11", "A Fisher of Men"},
    {HashMissionId("REV1"), "REV1", "reverend1", "Who Is Not without Sin"},
    {HashMissionId("HNT1"), "HNT1", "hunting1", "Exit Pursued by a Bruised Ego"},
    {HashMissionId("FUD1"), "FUD1", "feud1", "The New South"},
    {HashMissionId("GRY1"), "GRY1", "grays1", "American Distillation"},
    {HashMissionId("GRY2"), "GRY2", "grays2", "Horse Flesh for Dinner"},
    {HashMissionId("GRY3"), "GRY3", "grays3", "The Course of True Love"},
    {HashMissionId("BRT1"), "BRT1", "braithwaites1", "Advertising, the New American Art"},
    {HashMissionId("BRT2"), "BRT2", "braithwaites2", "The Fine Joys of Tobacco"},
    {HashMissionId("BRT3"), "BRT3", "braithwaites3", "Blood Feuds, Ancient and Modern"},
    {HashMissionId("TRE1"), "TRE1", "trelawny1", "Magicians for Sport"},
    {HashMissionId("MOB1"), "MOB1", "mob1", "The Joys of Civilization"},
    {HashMissionId("MOB2"), "MOB2", "mob2", "Angelo Bronte, a Man of Honor"},
    {HashMissionId("MOB3"), "MOB3", "mob3", "Urban Pleasures"},
    {HashMissionId("MOB4"), "MOB4", "mob4", "Country Pursuits"},
    {HashMissionId("MOB5"), "MOB5", "mob5", "Revenge Is a Dish Best Eaten"},
    {HashMissionId("DST1"), "DST1", "odriscolls1", "Paying a Social Call"},
    {HashMissionId("DST3"), "DST3", "odriscolls3", "Blessed Are the Peacemakers"},
    {HashMissionId("ODR4"), "ODR4", "odriscolls4", "Horsemen, Apocalypses"},
    {HashMissionId("DST5"), "DST5", "odriscolls5", "Goodbye, Dear Friend"},
    {HashMissionId("IND1"), "IND1", "industry1", "The Gilded Cage"},
    {HashMissionId("IND3"), "IND3", "industry3", "A Fine Night of Debauchery"},
    {HashMissionId("NBD1"), "NBD1", "saint_denis1", "A Fine Night of Debauchery"},
    {HashMissionId("SUS1"), "SUS1", "susan1", "No, No and Thrice, No"},
    {HashMissionId("GUA1"), "GUA1", "guama1", "Welcome to the New World"},
    {HashMissionId("GUA2"), "GUA2", "guama2", "A Kind and Benevolent Despot"},
    {HashMissionId("GUA3"), "GUA3", "guama3", "Savagery Unleashed"},
    {HashMissionId("FUS1"), "FUS1", "fussar1", "A Kind and Benevolent Despot"},
    {HashMissionId("FUS2"), "FUS2", "fussar2", "Savagery Unleashed"},
    {HashMissionId("SMG2"), "SMG2", "smuggler2", "The Fine Art of Conversation"},
    {HashMissionId("GNG1"), "GNG1", "gang1", "Fleeting Joy"},
    {HashMissionId("GNG2"), "GNG2", "gang2", "Icarus and Friends"},
    {HashMissionId("GNG3"), "GNG3", "gang3", "Visiting Hours"},
    {HashMissionId("CRN1"), "CRN1", "cornwall1", "An Honest Mistake"},
    {HashMissionId("TRN1"), "TRN1", "train_robbery1", "Just a Social Call"},
    {HashMissionId("TRN2"), "TRN2", "train_robbery2", "The Delights of Van Horn"},
    {HashMissionId("TRN3"), "TRN3", "train_robbery3", "The Bridge to Nowhere"},
    {HashMissionId("TRN4"), "TRN4", "train_robbery4", "Our Best Selves"},
    {HashMissionId("NTV1"), "NTV1", "native1", "American Fathers"},
    {HashMissionId("NTV2"), "NTV2", "native2", "Archaeology for Beginners"},
    {HashMissionId("NTV3"), "NTV3", "native3", "My Last Boy"},
    {HashMissionId("NTS1"), "NTS1", "native_son1", "A Rage Unleashed"},
    {HashMissionId("NTS2"), "NTS2", "native_son2", "The Wisdom of the Elders"},
    {HashMissionId("NTS3"), "NTS3", "native_son3", "The Course of True Love"},
    {HashMissionId("FIN1"), "FIN1", "finale1", "Our Best Selves"},
    {HashMissionId("FIN2"), "FIN2", "finale2", "Red Dead Redemption"},
    {HashMissionId("FIN3"), "FIN3", "finale3", "Red Dead Redemption"},
    {HashMissionId("MAR1"), "MAR1", "marston1", "The Wheel"},
    {HashMissionId("MAR2"), "MAR2", "marston2", "Simple Pleasures"},
    {HashMissionId("MAR4"), "MAR4", "marston4", "Jim Milton Rides, Again?"},
    {HashMissionId("MAR5"), "MAR5", "marston5_1", "Fatherhood, for Beginners"},
    {HashMissionId("MR52"), "MR52", "marston5_2", "Fatherhood, for Beginners"},
    {HashMissionId("MR53"), "MR53", "marston5_3", "Fatherhood, for Beginners"},
    {HashMissionId("LAR1"), "LAR1", "laramie1", "Motherhood"},
    {HashMissionId("MAR6"), "MAR6", "marston6", "Home Improvement for Beginners"},
    {HashMissionId("MAR7"), "MAR7", "marston7", "An Honest Day's Labors"},
    {HashMissionId("MAR8"), "MAR8", "marston8", "American Venom"},
    {HashMissionId("BE22"), "BE22", "beechers2_2", "A New Jerusalem"},
    {HashMissionId("AB21"), "AB21", "abigail2_1", "The Tool Box"},
    {HashMissionId("SAD2"), "SAD2", "sadie2", "A Quick Favor for an Old Friend"},
    {HashMissionId("SAD3"), "SAD3", "sadie3", "Uncle's Bad Day"},
    {HashMissionId("SAD4"), "SAD4", "sadie4", "A Really Big Bastard"},
    {HashMissionId("SAD5"), "SAD5", "sadie5", "A New Future Imagined"},
    {HashMissionId("TL21"), "TL21", "dreamanim", "A New Jerusalem"},
};

inline constexpr int kMissionCatalogSize =
    static_cast<int>(sizeof(kMissionCatalog) / sizeof(kMissionCatalog[0]));

inline constexpr const MissionInfo* FindMission(std::uint32_t id)
{
    for (int i = 0; i < kMissionCatalogSize; ++i)
    {
        if (kMissionCatalog[i].id == id)
            return &kMissionCatalog[i];
    }
    return nullptr;
}

} // namespace missions
} // namespace coop
