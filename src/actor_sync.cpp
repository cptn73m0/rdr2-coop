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
#include <unordered_set>
#include <vector>

namespace coop {
namespace actor_sync {

namespace {

// ================= E-1: probe =================
// Раз в ~2 с (120 тиков) на хосте — лог 20 ближайших human NPC в 60 м.
// Только чтение мира; при npc_debug=0 функция не вызывается вовсе.
constexpr int      kProbeMaxPool     = 2048;
constexpr int      kProbeCount       = 20;   // сколько NPC логировать
constexpr float    kProbeRadius      = 60.0f;
constexpr float    kProbeRadius2     = kProbeRadius * kProbeRadius;
constexpr unsigned int kProbeIntervalTicks = 120; // ~2 с при 60 Гц

struct ProbeCandidate
{
    unsigned int handle = 0;
    float distance2 = 0.f;
};

std::unordered_set<unsigned long long> g_probeSeenCombos;
unsigned int g_lastProbeTick = 0;
bool g_probeWorldQueriesWarned = false;

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

bool DoesExist(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::DOES_ENTITY_EXIST);
    Push(handle);
    return Call() != 0;
}

unsigned int GetModel(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::GET_ENTITY_MODEL);
    Push(handle);
    return static_cast<unsigned int>(Call());
}

uint32_t GetMetaOutfitHash(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::GET_PED_META_OUTFIT_HASH);
    Push(ped);
    return static_cast<uint32_t>(Call());
}

bool IsMetaOutfitEquipped(unsigned int ped, uint32_t outfitHash)
{
    if (!outfitHash) return false;
    using namespace natives_api;
    Init(natives::_IS_META_PED_OUTFIT_EQUIPPED);
    Push(ped);
    Push(static_cast<unsigned long long>(outfitHash));
    return Call() != 0;
}

uint32_t GetMountModel(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::GET_MOUNT);
    Push(ped);
    const unsigned int mount = static_cast<unsigned int>(Call());
    if (!mount) return 0;
    return GetModel(mount);
}

uint32_t GetWeaponHash(unsigned int ped)
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

bool IsHuman(unsigned int ped)
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

bool IsMissionPed(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::IS_ENTITY_A_MISSION_ENTITY);
    Push(ped);
    return Call() != 0;
}

bool IsDeadPed(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::IS_PED_DEAD_OR_DYING);
    Push(ped);
    return Call() != 0;
}

// Один проход probe: 20 ближайших human NPC в 60 м. Строка на уникальную
// комбинацию model+metaOutfit (повторные комбинации throttled) + summary.
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
    unsigned int localMount = 0;
    {
        using namespace natives_api;
        Init(natives::GET_MOUNT);
        Push(localPed);
        localMount = static_cast<unsigned int>(Call());
    }

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
        if (d2 > kProbeRadius2)
            continue;

        // Счётчики состава пула — ДО фильтра human (task-38): total/humans/
        // animals по всем peds в радиусе. Дорогих нативов не добавляют:
        // IS_PED_HUMAN для уже полученного массива handles.
        ++totalInRadius;
        if (!IsHuman(handle))
        {
            ++animalsInRadius;
            continue; // top-20 — только human
        }
        ++humansInRadius;
        if (IsDeadPed(handle))
            continue; // top-20 — живые люди (в счётчиках пул учтён)

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
        const unsigned int model = GetModel(c.handle);
        const uint32_t metaOutfit = GetMetaOutfitHash(c.handle);
        const unsigned long long comboKey =
            (static_cast<unsigned long long>(model) << 32) |
            static_cast<unsigned long long>(metaOutfit);
        if (!g_probeSeenCombos.insert(comboKey).second)
            continue; // уже логировали такую комбинацию model+outfit

        const bool equipped = IsMetaOutfitEquipped(c.handle, metaOutfit);
        const uint32_t mountModel = GetMountModel(c.handle);
        const uint32_t weapon = GetWeaponHash(c.handle);
        const bool scenarioAny = IsScenarioActive(c.handle);
        const uint32_t dynScenario = GetDynamicScenario(c.handle);
        const int pointType = GetScenarioPointType(c.handle);
        const int pedType = GetPedType(c.handle);

        sh_log::Write(
            "probe: model=0x%X metaOutfit=0x%X equipped=%d mount=0x%X "
            "weapon=0x%X scenarioAny=%d dynScenario=0x%X pointType=0x%X "
            "pedType=%d human=%d mission=%d\n",
            model, metaOutfit, equipped ? 1 : 0, mountModel, weapon,
            scenarioAny ? 1 : 0, dynScenario,
            static_cast<unsigned int>(pointType), pedType,
            IsHuman(c.handle) ? 1 : 0, IsMissionPed(c.handle) ? 1 : 0);
        ++newCombos;
    }

    sh_log::Write("probe: scan humans=%u animals=%u total=%u logged=%u "
                  "newCombos=%u unique=%u\n",
                  humansInRadius, animalsInRadius, totalInRadius,
                  static_cast<unsigned int>(candidates.size()), newCombos,
                  static_cast<unsigned int>(g_probeSeenCombos.size()));
}

} // namespace

void Tick(unsigned int localPed, unsigned int tickCount, bool amHost,
          bool enabled)
{
    // npc_actor=0: немедленный выход — НИ нативов, НИ логов (как до E0).
    if (!enabled)
        return;
    // Probe — только на хосте и только при npc_debug=1 (E-1).
    if (!amHost)
        return;
    if (!coopcfg::Get().npcDebug)
        return;
    if (!localPed)
        return;
    ProbeTick(localPed, tickCount);
}

void Reset()
{
    g_probeSeenCombos.clear();
    g_lastProbeTick = 0;
    g_probeWorldQueriesWarned = false;
    // E1 TODO: очистить реестр акторов/очереди снапшотов.
}

// ---------------- E1 TODO hooks: пока заглушки ----------------

void OnSnapshot(const std::vector<uint8_t>& payload)
{
    (void)payload; // E1: скопировать в inbox под mutex (без нативов)
}

void ApplyPendingSnapshot(unsigned int tickCount)
{
    (void)tickCount; // E1: apply под бюджетом тика
}

void SendSnapshot(unsigned int localPed, unsigned int tickCount)
{
    (void)localPed;
    (void)tickCount; // E1: сканирование/батчинг/отправка
}

bool GetHandleForActorId(uint32_t actorId, unsigned int& handle)
{
    (void)actorId;
    handle = 0;
    return false; // E1: реестр actorId -> handle
}

} // namespace actor_sync
} // namespace coop
