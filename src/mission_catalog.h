#pragma once

// Story MissionData catalog (79-80 four-character IDs).
// Source: MIT-licensed rdr2-coop-story (JuliPodie), CampaignMissionCatalog.hpp;
// IDs transcribed from init_all_sp func_282 registry. Attribution kept here.
// Only IDs/titles are used; no third-party code/DLLs are bundled.

#include <cstdint>

namespace coop {
namespace missions {

// RAGE joaat hash for a 4-char MissionData ID (ASCII, uppercase).
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
    std::uint32_t id;
    const char* code;
    const char* title;
};

inline constexpr MissionInfo kMissionCatalog[] = {
    {HashMissionId("WNT1"), "WNT1", "Outlaws from the West"},
    {HashMissionId("WNT2"), "WNT2", "Enter, Pursued by a Memory"},
    {HashMissionId("WNT4"), "WNT4", "Old Friends"},
    {HashMissionId("MUD1"), "MUD1", "Americans at Rest"},
    {HashMissionId("MUD2"), "MUD2", "Who Is Not without Sin"},
    {HashMissionId("MUD3"), "MUD3", "Polite Society, Valentine Style"},
    {HashMissionId("MUD4"), "MUD4", "The Sheep and the Goats"},
    {HashMissionId("MUD5"), "MUD5", "Sodom? Back to Gomorrah"},
    {HashMissionId("MRY1"), "MRY1", "We Loved Once and True I"},
    {HashMissionId("MRY3"), "MRY3", "We Loved Once and True III"},
    {HashMissionId("SAL1"), "SAL1", "A Quiet Time"},
    {HashMissionId("UTP1"), "UTP1", "Blessed Are the Meek?"},
    {HashMissionId("UTP2"), "UTP2", "American Pastoral Scene"},
    {HashMissionId("SEN1"), "SEN1", "The First Shall Be Last"},
    {HashMissionId("MUD6"), "MUD6", "Pouring Forth Oil"},
    {HashMissionId("BOU1"), "BOU1", "Good, Honest, Snake Oil"},
    {HashMissionId("REV1"), "REV1", "Who Is Not without Sin"},
    {HashMissionId("GRY1"), "GRY1", "American Distillation"},
    {HashMissionId("GRY2"), "GRY2", "Horse Flesh for Dinner"},
    {HashMissionId("GRY3"), "GRY3", "The Course of True Love"},
    {HashMissionId("BRT1"), "BRT1", "Advertising, the New American Art"},
    {HashMissionId("BRT2"), "BRT2", "The Fine Joys of Tobacco"},
    {HashMissionId("BRT3"), "BRT3", "Blood Feuds, Ancient and Modern"},
    {HashMissionId("TRE1"), "TRE1", "Magicians for Sport"},
    {HashMissionId("MOB1"), "MOB1", "The Joys of Civilization"},
    {HashMissionId("MOB2"), "MOB2", "Angelo Bronte, a Man of Honor"},
    {HashMissionId("MOB3"), "MOB3", "Urban Pleasures"},
    {HashMissionId("MOB4"), "MOB4", "Country Pursuits"},
    {HashMissionId("MOB5"), "MOB5", "Revenge Is a Dish Best Eaten"},
    {HashMissionId("DST1"), "DST1", "Paying a Social Call"},
    {HashMissionId("DST3"), "DST3", "Blessed Are the Peacemakers"},
    {HashMissionId("ODR4"), "ODR4", "Horsemen, Apocalypses"},
    {HashMissionId("DST5"), "DST5", "Goodbye, Dear Friend"},
    {HashMissionId("IND1"), "IND1", "The Gilded Cage"},
    {HashMissionId("IND3"), "IND3", "A Fine Night of Debauchery"},
    {HashMissionId("NBD1"), "NBD1", "A Fine Night of Debauchery"},
    {HashMissionId("SUS1"), "SUS1", "No, No and Thrice, No"},
    {HashMissionId("GUA1"), "GUA1", "Welcome to the New World"},
    {HashMissionId("GUA2"), "GUA2", "A Kind and Benevolent Despot"},
    {HashMissionId("GUA3"), "GUA3", "Savagery Unleashed"},
    {HashMissionId("FUS1"), "FUS1", "A Kind and Benevolent Despot"},
    {HashMissionId("FUS2"), "FUS2", "Savagery Unleashed"},
    {HashMissionId("SMG2"), "SMG2", "The Fine Art of Conversation"},
    {HashMissionId("GNG1"), "GNG1", "Fleeting Joy"},
    {HashMissionId("GNG2"), "GNG2", "Icarus and Friends"},
    {HashMissionId("GNG3"), "GNG3", "Visiting Hours"},
    {HashMissionId("CRN1"), "CRN1", "An Honest Mistake"},
    {HashMissionId("TRN1"), "TRN1", "Just a Social Call"},
    {HashMissionId("TRN2"), "TRN2", "The Delights of Van Horn"},
    {HashMissionId("TRN3"), "TRN3", "The Bridge to Nowhere"},
    {HashMissionId("TRN4"), "TRN4", "Our Best Selves"},
    {HashMissionId("NTV1"), "NTV1", "American Fathers"},
    {HashMissionId("NTV2"), "NTV2", "Archaeology for Beginners"},
    {HashMissionId("NTV3"), "NTV3", "My Last Boy"},
    {HashMissionId("NTS1"), "NTS1", "A Rage Unleashed"},
    {HashMissionId("NTS2"), "NTS2", "The Wisdom of the Elders"},
    {HashMissionId("NTS3"), "NTS3", "The Course of True Love"},
    {HashMissionId("FIN1"), "FIN1", "Our Best Selves"},
    {HashMissionId("FIN2"), "FIN2", "Red Dead Redemption"},
    {HashMissionId("FIN3"), "FIN3", "Red Dead Redemption"},
    {HashMissionId("MAR1"), "MAR1", "The Wheel"},
    {HashMissionId("MAR2"), "MAR2", "Simple Pleasures"},
    {HashMissionId("MAR4"), "MAR4", "Jim Milton Rides, Again?"},
    {HashMissionId("MAR5"), "MAR5", "Fatherhood, for Beginners"},
    {HashMissionId("MR52"), "MR52", "Fatherhood, for Beginners"},
    {HashMissionId("MR53"), "MR53", "Fatherhood, for Beginners"},
    {HashMissionId("LAR1"), "LAR1", "Motherhood"},
    {HashMissionId("MAR6"), "MAR6", "Home Improvement for Beginners"},
    {HashMissionId("MAR7"), "MAR7", "An Honest Day's Labors"},
    {HashMissionId("MAR8"), "MAR8", "American Venom"},
    {HashMissionId("BE22"), "BE22", "A New Jerusalem"},
    {HashMissionId("AB21"), "AB21", "The Tool Box"},
    {HashMissionId("SAD2"), "SAD2", "A Quick Favor for an Old Friend"},
    {HashMissionId("SAD3"), "SAD3", "Uncle's Bad Day"},
    {HashMissionId("SAD4"), "SAD4", "A Really Big Bastard"},
    {HashMissionId("SAD5"), "SAD5", "A New Future Imagined"},
    {HashMissionId("TL21"), "TL21", "A New Jerusalem"},
    // 5-char IDs and named-constant entries are added explicitly (parser filter).
    {HashMissionId("RABI1"), "RABI1", "A Fisher of Men"},
    {HashMissionId("HNT1"), "HNT1", "Exit Pursued by a Bruised Ego"},
    {HashMissionId("FUD1"), "FUD1", "The New South"},
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
