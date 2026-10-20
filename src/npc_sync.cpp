#include "npc_sync.h"

#include <algorithm>
#include <cmath>
#include <cstring>
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
constexpr uint64_t kGhostTimeoutMs = 3000;  // нет кадров 3 с — удаляем призрака
constexpr float kTeleportDist = 15.0f;      // дальше — телепорт, а не догоняние
constexpr float kTaskRetargetDist = 1.0f;   // перевыдаём задачу при смещении цели

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

// ---------------- гость: призраки NPC ----------------

struct NpcGhost
{
    uint32_t netId = 0;
    unsigned int ped = 0;
    unsigned int mount = 0;
    uint32_t model = 0;
    uint32_t metaOutfit = 0;
    uint32_t mountModel = 0;
    uint32_t weapon = 0;
    bool mounted = false;
    bool weaponGiven = false;
    bool weaponEquipped = false;

    bool hasTarget = false;
    Vec3 target{0.f, 0.f, 0.f};
    float targetHeading = 0.f;
    uint64_t lastFrameMs = 0;

    uint8_t move = 0;
    uint8_t appliedMove = 0xFF;
    Vec3 taskTarget{0.f, 0.f, 0.f};
    bool taskValid = false;

    uint8_t action = 0;
    Vec3 aim{0.f, 0.f, 0.f};
    unsigned int lastShootTick = 0;
    unsigned int lastAimTick = 0;
};

std::mutex g_mtx;
std::vector<net::NpcSpawnPayload> g_pendingSpawns;
std::vector<net::NpcFramePayload> g_pendingFrames;
std::unordered_map<uint32_t, NpcGhost> g_ghosts;
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

uint8_t DetectAction(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::IS_PED_SHOOTING); Push(handle);
    if (Call()) return static_cast<uint8_t>(net::Action::Shooting);
    Init(natives::IS_PED_RELOADING); Push(handle);
    if (Call()) return static_cast<uint8_t>(net::Action::Reloading);
    Init(natives::IS_PED_WEAPON_READY_TO_SHOOT); Push(handle);
    if (Call()) return static_cast<uint8_t>(net::Action::Aiming);
    return static_cast<uint8_t>(net::Action::None);
}

void SetCoordsLocal(unsigned int handle, const Vec3& p)
{
    using namespace natives_api;
    Init(natives::SET_ENTITY_COORDS);
    Push(handle);
    Push(p.x); Push(p.y); Push(p.z);
    Push(false); Push(false); Push(false); Push(false);
    Call();
}

void SetHeadingLocal(unsigned int handle, float heading)
{
    using namespace natives_api;
    Init(natives::SET_ENTITY_HEADING);
    Push(handle);
    Push(heading);
    Call();
}

float GroundZ(float x, float y, float z)
{
    using namespace natives_api;
    Init(natives::GET_GROUND_Z_FOR_3D_COORD);
    Push(x); Push(y); Push(z);
    Push(false);
    return CallFloat();
}

bool HasModelLoadedLocal(unsigned int model)
{
    using namespace natives_api;
    Init(natives::HAS_MODEL_LOADED);
    Push(static_cast<unsigned long long>(model));
    return Call() != 0;
}

void RequestModelLocal(unsigned int model)
{
    using namespace natives_api;
    Init(natives::REQUEST_MODEL);
    Push(static_cast<unsigned long long>(model));
    Call();
}

void DeleteGhostEntity(unsigned int& handle)
{
    if (!handle)
        return;
    if (DoesExist(handle))
    {
        using namespace natives_api;
        int h = static_cast<int>(handle);
        Init(natives::DELETE_PED);
        Push(reinterpret_cast<unsigned long long>(&h));
        Call();
        if (DoesExist(handle))
        {
            int h2 = static_cast<int>(handle);
            Init(natives::SET_ENTITY_AS_NO_LONGER_NEEDED);
            Push(reinterpret_cast<unsigned long long>(&h2));
            Call();
        }
    }
    handle = 0;
}

void EquipMetaOutfit(unsigned int ped, uint32_t outfit)
{
    if (!outfit || !ped)
        return;
    using namespace natives_api;
    Init(natives::EQUIP_META_PED_OUTFIT);
    Push(ped);
    Push(static_cast<unsigned long long>(outfit));
    Call();
}

void GiveWeaponLocal(NpcGhost& g)
{
    if (!g.ped || !g.weapon || g.weaponGiven)
        return;
    using namespace natives_api;
    Init(natives::GIVE_WEAPON_TO_PED);
    Push(g.ped);
    Push(static_cast<unsigned long long>(g.weapon));
    Push(static_cast<unsigned long long>(60));
    Push(static_cast<unsigned long long>(0));
    Push(static_cast<unsigned long long>(0));
    Push(static_cast<unsigned long long>(0));
    Push(static_cast<unsigned long long>(0));
    Push(0.5f);
    Push(1.0f);
    Push(static_cast<unsigned long long>(0));
    Push(static_cast<unsigned long long>(1));
    Push(0.5f);
    Call();
    g.weaponGiven = true;
}

void EquipWeaponInHand(NpcGhost& g)
{
    if (!g.ped || !g.weapon || g.weaponEquipped)
        return;
    using namespace natives_api;
    Init(natives::SET_CURRENT_PED_WEAPON);
    Push(g.ped);
    Push(static_cast<unsigned long long>(g.weapon));
    Push(static_cast<unsigned long long>(1)); // equipNow
    Push(static_cast<unsigned long long>(0));
    Push(static_cast<unsigned long long>(0));
    Push(static_cast<unsigned long long>(0));
    Call();
    g.weaponEquipped = true;
}

void StandStill(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::TASK_STAND_STILL);
    Push(ped);
    Push(static_cast<unsigned long long>(0xFFFFFFFF));
    Call();
}

void FollowNavmesh(unsigned int ped, const Vec3& p, float speed)
{
    using namespace natives_api;
    Init(natives::TASK_FOLLOW_NAV_MESH_TO_COORD);
    Push(ped);
    Push(p.x); Push(p.y); Push(p.z);
    Push(speed);
    Push(static_cast<unsigned long long>(0xFFFFFFFF));
    Push(0.5f);
    Push(static_cast<unsigned long long>(4));
    Push(40000.f);
    Call();
}

void AimGunAt(unsigned int ped, const Vec3& t)
{
    using namespace natives_api;
    Init(natives::TASK_AIM_GUN_AT_COORD);
    Push(ped);
    Push(t.x); Push(t.y); Push(t.z);
    Push(static_cast<unsigned long long>(0xFFFFFFFF));
    Push(static_cast<unsigned long long>(0));
    Push(static_cast<unsigned long long>(0));
    Call();
}

void ShootAt(unsigned int ped, const Vec3& t)
{
    using namespace natives_api;
    Init(natives::TASK_SHOOT_AT_COORD);
    Push(ped);
    Push(t.x); Push(t.y); Push(t.z);
    Push(static_cast<unsigned long long>(120));
    Push(static_cast<unsigned long long>(0x5D60E4E0)); // SingleShot
    Push(static_cast<unsigned long long>(0));
    Call();
}

void CreateGhostPed(NpcGhost& g, const net::NpcFramePayload& f)
{
    RequestModelLocal(g.model);
    if (g.mounted && g.mountModel)
        RequestModelLocal(g.mountModel);
    if (!HasModelLoadedLocal(g.model))
        return;
    if (g.mounted && g.mountModel && !HasModelLoadedLocal(g.mountModel))
        return;

    using namespace natives_api;
    Init(natives::CREATE_PED);
    Push(static_cast<unsigned long long>(g.model));
    Push(f.x); Push(f.y); Push(f.z); Push(f.heading);
    Push(false); Push(false); Push(false); Push(false);
    g.ped = static_cast<unsigned int>(Call());
    if (!g.ped)
        return;

    Init(natives::SET_ENTITY_AS_MISSION_ENTITY);
    Push(g.ped); Push(true); Push(true); Call();
    Init(natives::SET_ENTITY_INVINCIBLE);
    Push(g.ped); Push(true); Call();
    Init(natives::SET_ENTITY_VISIBLE);
    Push(g.ped); Push(true); Call();
    Init(natives::SET_PED_RANDOM_COMPONENT_VARIATION);
    Push(g.ped); Call();
    EquipMetaOutfit(g.ped, g.metaOutfit);

    if (g.mounted && g.mountModel)
    {
        Init(natives::CREATE_PED);
        Push(static_cast<unsigned long long>(g.mountModel));
        Push(f.x); Push(f.y); Push(f.z); Push(f.heading);
        Push(false); Push(false); Push(false); Push(false);
        g.mount = static_cast<unsigned int>(Call());
        if (g.mount)
        {
            Init(natives::SET_ENTITY_AS_MISSION_ENTITY);
            Push(g.mount); Push(true); Push(true); Call();
            Init(natives::SET_ENTITY_INVINCIBLE);
            Push(g.mount); Push(true); Call();
            Init(natives::SET_ENTITY_VISIBLE);
            Push(g.mount); Push(true); Call();
            Init(natives::SET_PED_ONTO_MOUNT);
            Push(g.ped);
            Push(g.mount);
            Push(-1);
            Push(false);
            Call();
        }
    }

    GiveWeaponLocal(g);
    if (!g.mounted)
        StandStill(g.ped);

    sh_log::Write("npc: ghost created netId=%u model=0x%08X outfit=0x%08X "
                  "mount=0x%08X weapon=0x%08X\n",
                  g.netId, g.model, g.metaOutfit, g.mountModel, g.weapon);
}

void ApplyFrameToGhost(NpcGhost& g, const net::NpcFramePayload& f)
{
    if (!g.ped || !DoesExist(g.ped))
    {
        CreateGhostPed(g, f);
        if (!g.ped)
        {
            g.lastFrameMs = GetTickCount64();
            return;
        }
    }
    g.target = Vec3{f.x, f.y, f.z};
    g.targetHeading = f.heading;
    g.move = f.move;
    g.action = f.action;
    g.aim = Vec3{f.aimX, f.aimY, f.aimZ};
    g.hasTarget = true;
    g.lastFrameMs = GetTickCount64();
}

void UpdateGhostMove(NpcGhost& g, unsigned int tickCount)
{
    if (!g.ped || !DoesExist(g.ped))
        return;

    // Конный: едет лошадь (привязка к земле), всадник следует за ней.
    if (g.mounted && g.mount && DoesExist(g.mount))
    {
        const Vec3 cur = GetCoords(g.mount);
        const float dz = g.target.z - cur.z;
        const float dist2 = (g.target.x - cur.x) * (g.target.x - cur.x) +
                            (g.target.y - cur.y) * (g.target.y - cur.y);
        if (dist2 > kTeleportDist * kTeleportDist || std::fabs(dz) > 20.f)
        {
            const float gz = GroundZ(g.target.x, g.target.y, g.target.z);
            SetCoordsLocal(g.mount, Vec3{g.target.x, g.target.y, gz});
        }
        SetHeadingLocal(g.mount, g.targetHeading);
        using namespace natives_api;
        Init(natives::GET_MOUNT);
        Push(g.ped);
        if (static_cast<unsigned int>(Call()) != g.mount)
        {
            Init(natives::SET_PED_ONTO_MOUNT);
            Push(g.ped);
            Push(g.mount);
            Push(-1);
            Push(false);
            Call();
        }
        if (g.action == static_cast<uint8_t>(net::Action::Shooting) &&
            (tickCount - g.lastShootTick) >= 8)
        {
            EquipWeaponInHand(g);
            g.lastShootTick = tickCount;
            ShootAt(g.ped, g.aim);
        }
        else if (g.action == static_cast<uint8_t>(net::Action::Aiming) &&
                 (tickCount - g.lastAimTick) >= 15)
        {
            EquipWeaponInHand(g);
            g.lastAimTick = tickCount;
            AimGunAt(g.ped, g.aim);
        }
        return;
    }

    // Пеший: идём/бежим по навмешу, стоим на месте — как призрак игрока.
    const Vec3 cur = GetCoords(g.ped);
    const float dx = g.target.x - cur.x;
    const float dy = g.target.y - cur.y;
    const float dist2 = dx * dx + dy * dy;

    const bool walking =
        g.move == static_cast<uint8_t>(coop::MoveState::Walk);
    const bool running =
        g.move == static_cast<uint8_t>(coop::MoveState::Run);

    if (dist2 > kTeleportDist * kTeleportDist)
    {
        const float gz = GroundZ(g.target.x, g.target.y, g.target.z);
        SetCoordsLocal(g.ped, Vec3{g.target.x, g.target.y, gz});
        SetHeadingLocal(g.ped, g.targetHeading);
        g.taskValid = false;
    }
    else if (walking || running)
    {
        const float speed = running ? 2.0f : 1.0f;
        const bool retarget =
            !g.taskValid ||
            std::fabs(g.target.x - g.taskTarget.x) > kTaskRetargetDist ||
            std::fabs(g.target.y - g.taskTarget.y) > kTaskRetargetDist ||
            g.appliedMove != g.move;
        if (retarget)
        {
            const float gz = GroundZ(g.target.x, g.target.y, g.target.z);
            FollowNavmesh(g.ped, Vec3{g.target.x, g.target.y, gz}, speed);
            g.taskTarget = g.target;
            g.taskValid = true;
            g.appliedMove = g.move;
        }
        SetHeadingLocal(g.ped, g.targetHeading);
    }
    else
    {
        if (dist2 > 1.5f * 1.5f)
        {
            const float gz = GroundZ(g.target.x, g.target.y, g.target.z);
            if (!g.taskValid ||
                std::fabs(g.target.x - g.taskTarget.x) > kTaskRetargetDist ||
                std::fabs(g.target.y - g.taskTarget.y) > kTaskRetargetDist)
            {
                FollowNavmesh(g.ped, Vec3{g.target.x, g.target.y, gz}, 1.0f);
                g.taskTarget = g.target;
                g.taskValid = true;
            }
        }
        else if (g.taskValid || g.appliedMove != g.move)
        {
            StandStill(g.ped);
            g.taskValid = false;
            g.appliedMove = g.move;
        }
        SetHeadingLocal(g.ped, g.targetHeading);
    }

    if (g.action == static_cast<uint8_t>(net::Action::Shooting) &&
        (tickCount - g.lastShootTick) >= 8)
    {
        EquipWeaponInHand(g);
        g.lastShootTick = tickCount;
        ShootAt(g.ped, g.aim);
    }
    else if (g.action == static_cast<uint8_t>(net::Action::Aiming) &&
             (tickCount - g.lastAimTick) >= 15)
    {
        EquipWeaponInHand(g);
        g.lastAimTick = tickCount;
        AimGunAt(g.ped, g.aim);
    }
}

void GuestInterp(unsigned int tickCount)
{
    const uint64_t now = GetTickCount64();
    for (auto& kv : g_ghosts)
    {
        NpcGhost& g = kv.second;
        if (!g.ped)
            continue;
        if (!DoesExist(g.ped))
        {
            g.ped = 0;
            DeleteGhostEntity(g.mount);
            continue;
        }
        if (g.lastFrameMs != 0 && now - g.lastFrameMs > kGhostTimeoutMs)
        {
            if (g.mount)
                DeleteGhostEntity(g.mount);
            DeleteGhostEntity(g.ped);
            sh_log::Write("npc: ghost removed netId=%u (timeout)\n", g.netId);
            continue;
        }
        if (!g.hasTarget)
            continue;
        UpdateGhostMove(g, tickCount);
    }

    for (auto it = g_ghosts.begin(); it != g_ghosts.end();)
    {
        if (it->second.ped == 0)
            it = g_ghosts.erase(it);
        else
            ++it;
    }
}

void SuppressGuestAmbient()
{
    using namespace natives_api;
    Init(natives::_SET_AMBIENT_HUMAN_DENSITY_MULTIPLIER_THIS_FRAME);
    Push(0.0f); Call();
    Init(natives::_SET_AMBIENT_PED_DENSITY_MULTIPLIER_THIS_FRAME);
    Push(0.0f); Call();
}

// ---------------- хост: сканер ----------------

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
        f.action = DetectAction(npc.handle);
        if (f.action != static_cast<uint8_t>(net::Action::None))
        {
            const float h = f.heading * 0.017453292519943295f;
            f.aimX = p.x - std::sin(h) * 15.0f;
            f.aimY = p.y + std::cos(h) * 15.0f;
            f.aimZ = p.z + 1.2f;
        }
        coop::SendNpcFrame(f);
    }
    (void)tickCount;
}

// ---------------- гость: приём ----------------

void GuestDrain(unsigned int tickCount)
{
    std::vector<net::NpcSpawnPayload> spawns;
    std::vector<net::NpcFramePayload> frames;
    {
        std::lock_guard<std::mutex> lk(g_mtx);
        spawns.swap(g_pendingSpawns);
        frames.swap(g_pendingFrames);
    }

    for (const net::NpcSpawnPayload& sp : spawns)
    {
        NpcGhost& g = g_ghosts[sp.netId];
        g.netId = sp.netId;
        if (g.ped && (g.model != sp.model || g.mounted != ((sp.flags & 1u) != 0)))
        {
            DeleteGhostEntity(g.mount);
            DeleteGhostEntity(g.ped);
            g = NpcGhost{};
            g.netId = sp.netId;
        }
        g.model = sp.model;
        g.metaOutfit = sp.metaOutfit;
        g.mountModel = sp.mountModel;
        g.mounted = (sp.flags & 1u) != 0 && sp.mountModel != 0;
        if (g.weapon != sp.weaponHash)
        {
            g.weapon = sp.weaponHash;
            g.weaponGiven = false;
            g.weaponEquipped = false;
        }
    }

    for (const net::NpcFramePayload& f : frames)
    {
        auto it = g_ghosts.find(f.netId);
        if (it == g_ghosts.end())
            continue;
        ApplyFrameToGhost(it->second, f);
    }

    if (tickCount - g_lastGuestLogTick >= 300)
    {
        g_lastGuestLogTick = tickCount;
        unsigned int created = 0;
        for (const auto& kv : g_ghosts)
            if (kv.second.ped)
                ++created;
        sh_log::Write("npc: guest ghosts=%u (spawns=%u frames=%u this batch)\n",
                      created, (unsigned)spawns.size(), (unsigned)frames.size());
    }
}

void ResetInternal()
{
    g_roster.clear();
    g_npcNetIds.clear();
    g_sentSpawns.clear();
    for (auto& kv : g_ghosts)
    {
        DeleteGhostEntity(kv.second.mount);
        DeleteGhostEntity(kv.second.ped);
    }
    g_ghosts.clear();
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
        if (!g_roster.empty() || !g_sentSpawns.empty() || !g_ghosts.empty())
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
        GuestInterp(tickCount);
        SuppressGuestAmbient();
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
