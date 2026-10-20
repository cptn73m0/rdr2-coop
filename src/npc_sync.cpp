#include "npc_sync.h"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "config.h"
#include "coop.h"
#include "ghost_frame.h"
#include "log.h"
#include "native_call.h"
#include "natives.h"
#include "script_hook.h"

namespace coop {
namespace npc_sync {

namespace {

constexpr float kRadius2 = 60.0f * 60.0f;
constexpr unsigned int kSendEveryTicks = 6; // ~10 Гц

unsigned int g_lastHostTick = 0;
std::unordered_map<unsigned int, uint32_t> g_npcNetIds; // handle -> netId
uint32_t g_nextNetId = 1;

struct HostNpc
{
    uint32_t netId = 0;
    unsigned int handle = 0;
    uint32_t model = 0;
    uint32_t metaOutfit = 0;
    uint32_t mountModel = 0;
    bool mounted = false;
    uint32_t weapon = 0;
    float distance2 = 0.f;
};

std::vector<HostNpc> g_roster;
std::unordered_map<uint32_t, net::NpcSpawnPayload> g_sentSpawns;

// Гость: pending (сетевой поток -> игровой). Визуализация будет подключена
// следующим шагом; пока собираем и логируем.
std::mutex g_mtx;
std::vector<net::NpcSpawnPayload> g_pendingSpawns;
std::vector<net::NpcFramePayload> g_pendingFrames;
unsigned int g_lastGuestLogTick = 0;

bool DoesExist(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::DOES_ENTITY_EXIST);
    Push(handle);
    return Call() != 0;
}

Vec3 GetCoords(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::GET_ENTITY_COORDS);
    Push(handle); Push(false); Push(true);
    Vec3 v{0.f, 0.f, 0.f};
    CallVec3(v);
    return v;
}

float GetHeading(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::GET_ENTITY_HEADING);
    Push(handle);
    return CallFloat();
}

unsigned int GetModel(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::GET_ENTITY_MODEL);
    Push(handle);
    return static_cast<unsigned int>(Call());
}

float GetSpeed(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::GET_ENTITY_SPEED);
    Push(handle);
    return CallFloat();
}

bool IsMissionEntity(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::IS_ENTITY_A_MISSION_ENTITY);
    Push(handle);
    return Call() != 0;
}

uint8_t MoveStateForSpeed(float speed)
{
    if (speed > 4.5f) return static_cast<uint8_t>(coop::MoveState::Run);
    if (speed > 0.3f) return static_cast<uint8_t>(coop::MoveState::Walk);
    return static_cast<uint8_t>(coop::MoveState::Idle);
}

net::NpcSpawnPayload BuildSpawn(const HostNpc& npc)
{
    net::NpcSpawnPayload sp{};
    sp.netId = npc.netId;
    sp.model = npc.model;
    sp.metaOutfit = npc.metaOutfit;
    sp.mountModel = npc.mountModel;
    sp.weaponHash = npc.weapon;
    sp.flags = npc.mounted ? 1u : 0u;
    return sp;
}

void HostScan(unsigned int localPlayerPed, unsigned int tickCount)
{
    static int handles[2048];
    int count = sh::worldGetAllPeds(handles, 2048);
    if (count < 0) count = 0;
    if (count > 2048) count = 2048;

    const Vec3 origin = GetCoords(localPlayerPed);

    std::vector<HostNpc> found;
    found.reserve(32);
    for (int i = 0; i < count; ++i)
    {
        const unsigned int handle = static_cast<unsigned int>(handles[i]);
        if (!handle || handle == localPlayerPed)
            continue;
        if (coop::IsWorldSyncExcluded(handle))
            continue;
        if (!DoesExist(handle))
            continue;
        if (IsMissionEntity(handle))
            continue; // миссионных NPC не трогаем (защита сюжета)

        const Vec3 p = GetCoords(handle);
        const float dx = p.x - origin.x;
        const float dy = p.y - origin.y;
        if (dx * dx + dy * dy > kRadius2)
            continue;

        const unsigned int model = GetModel(handle);
        if (!model)
            continue;

        HostNpc npc;
        npc.handle = handle;
        npc.model = model;
        npc.distance2 = dx * dx + dy * dy;
        {
            using namespace natives_api;
            Init(natives::GET_PED_META_OUTFIT_HASH);
            Push(handle);
            npc.metaOutfit = static_cast<uint32_t>(Call());
            Init(natives::GET_MOUNT);
            Push(handle);
            const unsigned int mount = static_cast<unsigned int>(Call());
            if (mount && DoesExist(mount))
            {
                npc.mountModel = GetModel(mount);
                npc.mounted = npc.mountModel != 0;
            }
            Init(natives::GET_CURRENT_PED_WEAPON);
            Push(handle);
            npc.weapon = static_cast<uint32_t>(Call());
        }
        found.push_back(npc);
    }

    // Ближайшие 16 NPC.
    std::stable_sort(found.begin(), found.end(),
        [](const HostNpc& a, const HostNpc& b) {
            return a.distance2 < b.distance2;
        });
    if (found.size() > net::kMaxNpcGhosts)
        found.resize(net::kMaxNpcGhosts);

    g_roster.clear();
    for (HostNpc& npc : found)
    {
        uint32_t& netId = g_npcNetIds[npc.handle];
        if (netId == 0)
            netId = g_nextNetId++;
        npc.netId = netId;

        const net::NpcSpawnPayload sp = BuildSpawn(npc);
        auto it = g_sentSpawns.find(netId);
        if (it == g_sentSpawns.end() ||
            std::memcmp(&it->second, &sp, sizeof(sp)) != 0)
        {
            g_sentSpawns[netId] = sp;
            coop::SendNpcSpawn(sp);
            sh_log::Write("npc: spawn netId=%u model=0x%08X outfit=0x%08X "
                          "mount=0x%08X weapon=0x%08X\n",
                          sp.netId, sp.model, sp.metaOutfit, sp.mountModel,
                          sp.weaponHash);
        }
        g_roster.push_back(npc);
    }
    (void)tickCount;
}

void HostSendFrames(unsigned int tickCount)
{
    for (const HostNpc& npc : g_roster)
    {
        if (!DoesExist(npc.handle))
            continue;
        net::NpcFramePayload f{};
        f.netId = npc.netId;
        const Vec3 p = GetCoords(npc.handle);
        f.x = p.x; f.y = p.y; f.z = p.z;
        f.heading = GetHeading(npc.handle);
        f.move = MoveStateForSpeed(GetSpeed(npc.handle));
        coop::SendNpcFrame(f);
    }
    (void)tickCount;
}

void GuestDrain(unsigned int tickCount)
{
    std::vector<net::NpcSpawnPayload> spawns;
    std::vector<net::NpcFramePayload> frames;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        spawns.swap(g_pendingSpawns);
        frames.swap(g_pendingFrames);
    }
    if (spawns.empty() && frames.empty())
        return;

    // Визуализация призраков подключается следующим шагом; пока считаем и
    // раз в ~5 с логируем, что поток идёт.
    if (tickCount - g_lastGuestLogTick >= 300)
    {
        g_lastGuestLogTick = tickCount;
        sh_log::Write("npc: guest received spawns=%u frames=%u (visuals: next step)\n",
                      (unsigned)spawns.size(), (unsigned)frames.size());
    }
}

void ResetInternal()
{
    g_roster.clear();
    g_npcNetIds.clear();
    g_sentSpawns.clear();
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        g_pendingSpawns.clear();
        g_pendingFrames.clear();
    }
}

} // namespace

void Tick(unsigned int localPlayerPed, unsigned int tickCount,
          bool amHost, bool enabled)
{
    if (!enabled)
    {
        if (!g_roster.empty() || !g_sentSpawns.empty())
            ResetInternal();
        return;
    }
    if (amHost)
    {
        if (tickCount - g_lastHostTick < kSendEveryTicks)
            return;
        g_lastHostTick = tickCount;
        HostScan(localPlayerPed, tickCount);
        HostSendFrames(tickCount);
    }
    else
    {
        GuestDrain(tickCount);
    }
}

void OnSpawn(const net::NpcSpawnPayload& spawn)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_pendingSpawns.size() < 256)
        g_pendingSpawns.push_back(spawn);
}

void OnFrame(const net::NpcFramePayload& frame)
{
    std::lock_guard<std::mutex> lk(g_mtx);
    if (g_pendingFrames.size() < 2048)
        g_pendingFrames.push_back(frame);
}

void Reset()
{
    ResetInternal();
}

} // namespace npc_sync
} // namespace coop
