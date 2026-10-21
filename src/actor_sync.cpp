#include "actor_sync.h"

#include "config.h"
#include "coop.h"
#include "log.h"
#include "native_call.h"
#include "natives.h"
#include "script_hook.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <windows.h> // QueryPerformanceCounter для perf: actor p95

namespace coop {
namespace actor_sync {
namespace {

// ======================= общие константы =======================
constexpr unsigned int kDiscoveryPeriodTicks = 6;  // 10 Гц при 60 тиках/с
constexpr unsigned int kSpawnHeartbeatTicks = 150; // ~2.5 с
constexpr unsigned int kProbeIntervalTicks = 120;  // ~2 с (npc_debug=1)
constexpr unsigned int kResyncAfterTicks = 120;    // 2 с без кадров
constexpr unsigned int kDeleteAfterTicks = 600;    // 10 с — удалить прокси
constexpr int kMaxSpawnsPerTick = 2;               // лимит CREATE_PED/тик
constexpr int kMaxDeletesPerTick = 2;              // лимит DELETE_PED/тик
constexpr int kMaxModelRequestsPerTick = 2;
constexpr int kProbeMaxPool = 2048;
constexpr int kProbeCount = 20;
constexpr float kProbeRadius = 60.0f;
constexpr int kActorLodDist = 100;
constexpr float kTeleportDist = 12.0f;
constexpr float kRetargetDist = 1.0f;
constexpr float kHeadingDeadzone = 5.0f;
constexpr int kLogEveryTicks = 60;      // 1 строка/с на тип
constexpr int kSummaryEveryTicks = 300; // 1 строка/5 с

// P3-фиксы по ревью E1 (task-45).
constexpr unsigned int kPendingSpawnTimeoutTicks = 300; // 5 с ожидания модели
constexpr unsigned int kNoCollisionRefreshTicks = 60;   // 1 с: повторить no-collision
constexpr unsigned int kDeleteRetryOffsetTicks = 60;    // повтор DELETE через ~1 с
constexpr unsigned int kPerfLogEveryTicks = 600;        // perf-строка раз в ~10 с
constexpr int kPerfWindow = 128;                        // окно p95-замеров
constexpr int kDeleteFailedLogEveryTicks = 60;          // 1 строка/с на delete failed

// Причины ActorDespawnPayload.reason (контракт игрового слоя).
constexpr uint8_t kDespawnGone = 1;    // ушёл/далеко/не найден в пуле
constexpr uint8_t kDespawnDead = 2;    // умер
constexpr uint8_t kDespawnEvicted = 3; // вытеснен из cap/сменил модель

// ======================= QPC p95-метрики (perf: actor) =======================
unsigned long long QpcNow()
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<unsigned long long>(c.QuadPart);
}

unsigned long long QpcFreq()
{
    static const unsigned long long freq = []() {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<unsigned long long>(f.QuadPart);
    }();
    return freq;
}

unsigned long long QpcDeltaUs(unsigned long long start)
{
    const unsigned long long freq = QpcFreq();
    if (!freq) return 0;
    return (QpcNow() - start) * 1000000ull / freq;
}

struct PerfP95
{
    unsigned long long samples[kPerfWindow]{};
    int count = 0;
    int pos = 0;

    void AddUs(unsigned long long us)
    {
        samples[pos] = us;
        pos = (pos + 1) % kPerfWindow;
        if (count < kPerfWindow) ++count;
    }

    double AvgMs() const
    {
        if (!count) return 0.0;
        unsigned long long sum = 0;
        for (int i = 0; i < count; ++i) sum += samples[i];
        return static_cast<double>(sum) / count / 1000.0;
    }

    double MaxMs() const
    {
        if (!count) return 0.0;
        unsigned long long m = 0;
        for (int i = 0; i < count; ++i)
            if (samples[i] > m) m = samples[i];
        return static_cast<double>(m) / 1000.0;
    }

    double P95Ms() const
    {
        if (!count) return 0.0;
        unsigned long long tmp[kPerfWindow];
        for (int i = 0; i < count; ++i) tmp[i] = samples[i];
        std::sort(tmp, tmp + count);
        int idx = (count * 95 + 99) / 100 - 1; // ceil(0.95*n)-1
        if (idx < 0) idx = 0;
        if (idx >= count) idx = count - 1;
        return static_cast<double>(tmp[idx]) / 1000.0;
    }
};

PerfP95 g_perfScan;
PerfP95 g_perfSpawn;
PerfP95 g_perfApply;
unsigned int g_lastPerfLogTick = 0;

// ======================= helper natives =======================
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
    Push(handle);
    Push(false);
    Push(true);
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

float GetSpeed(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::GET_ENTITY_SPEED);
    Push(handle);
    return CallFloat();
}

unsigned int GetModelOf(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::GET_ENTITY_MODEL);
    Push(handle);
    return static_cast<unsigned int>(Call());
}

bool IsDeadPed(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::IS_PED_DEAD_OR_DYING);
    Push(ped);
    return Call() != 0;
}

bool IsHumanPed(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::IS_PED_HUMAN);
    Push(ped);
    return Call() != 0;
}

bool IsPlayerPed(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::IS_PED_A_PLAYER);
    Push(ped);
    return Call() != 0;
}

bool IsMissionEntity(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::IS_ENTITY_A_MISSION_ENTITY);
    Push(handle);
    return Call() != 0;
}

unsigned int GetMountOf(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::GET_MOUNT);
    Push(ped);
    return static_cast<unsigned int>(Call());
}

bool IsOnMount(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::IS_PED_ON_MOUNT);
    Push(ped);
    return Call() != 0;
}

bool IsInCombat(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::IS_PED_IN_COMBAT);
    Push(ped);
    Push(static_cast<unsigned long long>(0)); // target=0: любой
    return Call() != 0;
}

bool IsScenarioActive(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::IS_PED_USING_ANY_SCENARIO);
    Push(ped);
    return Call() != 0;
}

uint32_t GetDynamicScenario(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::_GET_ACTIVE_DYNAMIC_SCENARIO);
    Push(ped);
    return static_cast<uint32_t>(Call());
}

bool IsReloading(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::IS_PED_RELOADING);
    Push(ped);
    return Call() != 0;
}

bool IsShootingPed(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::IS_PED_SHOOTING);
    Push(ped);
    return Call() != 0;
}

bool IsWeaponReady(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::IS_PED_WEAPON_READY_TO_SHOOT);
    Push(ped);
    return Call() != 0;
}

bool IsWeaponHolstered(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::_IS_PED_CURRENT_WEAPON_HOLSTERED);
    Push(ped);
    return Call() != 0;
}

bool IsWeaponChanging(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::_IS_WEAPON_HOLSTER_STATE_CHANGING);
    Push(ped);
    return Call() != 0;
}

uint32_t GetHeldWeapon(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::_GET_PED_CURRENT_HELD_WEAPON);
    Push(ped);
    return static_cast<uint32_t>(Call());
}

uint32_t GetCurrentWeapon(unsigned int ped)
{
    using namespace natives_api;
    unsigned long long out = 0;
    Init(natives::GET_CURRENT_PED_WEAPON);
    Push(ped);
    Push(reinterpret_cast<unsigned long long>(&out));
    Push(static_cast<unsigned long long>(1)); // p2
    Push(static_cast<unsigned long long>(0)); // attachPoint = HAND_PRIMARY
    Push(static_cast<unsigned long long>(0)); // p4
    Call();
    return static_cast<uint32_t>(out);
}

uint32_t GetMetaOutfit(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::GET_PED_META_OUTFIT_HASH);
    Push(ped);
    return static_cast<uint32_t>(Call());
}

uint8_t GetHealthByte(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::GET_ENTITY_HEALTH);
    Push(ped);
    const int hp = static_cast<int>(Call()); // 0..200 для человека
    int v = hp / 2;
    if (v < 0) v = 0;
    if (v > 255) v = 255;
    return static_cast<uint8_t>(v);
}

uint8_t MoveStateFromSpeed(float speed)
{
    if (speed >= 4.5f) return 3; // sprint
    if (speed >= 2.0f) return 2; // run
    if (speed >= 0.3f) return 1; // walk
    return 0;                    // idle
}

uint16_t BuildFlags(unsigned int ped)
{
    uint16_t f = 0;
    if (IsOnMount(ped) || GetMountOf(ped) != 0) f |= net::kActorFlagMounted;
    if (IsDeadPed(ped)) f |= net::kActorFlagDead;
    if (!IsWeaponHolstered(ped)) f |= net::kActorFlagDrawn;
    if (IsWeaponChanging(ped)) f |= net::kActorFlagChanging;
    if (IsInCombat(ped)) f |= net::kActorFlagCombat;
    if (IsScenarioActive(ped)) f |= net::kActorFlagScenario;
    if (IsReloading(ped)) f |= net::kActorFlagReloading;
    if (IsWeaponReady(ped) || IsShootingPed(ped)) f |= net::kActorFlagAiming;
    return f;
}

void SetCoordsNative(unsigned int handle, float x, float y, float z)
{
    using namespace natives_api;
    Init(natives::SET_ENTITY_COORDS);
    Push(handle);
    Push(x); Push(y); Push(z);
    Push(false); Push(false); Push(false);
    Push(false); // clearTasks=false: не рвём текущую задачу
    Call();
}

void SetHeadingNative(unsigned int handle, float heading)
{
    using namespace natives_api;
    Init(natives::SET_ENTITY_HEADING);
    Push(handle);
    Push(heading);
    Call();
}

void SetMissionEntity(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::SET_ENTITY_AS_MISSION_ENTITY);
    Push(handle);
    Push(true);
    Push(true);
    Call();
}

// P2-фикс (task-45): снять mission-entity перед DELETE_PED — иначе движок
// может не удалить сущность, а книга g_proxies уже стёрта (утечка/дубли).
void SetMissionEntityFlag(unsigned int handle, bool value)
{
    using namespace natives_api;
    Init(natives::SET_ENTITY_AS_MISSION_ENTITY);
    Push(handle);
    Push(value);
    Push(value);
    Call();
}

void SetInvincible(unsigned int handle, bool value)
{
    using namespace natives_api;
    Init(natives::SET_ENTITY_INVINCIBLE);
    Push(handle);
    Push(static_cast<unsigned long long>(value ? 1 : 0));
    Call();
}

void SetVisible(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::SET_ENTITY_VISIBLE);
    Push(handle);
    Push(true);
    Call();
}

void SetLodDist(unsigned int handle, int dist)
{
    using namespace natives_api;
    Init(natives::SET_ENTITY_LOD_DIST);
    Push(handle);
    Push(static_cast<unsigned long long>(dist));
    Call();
}

void SetNoCollisionWith(unsigned int a, unsigned int b)
{
    if (!a || !b || a == b) return;
    using namespace natives_api;
    Init(natives::SET_ENTITY_NO_COLLISION_ENTITY);
    Push(a);
    Push(b);
    Push(false); // thisFrameOnly=false
    Call();
}

void SetCanBeTargetted(unsigned int ped, bool value)
{
    using namespace natives_api;
    Init(natives::SET_PED_CAN_BE_TARGETTED);
    Push(ped);
    Push(static_cast<unsigned long long>(value ? 1 : 0));
    Call();
}

void SetBlockingOfNonTemporaryEvents(unsigned int ped, bool value)
{
    using namespace natives_api;
    Init(natives::SET_BLOCKING_OF_NON_TEMPORARY_EVENTS);
    Push(ped);
    Push(static_cast<unsigned long long>(value ? 1 : 0));
    Call();
}

void RandomizeComponents(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::SET_PED_RANDOM_COMPONENT_VARIATION);
    Push(ped);
    Push(static_cast<unsigned long long>(0));
    Call();
}

void SetMoveBlend(unsigned int ped, float value)
{
    using namespace natives_api;
    Init(natives::SET_PED_MAX_MOVE_BLEND_RATIO);
    Push(ped);
    Push(value);
    Call();
}

void FollowNavmesh(unsigned int ped, float x, float y, float z, float speed,
                   float heading)
{
    using namespace natives_api;
    Init(natives::TASK_FOLLOW_NAV_MESH_TO_COORD);
    Push(ped);
    Push(x); Push(y); Push(z);
    Push(speed);
    Push(static_cast<unsigned long long>(0xFFFFFFFF)); // timeout = -1
    Push(0.5f);                                        // stoppingRange
    Push(static_cast<unsigned long long>(4));          // flags
    Push(heading);                                     // p8: heading (0 = авто)
    Call();
}

void StandStill(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::TASK_STAND_STILL);
    Push(ped);
    Push(static_cast<unsigned long long>(0xFFFFFFFF));
    Call();
}

bool HasModelLoaded(unsigned int model)
{
    using namespace natives_api;
    Init(natives::HAS_MODEL_LOADED);
    Push(static_cast<unsigned long long>(model));
    return Call() != 0;
}

void RequestModel(unsigned int model)
{
    using namespace natives_api;
    Init(natives::REQUEST_MODEL);
    Push(static_cast<unsigned long long>(model));
    Push(false); // p1 обязателен (иначе движок читает мусор)
    Call();
}

void SetModelAsNoLongerNeeded(unsigned int model)
{
    if (!model) return;
    using namespace natives_api;
    Init(natives::SET_MODEL_AS_NO_LONGER_NEEDED);
    Push(static_cast<unsigned long long>(model));
    Call();
}

unsigned int CreatePed(unsigned int model, float x, float y, float z,
                       float heading)
{
    using namespace natives_api;
    Init(natives::CREATE_PED);
    Push(static_cast<unsigned long long>(model));
    Push(x); Push(y); Push(z); Push(heading);
    Push(false); // isNetwork
    Push(false); // bScriptHostPed
    Push(false); // p7
    Push(false); // p8
    return static_cast<unsigned int>(Call());
}

void DeletePedHandle(unsigned int handle)
{
    if (!handle) return;
    int h = static_cast<int>(handle);
    {
        using namespace natives_api;
        Init(natives::DELETE_PED);
        Push(reinterpret_cast<unsigned long long>(&h));
        Call();
    }
    if (DoesExist(handle))
    {
        int h2 = static_cast<int>(handle);
        using namespace natives_api;
        Init(natives::SET_ENTITY_AS_NO_LONGER_NEEDED);
        Push(reinterpret_cast<unsigned long long>(&h2));
        Call();
    }
}

bool GroundZ(float x, float y, float z, float& outZ)
{
    using namespace natives_api;
    Init(natives::GET_GROUND_Z_FOR_3D_COORD);
    Push(x); Push(y); Push(z);
    outZ = 0.f;
    Push(reinterpret_cast<unsigned long long>(&outZ));
    Push(static_cast<unsigned long long>(0)); // p4
    return Call() != 0;
}

// ======================= лог-хелперы =======================
unsigned int g_logTick = 0;
unsigned int g_lastSpawnLogTick = 0;
unsigned int g_lastDespawnLogTick = 0;
unsigned int g_lastSnapshotLogTick = 0;
unsigned int g_lastSummaryLogTick = 0;
unsigned int g_lastDeleteFailLogTick = 0;
unsigned int g_lastSpawnTimeoutLogTick = 0;
unsigned int g_lastProxyCapLogTick = 0;

void LogSpawn(uint32_t actorId, uint32_t model, unsigned int n)
{
    if (g_logTick - g_lastSpawnLogTick < kLogEveryTicks) return;
    g_lastSpawnLogTick = g_logTick;
    sh_log::Write("actor: spawn id=%u model=0x%X n=%u\n", actorId, model, n);
}

void LogDespawn(uint32_t actorId, unsigned int reason)
{
    if (g_logTick - g_lastDespawnLogTick < kLogEveryTicks) return;
    g_lastDespawnLogTick = g_logTick;
    sh_log::Write("actor: despawn id=%u reason=%u\n", actorId, reason);
}

void LogSnapshot(uint32_t epoch, unsigned int count, unsigned int bytes)
{
    if (g_logTick - g_lastSnapshotLogTick < kLogEveryTicks) return;
    g_lastSnapshotLogTick = g_logTick;
    sh_log::Write("actor: snapshot epoch=%u count=%u bytes=%u\n",
                  epoch, count, bytes);
}

void LogDeleteFailed(uint32_t actorId, unsigned int handle)
{
    if (g_logTick - g_lastDeleteFailLogTick < kDeleteFailedLogEveryTicks) return;
    g_lastDeleteFailLogTick = g_logTick;
    sh_log::Write("actor: delete failed id=%u handle=%u\n", actorId, handle);
}

void LogSpawnTimeout(uint32_t actorId)
{
    if (g_logTick - g_lastSpawnTimeoutLogTick < kLogEveryTicks) return;
    g_lastSpawnTimeoutLogTick = g_logTick;
    sh_log::Write("actor: spawn timeout id=%u\n", actorId);
}

void LogProxyCap(unsigned int have, unsigned int cap)
{
    if (g_logTick - g_lastProxyCapLogTick < kLogEveryTicks) return;
    g_lastProxyCapLogTick = g_logTick;
    sh_log::Write("actor: proxy cap reached have=%u cap=%u\n", have, cap);
}

// ======================= perf: actor p95 (npc_debug=1) =======================
void LogPerfIfDue(unsigned int tick)
{
    if (!coopcfg::Get().npcDebug) return;
    if (tick - g_lastPerfLogTick < kPerfLogEveryTicks) return;
    g_lastPerfLogTick = tick;
    sh_log::Write(
        "perf: actor scan=%.2f p95=%.2f max=%.2f | "
        "spawn=%.2f p95=%.2f max=%.2f | apply=%.2f p95=%.2f max=%.2f\n",
        g_perfScan.AvgMs(), g_perfScan.P95Ms(), g_perfScan.MaxMs(),
        g_perfSpawn.AvgMs(), g_perfSpawn.P95Ms(), g_perfSpawn.MaxMs(),
        g_perfApply.AvgMs(), g_perfApply.P95Ms(), g_perfApply.MaxMs());
}

// ======================= inbox сетевого потока =======================
// Объявлен ДО host/guest-логики: HostTick проверяет resync-запрос, гостевые
// функции забирают батчи. Только копии под mutex, без нативов в callback.
std::mutex g_inboxMutex;
struct Inbox
{
    bool haveSpawn = false;
    uint32_t spawnEpoch = 0;
    std::vector<net::ActorSpawn> spawns;
    bool haveFrames = false;
    uint32_t framesEpoch = 0;
    uint16_t framesSeq = 0;
    std::vector<net::ActorFrame> frames;
    std::deque<net::ActorDespawnPayload> despawns;
    uint32_t events = 0;
    uint32_t outfits = 0;
    bool resyncRequested = false;
    uint32_t resyncEpoch = 0;
};
Inbox g_inbox;

void ClearInboxLocked()
{
    g_inbox.haveSpawn = false;
    g_inbox.spawns.clear();
    g_inbox.haveFrames = false;
    g_inbox.frames.clear();
    g_inbox.despawns.clear();
    g_inbox.events = 0;
    g_inbox.outfits = 0;
    g_inbox.resyncRequested = false;
}

// ======================= E-1 probe (npc_debug=1) =======================
struct ProbeCandidate
{
    unsigned int handle = 0;
    float distance2 = 0.f;
};

std::unordered_set<unsigned long long> g_probeSeenCombos;
unsigned int g_lastProbeTick = 0;
bool g_probeWorldQueriesWarned = false;

bool IsMetaOutfitEquipped(unsigned int ped, uint32_t outfitHash)
{
    if (!outfitHash) return false;
    using namespace natives_api;
    Init(natives::_IS_META_PED_OUTFIT_EQUIPPED);
    Push(ped);
    Push(static_cast<unsigned long long>(outfitHash));
    return Call() != 0;
}

int GetScenarioPointType(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::_GET_SCENARIO_POINT_TYPE_PED_IS_USING);
    Push(ped);
    return static_cast<int>(Call());
}

int GetPedType(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::GET_PED_TYPE);
    Push(ped);
    return static_cast<int>(Call());
}

void ProbeTick(unsigned int localPed, unsigned int tickCount)
{
    if (g_lastProbeTick == 0)
        g_lastProbeTick = tickCount - kProbeIntervalTicks;
    if (tickCount - g_lastProbeTick < kProbeIntervalTicks)
        return;
    g_lastProbeTick = tickCount;

    if (!sh::HasWorldQueries() || !sh::worldGetAllPeds)
    {
        if (!g_probeWorldQueriesWarned)
        {
            g_probeWorldQueriesWarned = true;
            sh_log::Write("probe: worldGetAllPeds unavailable, probe disabled\n");
        }
        return;
    }

    static int handles[kProbeMaxPool];
    int count = sh::worldGetAllPeds(handles, kProbeMaxPool);
    if (count < 0) count = 0;
    if (count > kProbeMaxPool) count = kProbeMaxPool;

    const Vec3 origin = GetCoords(localPed);
    const unsigned int localMount = GetMountOf(localPed);

    static std::vector<ProbeCandidate> candidates;
    candidates.clear();
    candidates.reserve(64);

    unsigned int humansInRadius = 0;
    unsigned int animalsInRadius = 0;
    unsigned int totalInRadius = 0;
    for (int i = 0; i < count; ++i)
    {
        const unsigned int handle = static_cast<unsigned int>(handles[i]);
        if (!handle || handle == localPed || handle == localMount)
            continue;
        if (coop::IsWorldSyncExcluded(handle))
            continue;
        if (!DoesExist(handle))
            continue;
        if (IsPlayerPed(handle))
            continue;

        const Vec3 p = GetCoords(handle);
        const float dx = p.x - origin.x;
        const float dy = p.y - origin.y;
        const float d2 = dx * dx + dy * dy;
        if (d2 > kProbeRadius * kProbeRadius)
            continue;

        ++totalInRadius;
        if (!IsHumanPed(handle))
        {
            ++animalsInRadius;
            continue;
        }
        ++humansInRadius;
        if (IsDeadPed(handle))
            continue;

        ProbeCandidate c;
        c.handle = handle;
        c.distance2 = d2;
        candidates.push_back(c);
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const ProbeCandidate& a, const ProbeCandidate& b) {
                  return a.distance2 < b.distance2;
              });
    if (static_cast<int>(candidates.size()) > kProbeCount)
        candidates.resize(kProbeCount);

    unsigned int newCombos = 0;
    for (const ProbeCandidate& c : candidates)
    {
        const unsigned int model = GetModelOf(c.handle);
        const uint32_t metaOutfit = GetMetaOutfit(c.handle);
        const unsigned long long comboKey =
            (static_cast<unsigned long long>(model) << 32) |
            static_cast<unsigned long long>(metaOutfit);
        if (!g_probeSeenCombos.insert(comboKey).second)
            continue;

        const unsigned int mountHandle = GetMountOf(c.handle);
        const uint32_t mountModel = mountHandle ? GetModelOf(mountHandle) : 0;
        sh_log::Write(
            "probe: model=0x%X metaOutfit=0x%X equipped=%d mount=0x%X "
            "weapon=0x%X scenarioAny=%d dynScenario=0x%X pointType=0x%X "
            "pedType=%d human=%d mission=%d\n",
            model, metaOutfit,
            IsMetaOutfitEquipped(c.handle, metaOutfit) ? 1 : 0, mountModel,
            GetHeldWeapon(c.handle) ? GetHeldWeapon(c.handle)
                                    : GetCurrentWeapon(c.handle),
            IsScenarioActive(c.handle) ? 1 : 0, GetDynamicScenario(c.handle),
            static_cast<unsigned int>(GetScenarioPointType(c.handle)),
            GetPedType(c.handle), IsHumanPed(c.handle) ? 1 : 0,
            IsMissionEntity(c.handle) ? 1 : 0);
        ++newCombos;
    }

    sh_log::Write("probe: scan humans=%u animals=%u total=%u logged=%u "
                  "newCombos=%u unique=%u\n",
                  humansInRadius, animalsInRadius, totalInRadius,
                  static_cast<unsigned int>(candidates.size()), newCombos,
                  static_cast<unsigned int>(g_probeSeenCombos.size()));
}

// ======================= HOST: sender =======================
struct RosterEntry
{
    uint32_t actorId = 0;
    uint32_t model = 0;
    unsigned int lastSeenTick = 0;
};

std::map<unsigned int, RosterEntry> g_roster;
uint32_t g_nextActorId = 1;
uint32_t g_hostEpoch = 0;
uint32_t g_sessionCounter = 0;
unsigned int g_lastDiscoveryTick = 0;
unsigned int g_lastHeartbeatTick = 0;
uint16_t g_frameSeq = 0;
std::deque<net::ActorDespawnPayload> g_despawnQueue;
bool g_resendAllSpawns = false;

uint32_t MakeEpoch(unsigned int tickCount)
{
    ++g_sessionCounter;
    uint32_t e = (static_cast<uint32_t>(tickCount) ^
                  (g_sessionCounter * 0x9E3779B9u)) & 0xFFFFu;
    if (!e) e = 1;
    return e;
}

net::ActorSpawn BuildSpawn(unsigned int handle, uint32_t actorId)
{
    net::ActorSpawn s{};
    s.actorId = actorId;
    s.epoch = static_cast<uint16_t>(g_hostEpoch & 0xFFFFu);
    s.kind = net::kActorKindHuman;
    s.flags = static_cast<uint8_t>(BuildFlags(handle));
    s.model = GetModelOf(handle);
    s.metaOutfit = GetMetaOutfit(handle);
    s.mountActorId = 0; // E1: human-only, лошади не акторы
    const uint32_t held = GetHeldWeapon(handle);
    s.weaponHash = held ? held : GetCurrentWeapon(handle);
    s.scenarioHash = GetDynamicScenario(handle);
    const Vec3 p = GetCoords(handle);
    s.x = p.x; s.y = p.y; s.z = p.z;
    s.heading = GetHeading(handle);
    s.health = GetHealthByte(handle);
    s.moveStyle = MoveStateFromSpeed(GetSpeed(handle));
    s.phase = 0;
    return s;
}

net::ActorFrame BuildFrame(unsigned int handle, uint32_t actorId)
{
    net::ActorFrame f{};
    f.actorId = actorId;
    const float speed = GetSpeed(handle);
    const uint8_t move = MoveStateFromSpeed(speed);
    f.flags = BuildFlags(handle);
    f.animState = move;
    float sp = speed / 10.0f; // 0..1 доля максимальной скорости
    if (sp < 0.f) sp = 0.f;
    if (sp > 1.f) sp = 1.f;
    f.speed = static_cast<uint8_t>(sp * 255.0f);
    const Vec3 p = GetCoords(handle);
    f.x = p.x; f.y = p.y; f.z = p.z;
    f.heading = GetHeading(handle);
    f.targetActorId = 0;
    f.seq = g_frameSeq;
    return f;
}

void QueueDespawn(uint32_t actorId, uint8_t reason)
{
    net::ActorDespawnPayload d{};
    d.epoch = g_hostEpoch;
    d.actorId = actorId;
    d.reason = reason;
    if (g_despawnQueue.size() < 32)
        g_despawnQueue.push_back(d);
}

void HostDiscovery(const TickContext& ctx)
{
    const unsigned int tick = ctx.tickCount;
    if (g_hostEpoch == 0)
        g_hostEpoch = MakeEpoch(tick);
    if (tick - g_lastDiscoveryTick < kDiscoveryPeriodTicks)
        return;
    g_lastDiscoveryTick = tick;
    const unsigned long long perfScanStart = QpcNow();

    int cap = coopcfg::Get().npcCap;
    if (cap < 1) cap = 1;
    if (cap > net::kMaxActors) cap = net::kMaxActors;
    float radius = static_cast<float>(coopcfg::Get().npcRadius);
    if (radius < 20.f) radius = 20.f;
    if (radius > 100.f) radius = 100.f;
    const float radius2 = radius * radius;

    struct Cand
    {
        unsigned int handle = 0;
        float distance2 = 1e30f;
    };
    static std::vector<Cand> candidates;
    candidates.clear();
    candidates.reserve(64);

    const Vec3 local = GetCoords(ctx.localPed);
    const unsigned int localMount = GetMountOf(ctx.localPed);

    static int handles[2048];
    int count = sh::worldGetAllPeds(handles, 2048);
    if (count < 0) count = 0;
    if (count > 2048) count = 2048;
    for (int i = 0; i < count; ++i)
    {
        const unsigned int handle = static_cast<unsigned int>(handles[i]);
        if (!handle || handle == ctx.localPed || handle == localMount)
            continue;
        if (handle == ctx.partnerGhost || handle == ctx.partnerMount)
            continue;
        if (coop::IsWorldSyncExcluded(handle))
            continue;
        if (!DoesExist(handle))
            continue;

        // P2-фикс (task-45): distance-first. В дикой зоне пул peds — сотни,
        // поэтому дорогие фильтры (player/human/mission/dead) вызываем только
        // для педов в радиусе; сначала координаты и dist² до обоих игроков.
        const Vec3 p = GetCoords(handle);
        const float dx = p.x - local.x;
        const float dy = p.y - local.y;
        float d2 = dx * dx + dy * dy;
        if (ctx.partnerValid)
        {
            const float px = p.x - ctx.partnerX;
            const float py = p.y - ctx.partnerY;
            const float d2p = px * px + py * py;
            if (d2p < d2) d2 = d2p;
        }
        if (d2 > radius2)
            continue;

        if (IsPlayerPed(handle))
            continue;
        if (!IsHumanPed(handle))
            continue;
        if (IsMissionEntity(handle))
            continue; // MVP: mission NPC не реплицируем
        if (IsDeadPed(handle))
            continue;

        Cand c;
        c.handle = handle;
        c.distance2 = d2;
        candidates.push_back(c);
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const Cand& a, const Cand& b) {
                  return a.distance2 < b.distance2;
              });

    // Стабильный roster: старые акторы сохраняют actorId, новые получают
    // следующий монотонный id. cap ограничивает число активных акторов.
    std::vector<unsigned int> selected;
    selected.reserve(cap);
    std::unordered_set<unsigned int> present;
    present.reserve(candidates.size() * 2 + 1);
    for (const Cand& c : candidates)
        present.insert(c.handle);

    // Порог despawn — 70 м (radius+10): старые акторы между радиусом
    // обнаружения и 70 м не пропадают, продолжаем слать им кадры.
    const float keepRadius2 = (radius + 10.0f) * (radius + 10.0f);
    for (const auto& kv : g_roster)
    {
        if (present.count(kv.first)) continue;
        const unsigned int handle = kv.first;
        if (!DoesExist(handle)) continue;
        if (IsDeadPed(handle)) continue;
        const Vec3 p = GetCoords(handle);
        const float dx = p.x - local.x;
        const float dy = p.y - local.y;
        float d2 = dx * dx + dy * dy;
        if (ctx.partnerValid)
        {
            const float px = p.x - ctx.partnerX;
            const float py = p.y - ctx.partnerY;
            const float d2p = px * px + py * py;
            if (d2p < d2) d2 = d2p;
        }
        if (d2 > keepRadius2) continue;
        Cand c;
        c.handle = handle;
        c.distance2 = d2;
        candidates.push_back(c);
        present.insert(handle);
    }
    std::sort(candidates.begin(), candidates.end(),
              [](const Cand& a, const Cand& b) {
                  return a.distance2 < b.distance2;
              });

    // 1) уже известные акторы, которые всё ещё в кандидатах
    for (const auto& kv : g_roster)
    {
        if (static_cast<int>(selected.size()) >= cap) break;
        if (present.count(kv.first))
            selected.push_back(kv.first);
    }
    // 2) добор новыми ближайшими
    std::unordered_set<unsigned int> selectedSet(selected.begin(), selected.end());
    for (const Cand& c : candidates)
    {
        if (static_cast<int>(selected.size()) >= cap) break;
        if (selectedSet.count(c.handle)) continue;
        selected.push_back(c.handle);
        selectedSet.insert(c.handle);
    }

    // 3) остальных из roster — despawn (умер/вытеснен/ушёл)
    std::vector<unsigned int> dropHandles;
    for (const auto& kv : g_roster)
    {
        if (selectedSet.count(kv.first)) continue;
        const bool dead = DoesExist(kv.first) && IsDeadPed(kv.first);
        const bool stillPresent = present.count(kv.first) != 0;
        QueueDespawn(kv.second.actorId,
                     dead ? kDespawnDead
                          : (stillPresent ? kDespawnEvicted : kDespawnGone));
        dropHandles.push_back(kv.first);
    }
    for (unsigned int h : dropHandles)
        g_roster.erase(h);

    // 4) новым — actorId; у существующих проверяем смену модели (тогда старый
    // актор despawn'ится, новый получает новый actorId — id не переиспользуем).
    std::vector<net::ActorSpawn> newSpawns;
    for (unsigned int handle : selected)
    {
        auto it = g_roster.find(handle);
        if (it != g_roster.end())
        {
            const uint32_t modelNow = GetModelOf(handle);
            if (modelNow == it->second.model)
            {
                it->second.lastSeenTick = tick;
                continue;
            }
            QueueDespawn(it->second.actorId, kDespawnEvicted);
            g_roster.erase(it);
        }
        RosterEntry e;
        e.actorId = g_nextActorId++;
        if (e.actorId == 0) e.actorId = g_nextActorId++;
        e.model = GetModelOf(handle);
        e.lastSeenTick = tick;
        g_roster[handle] = e;
        newSpawns.push_back(BuildSpawn(handle, e.actorId));
    }

    // 5) спавн-батч: новые акторы ИЛИ heartbeat (join/resync/20-).
    const bool heartbeat =
        (tick - g_lastHeartbeatTick) >= kSpawnHeartbeatTicks || g_resendAllSpawns;
    if (!newSpawns.empty() || heartbeat)
    {
        std::vector<net::ActorSpawn> out;
        if (heartbeat)
        {
            out.reserve(g_roster.size());
            for (const auto& kv : g_roster)
            {
                if (!DoesExist(kv.first)) continue;
                out.push_back(BuildSpawn(kv.first, kv.second.actorId));
            }
        }
        else
        {
            out = newSpawns;
        }
        if (!out.empty())
        {
            coop::SendActorSpawnBatch(g_hostEpoch, out);
            LogSpawn(out[0].actorId, out[0].model,
                     static_cast<unsigned int>(out.size()));
            g_lastHeartbeatTick = tick;
        }
        g_resendAllSpawns = false;
    }

    // 6) кадры движения 10 Гц
    std::vector<net::ActorFrame> frames;
    frames.reserve(g_roster.size());
    for (const auto& kv : g_roster)
    {
        if (!DoesExist(kv.first)) continue;
        frames.push_back(BuildFrame(kv.first, kv.second.actorId));
    }
    if (!frames.empty())
    {
        coop::SendActorFrames(g_hostEpoch, g_frameSeq++, frames);
    }

    g_perfScan.AddUs(QpcDeltaUs(perfScanStart));
}

void HostResetState()
{
    g_roster.clear();
    g_despawnQueue.clear();
    g_hostEpoch = 0;
    g_nextActorId = 1;
    g_lastDiscoveryTick = 0;
    g_lastHeartbeatTick = 0;
    g_resendAllSpawns = false;
}

void HostTick(const TickContext& ctx)
{
    if (!sh::HasWorldQueries() || !sh::worldGetAllPeds)
        return; // fail closed: без пула peds репликация невозможна

    // Гость может попросить resync (callback из сетевого потока).
    {
        std::lock_guard<std::mutex> lk(g_inboxMutex);
        if (g_inbox.resyncRequested)
        {
            g_inbox.resyncRequested = false;
            g_resendAllSpawns = true;
        }
    }

    if (coopcfg::Get().npcDebug)
        ProbeTick(ctx.localPed, ctx.tickCount);

    HostDiscovery(ctx);

    // despawn — не чаще 1 сообщения за тик
    if (!g_despawnQueue.empty())
    {
        net::ActorDespawnPayload d = g_despawnQueue.front();
        g_despawnQueue.pop_front();
        coop::SendActorDespawn(d);
        LogDespawn(d.actorId, d.reason);
    }
}

// ======================= GUEST: receiver =======================
struct Proxy
{
    uint32_t actorId = 0;
    uint32_t epoch = 0;
    unsigned int handle = 0;
    unsigned int model = 0;
    float tx = 0.f, ty = 0.f, tz = 0.f, heading = 0.f;
    uint8_t moveState = 0;
    uint16_t flags = 0;
    unsigned int lastFrameTick = 0;
    unsigned int spawnedTick = 0;
    int lastMoveState = -1;
    bool moveTaskValid = false;
    Vec3 moveTarget{0.f, 0.f, 0.f};
    bool dead = false;
    bool resyncLogged = false;
};

std::map<uint32_t, Proxy> g_proxies;
std::unordered_map<uint32_t, unsigned int> g_handleByActor;
uint32_t g_guestEpoch = 0;

// P3-фикс (task-45): pending spawn хранит тик первого появления — нужен
// таймаут 5 с, если модель так и не загрузилась/нет слота под cap.
struct PendingSpawn
{
    net::ActorSpawn spawn{};
    unsigned int firstSeenTick = 0;
};
std::vector<PendingSpawn> g_pendingSpawns;
std::unordered_set<uint32_t> g_pendingSpawnIds;
std::vector<net::ActorFrame> g_latestFrames;
bool g_haveFrames = false;
std::deque<net::ActorDespawnPayload> g_guestDespawnQueue;
int g_modelRequestsThisTick = 0;
unsigned int g_appliedFrames = 0;
unsigned int g_timeouts = 0;
unsigned int g_lastNoCollisionTick = 0;

// P2-фикс (task-45): перед DELETE_PED снимаем mission-entity; если сущность
// всё равно существует — НЕ теряем учёт (proxy остаётся в g_proxies, повтор).
bool DeleteProxy(Proxy& proxy)
{
    if (!proxy.handle)
    {
        g_handleByActor.erase(proxy.actorId);
        return true;
    }

    SetMissionEntityFlag(proxy.handle, false);
    DeletePedHandle(proxy.handle);

    if (DoesExist(proxy.handle))
    {
        LogDeleteFailed(proxy.actorId, proxy.handle);
        return false;
    }

    SetModelAsNoLongerNeeded(proxy.model);
    g_handleByActor.erase(proxy.actorId);
    proxy.handle = 0;
    return true;
}

void DeleteAllProxies()
{
    std::vector<uint32_t> removed;
    for (auto& kv : g_proxies)
    {
        if (DeleteProxy(kv.second))
            removed.push_back(kv.first);
    }
    for (uint32_t id : removed)
        g_proxies.erase(id);
}

bool CreateProxy(const net::ActorSpawn& s, const TickContext& ctx)
{
    // P3-фикс (task-45): не создавать сверх cap (12) — pending не теряем,
    // он подождёт освобождения слота либо словит таймаут 5 с.
    if (static_cast<int>(g_proxies.size()) >= net::kMaxActors)
    {
        LogProxyCap(static_cast<unsigned int>(g_proxies.size()),
                    static_cast<unsigned int>(net::kMaxActors));
        return false;
    }

    float z = s.z;
    float gz = 0.f;
    if (GroundZ(s.x, s.y, s.z + 1.0f, gz))
        z = gz + 0.2f;

    const unsigned int handle = CreatePed(s.model, s.x, s.y, z, s.heading);
    if (!handle)
        return false;

    SetMissionEntity(handle);
    SetInvincible(handle, true);
    SetVisible(handle);
    SetLodDist(handle, kActorLodDist);
    SetNoCollisionWith(handle, ctx.localPed);
    SetNoCollisionWith(handle, ctx.partnerGhost);
    SetNoCollisionWith(handle, ctx.partnerMount);
    SetCanBeTargetted(handle, false);
    SetBlockingOfNonTemporaryEvents(handle, true);
    RandomizeComponents(handle);
    SetCoordsNative(handle, s.x, s.y, z);
    SetHeadingNative(handle, s.heading);

    Proxy p;
    p.actorId = s.actorId;
    p.epoch = g_guestEpoch;
    p.handle = handle;
    p.model = s.model;
    p.tx = s.x; p.ty = s.y; p.tz = z;
    p.heading = s.heading;
    p.moveState = s.moveStyle;
    p.flags = s.flags;
    p.lastFrameTick = ctx.tickCount;
    p.spawnedTick = ctx.tickCount;
    p.lastMoveState = -1;
    p.dead = (s.flags & net::kActorFlagDead) != 0;
    g_handleByActor[handle] = p.actorId;
    g_proxies[p.actorId] = p;
    LogSpawn(p.actorId, p.model, static_cast<unsigned int>(g_proxies.size()));
    return true;
}

void MoveProxy(Proxy& p)
{
    if (!DoesExist(p.handle))
        return;
    if (p.dead)
        return; // труп не двигаем; хост пришлёт despawn

    const Vec3 cur = GetCoords(p.handle);
    const float dx = p.tx - cur.x;
    const float dy = p.ty - cur.y;
    const float dz = p.tz - cur.z;
    const float dist2 = dx * dx + dy * dy;
    const float dist = std::sqrt(dist2);

    if (dist > kTeleportDist || std::fabs(dz) > 8.0f)
    {
        SetCoordsNative(p.handle, p.tx, p.ty, p.tz);
        SetHeadingNative(p.handle, p.heading);
        p.moveTaskValid = false;
        p.lastMoveState = -1;
        return;
    }

    if (p.moveState == 0)
    {
        if (p.lastMoveState != 0)
        {
            SetMoveBlend(p.handle, 0.0f);
            StandStill(p.handle);
            p.lastMoveState = 0;
            p.moveTaskValid = false;
        }
    }
    else
    {
        const bool targetMoved =
            !p.moveTaskValid ||
            std::fabs(p.tx - p.moveTarget.x) > kRetargetDist ||
            std::fabs(p.ty - p.moveTarget.y) > kRetargetDist;
        const bool stateChanged = p.lastMoveState != static_cast<int>(p.moveState);
        if (targetMoved || stateChanged)
        {
            const float speed = (p.moveState >= 3) ? 3.0f
                              : (p.moveState == 2) ? 2.0f : 1.0f;
            SetMoveBlend(p.handle, speed);
            // P3-фикс (task-45): последний арг — heading, не 40000.f.
            // 0.f = без принудительного разворота (heading ведём отдельно).
            FollowNavmesh(p.handle, p.tx, p.ty, p.tz, speed, 0.f);
            p.moveTarget = Vec3{p.tx, p.ty, p.tz};
            p.moveTaskValid = true;
            p.lastMoveState = static_cast<int>(p.moveState);
        }
    }

    float norm = p.heading - GetHeading(p.handle);
    while (norm > 180.f) norm -= 360.f;
    while (norm < -180.f) norm += 360.f;
    if (std::fabs(norm) > kHeadingDeadzone)
        SetHeadingNative(p.handle, p.heading);
}

// P3-фикс (task-45): только ставит акторов в pending; epoch-change и
// удаление старых прокси делает GuestTick ВНЕ g_inboxMutex.
void QueueSpawnBatch(uint32_t epoch,
                     const std::vector<net::ActorSpawn>& actors,
                     unsigned int tick)
{
    for (const net::ActorSpawn& s : actors)
    {
        if (g_proxies.count(s.actorId))
            continue;
        if (g_pendingSpawnIds.count(s.actorId))
            continue;
        if (g_pendingSpawns.size() >=
            static_cast<std::size_t>(net::kMaxActors))
            break;
        PendingSpawn p;
        p.spawn = s;
        p.firstSeenTick = tick;
        g_pendingSpawns.push_back(p);
        g_pendingSpawnIds.insert(s.actorId);
    }
    LogSnapshot(epoch, static_cast<unsigned int>(actors.size()),
                static_cast<unsigned int>(actors.size() *
                                          sizeof(net::ActorSpawn)));
}

// P3-фикс (task-45): смена epoch — удалить старые прокси (вне мьютекса) и
// сбросить кадры, иначе кадры прошлой эпохи продолжают двигать мёртвых.
void HandleGuestEpochChange(uint32_t newEpoch)
{
    if (g_guestEpoch == 0)
    {
        g_guestEpoch = newEpoch;
        return;
    }
    if (newEpoch == g_guestEpoch)
        return;

    DeleteAllProxies();
    g_pendingSpawns.clear();
    g_pendingSpawnIds.clear();
    g_latestFrames.clear();
    g_haveFrames = false;
    g_guestDespawnQueue.clear();
    g_guestEpoch = newEpoch;
}

void GuestResetState()
{
    g_guestEpoch = 0;
    g_pendingSpawns.clear();
    g_pendingSpawnIds.clear();
    g_latestFrames.clear();
    g_haveFrames = false;
    g_guestDespawnQueue.clear();
    g_appliedFrames = 0;
    g_timeouts = 0;
    g_lastNoCollisionTick = 0;
}

void GuestTick(const TickContext& ctx)
{
    const unsigned int tick = ctx.tickCount;
    g_modelRequestsThisTick = 0;

    // 1) inbox: spawn/despawn/frames. Под мьютексом — ТОЛЬКО копирование;
    // нативы (в т.ч. удаление прокси при смене epoch) — после unlock.
    std::vector<net::ActorSpawn> spawns;
    bool haveSpawn = false;
    uint32_t spawnEpoch = 0;
    std::vector<net::ActorFrame> frames;
    bool haveFrames = false;
    uint32_t framesEpoch = 0;
    std::deque<net::ActorDespawnPayload> despawns;
    {
        std::lock_guard<std::mutex> lk(g_inboxMutex);
        if (g_inbox.haveSpawn)
        {
            spawns.swap(g_inbox.spawns);
            spawnEpoch = g_inbox.spawnEpoch;
            g_inbox.haveSpawn = false;
            haveSpawn = true;
        }
        if (g_inbox.haveFrames)
        {
            frames.swap(g_inbox.frames);
            framesEpoch = g_inbox.framesEpoch;
            g_inbox.haveFrames = false;
            haveFrames = true;
        }
        despawns.swap(g_inbox.despawns);
        g_inbox.events = 0;
        g_inbox.outfits = 0;
    }

    // P3-фикс (task-45): epoch-change (DeleteAllProxies) — вне мьютекса.
    if (haveSpawn)
    {
        HandleGuestEpochChange(spawnEpoch);
        QueueSpawnBatch(spawnEpoch, spawns, tick);
    }

    if (haveFrames)
    {
        if (framesEpoch == g_guestEpoch)
        {
            g_latestFrames = frames;
            g_haveFrames = true;
            LogSnapshot(g_guestEpoch,
                        static_cast<unsigned int>(frames.size()),
                        static_cast<unsigned int>(frames.size() *
                                                  sizeof(net::ActorFrame)));
        }
        else
        {
            // Кадры другой эпохи — просим resync (хост перешлёт spawn batch).
            coop::SendActorResync(framesEpoch, 0, 1);
        }
    }

    // 2) despawn: не более 2 удалений за тик; при неудаче DELETE_PED —
    // НЕ теряем учёт, ставим в очередь на повтор (P2-фикс task-45).
    int deleted = 0;
    auto processDespawn = [&](const net::ActorDespawnPayload& d) {
        if (deleted >= kMaxDeletesPerTick)
        {
            if (g_guestDespawnQueue.size() < 32)
                g_guestDespawnQueue.push_back(d);
            return;
        }
        if (d.epoch != 0 && g_guestEpoch != 0 && d.epoch != g_guestEpoch)
            return;
        auto it = g_proxies.find(d.actorId);
        if (it == g_proxies.end())
            return;
        LogDespawn(d.actorId, d.reason);
        if (DeleteProxy(it->second))
        {
            g_proxies.erase(it);
            ++deleted;
        }
        else if (g_guestDespawnQueue.size() < 32)
        {
            g_guestDespawnQueue.push_back(d); // повтор на следующем тике
        }
    };
    while (!despawns.empty() && deleted < kMaxDeletesPerTick)
    {
        processDespawn(despawns.front());
        despawns.pop_front();
    }
    for (const auto& d : despawns)
    {
        if (g_guestDespawnQueue.size() < 32)
            g_guestDespawnQueue.push_back(d);
    }
    {
        int attempts = 0;
        while (!g_guestDespawnQueue.empty() &&
               deleted < kMaxDeletesPerTick && attempts < kMaxDeletesPerTick)
        {
            ++attempts;
            const net::ActorDespawnPayload d = g_guestDespawnQueue.front();
            g_guestDespawnQueue.pop_front();
            processDespawn(d);
        }
    }

    // 3) pending spawns: REQUEST_MODEL -> CREATE_PED (<=2 за тик).
    // P3-фиксы (task-45): cap 12 в CreateProxy (pending не теряем) и таймаут
    // 5 с, если модель так и не загрузилась.
    int spawned = 0;
    const unsigned long long perfSpawnStart = QpcNow();
    for (auto it = g_pendingSpawns.begin();
         it != g_pendingSpawns.end() && spawned < kMaxSpawnsPerTick;)
    {
        const net::ActorSpawn& s = it->spawn;
        if (tick - it->firstSeenTick > kPendingSpawnTimeoutTicks)
        {
            LogSpawnTimeout(s.actorId);
            g_pendingSpawnIds.erase(s.actorId);
            it = g_pendingSpawns.erase(it);
            continue;
        }
        if (!HasModelLoaded(s.model))
        {
            if (g_modelRequestsThisTick < kMaxModelRequestsPerTick)
            {
                RequestModel(s.model);
                ++g_modelRequestsThisTick;
            }
            ++it;
            continue;
        }
        if (CreateProxy(s, ctx))
        {
            g_pendingSpawnIds.erase(s.actorId);
            it = g_pendingSpawns.erase(it);
            ++spawned;
        }
        else
        {
            ++it; // cap ещё занят: pending ждёт слота/таймаута
        }
    }
    g_perfSpawn.AddUs(QpcDeltaUs(perfSpawnStart));

    // 4) кадры -> target позиция/флаги; 5) движение — замеряем p95 apply.
    const unsigned long long perfApplyStart = QpcNow();
    if (g_haveFrames)
    {
        for (const net::ActorFrame& f : g_latestFrames)
        {
            auto it = g_proxies.find(f.actorId);
            if (it == g_proxies.end())
                continue;
            Proxy& p = it->second;
            p.tx = f.x; p.ty = f.y; p.tz = f.z;
            p.heading = f.heading;
            p.moveState = f.animState;
            p.flags = static_cast<uint16_t>(f.flags);
            p.dead = (f.flags & net::kActorFlagDead) != 0;
            p.lastFrameTick = tick;
            p.resyncLogged = false;
            ++g_appliedFrames;
        }
        g_haveFrames = false;
    }

    // 5) движение/поворот каждый тик
    for (auto& kv : g_proxies)
        MoveProxy(kv.second);

    // P3-фикс (task-45): повторяем no-collision раз в ~1 с — partnerGhost
    // мог появиться уже после спавна прокси (в момент спавна он был 0).
    if (tick - g_lastNoCollisionTick >= kNoCollisionRefreshTicks)
    {
        g_lastNoCollisionTick = tick;
        for (auto& kv : g_proxies)
        {
            Proxy& p = kv.second;
            if (!p.handle) continue;
            if (!DoesExist(p.handle)) continue;
            SetNoCollisionWith(p.handle, ctx.localPed);
            SetNoCollisionWith(p.handle, ctx.partnerGhost);
            SetNoCollisionWith(p.handle, ctx.partnerMount);
        }
    }
    g_perfApply.AddUs(QpcDeltaUs(perfApplyStart));

    // 6) lifecycle: 2 с без кадров -> resync; 10 с -> delete. При неудачном
    // DELETE_PED учёт не теряем: повтор через ~1 с (P2-фикс task-45).
    for (auto it = g_proxies.begin(); it != g_proxies.end();)
    {
        Proxy& p = it->second;
        const unsigned int idle = tick - p.lastFrameTick;
        if (idle > kDeleteAfterTicks)
        {
            LogDespawn(p.actorId, kDespawnGone);
            if (DeleteProxy(p))
            {
                ++g_timeouts;
                it = g_proxies.erase(it);
            }
            else
            {
                p.lastFrameTick = tick - kDeleteAfterTicks + kDeleteRetryOffsetTicks;
                ++it;
            }
            continue;
        }
        if (idle > kResyncAfterTicks && !p.resyncLogged)
        {
            p.resyncLogged = true;
            ++g_timeouts;
            coop::SendActorResync(g_guestEpoch, 0, 1);
            sh_log::Write("actor: resync needed id=%u idle=%u\n",
                          p.actorId, idle);
        }
        ++it;
    }

    // 7) сводка (1 строка / 5 с)
    if (tick - g_lastSummaryLogTick >= kSummaryEveryTicks)
    {
        g_lastSummaryLogTick = tick;
        sh_log::Write("actor: proxies=%u appliedFrames=%u timeouts=%u\n",
                      static_cast<unsigned int>(g_proxies.size()),
                      g_appliedFrames, g_timeouts);
        g_appliedFrames = 0;
        g_timeouts = 0;
    }
}

} // namespace

// ======================= публичный API =======================
void Tick(const TickContext& ctx)
{
    // npc_actor=0: ноль нативов и ноль строк actor:. Только чистим inbox.
    if (!ctx.enabled)
    {
        std::lock_guard<std::mutex> lk(g_inboxMutex);
        ClearInboxLocked();
        return;
    }
    if (!ctx.localPed)
        return;

    g_logTick = ctx.tickCount;

    if (ctx.amHost)
    {
        if (!g_proxies.empty())
            DeleteAllProxies();
        GuestResetState();
        HostTick(ctx);
    }
    else
    {
        if (g_hostEpoch != 0 || !g_roster.empty())
            HostResetState();
        GuestTick(ctx);
    }

    // perf: actor scan/spawn/apply p95 — только при npc_debug=1.
    LogPerfIfDue(ctx.tickCount);
}

void Reset()
{
    // При выключенном npc_actor — только очистка состояния, без нативов/логов.
    if (!coopcfg::Get().npcActor)
    {
        {
            std::lock_guard<std::mutex> lk(g_inboxMutex);
            ClearInboxLocked();
        }
        HostResetState();
        GuestResetState();
        return;
    }

    DeleteAllProxies();
    GuestResetState();
    {
        std::lock_guard<std::mutex> lk(g_inboxMutex);
        ClearInboxLocked();
    }
    HostResetState();
    ++g_sessionCounter; // следующая эпоха не совпадёт с прошлой
    sh_log::Write("actor: reset (session=%u)\n", g_sessionCounter);
}

bool GetHandleForActorId(uint32_t actorId, unsigned int& handle)
{
    auto it = g_proxies.find(actorId);
    if (it == g_proxies.end() || !it->second.handle)
    {
        handle = 0;
        return false;
    }
    handle = it->second.handle;
    return true;
}

// ---------------- callbacks net/client (сетевой поток) ----------------
void OnActorSpawnBatch(uint32_t epoch,
                       const std::vector<net::ActorSpawn>& actors)
{
    if (!coopcfg::Get().npcActor) return;
    std::lock_guard<std::mutex> lk(g_inboxMutex);
    g_inbox.haveSpawn = true;
    g_inbox.spawnEpoch = epoch;
    g_inbox.spawns = actors;
}

void OnActorFrames(uint32_t epoch, uint16_t seq,
                   const std::vector<net::ActorFrame>& frames)
{
    if (!coopcfg::Get().npcActor) return;
    std::lock_guard<std::mutex> lk(g_inboxMutex);
    g_inbox.haveFrames = true;
    g_inbox.framesEpoch = epoch;
    g_inbox.framesSeq = seq;
    g_inbox.frames = frames;
}

void OnActorDespawn(const net::ActorDespawnPayload& msg)
{
    if (!coopcfg::Get().npcActor) return;
    std::lock_guard<std::mutex> lk(g_inboxMutex);
    if (g_inbox.despawns.size() < 32)
        g_inbox.despawns.push_back(msg);
}

void OnActorEvent(const net::ActorEventPayload& ev)
{
    if (!coopcfg::Get().npcActor) return;
    std::lock_guard<std::mutex> lk(g_inboxMutex);
    ++g_inbox.events; // E1: события не применяем
    (void)ev;
}

void OnActorOutfit(uint32_t epoch, uint32_t actorId,
                   const net::OutfitState& outfit)
{
    if (!coopcfg::Get().npcActor) return;
    std::lock_guard<std::mutex> lk(g_inboxMutex);
    ++g_inbox.outfits; // E1: outfit актора не применяем
    (void)epoch; (void)actorId; (void)outfit;
}

void OnActorResyncRequest(const net::ActorResyncPayload& req)
{
    if (!coopcfg::Get().npcActor) return;
    std::lock_guard<std::mutex> lk(g_inboxMutex);
    g_inbox.resyncRequested = true;
    g_inbox.resyncEpoch = req.epoch;
    (void)req.lastSeq;
}

} // namespace actor_sync
} // namespace coop
