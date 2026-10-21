#include "world_scan.h"

#include "log.h"
#include "native_call.h"
#include "natives.h"
#include "script_hook.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace coop {
namespace world_scan {
namespace {

constexpr int kMaxPeds     = 2048;
constexpr int kMaxVehicles = 512;
constexpr int kMaxObjects  = 1024;
constexpr int kMaxPickups  = 512;

// ~1 секунда при 60 тиках/с. Тики считаются в coop::Tick(), а не в мс.
constexpr unsigned int kLogEveryTicks = 60;

int g_peds[kMaxPeds];
int g_vehicles[kMaxVehicles];
int g_objects[kMaxObjects];
int g_pickups[kMaxPickups];

bool      g_missingLogged = false;
ULONGLONG g_lastFpsTime   = 0;
unsigned int g_framesSinceFps = 0;

Vec3 EntityCoords(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::GET_ENTITY_COORDS);
    Push(handle);
    Push(false);
    Push(true);
    Vec3 v{0.f, 0.f, 0.f};
    CallVec3(v);
    return v;
}

float Dist2(const Vec3& a, const Vec3& b)
{
    const float dx = a.x - b.x;
    const float dy = a.y - b.y;
    const float dz = a.z - b.z;
    return dx * dx + dy * dy + dz * dz;
}

bool IsMissionEntity(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::IS_ENTITY_A_MISSION_ENTITY);
    Push(handle);
    return Call() != 0;
}

unsigned int ModelOf(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::GET_ENTITY_MODEL);
    Push(handle);
    return static_cast<unsigned int>(Call());
}

struct PedStats
{
    int total = 0;
    int near30 = 0;
    int near60 = 0;
    int near100 = 0;
    int mission = 0;
    unsigned int model[5] = {0, 0, 0, 0, 0};
    float dist[5] = {1e9f, 1e9f, 1e9f, 1e9f, 1e9f};
};

PedStats ScanPeds(const Vec3& origin, unsigned int localPed)
{
    PedStats stats;
    int count = sh::worldGetAllPeds(g_peds, kMaxPeds);
    if (count < 0) count = 0;
    if (count > kMaxPeds) count = kMaxPeds; // защита от OOB: движок может вернуть больше
    stats.total = count;
    const float r30 = 30.f * 30.f;
    const float r60 = 60.f * 60.f;
    const float r100 = 100.f * 100.f;

    for (int i = 0; i < count; ++i)
    {
        const unsigned int handle = static_cast<unsigned int>(g_peds[i]);
        if (!handle || handle == localPed)
            continue;

        const Vec3 p = EntityCoords(handle);
        const float d2 = Dist2(origin, p);
        if (d2 > r100)
            continue;

        ++stats.near100;
        if (d2 <= r60) ++stats.near60;
        if (d2 <= r30) ++stats.near30;

        if (IsMissionEntity(handle))
            ++stats.mission;

        // Пять ближайших — для диагностики моделей (видим ли mission-педов).
        if (d2 < stats.dist[4])
        {
            int slot = 4;
            while (slot > 0 && d2 < stats.dist[slot - 1])
            {
                stats.dist[slot] = stats.dist[slot - 1];
                stats.model[slot] = stats.model[slot - 1];
                --slot;
            }
            stats.dist[slot] = d2;
            stats.model[slot] = ModelOf(handle);
        }
    }
    return stats;
}

int CountNear(const int* handles, int count, const Vec3& origin, float radius, int* missionOut)
{
    // ВАЖНО: не называть переменную near/far — в windows.h это макросы.
    int nearCount = 0;
    int mission = 0;
    const float r2 = radius * radius;
    for (int i = 0; i < count; ++i)
    {
        const unsigned int handle = static_cast<unsigned int>(handles[i]);
        if (!handle)
            continue;
        const Vec3 p = EntityCoords(handle);
        if (Dist2(origin, p) > r2)
            continue;
        ++nearCount;
        if (missionOut && IsMissionEntity(handle))
            ++mission;
    }
    if (missionOut) *missionOut = mission;
    return nearCount;
}

void LogMissingOnce()
{
    if (g_missingLogged)
        return;
    g_missingLogged = true;
    sh_log::Write("scan: world queries unavailable in ScriptHookRDR2.dll "
                  "(worldGetAll* exports not found); world scan disabled\n");
}

} // namespace

void Tick(unsigned int localPlayerPed, unsigned int tickCount)
{
    ++g_framesSinceFps;
    const ULONGLONG now = GetTickCount64();

    if (g_lastFpsTime == 0)
        g_lastFpsTime = now;

    // Троттлинг: раз в ~kLogEveryTicks тиков, привязан к счётчику Tick().
    if (tickCount < kLogEveryTicks || (tickCount % kLogEveryTicks) != 1)
        return;

    if (!sh::HasWorldQueries())
    {
        LogMissingOnce();
        return;
    }

    unsigned int fps = 0;
    if (now > g_lastFpsTime)
    {
        fps = static_cast<unsigned int>(
            (static_cast<unsigned long long>(g_framesSinceFps) * 1000ull) /
            (now - g_lastFpsTime));
    }
    g_lastFpsTime = now;
    g_framesSinceFps = 0;

    if (!localPlayerPed)
        return;

    const Vec3 origin = EntityCoords(localPlayerPed);
    const PedStats peds = ScanPeds(origin, localPlayerPed);

    int nVehicles = sh::worldGetAllVehicles(g_vehicles, kMaxVehicles);
    int nObjects  = sh::worldGetAllObjects(g_objects, kMaxObjects);
    int nPickups  = sh::worldGetAllPickups(g_pickups, kMaxPickups);
    if (nVehicles < 0) nVehicles = 0;
    if (nObjects < 0) nObjects = 0;
    if (nPickups < 0) nPickups = 0;
    if (nVehicles > kMaxVehicles) nVehicles = kMaxVehicles;
    if (nObjects > kMaxObjects) nObjects = kMaxObjects;
    if (nPickups > kMaxPickups) nPickups = kMaxPickups;

    int missionVehicles = 0;
    const int nearVehicles = CountNear(g_vehicles, nVehicles, origin, 100.f, &missionVehicles);
    int missionObjects = 0;
    const int nearObjects = CountNear(g_objects, nObjects, origin, 60.f, &missionObjects);
    const int nearPickups = CountNear(g_pickups, nPickups, origin, 60.f, nullptr);

    sh_log::Write(
        "scan: fps=%u peds=%d near(30/60/100)=%d/%d/%d missionPeds=%d "
        "vehicles=%d/%d missionVeh=%d objects=%d/%d missionObj=%d pickups=%d/%d "
        "nearest=[0x%08X %.0fm, 0x%08X %.0fm, 0x%08X %.0fm, 0x%08X %.0fm, 0x%08X %.0fm]\n",
        fps,
        peds.total,
        peds.near30, peds.near60, peds.near100, peds.mission,
        nearVehicles, (nVehicles > kMaxVehicles ? kMaxVehicles : nVehicles), missionVehicles,
        nearObjects, (nObjects > kMaxObjects ? kMaxObjects : nObjects), missionObjects,
        nearPickups, (nPickups > kMaxPickups ? kMaxPickups : nPickups),
        peds.model[0], std::sqrt(peds.dist[0]),
        peds.model[1], std::sqrt(peds.dist[1]),
        peds.model[2], std::sqrt(peds.dist[2]),
        peds.model[3], std::sqrt(peds.dist[3]),
        peds.model[4], std::sqrt(peds.dist[4]));
}

} // namespace world_scan
} // namespace coop
