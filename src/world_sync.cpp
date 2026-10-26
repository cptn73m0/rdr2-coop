#include "world_sync.h"

#include "coop.h"
#include "actor_sync.h"
#include "config.h"
#include "log.h"
#include "native_call.h"
#include "natives.h"
#include "net/protocol.h"
#include "script_hook.h"
#include "world_snapshot_parse.h"
#include "world_entity_budget.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace coop {
namespace world_sync {
namespace {

constexpr float kRadius = 60.0f;             // радиус репликации NPC
constexpr float kRadius2 = kRadius * kRadius;
constexpr unsigned int kSendEveryTicks = 6;  // ~10 Гц при 60 тиках/с
constexpr unsigned int kGuestTimeoutMs = 3000;
constexpr int kSpawnBudgetPerTick = 4;       // не более N новых прокси за тик
constexpr unsigned int kProxyGraceSnapshots = 3;
constexpr unsigned int kHostIdTtlTicks = 300;
constexpr unsigned int kLogEveryTicks = 300;
// Бюджет снапшота: 32 ближайших Ped + 16 Vehicle + 16 mission Object.
constexpr int kMaxPeds = 32;
constexpr int kMaxVehicles = 16;
constexpr int kMaxObjects = 16;
static_assert(kMaxPeds + kMaxVehicles + kMaxObjects == net::kMaxWorldEntities,
              "world entity budget must match protocol");

// ---------------- общие native-хелперы ----------------

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

bool DoesExist(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::DOES_ENTITY_EXIST);
    Push(handle);
    return Call() != 0;
}

bool IsMissionEntity(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::IS_ENTITY_A_MISSION_ENTITY);
    Push(handle);
    return Call() != 0;
}

bool IsDeadPed(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::IS_PED_DEAD_OR_DYING);
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
    Push(false); // p1: 2-й аргумент обязателен (иначе движок читает мусор)
    Call();
}

void SetCoords(unsigned int handle, float x, float y, float z)
{
    using namespace natives_api;
    Init(natives::SET_ENTITY_COORDS);
    Push(handle);
    Push(x); Push(y); Push(z);
    Push(false); Push(false); Push(false);
    Push(false); // clearTasks=false: не рвём задачи анимации
    Call();
}

void SetHeading(unsigned int handle, float heading)
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
    Push(handle); Push(true); Push(true);
    Call();
}

void SetVisible(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::SET_ENTITY_VISIBLE);
    Push(handle); Push(true);
    Call();
}

void SetEntityVisible(unsigned int handle, bool visible)
{
    if (!handle || !DoesExist(handle))
        return;
    using namespace natives_api;
    Init(natives::SET_ENTITY_VISIBLE);
    Push(handle);
    Push(visible ? static_cast<unsigned long long>(1)
                 : static_cast<unsigned long long>(0));
    Call();
}

// E3d-c: не даём движку авто-создавать/отцеплять/ломать draft-лошадей у
// гостевого mission-vehicle; attach делаем руками через harness native.
void ConfigureDraftVehicleProtection(unsigned int veh)
{
    using namespace natives_api;
    Init(natives::IS_DRAFT_VEHICLE);
    Push(veh);
    if (Call() == 0)
        return;
    Init(natives::_SET_DRAFT_VEHICLE_ALLOW_DRAFT_ANIMAL_AUTO_CREATION);
    Push(veh); Push(false); Call();
    Init(natives::_SET_DRAFT_VEHICLE_ANIMALS_CAN_DETACH);
    Push(veh); Push(false); Call();
    Init(natives::_SET_DRAFT_VEHICLE_YOKE_CAN_BREAK);
    Push(veh); Push(false); Call();
}

void SetInvincible(unsigned int handle, bool value)
{
    using namespace natives_api;
    Init(natives::SET_ENTITY_INVINCIBLE);
    Push(handle);
    Push(static_cast<unsigned long long>(value ? 1 : 0));
    Call();
}

void SetNoCollision(unsigned int handle, bool value)
{
    using namespace natives_api;
    Init(natives::SET_ENTITY_COLLISION);
    Push(handle);
    Push(value ? 0 : 1); // SET_ENTITY_COLLISION(entity, toggle, keepPhysics)
    Push(false);
    Call();
}

void StandStill(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::TASK_STAND_STILL);
    Push(handle);
    Push(static_cast<unsigned long long>(0xFFFFFFFF));
    Call();
}

void SetMoveBlend(unsigned int handle, float value)
{
    using namespace natives_api;
    Init(natives::SET_PED_MAX_MOVE_BLEND_RATIO);
    Push(handle);
    Push(value);
    Call();
}

void FollowNavmesh(unsigned int handle, float x, float y, float z,
                   float speedMultiplier)
{
    using namespace natives_api;
    Init(natives::TASK_FOLLOW_NAV_MESH_TO_COORD);
    Push(handle);
    Push(x); Push(y); Push(z);
    Push(speedMultiplier);
    Push(static_cast<unsigned long long>(0xFFFFFFFF)); // timeout -1
    Push(0.5f);                                        // stoppingRange
    Push(static_cast<unsigned long long>(4));          // flags
    Push(40000.f);
    Call();
}

void RandomizeComponents(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::SET_PED_RANDOM_COMPONENT_VARIATION);
    Push(handle);
    Push(static_cast<unsigned long long>(0));
    Call();
}

void DeleteEntity(unsigned int handle)
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

void DeleteVehicleEntity(unsigned int handle)
{
    using namespace natives_api;
    int h = static_cast<int>(handle);
    Init(natives::DELETE_VEHICLE);
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

void DeleteObjectEntity(unsigned int handle)
{
    using namespace natives_api;
    int h = static_cast<int>(handle);
    Init(natives::DELETE_OBJECT);
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

void SetHealthDead(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::SET_ENTITY_HEALTH);
    Push(handle);
    Push(static_cast<unsigned long long>(0));
    Push(static_cast<unsigned long long>(0));
    Call();
}

// ---------------- хост: сканирование и снапшоты ----------------

struct HostIdState
{
    uint64_t netId = 0;
    unsigned int model = 0;
    unsigned int lastSeenTick = 0;
};

struct Candidate
{
    net::WorldEntity entity{};
    float distance2 = 0.f;
    bool mission = false;
};

std::unordered_map<unsigned int, HostIdState> g_hostIds;
uint64_t g_nextNetId = 1;
unsigned int g_lastHostScanTick = 0;
unsigned int g_hostSnapshotCount = 0;
bool g_missingWorldQueriesLogged = false;

// ================= E3d-c (v19): host occupants =================
// Последние отправленные occupants по vehicleNetId + время (heartbeat 1 с).
std::map<uint32_t, std::vector<net::VehicleOccupantEntry>> g_lastOccupantsSent;
std::map<uint32_t, unsigned int> g_lastOccupantsSendTick;
// Сервер релеит <=20/с: держим общий лимит отправок 0x21.
unsigned int g_occupantsSendWindowTick = 0;
unsigned int g_occupantsSendWindowCount = 0;

bool FindHostHandleByNetId(uint32_t netId, unsigned int& handle)
{
    for (const auto& kv : g_hostIds)
    {
        if (kv.second.netId == netId && kv.first && DoesExist(kv.first))
        {
            handle = kv.first;
            return true;
        }
    }
    return false;
}

void AppendOccupantEntry(std::vector<net::VehicleOccupantEntry>& out,
                         uint32_t actorId, int seat, uint8_t flags,
                         uint8_t harness)
{
    if (!actorId || out.size() >= static_cast<std::size_t>(net::kMaxVehicleOccupants))
        return;
    net::VehicleOccupantEntry e{};
    e.actorId = actorId;
    e.seat = static_cast<int8_t>(seat);
    e.flags = flags;
    e.carIndex = 0;
    e.harness = harness;
    out.push_back(e);
}

bool OccupantsEqual(const std::vector<net::VehicleOccupantEntry>& a,
                    const std::vector<net::VehicleOccupantEntry>& b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
    {
        const net::VehicleOccupantEntry& x = a[i];
        const net::VehicleOccupantEntry& y = b[i];
        if (x.actorId != y.actorId || x.seat != y.seat ||
            x.flags != y.flags || x.carIndex != y.carIndex ||
            x.harness != y.harness)
            return false;
    }
    return true;
}

// Собрать occupant-entries для vehicle: driver seat=-1, passengers 0..max-1,
// draft-лошади harness 0..5. Возвращает handles (для actor discovery).
std::vector<unsigned int> CollectVehicleOccupants(
    unsigned int vehicle, unsigned int localPlayerPed, uint32_t netId,
    std::vector<net::VehicleOccupantEntry>& out, uint32_t& driverActorId)
{
    using namespace natives_api;
    driverActorId = 0;
    std::vector<unsigned int> handles;
    int attempted = 0;
    auto addPed = [&](unsigned int ped, int seat, uint8_t flags,
                      uint8_t harness) {
        if (!ped || ped == localPlayerPed)
            return;
        ++attempted;
        handles.push_back(ped);
        uint32_t aid = 0;
        if (!actor_sync::GetHostActorIdForHandle(ped, aid))
            return; // roster ещё не содержит — повторим в следующем скане
        if (flags & net::kOccupantFlagDriver)
            driverActorId = aid;
        AppendOccupantEntry(out, aid, seat, flags, harness);
    };

    Init(natives::GET_DRIVER_OF_VEHICLE);
    Push(vehicle);
    const unsigned int driver = static_cast<unsigned int>(Call());
    addPed(driver, -1, net::kOccupantFlagDriver, 0);

    Init(natives::GET_VEHICLE_MAX_NUMBER_OF_PASSENGERS);
    Push(vehicle);
    int maxPass = static_cast<int>(Call());
    if (maxPass < 0) maxPass = 0;
    if (maxPass > net::kMaxVehicleSeat + 1) maxPass = net::kMaxVehicleSeat + 1;
    for (int seat = 0; seat < maxPass; ++seat)
    {
        Init(natives::GET_PED_IN_VEHICLE_SEAT);
        Push(vehicle);
        Push(static_cast<unsigned long long>(seat));
        const unsigned int ped = static_cast<unsigned int>(Call());
        if (ped == driver)
            continue;
        addPed(ped, seat, net::kOccupantFlagPassenger, 0);
    }

    Init(natives::IS_DRAFT_VEHICLE);
    Push(vehicle);
    if (Call() != 0)
    {
        for (unsigned int h = 0; h <= static_cast<unsigned int>(net::kMaxDraftHarness);
             ++h)
        {
            Init(natives::_GET_PED_IN_DRAFT_HARNESS);
            Push(vehicle);
            Push(static_cast<unsigned long long>(h));
            const unsigned int horse = static_cast<unsigned int>(Call());
            addPed(horse, -1, net::kOccupantFlagDraftHorse,
                   static_cast<uint8_t>(h));
        }
    }
    (void)netId;
    // task-143 P2.6: протокол cap 8; приоритет driver+passengers, затем draft.
    if (attempted > net::kMaxVehicleOccupants)
    {
        static unsigned long long lastTruncLogMs = 0;
        const unsigned long long nowMs = GetTickCount64();
        if (coopcfg::Get().npcDebug &&
            (lastTruncLogMs == 0 || nowMs - lastTruncLogMs > 1000ull))
        {
            lastTruncLogMs = nowMs;
            sh_log::Write("world: occupants truncated n=%u total=%u\n",
                          static_cast<unsigned int>(net::kMaxVehicleOccupants),
                          static_cast<unsigned int>(attempted));
        }
    }
    return handles;
}

// ================= E3d-b (v19): mission-train replication =================
// 87 config-хэшей из third_party/rdr2-hashdb/TrainConfigs.json (signed int32
// -> uint32); перебор с бюджетом нативов/тик и кэшем handle->config.
static const uint32_t kTrainConfigHashes[] = {
    0x005E03ADu,
    0x0392C83Au,
    0x0660E567u,
    0x0941ADB7u,
    0x09B679D6u,
    0x0CCC2F70u,
    0x0D03C58Du,
    0x0E62D710u,
    0x10461E19u,
    0x124A1F89u,
    0x19A0A288u,
    0x1C043595u,
    0x1C9936BBu,
    0x1EEC5C2Au,
    0x1EF82A51u,
    0x25E5D8FFu,
    0x26509FBBu,
    0x29C81ACBu,
    0x2D1A6F0Cu,
    0x2D3645FAu,
    0x31656D23u,
    0x3260CE89u,
    0x35D17C43u,
    0x3ADC4DA9u,
    0x3D72571Du,
    0x3EDA466Du,
    0x41436136u,
    0x487B2BE7u,
    0x4A73E49Cu,
    0x4C9CCB22u,
    0x515E31EDu,
    0x57C209C4u,
    0x592A5CD0u,
    0x5AA369CAu,
    0x5D9928A4u,
    0x68CF495Fu,
    0x6CC26E27u,
    0x6D69A954u,
    0x73722125u,
    0x761CE0ADu,
    0x767DEB32u,
    0x7BD58C4Du,
    0x8864D73Au,
    0x8D0766BCu,
    0x8EAC625Cu,
    0x90CB53CAu,
    0x9296570Eu,
    0x96563327u,
    0x98427740u,
    0x9897FF51u,
    0x998A0CBCu,
    0x9CBE6FECu,
    0x9E096E46u,
    0xA3BF0BEBu,
    0xA8B1CEB7u,
    0xA91041A2u,
    0xAA3E691Eu,
    0xAC18A9F4u,
    0xAE47CA77u,
    0xAEE0ECF5u,
    0xB1F69614u,
    0xBF69518Fu,
    0xC1F1DD80u,
    0xC732CDC8u,
    0xC75AA08Cu,
    0xCA19C62Au,
    0xCD2C7CA1u,
    0xD233B18Du,
    0xD42DD3EEu,
    0xD5DF2D82u,
    0xD8CF6395u,
    0xD92B16AEu,
    0xD93C36C2u,
    0xDA2EDE2Fu,
    0xDC9DD041u,
    0xDD920DAFu,
    0xE0898B89u,
    0xE16CA3EFu,
    0xEB8B2439u,
    0xEF9FC71Du,
    0xEFBFBDD8u,
    0xF19E48CAu,
    0xF6AA98F4u,
    0xF9B038FCu,
    0xFAB2FFB9u,
    0xFAC328F0u,
    0xFD8810E8u
};
constexpr unsigned int kTrainConfigCount =
    static_cast<unsigned int>(sizeof(kTrainConfigHashes) / sizeof(uint32_t));
constexpr unsigned int kTrainSendEveryTicks = 15; // <=4/с
constexpr float kTrainSearchRadius = 250.0f;
constexpr float kTrainSearchRadius2 = kTrainSearchRadius * kTrainSearchRadius;
constexpr unsigned int kTrainSnapshotTimeoutTicks = 120; // 2 c
// task-143 P1: throttle скана поездов/кэш engine и model->IsTrainModel.
constexpr unsigned int kTrainScanEveryTicks = 6;   // <=10 Гц
constexpr unsigned int kTrainEngineCacheTicks = 120; // 2 c
// Vehicle interpolation: time-based lerp + extrapolation <=150 мс.
constexpr unsigned long long kVehicleExtrapMaxMs = 150ull;
constexpr unsigned long long kVehicleHoldMs = 300ull;
std::unordered_map<unsigned int, bool> g_modelIsTrainCache;

bool TrainsEnabled()
{
    // Активен только в E3d-a-режиме: npc_actor=1 && world_vehicles=1, и
    // world_trains=1. Полный world_sync (=1) оставляем старому R&D-пути.
    return coopcfg::Get().npcActor && coopcfg::Get().worldVehicles &&
           coopcfg::Get().worldTrains && !coopcfg::Get().worldSync;
}

bool IsTrainModel(unsigned int model)
{
    auto it = g_modelIsTrainCache.find(model);
    if (it != g_modelIsTrainCache.end())
        return it->second;
    using namespace natives_api;
    Init(natives::IS_THIS_MODEL_A_TRAIN);
    Push(static_cast<unsigned long long>(model));
    const bool train = Call() != 0;
    g_modelIsTrainCache[model] = train;
    return train;
}

// --- host: config match / snapshot ---
struct TrainScanState
{
    unsigned int handle = 0;
    std::size_t nextConfig = 0;
    uint32_t found = 0;
    bool done = false;
    bool ambiguous = false;
    unsigned int lastLogTick = 0;
};
std::map<unsigned int, TrainScanState> g_trainScans;
std::map<unsigned int, uint32_t> g_trainConfigs; // cache handle -> config
std::map<unsigned int, uint32_t> g_trainIds;     // handle -> stable trainId
std::map<unsigned int, uint8_t> g_trainConfigCars; // cache signature: carCount
std::map<unsigned int, uint32_t> g_trainConfigModel0; // engine model
std::map<unsigned int, unsigned int> g_trainLastSeen;
unsigned int g_trainScanTick = 0;
unsigned int g_trainEngine = 0;
unsigned int g_trainEngineExpiry = 0;
uint32_t g_nextTrainId = 1;
uint32_t g_trainEpoch = 0;
std::map<uint32_t, net::TrainSnapshotV2Payload> g_lastTrainSnapshot;
std::map<uint32_t, unsigned int> g_lastTrainSendTick;

uint32_t EnsureTrainId(unsigned int engine)
{
    auto it = g_trainIds.find(engine);
    if (it != g_trainIds.end())
        return it->second;
    const uint32_t id = ++g_nextTrainId;
    g_trainIds[engine] = id;
    return id;
}

// Собрать реальные модели состава: engine первым, затем вагоны по порядку.
int CollectActualTrainModels(unsigned int engine, uint32_t outModels[16],
                             int maxCars)
{
    int count = 0;
    if (!engine || !DoesExist(engine))
        return 0;
    outModels[count++] = GetModel(engine);
    using namespace natives_api;
    Init(natives::_GET_TRAIN_CARRIAGE_TRAILER_NUMBER);
    Push(engine);
    const int trailers = static_cast<int>(Call());
    std::unordered_set<unsigned int> seen;
    seen.insert(engine);
    for (int i = 0; i <= trailers && count < maxCars; ++i)
    {
        Init(natives::GET_TRAIN_CARRIAGE);
        Push(engine);
        Push(static_cast<unsigned long long>(i));
        const unsigned int car = static_cast<unsigned int>(Call());
        if (!car || !DoesExist(car) || seen.count(car))
            continue;
        seen.insert(car);
        outModels[count++] = GetModel(car);
    }
    return count;
}

// Перебор конфигов с бюджетом 3-6 конфигов/тик и кэшем.
uint32_t ResolveTrainConfig(unsigned int engine, unsigned int tickCount,
                            bool& outUnknown)
{
    outUnknown = false;
    auto cached = g_trainConfigs.find(engine);
    if (cached != g_trainConfigs.end())
        return cached->second;

    TrainScanState& st = g_trainScans[engine];
    if (st.handle != engine)
    {
        st = TrainScanState{};
        st.handle = engine;
    }
    if (st.done)
    {
        if (st.found && !st.ambiguous)
            return st.found;
        outUnknown = true;
        return 0;
    }

    uint32_t actual[16]{};
    const int cars = CollectActualTrainModels(engine, actual, 16);
    if (cars <= 0)
    {
        st.done = true;
        outUnknown = true;
        return 0;
    }

    int configBudget = 5;   // 3-6 конфигов/тик
    int modelBudget = 80;   // защита от дорогих carCount
    using namespace natives_api;
    while (st.nextConfig < kTrainConfigCount && configBudget > 0 &&
           modelBudget > 0)
    {
        const uint32_t cfg = kTrainConfigHashes[st.nextConfig++];
        --configBudget;
        Init(natives::_GET_NUM_CARS_FROM_TRAIN_CONFIG);
        Push(static_cast<unsigned long long>(cfg));
        const int cfgCars = static_cast<int>(Call());
        if (cfgCars <= 0)
            continue;
        if (cfgCars != cars)
            continue;
        bool forward = true;
        bool reverse = true;
        for (int i = 0; i < cars; ++i)
        {
            if (forward || reverse)
            {
                Init(natives::_GET_TRAIN_MODEL_FROM_TRAIN_CONFIG_BY_CAR_INDEX);
                Push(static_cast<unsigned long long>(cfg));
                Push(static_cast<unsigned long long>(i));
                const uint32_t want = static_cast<uint32_t>(Call());
                --modelBudget;
                if (want != actual[i])
                    forward = false;
                if (want != actual[cars - 1 - i])
                    reverse = false;
            }
        }
        if (forward || reverse)
        {
            if (st.found == 0)
                st.found = cfg;
            else if (st.found != cfg)
                st.ambiguous = true;
        }
    }

    if (st.nextConfig >= kTrainConfigCount)
    {
        st.done = true;
        if (st.found && !st.ambiguous)
        {
            const uint32_t found = st.found;
            g_trainConfigs[engine] = found;
            g_trainConfigCars[engine] = static_cast<uint8_t>(
                cars > 255 ? 255 : cars);
            g_trainConfigModel0[engine] = actual[0];
            g_trainScans.erase(engine);
            return found;
        }
        outUnknown = true;
        if (st.lastLogTick == 0 || tickCount - st.lastLogTick >= 600)
        {
            st.lastLogTick = tickCount;
            sh_log::Write("world: train config skip engine=%u reason=%s\n",
                          engine, st.ambiguous ? "ambiguous" : "config_unknown");
        }
        return 0;
    }
    return 0; // ещё перебираем (следующие тики)
}

bool FindNearestTrain(unsigned int localPlayerPed, float partnerX,
                      float partnerY, bool partnerValid,
                      unsigned int& outEngine, float& outDist)
{
    static int handles[1024];
    int count = sh::worldGetAllVehicles(handles, 1024);
    if (count < 0) count = 0;
    if (count > 1024) count = 1024;
    const Vec3 origin = GetCoords(localPlayerPed);
    float best = kTrainSearchRadius2;
    int bestTrailers = -1;
    bool found = false;
    using namespace natives_api;
    for (int i = 0; i < count; ++i)
    {
        const unsigned int h = static_cast<unsigned int>(handles[i]);
        if (!h || !DoesExist(h))
            continue;
        const unsigned int model = GetModel(h);
        if (!model || !IsTrainModel(model))
            continue;
        // task-143 P1.1: engine — только вагон с trailerCount>0 (машины/
        // отдельные вагоны/random train-vehicles отбрасываем).
        Init(natives::_GET_TRAIN_CARRIAGE_TRAILER_NUMBER);
        Push(h);
        const int trailers = static_cast<int>(Call());
        if (trailers <= 0)
            continue;
        const Vec3 p = GetCoords(h);
        const float dx = p.x - origin.x;
        const float dy = p.y - origin.y;
        float d2 = dx * dx + dy * dy;
        if (partnerValid)
        {
            const float px = p.x - partnerX;
            const float py = p.y - partnerY;
            const float p2 = px * px + py * py;
            if (p2 < d2) d2 = p2;
        }
        // engine = максимум trailerCount (консистентность), затем ближайший.
        if (trailers > bestTrailers ||
            (trailers == bestTrailers && d2 < best))
        {
            bestTrailers = trailers;
            best = d2;
            outEngine = h;
            found = true;
        }
    }
    outDist = found ? std::sqrt(best) : 0.f;
    return found;
}

bool SameTrainSnapshot(const net::TrainSnapshotV2Payload& a,
                       const net::TrainSnapshotV2Payload& b)
{
    return a.epoch == b.epoch && a.trainId == b.trainId &&
           a.configHash == b.configHash && a.flags == b.flags &&
           a.stationIndex == b.stationIndex && a.carCount == b.carCount &&
           a.speed == b.speed && a.x == b.x && a.y == b.y && a.z == b.z &&
           a.heading == b.heading && a.trackIndex == b.trackIndex;
}

void TickHostTrains(unsigned int localPlayerPed, unsigned int tickCount,
                    float partnerX, float partnerY, bool partnerValid)
{
    if (!TrainsEnabled())
        return;
    // task-143 P1.3: тяжёлый скан worldGetAllVehicles — не чаще 1/6 тиков.
    if (g_trainScanTick != 0 &&
        tickCount - g_trainScanTick < kTrainScanEveryTicks)
        return;
    g_trainScanTick = tickCount;

    unsigned int engine = 0;
    float dist = 0.f;
    bool reuse = g_trainEngine != 0 && DoesExist(g_trainEngine) &&
                 tickCount < g_trainEngineExpiry &&
                 IsTrainModel(GetModel(g_trainEngine));
    if (reuse)
    {
        engine = g_trainEngine;
    }
    else
    {
        if (!FindNearestTrain(localPlayerPed, partnerX, partnerY, partnerValid,
                              engine, dist))
        {
            g_trainEngine = 0;
            g_trainEngineExpiry = 0;
            return;
        }
        g_trainEngine = engine;
        g_trainEngineExpiry = tickCount + kTrainEngineCacheTicks;
    }
    g_trainLastSeen[engine] = tickCount;

    // Периодическая чистка кэшей исчезнувших поездов (handle TTL).
    if ((tickCount % 300) == 2)
    {
        for (auto it = g_trainLastSeen.begin(); it != g_trainLastSeen.end();)
        {
            if (!DoesExist(it->first) || tickCount - it->second > 600)
            {
                const unsigned int h = it->first;
                auto idIt = g_trainIds.find(h);
                if (idIt != g_trainIds.end())
                {
                    g_lastTrainSnapshot.erase(idIt->second);
                    g_lastTrainSendTick.erase(idIt->second);
                }
                g_trainConfigs.erase(h);
                g_trainConfigCars.erase(h);
                g_trainConfigModel0.erase(h);
                g_trainScans.erase(h);
                g_trainIds.erase(h);
                it = g_trainLastSeen.erase(it);
            }
            else
            {
                ++it;
            }
        }
        for (auto it = g_lastTrainSendTick.begin();
             it != g_lastTrainSendTick.end();)
        {
            bool alive = false;
            for (const auto& kv : g_trainIds)
                if (kv.second == it->first) { alive = true; break; }
            if (!alive || tickCount - it->second > 900)
                it = g_lastTrainSendTick.erase(it);
            else
                ++it;
        }
    }

    const uint32_t modelNow = GetModel(engine);
    bool unknown = false;
    const uint32_t cfg = ResolveTrainConfig(engine, tickCount, unknown);
    if (!cfg)
        return;

    net::TrainSnapshotV2Payload snap{};
    if (!g_trainEpoch)
        g_trainEpoch = (static_cast<uint32_t>(GetTickCount64()) & 0x7FFFFFFFu) | 1u;
    snap.epoch = g_trainEpoch;
    snap.trainId = EnsureTrainId(engine);
    snap.configHash = cfg;
    using namespace natives_api;
    Init(natives::_GET_TRAIN_CARRIAGE_TRAILER_NUMBER);
    Push(engine);
    int cars = static_cast<int>(Call()) + 1;
    if (cars < 1) cars = 1;
    if (cars > net::kMaxTrainCars) cars = net::kMaxTrainCars;
    // task-143 P1.10: кэш конфига валиден только для (model, carCount).
    auto cc = g_trainConfigCars.find(engine);
    auto cm = g_trainConfigModel0.find(engine);
    if (cc != g_trainConfigCars.end() &&
        (cc->second != static_cast<uint8_t>(cars) ||
         cm == g_trainConfigModel0.end() || cm->second != modelNow))
    {
        g_trainConfigs.erase(engine);
        g_trainConfigCars.erase(engine);
        g_trainConfigModel0.erase(engine);
        g_trainScans.erase(engine);
        return; // пересоберём конфиг на следующем скане
    }
    snap.carCount = static_cast<uint8_t>(cars);
    snap.speed = GetSpeed(engine);
    if (snap.speed < net::kMinTrainSpeed) snap.speed = net::kMinTrainSpeed;
    if (snap.speed > net::kMaxTrainSpeed) snap.speed = net::kMaxTrainSpeed;
    const Vec3 p = GetCoords(engine);
    snap.x = p.x;
    snap.y = p.y;
    snap.z = p.z;
    snap.heading = GetHeading(engine);

    Init(natives::_GET_TRAIN_DIRECTION);
    Push(engine);
    const bool direction = Call() != 0;
    const bool waiting = [&]() {
        Init(natives::IS_TRAIN_WAITING_AT_STATION);
        Push(engine);
        return Call() != 0;
    }();
    if (waiting)
        snap.flags |= net::kTrainFlagWaitingAtStation;
    if (std::fabs(snap.speed) < 0.1f)
        snap.flags |= net::kTrainFlagHalted;
    if (!direction)
        snap.flags |= net::kTrainFlagReversing;

    Init(natives::GET_CURRENT_STATION_FOR_TRAIN);
    Push(engine);
    const int station = static_cast<int>(Call());
    snap.stationIndex = static_cast<uint8_t>(
        station < 0 ? 0 : (station > 255 ? 255 : station));
    Init(natives::_GET_CURRENT_TRACK_FOR_TRAIN);
    Push(engine);
    snap.trackIndex = static_cast<uint32_t>(Call());

    const unsigned int localVeh = [&]() {
        Init(natives::GET_VEHICLE_PED_IS_IN);
        Push(localPlayerPed);
        Push(false);
        return static_cast<unsigned int>(Call());
    }();
    if (localVeh == engine)
        snap.flags |= net::kTrainFlagPlayerAboard;

    const auto last = g_lastTrainSnapshot.find(snap.trainId);
    const bool changed = last == g_lastTrainSnapshot.end() ||
                         !SameTrainSnapshot(last->second, snap);
    const unsigned int lastTick = g_lastTrainSendTick[snap.trainId];
    // 0x22 <=4/с: не чаще kTrainSendEveryTicks (даже при changed).
    if (lastTick != 0 && tickCount - lastTick < kTrainSendEveryTicks)
        return;
    (void)changed;
    coop::SendTrainSnapshotV2(snap);
    g_lastTrainSnapshot[snap.trainId] = snap;
    g_lastTrainSendTick[snap.trainId] = tickCount;
    if (coopcfg::Get().npcDebug)
        sh_log::Write(
            "world: train send config=0x%X cars=%u speed=%.1f state=%X\n",
            cfg, static_cast<unsigned int>(snap.carCount), snap.speed,
            static_cast<unsigned int>(snap.flags));
}

// --- guest: mission train ---
std::mutex g_trainV2Mutex;
net::TrainSnapshotV2Payload g_pendingTrainV2{};
bool g_hasPendingTrainV2 = false;
net::TrainSnapshotV2Payload g_guestTrainSnap{};
bool g_guestTrainSnapValid = false;
unsigned int g_guestTrainLastSnapTick = 0;
unsigned int g_guestTrain = 0;
uint32_t g_guestTrainConfig = 0;
uint8_t g_guestTrainCars = 0;
bool g_guestTrainWaiting = false;
bool g_guestTrainReverse = false;
bool g_guestTrainRandomDisabled = false;
unsigned int g_guestTrainLastSoftSync = 0;
unsigned int g_guestTrainSpawnLogTick = 0;
// task-143 P2.4: backoff создания mission-train (1/с, max 5).
unsigned int g_guestTrainSpawnAttempts = 0;
unsigned int g_guestTrainLastSpawnTick = 0;
uint32_t g_guestTrainRetryConfig = 0;
uint32_t g_guestTrainRetryTrainId = 0;

void DeleteGuestMissionTrain(unsigned int reason)
{
    if (g_guestTrain)
    {
        unsigned int train = g_guestTrain;
        int h = static_cast<int>(train);
        using namespace natives_api;
        Init(natives::SET_MISSION_TRAIN_AS_NO_LONGER_NEEDED);
        Push(reinterpret_cast<unsigned long long>(&h));
        Push(static_cast<unsigned long long>(0));
        Call();
        h = static_cast<int>(train);
        Init(natives::DELETE_MISSION_TRAIN);
        Push(reinterpret_cast<unsigned long long>(&h));
        Call();
        if (coopcfg::Get().npcDebug)
            sh_log::Write("world: train despawn reason=%u\n", reason);
    }
    g_guestTrain = 0;
    g_guestTrainConfig = 0;
    g_guestTrainCars = 0;
    g_guestTrainWaiting = false;
    g_guestTrainReverse = false;
}

void CreateGuestTrain(unsigned int tickCount)
{
    if (!g_guestTrainSnapValid)
        return;
    if (g_guestTrainSpawnAttempts >= 5)
        return;
    if (g_guestTrainLastSpawnTick != 0 &&
        tickCount - g_guestTrainLastSpawnTick < 60)
        return;
    const net::TrainSnapshotV2Payload& snap = g_guestTrainSnap;
    if (!snap.configHash)
        return;
    bool direction = (snap.flags & net::kTrainFlagReversing) == 0;
    float x = snap.x;
    float y = snap.y;
    float z = snap.z;
    bool trackFallback = false;
    using namespace natives_api;
    Init(natives::_IS_POSITION_VALID_FOR_TRAIN);
    Push(static_cast<unsigned long long>(snap.configHash));
    Push(x); Push(y); Push(z);
    Push(direction ? static_cast<unsigned long long>(1)
                   : static_cast<unsigned long long>(0));
    Push(false);
    const bool valid = Call() != 0;
    if (!valid)
    {
        Vec3 np{0.f, 0.f, 0.f};
        Init(natives::_GET_NEAREST_TRAIN_TRACK_POSITION);
        Push(x); Push(y); Push(z);
        CallVec3(np);
        if (np.x != 0.f || np.y != 0.f)
        {
            x = np.x;
            y = np.y;
            z = np.z;
            trackFallback = true;
        }
    }
    Init(natives::_CREATE_MISSION_TRAIN);
    Push(static_cast<unsigned long long>(snap.configHash));
    Push(x); Push(y); Push(z);
    Push(direction ? static_cast<unsigned long long>(1)
                   : static_cast<unsigned long long>(0));
    Push(false); // passengers
    Push(false); // p6
    Push(true);  // conductor
    const unsigned int train = static_cast<unsigned int>(Call());
    if (!train)
    {
        ++g_guestTrainSpawnAttempts;
        g_guestTrainLastSpawnTick = tickCount;
        g_guestTrainRetryConfig = snap.configHash;
        g_guestTrainRetryTrainId = snap.trainId;
        if (coopcfg::Get().npcDebug &&
            (g_guestTrainSpawnLogTick == 0 ||
             tickCount - g_guestTrainSpawnLogTick >= 60))
        {
            g_guestTrainSpawnLogTick = tickCount;
            sh_log::Write("world: train spawn config=0x%X ok=0 attempt=%u\n",
                          snap.configHash, g_guestTrainSpawnAttempts);
        }
        if (g_guestTrainSpawnAttempts >= 5 && coopcfg::Get().npcDebug)
            sh_log::Write("world: train spawn skip config=0x%X reason=retry_limit\n",
                          snap.configHash);
        return;
    }
    g_guestTrainSpawnAttempts = 0;
    g_guestTrainLastSpawnTick = 0;
    g_guestTrain = train;
    g_guestTrainConfig = snap.configHash;
    g_guestTrainCars = snap.carCount;
    g_guestTrainWaiting = false;
    g_guestTrainReverse = false;
    Init(natives::_SET_TRAIN_MAX_SPEED);
    Push(train); Push(30.0f); Call();
    Init(natives::_SET_TRAIN_COLLISION_AVOIDANCE_ENABLED);
    Push(train); Push(false); Call();
    Init(natives::_SET_TRAIN_STOPS_FOR_STATIONS);
    Push(train); Push(false); Call();
    Init(natives::_SET_TRAIN_DESTRUCTION_ENABLED);
    Push(train); Push(false); Call();

    // Визуальное население вагонов (точные пассажиры — E3d-c P1).
    Init(natives::_GET_TRAIN_CARRIAGE_TRAILER_NUMBER);
    Push(train);
    const int trailers = static_cast<int>(Call());
    for (int i = 0; i <= trailers; ++i)
    {
        Init(natives::GET_TRAIN_CARRIAGE);
        Push(train);
        Push(static_cast<unsigned long long>(i));
        const unsigned int car = static_cast<unsigned int>(Call());
        if (!car || car == train)
            continue;
        Init(natives::_FORCE_TRAIN_WAGON_POPULATION);
        Push(car); Push(true); Call();
    }
    if (coopcfg::Get().npcDebug)
        sh_log::Write("world: train spawn config=0x%X ok=1 fallback=%d\n",
                      snap.configHash, trackFallback ? 1 : 0);
}

void TickGuestTrains(unsigned int tickCount)
{
    if (!TrainsEnabled())
    {
        if (g_guestTrainRandomDisabled)
        {
            using namespace natives_api;
            Init(natives::SET_RANDOM_TRAINS);
            Push(true);
            Call();
            g_guestTrainRandomDisabled = false;
        }
        DeleteGuestMissionTrain(5);
        g_guestTrainSnapValid = false;
        g_guestTrainLastSnapTick = 0;
        return;
    }

    {
        std::lock_guard<std::mutex> lk(g_trainV2Mutex);
        if (g_hasPendingTrainV2)
        {
            if (g_pendingTrainV2.configHash != g_guestTrainRetryConfig ||
                g_pendingTrainV2.trainId != g_guestTrainRetryTrainId)
            {
                g_guestTrainSpawnAttempts = 0;
                g_guestTrainLastSpawnTick = 0;
                g_guestTrainRetryConfig = g_pendingTrainV2.configHash;
                g_guestTrainRetryTrainId = g_pendingTrainV2.trainId;
            }
            g_guestTrainSnap = g_pendingTrainV2;
            g_guestTrainSnapValid = true;
            g_hasPendingTrainV2 = false;
            g_guestTrainLastSnapTick = tickCount;
        }
    }

    if (!g_guestTrainRandomDisabled)
    {
        using namespace natives_api;
        Init(natives::SET_RANDOM_TRAINS);
        Push(false);
        Call();
        g_guestTrainRandomDisabled = true;
    }

    if (g_guestTrain && !DoesExist(g_guestTrain))
    {
        g_guestTrain = 0;
        g_guestTrainConfig = 0;
    }
    if (g_guestTrainLastSnapTick != 0 &&
        tickCount - g_guestTrainLastSnapTick > kTrainSnapshotTimeoutTicks)
    {
        DeleteGuestMissionTrain(2); // timeout >2 c
        g_guestTrainSnapValid = false;
    }
    if (!g_guestTrain)
    {
        CreateGuestTrain(tickCount);
        return;
    }
    if (!g_guestTrainSnapValid)
        return;
    const net::TrainSnapshotV2Payload& snap = g_guestTrainSnap;
    if (snap.configHash != g_guestTrainConfig ||
        snap.carCount != g_guestTrainCars)
    {
        DeleteGuestMissionTrain(3); // config/carCount изменились — пересоздаём
        return;
    }

    using namespace natives_api;
    const unsigned int train = g_guestTrain;
    const float curSpeed = GetSpeed(train);
    if (std::fabs(curSpeed - snap.speed) > 0.5f)
    {
        Init(natives::SET_TRAIN_CRUISE_SPEED);
        Push(train); Push(snap.speed); Call();
        Init(natives::SET_TRAIN_SPEED);
        Push(train); Push(snap.speed); Call();
    }

    const Vec3 cp = GetCoords(train);
    const float dx = snap.x - cp.x;
    const float dy = snap.y - cp.y;
    const float dz = snap.z - cp.z;
    const float dist = std::sqrt(dx * dx + dy * dy);
    const bool waitFlag = (snap.flags & net::kTrainFlagWaitingAtStation) != 0;
    const bool reverseFlag = (snap.flags & net::kTrainFlagReversing) != 0;
    if (dist > 25.0f || std::fabs(dz) > 15.0f)
    {
        Init(natives::_SET_MISSION_TRAIN_WARP_TO_COORDS);
        Push(train);
        Push(snap.x); Push(snap.y); Push(snap.z);
        Push(reverseFlag ? static_cast<unsigned long long>(0)
                         : static_cast<unsigned long long>(1));
        Call();
        g_guestTrainLastSoftSync = tickCount;
    }
    else if (dist > 5.0f && tickCount - g_guestTrainLastSoftSync >= 30)
    {
        Init(natives::SET_MISSION_TRAIN_COORDS);
        Push(train);
        Push(snap.x); Push(snap.y); Push(snap.z);
        Call();
        g_guestTrainLastSoftSync = tickCount;
    }

    if (waitFlag && !g_guestTrainWaiting)
    {
        Init(natives::_SET_TRAIN_HALT);
        Push(train);
        Call();
        g_guestTrainWaiting = true;
    }
    else if (!waitFlag && g_guestTrainWaiting)
    {
        Init(natives::_SET_TRAIN_LEAVE_STATION);
        Push(train);
        Call();
        g_guestTrainWaiting = false;
    }

    if (reverseFlag != g_guestTrainReverse)
    {
        Init(natives::_SET_TRAIN_REVERSE_ENABLED);
        Push(train);
        Push(reverseFlag ? static_cast<unsigned long long>(1)
                         : static_cast<unsigned long long>(0));
        Call();
        g_guestTrainReverse = reverseFlag;
    }
}

void SendHostSnapshot(unsigned int localPlayerPed, unsigned int tickCount,
                      bool includeTrains, bool vehiclesOnly,
                      int vehicleRadius, float partnerX, float partnerY,
                      float partnerZ, bool partnerValid)
{
    if (tickCount - g_lastHostScanTick < kSendEveryTicks)
        return;
    g_lastHostScanTick = tickCount;

    // E3d-a: в vehicle-only режиме радиус отдельный (npc_vehicle_radius).
    float radius = kRadius;
    if (vehiclesOnly)
    {
        radius = static_cast<float>(vehicleRadius);
        if (radius < 20.f) radius = 20.f;
        if (radius > 200.f) radius = 200.f;
    }
    const float radius2 = radius * radius;

    static int handles[2048];
    int count = 0;
    if (!vehiclesOnly)
    {
        count = sh::worldGetAllPeds(handles, 2048);
        if (count < 0) count = 0;
        if (count > 2048) count = 2048;
    }

    const Vec3 origin = GetCoords(localPlayerPed);
    const unsigned int localMount = GetMountOf(localPlayerPed);
    // E3d-a: в vehicle-only режиме сканируем вокруг обоих игроков.
    const bool usePartner = vehiclesOnly && partnerValid;
    auto dist2ToPlayers = [&](float x, float y) -> float {
        const float dx = x - origin.x;
        const float dy = y - origin.y;
        float d2 = dx * dx + dy * dy;
        if (usePartner)
        {
            const float px = x - partnerX;
            const float py = y - partnerY;
            const float p2 = px * px + py * py;
            if (p2 < d2) d2 = p2;
        }
        return d2;
    };

    static std::vector<Candidate> candidates;
    candidates.clear();
    candidates.reserve(128);

    for (int i = 0; i < count; ++i)
    {
        const unsigned int handle = static_cast<unsigned int>(handles[i]);
        if (!handle || handle == localPlayerPed || handle == localMount)
            continue;
        if (coop::IsWorldSyncExcluded(handle))
            continue;
        if (!DoesExist(handle))
            continue;

        const Vec3 p = GetCoords(handle);
        const float dx = p.x - origin.x;
        const float dy = p.y - origin.y;
        const float d2 = dx * dx + dy * dy;
        if (d2 > radius2)
            continue;

        const unsigned int model = GetModel(handle);
        if (!model)
            continue;

        HostIdState& state = g_hostIds[handle];
        if (state.netId == 0 || state.model != model)
        {
            state.netId = g_nextNetId++;
            state.model = model;
        }
        state.lastSeenTick = tickCount;

        net::WorldEntity e{};
        e.netId = state.netId;
        e.model = model;
        e.x = p.x; e.y = p.y; e.z = p.z;
        e.heading = GetHeading(handle);
        e.kind = net::kEntityKindPed;
        e.flags = 0;
        const bool mission = IsMissionEntity(handle);
        if (mission) e.flags |= net::kEntityFlagMission;
        if (IsDeadPed(handle)) e.flags |= net::kEntityFlagDead;

        const float speed = GetSpeed(handle);
        e.state = (speed > 4.5f) ? net::kEntityStateRun
                : (speed > 0.3f) ? net::kEntityStateWalk
                                 : net::kEntityStateIdle;

        Candidate c;
        c.entity = e;
        c.distance2 = d2;
        c.mission = mission;
        candidates.push_back(c);
    }

    // Транспорт RDR2: дилижансы, повозки, лодки, трамваи (в движке это
    // Vehicle). Поезда пока пропускаем: это длинный движущийся состав,
    // которому нужна отдельная репликация вагонов, а не один прокси.
    unsigned int trainsSkipped = 0;
    unsigned int trainsIncluded = 0;
    {
        static int vehicleHandles[1024];
        int vehicleCount = sh::worldGetAllVehicles(vehicleHandles, 1024);
        if (vehicleCount < 0) vehicleCount = 0;
        if (vehicleCount > 1024) vehicleCount = 1024;

        unsigned int localVehicle = 0;
        {
            using namespace natives_api;
            Init(natives::GET_VEHICLE_PED_IS_USING);
            Push(localPlayerPed);
            localVehicle = static_cast<unsigned int>(Call());
        }
        // E3d-a: в vehicle-only режиме транспорт хоста нужен гостю (везёт
        // игрока/экипаж). Исключаем только если им уже занимается
        // PlayerVehicleState (vehicle_sync=1) — иначе был бы дубль.
        const bool excludeLocalVehicle =
            !vehiclesOnly || coopcfg::Get().vehicleSync;

        std::unordered_set<unsigned int> seenVehicles;
        std::vector<unsigned int> trainEngines;

        // Добавляет один vehicle-handle как кандидат. Радиус/модель/netId —
        // те же правила, что и в основном цикле ниже.
        auto addVehicleCandidate = [&](unsigned int handle,
                                       net::WorldEntity* outEntity = nullptr) -> bool
        {
            if (!handle || (handle == localVehicle && excludeLocalVehicle))
                return false;
            if (coop::IsWorldSyncExcluded(handle) || !DoesExist(handle))
                return false;
            const Vec3 p = GetCoords(handle);
            const float d2 = dist2ToPlayers(p.x, p.y);
            if (d2 > radius2)
                return false;
            const unsigned int model = GetModel(handle);
            if (!model)
                return false;

            HostIdState& state = g_hostIds[handle];
            if (state.netId == 0 || state.model != model)
            {
                state.netId = g_nextNetId++;
                state.model = model;
            }
            state.lastSeenTick = tickCount;

            net::WorldEntity e{};
            e.netId = state.netId;
            e.model = model;
            e.x = p.x; e.y = p.y; e.z = p.z;
            e.heading = GetHeading(handle);
            e.kind = net::kEntityKindVehicle;
            e.flags = 0;
            const bool mission = IsMissionEntity(handle);
            if (mission) e.flags |= net::kEntityFlagMission;
            {
                using namespace natives_api;
                Init(natives::IS_VEHICLE_WRECKED);
                Push(handle);
                if (Call()) e.flags |= net::kEntityFlagDead;
            }
            e.state = 0;

            Candidate c;
            c.entity = e;
            c.distance2 = d2;
            c.mission = mission;
            candidates.push_back(c);
            if (outEntity)
                *outEntity = e;
            return true;
        };

        for (int i = 0; i < vehicleCount; ++i)
        {
            const unsigned int handle =
                static_cast<unsigned int>(vehicleHandles[i]);
            if (!handle || (handle == localVehicle && excludeLocalVehicle))
                continue;
            if (coop::IsWorldSyncExcluded(handle))
                continue;
            if (!DoesExist(handle))
                continue;
            seenVehicles.insert(handle);

            const Vec3 p = GetCoords(handle);
            const float d2 = dist2ToPlayers(p.x, p.y);
            if (d2 > radius2)
                continue;

            const unsigned int model = GetModel(handle);
            if (!model)
                continue;
            if (TrainsEnabled() && IsTrainModel(model))
            {
                ++trainsSkipped; // E3d-b: поезда идут отдельным 0x22
                continue;
            }

            {
                using namespace natives_api;
                Init(natives::IS_THIS_MODEL_A_TRAIN);
                Push(static_cast<unsigned long long>(model));
                if (Call())
                {
                    // R&D (world_trains=1): поезд/вагоны идут как обычные
                    // Vehicle-прокси. Это НЕ полноценный состав: нет рельсовой
                    // привязки/сцепки/скорости — только позиции из снапшота.
                    if (!includeTrains)
                    {
                        ++trainsSkipped;
                        continue;
                    }
                    ++trainsIncluded;
                    // R&D: если это локомотив/головной вагон, запоминаем его,
                    // чтобы затем добрать вагоны через GET_TRAIN_CARRIAGE.
                    Init(natives::_GET_TRAIN_CARRIAGE_TRAILER_NUMBER);
                    Push(handle);
                    if (static_cast<int>(Call()) > 0)
                        trainEngines.push_back(handle);
                }
            }

            HostIdState& state = g_hostIds[handle];
            if (state.netId == 0 || state.model != model)
            {
                state.netId = g_nextNetId++;
                state.model = model;
            }
            state.lastSeenTick = tickCount;

            net::WorldEntity e{};
            e.netId = state.netId;
            e.model = model;
            e.x = p.x; e.y = p.y; e.z = p.z;
            e.heading = GetHeading(handle);
            e.kind = net::kEntityKindVehicle;
            e.flags = 0;
            const bool mission = IsMissionEntity(handle);
            if (mission) e.flags |= net::kEntityFlagMission;
            {
                using namespace natives_api;
                Init(natives::IS_VEHICLE_WRECKED);
                Push(handle);
                if (Call()) e.flags |= net::kEntityFlagDead;
            }
            e.state = 0;

            Candidate c;
            c.entity = e;
            c.distance2 = d2;
            c.mission = mission;
            candidates.push_back(c);
        }

        // R&D: добрать вагоны состава, даже если worldGetAllVehicles вернул
        // только локомотив. Сцепка/рельсы/скорость по-прежнему не
        // моделируются — это только позиции вагонов для прокси.
        unsigned int trainCarsAdded = 0;
        std::vector<net::WorldEntity> trainCarEntities;
        for (const unsigned int engine : trainEngines)
        {
            using namespace natives_api;
            Init(natives::_GET_TRAIN_CARRIAGE_TRAILER_NUMBER);
            Push(engine);
            const int trailerCount = static_cast<int>(Call());
            if (trailerCount <= 0)
                continue;
            Init(natives::_GET_TRAIN_DIRECTION);
            Push(engine);
            const bool trainDirection = Call() != 0;
            const float trainSpeed = GetSpeed(engine);
            for (int carIndex = 0; carIndex <= trailerCount; ++carIndex)
            {
                Init(natives::GET_TRAIN_CARRIAGE);
                Push(engine);
                Push(static_cast<unsigned long long>(carIndex));
                const unsigned int car = static_cast<unsigned int>(Call());
                if (!car || car == engine || seenVehicles.count(car))
                    continue;
                seenVehicles.insert(car);
                net::WorldEntity carEntity{};
                if (addVehicleCandidate(car, &carEntity))
                {
                    ++trainCarsAdded;
                    ++trainsIncluded;
                    if (trainCarEntities.size() <
                        static_cast<std::size_t>(net::kMaxTrainCars))
                    {
                        trainCarEntities.push_back(carEntity);
                    }
                }
            }
            sh_log::Write("train: consist engine=%u trailerCount=%d added=%u "
                          "dir=%d speed=%.1f\n",
                          engine, trailerCount, trainCarsAdded,
                          trainDirection ? 1 : 0, trainSpeed);
        }

        // R&D TrainSnapshot: engine netId/model + direction/speed + вагоны
        // по порядку (только те, что удалось добавить в этом проходе).
        if (!trainEngines.empty())
        {
            using namespace natives_api;
            const unsigned int engine = trainEngines.front();
            net::TrainSnapshotHeader h{};
            h.engineNetId = static_cast<uint32_t>(g_hostIds[engine].netId);
            h.engineModel = GetModel(engine);
            Init(natives::_GET_TRAIN_DIRECTION);
            Push(engine);
            h.direction = Call() ? 1 : 0;
            h.speed = GetSpeed(engine);
            h.carCount = static_cast<uint8_t>(trainCarEntities.size());

            std::vector<uint8_t> payload(sizeof(h) +
                trainCarEntities.size() * sizeof(net::WorldEntity));
            std::memcpy(payload.data(), &h, sizeof(h));
            for (std::size_t i = 0; i < trainCarEntities.size(); ++i)
            {
                std::memcpy(payload.data() + sizeof(h) +
                                i * sizeof(net::WorldEntity),
                            &trainCarEntities[i], sizeof(net::WorldEntity));
            }
            coop::SendTrainSnapshot(payload);
        }
    }

    // Объекты: ТОЛЬКО mission-object. Амбиентные пропсы уже есть в мире
    // гостя (та же карта) — их дублирование дало бы двойные объекты/z-fighting.
    if (!vehiclesOnly)
    {
        static int objectHandles[2048];
        int objectCount = sh::worldGetAllObjects(objectHandles, 2048);
        if (objectCount < 0) objectCount = 0;
        if (objectCount > 2048) objectCount = 2048;

        for (int i = 0; i < objectCount; ++i)
        {
            const unsigned int handle =
                static_cast<unsigned int>(objectHandles[i]);
            if (!handle)
                continue;
            if (coop::IsWorldSyncExcluded(handle))
                continue;
            if (!DoesExist(handle))
                continue;
            if (!IsMissionEntity(handle))
                continue; // только миссионные объекты

            const Vec3 p = GetCoords(handle);
            const float dx = p.x - origin.x;
            const float dy = p.y - origin.y;
            const float d2 = dx * dx + dy * dy;
            if (d2 > radius2)
                continue;

            const unsigned int model = GetModel(handle);
            if (!model)
                continue;

            HostIdState& state = g_hostIds[handle];
            if (state.netId == 0 || state.model != model)
            {
                state.netId = g_nextNetId++;
                state.model = model;
            }
            state.lastSeenTick = tickCount;

            net::WorldEntity e{};
            e.netId = state.netId;
            e.model = model;
            e.x = p.x; e.y = p.y; e.z = p.z;
            e.heading = GetHeading(handle);
            e.kind = net::kEntityKindObject;
            e.flags = net::kEntityFlagMission;
            e.state = 0;

            Candidate c;
            c.entity = e;
            c.distance2 = d2;
            c.mission = true;
            candidates.push_back(c);
        }
    }

    // Приоритет: mission-entity, затем ближайшие; отдельные лимиты на
    // Ped/Vehicle/Object (чистая функция покрыта C++ stress-тестом).
    std::vector<BudgetCandidate> budgetInputs;
    budgetInputs.reserve(candidates.size());
    for (const Candidate& c : candidates)
    {
        BudgetCandidate b;
        b.mission = c.mission;
        b.distance2 = c.distance2;
        if (c.entity.kind == net::kEntityKindVehicle)
            b.kind = BudgetKind::Vehicle;
        else if (c.entity.kind == net::kEntityKindObject)
            b.kind = BudgetKind::Object;
        else
            b.kind = BudgetKind::Ped;
        budgetInputs.push_back(b);
    }
    BudgetCaps caps;
    caps.peds = kMaxPeds;
    caps.vehicles = kMaxVehicles;
    caps.objects = kMaxObjects;
    const std::vector<int> order = SelectWorldCandidates(budgetInputs, caps);
    std::vector<Candidate> selected;
    selected.reserve(order.size());
    for (const int index : order)
        selected.push_back(candidates[static_cast<std::size_t>(index)]);
    candidates.swap(selected);

    net::WorldSnapshotHeader header{};
    header.count = static_cast<uint16_t>(candidates.size());
    std::vector<uint8_t> payload(sizeof(header) +
                                 candidates.size() * sizeof(net::WorldEntity));
    std::memcpy(payload.data(), &header, sizeof(header));
    for (std::size_t i = 0; i < candidates.size(); ++i)
    {
        std::memcpy(payload.data() + sizeof(header) + i * sizeof(net::WorldEntity),
                    &candidates[i].entity, sizeof(net::WorldEntity));
    }
    coop::SendWorldSnapshot(payload);
    ++g_hostSnapshotCount;

    // E3d-c (v19): occupants транспорта + draft-лошади. Собираем handles для
    // actor discovery (mission-occupants разрешены), шлём 0x21 при изменении и
    // heartbeat 1/с; count=0 очищает occupants у гостя.
    if (coopcfg::Get().npcActor && coopcfg::Get().npcOccupants)
    {
        std::vector<unsigned int> occupantHandles;
        int sendBudget = 6; // не флудим 0x21 в одном скане
        for (const Candidate& c : candidates)
        {
            if (c.entity.kind != net::kEntityKindVehicle)
                continue;
            unsigned int veh = 0;
            if (!FindHostHandleByNetId(c.entity.netId, veh))
                continue;
            std::vector<net::VehicleOccupantEntry> entries;
            uint32_t driverActorId = 0;
            const std::vector<unsigned int> hs = CollectVehicleOccupants(
                veh, localPlayerPed, c.entity.netId, entries, driverActorId);
            occupantHandles.insert(occupantHandles.end(), hs.begin(), hs.end());

            auto sent = g_lastOccupantsSent.find(c.entity.netId);
            const bool changed = (sent == g_lastOccupantsSent.end())
                                     ? !entries.empty()
                                     : !OccupantsEqual(sent->second, entries);
            const unsigned int lastTick =
                g_lastOccupantsSendTick[c.entity.netId];
            const bool heartbeat =
                !entries.empty() &&
                (tickCount - lastTick) >= 60; // ~1 c
            if ((!changed && !heartbeat) || sendBudget <= 0)
                continue;
            if (g_occupantsSendWindowTick == 0 ||
                tickCount - g_occupantsSendWindowTick >= 60)
            {
                g_occupantsSendWindowTick = tickCount;
                g_occupantsSendWindowCount = 0;
            }
            if (g_occupantsSendWindowCount >= 18)
                continue; // сервер всё равно rate-limit'ит 20/с
            ++g_occupantsSendWindowCount;
            coop::SendVehicleOccupants(c.entity.netId, entries);
            g_lastOccupantsSent[c.entity.netId] = entries;
            g_lastOccupantsSendTick[c.entity.netId] = tickCount;
            --sendBudget;
            if (coopcfg::Get().npcDebug)
                sh_log::Write(
                    "world: occupants vehicle=%u n=%u driver=%u\n",
                    c.entity.netId, static_cast<unsigned int>(entries.size()),
                    driverActorId);
        }
        actor_sync::SetOccupantHandles(occupantHandles);
    }

    if (vehiclesOnly && coopcfg::Get().npcDebug &&
        (tickCount % kLogEveryTicks) == 1)
    {
        unsigned int veh = 0;
        for (const Candidate& c : candidates)
            if (c.entity.kind == net::kEntityKindVehicle) ++veh;
        sh_log::Write("world: vehicles send n=%u bytes=%u\n", veh,
                      static_cast<unsigned int>(payload.size()));
    }

    if (!vehiclesOnly && (tickCount % kLogEveryTicks) == 1)
    {
        unsigned int missionCount = 0;
        unsigned int pedCount = 0;
        unsigned int vehicleCount = 0;
        unsigned int objectCount = 0;
        for (const Candidate& c : candidates)
        {
            if (c.mission) ++missionCount;
            if (c.entity.kind == net::kEntityKindVehicle) ++vehicleCount;
            else if (c.entity.kind == net::kEntityKindObject) ++objectCount;
            else ++pedCount;
        }
        sh_log::Write("world: host snapshot n=%u ped=%u veh=%u obj=%u "
                      "mission=%u trains_skipped=%u trains_included=%u bytes=%u\n",
                      static_cast<unsigned int>(candidates.size()), pedCount,
                      vehicleCount, objectCount, missionCount, trainsSkipped,
                      trainsIncluded,
                      static_cast<unsigned int>(payload.size()));
    }

    // Чистка забытых handle → netId, чтобы не росла карта.
    if ((tickCount % kLogEveryTicks) == 2)
    {
        for (auto it = g_hostIds.begin(); it != g_hostIds.end();)
        {
            if (tickCount - it->second.lastSeenTick > kHostIdTtlTicks)
                it = g_hostIds.erase(it);
            else
                ++it;
        }
    }
}

// ---------------- гость: прокси и подавление мира ----------------

struct Proxy
{
    unsigned int handle = 0;
    unsigned int model = 0;
    unsigned int kind = 0; // net::kEntityKindPed / kEntityKindVehicle / kEntityKindObject
    bool dead = false;
    unsigned int lastSeenSnapshot = 0;
    // P1: анимация/движение прокси-педа (чтобы не телепортировался каждый снапшот).
    uint8_t moveState = net::kEntityStateIdle;
    bool moveTaskValid = false;
    Vec3 moveTarget{0.f, 0.f, 0.f};
    // R&D: вагон привязан к локомотиву (позицию ведёт attachment).
    bool trainAttached = false;
    unsigned int trainAttachedTo = 0;
    // E3d-c (v19): 60 Гц интерполяция vehicle-прокси (prev/cur snapshot).
    bool vehHasTarget = false;
    float vehPrevX = 0.f, vehPrevY = 0.f, vehPrevZ = 0.f, vehPrevH = 0.f;
    unsigned long long vehPrevMs = 0;
    float vehCurX = 0.f, vehCurY = 0.f, vehCurZ = 0.f, vehCurH = 0.f;
    unsigned long long vehCurMs = 0;
    bool vehSnapWarp = false;
};

std::map<uint64_t, Proxy> g_proxies;
std::mutex g_snapshotMutex;
std::vector<uint8_t> g_pendingSnapshot;
bool g_hasPendingSnapshot = false;
std::atomic<unsigned long long> g_lastSnapshotMs{0};
uint64_t g_snapshotSeq = 0;
// R&D: pending TrainSnapshot (сетевой поток -> игровой).
std::mutex g_trainMutex;
std::vector<uint8_t> g_pendingTrain;
bool g_hasPendingTrain = false;

// ================= E3d-c (v19): guest occupants =================
struct OccupantsInboxMsg
{
    uint32_t vehicleNetId = 0;
    std::vector<net::VehicleOccupantEntry> entries;
};
std::mutex g_occupantsMutex;
std::vector<OccupantsInboxMsg> g_occupantsInbox;
bool g_haveOccupantsInbox = false;

struct SeatAttempt
{
    uint32_t actorId = 0;
    int8_t seat = -1;
    uint8_t flags = 0;
    uint8_t harness = 0;
    unsigned int attempts = 0;
    unsigned int nextRetryTick = 0; // task-143 P2.7: backoff 2-5 c
    bool applied = false;
    bool failed = false;
};
struct VehicleOccupantsState
{
    std::vector<SeatAttempt> entries;
    unsigned int firstSeenTick = 0;
};
std::map<uint32_t, VehicleOccupantsState> g_occupantVehicles;
unsigned int g_occupantsErrors = 0;

// E7.1: общий набор THIS_FRAME-множителей ambient-плотности.
// suppressVehicles=false — трафик не трогаем (npc_visitor_vehicles=0).
void SuppressAmbientDensities(bool suppressVehicles)
{
    using namespace natives_api;
    Init(natives::_SET_AMBIENT_HUMAN_DENSITY_MULTIPLIER_THIS_FRAME);
    Push(0.0f); Call();
    Init(natives::_SET_AMBIENT_PED_DENSITY_MULTIPLIER_THIS_FRAME);
    Push(0.0f); Call();
    Init(natives::_SET_AMBIENT_ANIMAL_DENSITY_MULTIPLIER_THIS_FRAME);
    Push(0.0f); Call();
    // E7.1 v2: scenario density НЕ обнуляем — продавцы/работники/события
    // должны остаться в мире гостя (пользовательское требование).
    if (!suppressVehicles)
        return;
    Init(natives::SET_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME);
    Push(0.0f); Call();
    Init(natives::SET_RANDOM_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME);
    Push(0.0f); Call();
}

void SuppressLocalWorld()
{
    SuppressAmbientDensities(true);
}

unsigned int CreateProxy(const net::WorldEntity& e)
{
    using namespace natives_api;
    if (e.kind == net::kEntityKindObject)
    {
        Init(natives::CREATE_OBJECT);
        Push(static_cast<unsigned long long>(e.model));
        Push(e.x); Push(e.y); Push(e.z);
        Push(false); // isNetwork
        Push(false); // bScriptHostObj
        Push(false); // dynamic
        Push(false); // p7
        Push(false); // p8
        const unsigned int obj = static_cast<unsigned int>(Call());
        if (!obj)
            return 0;
        SetMissionEntity(obj);
        SetInvincible(obj, true);
        SetVisible(obj);
        SetNoCollision(obj, true);
        return obj;
    }

    if (e.kind == net::kEntityKindVehicle)
    {
        Init(natives::CREATE_VEHICLE);
        Push(static_cast<unsigned long long>(e.model));
        Push(e.x); Push(e.y); Push(e.z); Push(e.heading);
        Push(false); // isNetwork
        Push(false); // bScriptHostVeh
        Push(true);  // bDontAutoCreateDraftAnimals: тягловых лошадей не плодим
        Push(false); // p8
        const unsigned int veh = static_cast<unsigned int>(Call());
        if (!veh)
            return 0;
        SetMissionEntity(veh);
        SetInvincible(veh, true);
        SetVisible(veh);
        SetNoCollision(veh, true);
        ConfigureDraftVehicleProtection(veh);
        return veh;
    }

    Init(natives::CREATE_PED);
    Push(static_cast<unsigned long long>(e.model));
    Push(e.x); Push(e.y); Push(e.z); Push(e.heading);
    Push(false); Push(false); Push(false); Push(false);
    const unsigned int handle = static_cast<unsigned int>(Call());
    if (!handle)
        return 0;

    SetMissionEntity(handle);
    SetInvincible(handle, true);
    RandomizeComponents(handle);
    SetVisible(handle);
    SetNoCollision(handle, true);
    StandStill(handle);
    return handle;
}

void UpdateProxy(Proxy& proxy, const net::WorldEntity& e)
{
    if (!proxy.handle || !DoesExist(proxy.handle))
        return; // будет пересоздан при следующем снапшоте

    if (proxy.kind != net::kEntityKindPed)
    {
        // R&D: вагон, привязанный к локомотиву, ведёт attachment; не
        // перетираем его позицию из WorldSnapshot каждый тик.
        if (proxy.trainAttached)
            return;
        // E3d-c (v19): vehicle-прокси интерполируем на 60 Гц; здесь только
        // задаём цель из снапшота (позицию двигает InterpolateVehicleProxies).
        if (proxy.kind == net::kEntityKindVehicle)
        {
            const unsigned long long nowMs = GetTickCount64();
            const bool first = !proxy.vehHasTarget;
            const float ddx = e.x - proxy.vehCurX;
            const float ddy = e.y - proxy.vehCurY;
            const float ddz = e.z - proxy.vehCurZ;
            const bool warp = first || (ddx * ddx + ddy * ddy > 64.0f) ||
                              std::fabs(ddz) > 8.0f;
            if (warp)
            {
                proxy.vehPrevX = proxy.vehCurX = e.x;
                proxy.vehPrevY = proxy.vehCurY = e.y;
                proxy.vehPrevZ = proxy.vehCurZ = e.z;
                proxy.vehPrevH = proxy.vehCurH = e.heading;
                proxy.vehPrevMs = proxy.vehCurMs = nowMs;
                SetCoords(proxy.handle, e.x, e.y, e.z);
                SetHeading(proxy.handle, e.heading);
                if (!first)
                    proxy.vehSnapWarp = true;
            }
            else
            {
                proxy.vehPrevX = proxy.vehCurX;
                proxy.vehPrevY = proxy.vehCurY;
                proxy.vehPrevZ = proxy.vehCurZ;
                proxy.vehPrevH = proxy.vehCurH;
                proxy.vehPrevMs = proxy.vehCurMs;
                proxy.vehCurX = e.x;
                proxy.vehCurY = e.y;
                proxy.vehCurZ = e.z;
                proxy.vehCurH = e.heading;
                proxy.vehCurMs = nowMs;
            }
            proxy.vehHasTarget = true;
            return;
        }
        // Объекты: позиция из снапшота (физика не симулируется).
        SetCoords(proxy.handle, e.x, e.y, e.z);
        SetHeading(proxy.handle, e.heading);
        return;
    }

    // Мёртвый пед: фиксируем смерть и позицию, задачи движения не ставим.
    const bool dead = (e.flags & net::kEntityFlagDead) != 0;
    if (dead)
    {
        if (!proxy.dead)
        {
            SetInvincible(proxy.handle, false);
            SetHealthDead(proxy.handle);
            proxy.dead = true;
            proxy.moveTaskValid = false;
        }
        SetCoords(proxy.handle, e.x, e.y, e.z);
        SetHeading(proxy.handle, e.heading);
        return;
    }
    if (proxy.dead)
    {
        // Воскрешение/пересоздание — сбрасываем задачу и позицию.
        proxy.dead = false;
        proxy.moveTaskValid = false;
    }

    const Vec3 target{e.x, e.y, e.z};
    const Vec3 cur = GetCoords(proxy.handle);
    const float dx = target.x - cur.x;
    const float dy = target.y - cur.y;
    const float dxz2 = dx * dx + dy * dy;
    const float dist = std::sqrt(dxz2);

    // Большой разрыв (спавн/телепорт/лаг) — переносим, иначе пед будет
    // бесконечно догонять через полкарты.
    if (dist > 8.0f)
    {
        SetCoords(proxy.handle, e.x, e.y, e.z);
        SetHeading(proxy.handle, e.heading);
        proxy.moveTaskValid = false;
    }
    else
    {
        const uint8_t st = e.state;
        const bool walking = (st == net::kEntityStateWalk);
        const bool running = (st == net::kEntityStateRun);
        const bool targetMoved =
            !proxy.moveTaskValid ||
            std::fabs(target.x - proxy.moveTarget.x) > 1.0f ||
            std::fabs(target.y - proxy.moveTarget.y) > 1.0f;
        if (walking || running)
        {
            const float speed = running ? 2.0f : 1.0f;
            if (targetMoved || proxy.moveState != st)
            {
                SetMoveBlend(proxy.handle, speed);
                FollowNavmesh(proxy.handle, e.x, e.y, e.z, speed);
                proxy.moveTarget = target;
                proxy.moveTaskValid = true;
            }
        }
        else
        {
            // Idle: если стояли задачей — фиксируем стойку; если отстали
            // больше 1.5 м, подводим шагом.
            if (dist > 1.5f)
            {
                if (!proxy.moveTaskValid ||
                    proxy.moveState != net::kEntityStateIdle ||
                    targetMoved)
                {
                    SetMoveBlend(proxy.handle, 1.0f);
                    FollowNavmesh(proxy.handle, e.x, e.y, e.z, 1.0f);
                    proxy.moveTarget = target;
                    proxy.moveTaskValid = true;
                }
            }
            else if (proxy.moveTaskValid)
            {
                SetMoveBlend(proxy.handle, 0.0f);
                StandStill(proxy.handle);
                proxy.moveTaskValid = false;
            }
        }
        if (dist < 3.0f)
            SetHeading(proxy.handle, e.heading);
    }

    proxy.moveState = e.state;
}

void DetachEntitySafe(unsigned int handle)
{
    if (!handle || !DoesExist(handle))
        return;
    using namespace natives_api;
    Init(natives::DETACH_ENTITY);
    Push(handle);
    Push(true);
    Push(true);
    Call();
}

void DeleteProxy(Proxy& proxy)
{
    if (proxy.handle)
    {
        // R&D train: сначала отцепляем вагоны, привязанные к этому entity.
        for (auto& kv : g_proxies)
        {
            if (kv.second.trainAttached &&
                kv.second.trainAttachedTo == proxy.handle)
            {
                DetachEntitySafe(kv.second.handle);
                kv.second.trainAttached = false;
                kv.second.trainAttachedTo = 0;
            }
        }
        if (proxy.trainAttached)
        {
            DetachEntitySafe(proxy.handle);
            proxy.trainAttached = false;
            proxy.trainAttachedTo = 0;
        }
        if (proxy.kind == net::kEntityKindVehicle)
            DeleteVehicleEntity(proxy.handle);
        else if (proxy.kind == net::kEntityKindObject)
            DeleteObjectEntity(proxy.handle);
        else
            DeleteEntity(proxy.handle);
    }
    proxy.handle = 0;
}

// ================= E3d-c (v19): guest occupant helpers =================
bool IsPedInVehicleNative(unsigned int ped, unsigned int vehicle)
{
    if (!ped || !vehicle)
        return false;
    using namespace natives_api;
    Init(natives::IS_PED_IN_VEHICLE);
    Push(ped);
    Push(vehicle);
    Push(false);
    return Call() != 0;
}

void SetPedIntoVehicleNative(unsigned int ped, unsigned int vehicle, int seat)
{
    using namespace natives_api;
    Init(natives::SET_PED_INTO_VEHICLE);
    Push(ped);
    Push(vehicle);
    Push(static_cast<unsigned long long>(static_cast<long long>(seat)));
    Call();
}

bool AttachDraftNative(unsigned int horse, unsigned int vehicle,
                       unsigned int harness)
{
    using namespace natives_api;
    Init(natives::_ATTACH_DRAFT_VEHICLE_HARNESS_PED);
    Push(horse);
    Push(vehicle);
    Push(static_cast<unsigned long long>(harness));
    return Call() != 0;
}

void DetachDraftNative(unsigned int vehicle, unsigned int horse)
{
    using namespace natives_api;
    Init(natives::_DETACH_DRAFT_VEHICLE_HARNESS_PED);
    Push(vehicle);
    Push(horse);
    Call();
}

// Снять attach-состояние с attempt (vehicle мог исчезнуть).
void CleanupSeatAttempt(SeatAttempt& e, uint32_t netId)
{
    unsigned int actor = 0;
    const bool hasActor = actor_sync::GetHandleForActorId(e.actorId, actor) &&
                          DoesExist(actor);
    if (e.flags & net::kOccupantFlagDraftHorse)
    {
        unsigned int veh = 0;
        if (hasActor && GetHandleForNetId(static_cast<uint64_t>(netId), veh) &&
            DoesExist(veh))
            DetachDraftNative(veh, actor);
        actor_sync::ClearActorDraftAttached(e.actorId);
    }
    else
    {
        actor_sync::ClearActorSeated(e.actorId);
    }
    if (hasActor)
        SetEntityVisible(actor, true);
    e.applied = false;
}

void CleanupVehicleOccupants(uint32_t netId)
{
    // task-143 P3: чистим кэши исчезнувшего vehicle (0x21 host cache).
    g_lastOccupantsSent.erase(netId);
    g_lastOccupantsSendTick.erase(netId);
    auto it = g_occupantVehicles.find(netId);
    if (it == g_occupantVehicles.end())
        return;
    for (SeatAttempt& e : it->second.entries)
    {
        SeatAttempt tmp = e;
        CleanupSeatAttempt(tmp, netId);
    }
    g_occupantVehicles.erase(it);
}

// Применить pending occupants: ждём vehicle+actor-прокси; attach до 3 попыток.
void ApplyPendingOccupants(unsigned int tickCount)
{
    if (!coopcfg::Get().npcActor || !coopcfg::Get().npcOccupants)
        return;
    std::vector<OccupantsInboxMsg> msgs;
    {
        std::lock_guard<std::mutex> lk(g_occupantsMutex);
        msgs.swap(g_occupantsInbox);
        g_haveOccupantsInbox = false;
    }
    for (OccupantsInboxMsg& msg : msgs)
    {
        if (!msg.vehicleNetId)
            continue;
        VehicleOccupantsState& st = g_occupantVehicles[msg.vehicleNetId];
        if (st.firstSeenTick == 0)
            st.firstSeenTick = tickCount;
        std::vector<SeatAttempt> old = st.entries;
        st.entries.clear();
        for (const net::VehicleOccupantEntry& ne : msg.entries)
        {
            SeatAttempt a;
            a.actorId = ne.actorId;
            a.seat = ne.seat;
            a.flags = ne.flags;
            a.harness = ne.harness;
            for (const SeatAttempt& o : old)
            {
                if (o.actorId == a.actorId && o.flags == a.flags &&
                    o.seat == a.seat && o.harness == a.harness)
                {
                    a.attempts = o.attempts;
                    a.applied = o.applied;
                    a.failed = o.failed;
                    break;
                }
            }
            if (a.actorId)
                st.entries.push_back(a);
        }
        for (const SeatAttempt& o : old)
        {
            bool still = false;
            for (const SeatAttempt& n : st.entries)
                if (n.actorId == o.actorId)
                {
                    still = true;
                    break;
                }
            if (!still)
            {
                SeatAttempt tmp = o;
                CleanupSeatAttempt(tmp, msg.vehicleNetId);
            }
        }
        if (msg.entries.empty())
            g_occupantVehicles.erase(msg.vehicleNetId);
    }

    for (auto it = g_occupantVehicles.begin(); it != g_occupantVehicles.end();)
    {
        const uint32_t netId = it->first;
        VehicleOccupantsState& st = it->second;
        unsigned int veh = 0;
        if (!GetHandleForNetId(static_cast<uint64_t>(netId), veh) ||
            !DoesExist(veh))
        {
            if (tickCount - st.firstSeenTick > 300)
            {
                for (SeatAttempt& e : st.entries)
                {
                    SeatAttempt tmp = e;
                    CleanupSeatAttempt(tmp, netId);
                }
                ++g_occupantsErrors;
                it = g_occupantVehicles.erase(it);
                continue;
            }
            ++it;
            continue;
        }

        auto pit = g_proxies.find(static_cast<uint64_t>(netId));
        if (pit != g_proxies.end() && pit->second.vehSnapWarp)
        {
            pit->second.vehSnapWarp = false;
            for (SeatAttempt& e : st.entries)
            {
                if (e.applied && !(e.flags & net::kOccupantFlagDraftHorse))
                {
                    e.applied = false;
                    e.attempts = 0;
                    actor_sync::ClearActorSeated(e.actorId);
                }
            }
        }

        for (std::size_t idx = 0; idx < st.entries.size();)
        {
            SeatAttempt& e = st.entries[idx];
            if (e.failed || (e.nextRetryTick != 0 &&
                             tickCount < e.nextRetryTick))
            {
                ++idx;
                continue;
            }
            unsigned int actor = 0;
            if (!actor_sync::GetHandleForActorId(e.actorId, actor) ||
                !DoesExist(actor))
            {
                if (tickCount - st.firstSeenTick > 180)
                {
                    SeatAttempt tmp = e;
                    CleanupSeatAttempt(tmp, netId);
                    ++g_occupantsErrors;
                    st.entries.erase(st.entries.begin() +
                                     static_cast<std::ptrdiff_t>(idx));
                    continue;
                }
                ++idx;
                continue;
            }

            if (e.flags & net::kOccupantFlagDraftHorse)
            {
                if (!e.applied)
                {
                    SetEntityVisible(actor, false);
                    const bool ok = AttachDraftNative(actor, veh, e.harness);
                    if (ok)
                    {
                        SetEntityVisible(actor, true);
                        actor_sync::SetActorDraftAttached(e.actorId, veh,
                                                          e.harness);
                        e.applied = true;
                        e.attempts = 0;
                        e.nextRetryTick = 0;
                        if (coopcfg::Get().npcDebug)
                            sh_log::Write(
                                "actor: draft attach horse=%u vehicle=%u "
                                "harness=%u ok=1\n",
                                e.actorId, netId, e.harness);
                    }
                    else
                    {
                        ++e.attempts;
                        ++g_occupantsErrors;
                        SetEntityVisible(actor, false);
                        if (e.attempts < 6)
                        {
                            // task-143 P2.7: retry backoff 2-5 с.
                            e.nextRetryTick =
                                tickCount + 120 + e.attempts * 30;
                        }
                        else
                        {
                            // Показать, чтобы пед не висел невидимым вечно.
                            SetEntityVisible(actor, true);
                            actor_sync::ClearActorDraftAttached(e.actorId);
                            e.failed = true;
                            if (coopcfg::Get().npcDebug)
                                sh_log::Write(
                                    "actor: draft attach horse=%u vehicle=%u "
                                    "harness=%u ok=0 give-up\n",
                                    e.actorId, netId, e.harness);
                        }
                    }
                }
            }
            else
            {
                if (e.applied && (tickCount % 10) == 0 &&
                    !IsPedInVehicleNative(actor, veh))
                {
                    e.applied = false;
                    e.attempts = 0;
                    actor_sync::ClearActorSeated(e.actorId);
                }
                if (!e.applied)
                {
                    SetEntityVisible(actor, false);
                    SetPedIntoVehicleNative(actor, veh, e.seat);
                    const bool ok = IsPedInVehicleNative(actor, veh);
                    if (ok)
                    {
                        SetEntityVisible(actor, true);
                        actor_sync::SetActorSeated(e.actorId, veh, e.seat,
                                                   e.flags);
                        e.applied = true;
                        e.attempts = 0;
                        e.nextRetryTick = 0;
                        if (coopcfg::Get().npcDebug)
                            sh_log::Write(
                                "actor: seat attach actor=%u vehicle=%u "
                                "seat=%d ok=1\n",
                                e.actorId, netId,
                                static_cast<int>(e.seat));
                    }
                    else
                    {
                        ++e.attempts;
                        ++g_occupantsErrors;
                        SetEntityVisible(actor, false);
                        if (e.attempts < 6)
                        {
                            // task-143 P2.7: retry backoff 2-5 с.
                            e.nextRetryTick =
                                tickCount + 120 + e.attempts * 30;
                        }
                        else
                        {
                            SetEntityVisible(actor, true);
                            actor_sync::ClearActorSeated(e.actorId);
                            e.failed = true;
                            if (coopcfg::Get().npcDebug)
                                sh_log::Write(
                                    "actor: seat attach actor=%u vehicle=%u "
                                    "seat=%d ok=0 give-up\n",
                                    e.actorId, netId,
                                    static_cast<int>(e.seat));
                        }
                    }
                }
            }
            ++idx;
        }
        ++it;
    }

    // task-143 P2.8: акторы, ждущие attach, держим invisible от спавна.
    std::vector<uint32_t> pendingHide;
    for (const auto& kv : g_occupantVehicles)
    {
        for (const SeatAttempt& e : kv.second.entries)
        {
            if (e.actorId && !e.applied && !e.failed)
                pendingHide.push_back(e.actorId);
        }
    }
    actor_sync::SetOccupantPendingActors(pendingHide);
}

// task-143 P1.2: time-based lerp vehPrev->vehCur по (now-vehCurMs)/interval,
// экстраполяция <=150 мс, нет данных >300 мс — держим позу. Heading —
// shortest-arc; warp>8 м по-прежнему snap в UpdateProxy.
void InterpolateVehicleProxies()
{
    if (!coopcfg::Get().npcActor)
        return;
    const unsigned long long now = GetTickCount64();
    for (auto& kv : g_proxies)
    {
        Proxy& p = kv.second;
        if (p.kind != net::kEntityKindVehicle || !p.vehHasTarget ||
            p.trainAttached || !DoesExist(p.handle))
            continue;
        const unsigned long long span =
            p.vehCurMs > p.vehPrevMs ? p.vehCurMs - p.vehPrevMs : 0ull;
        const unsigned long long elapsed =
            now > p.vehCurMs ? now - p.vehCurMs : 0ull;
        float t = 1.f;
        unsigned long long extrapMs = 0;
        if (span > 2ull)
        {
            t = static_cast<float>(elapsed) / static_cast<float>(span);
            if (t > 1.f)
            {
                t = 1.f;
                const unsigned long long over = elapsed - span;
                extrapMs = over < kVehicleExtrapMaxMs ? over : kVehicleExtrapMaxMs;
            }
            if (elapsed > kVehicleHoldMs)
                extrapMs = 0;
        }
        float x = p.vehPrevX + (p.vehCurX - p.vehPrevX) * t;
        float y = p.vehPrevY + (p.vehCurY - p.vehPrevY) * t;
        float z = p.vehPrevZ + (p.vehCurZ - p.vehPrevZ) * t;
        float h = p.vehPrevH;
        {
            float dh = p.vehCurH - p.vehPrevH;
            while (dh > 180.f) dh -= 360.f;
            while (dh < -180.f) dh += 360.f;
            h = p.vehPrevH + dh * t;
        }
        if (extrapMs > 0 && span > 2ull)
        {
            const float spanSec = static_cast<float>(span) / 1000.f;
            const float exSec = static_cast<float>(extrapMs) / 1000.f;
            x += (p.vehCurX - p.vehPrevX) / spanSec * exSec;
            y += (p.vehCurY - p.vehPrevY) / spanSec * exSec;
            z += (p.vehCurZ - p.vehPrevZ) / spanSec * exSec;
            float dh = p.vehCurH - p.vehPrevH;
            while (dh > 180.f) dh -= 360.f;
            while (dh < -180.f) dh += 360.f;
            h += dh / spanSec * exSec;
        }
        SetCoords(p.handle, x, y, z);
        SetHeading(p.handle, h);
    }
}

void ApplyPendingSnapshot(unsigned int tickCount, bool vehiclesOnly)
{
    std::vector<uint8_t> payload;
    {
        std::lock_guard<std::mutex> lk(g_snapshotMutex);
        if (!g_hasPendingSnapshot)
            return;
        payload.swap(g_pendingSnapshot);
        g_hasPendingSnapshot = false;
    }

    ParsedWorldSnapshot snapshot;
    if (!ParseWorldSnapshot(payload, snapshot))
        return;

    ++g_snapshotSeq;
    int spawned = 0;

    for (const net::WorldEntity& e : snapshot.entities)
    {
        if (vehiclesOnly && e.kind != net::kEntityKindVehicle)
            continue; // E3d-a: в vehicle-only режиме peds/objects не применяем
        auto it = g_proxies.find(e.netId);
        if (it != g_proxies.end() && !DoesExist(it->second.handle))
        {
            if (it->second.kind == net::kEntityKindVehicle &&
                coopcfg::Get().npcDebug)
                sh_log::Write("world: vehicle despawn netId=%llu reason=3\n",
                              static_cast<unsigned long long>(it->first));
            CleanupVehicleOccupants(static_cast<uint32_t>(it->first));
            DeleteProxy(it->second);
            it = g_proxies.erase(it);
        }
        if (it == g_proxies.end())
        {
            if (static_cast<int>(g_proxies.size()) >= net::kMaxWorldEntities)
                continue;
            if (!HasModelLoaded(e.model))
            {
                RequestModel(e.model);
                continue;
            }
            if (spawned >= kSpawnBudgetPerTick)
                continue;
            const unsigned int handle = CreateProxy(e);
            if (!handle)
                continue;
            Proxy proxy;
            proxy.handle = handle;
            proxy.model = e.model;
            proxy.kind = e.kind;
            proxy.lastSeenSnapshot = g_snapshotSeq;
            auto inserted = g_proxies.emplace(e.netId, proxy);
            UpdateProxy(inserted.first->second, e);
            if (e.kind == net::kEntityKindVehicle && coopcfg::Get().npcDebug)
                sh_log::Write("world: vehicle spawn netId=%llu model=0x%X\n",
                              static_cast<unsigned long long>(e.netId),
                              e.model);
            ++spawned;
        }
        else
        {
            it->second.lastSeenSnapshot = g_snapshotSeq;
            UpdateProxy(it->second, e);
        }
    }

    // Снапшот — полный список в радиусе: не увиденные несколько снапшотов
    // удаляем (tombstones-эквивалент для TCP-режима).
    for (auto it = g_proxies.begin(); it != g_proxies.end();)
    {
        if (g_snapshotSeq - it->second.lastSeenSnapshot > kProxyGraceSnapshots)
        {
            if (it->second.kind == net::kEntityKindVehicle &&
                coopcfg::Get().npcDebug)
                sh_log::Write("world: vehicle despawn netId=%llu reason=1\n",
                              static_cast<unsigned long long>(it->first));
            CleanupVehicleOccupants(static_cast<uint32_t>(it->first));
            DeleteProxy(it->second);
            it = g_proxies.erase(it);
        }
        else
        {
            ++it;
        }
    }

    if ((tickCount % kLogEveryTicks) == 1)
    {
        sh_log::Write("world: guest proxies=%u seq=%llu\n",
                      static_cast<unsigned int>(g_proxies.size()),
                      static_cast<unsigned long long>(g_snapshotSeq));
        if (g_occupantsErrors > 0)
        {
            sh_log::Write("actor: occupants errors=%u\n", g_occupantsErrors);
            g_occupantsErrors = 0;
        }
    }
}

// R&D: привязать вагоны-прокси к локомотиву по TrainSnapshot.
void ApplyPendingTrainSnapshot(unsigned int tickCount)
{
    std::vector<uint8_t> payload;
    {
        std::lock_guard<std::mutex> lk(g_trainMutex);
        if (!g_hasPendingTrain)
            return;
        payload.swap(g_pendingTrain);
        g_hasPendingTrain = false;
    }
    if (payload.size() < sizeof(net::TrainSnapshotHeader))
        return;
    net::TrainSnapshotHeader h{};
    std::memcpy(&h, payload.data(), sizeof(h));
    if (h.carCount > net::kMaxTrainCars)
        return;
    const std::size_t need = sizeof(h) +
        static_cast<std::size_t>(h.carCount) * sizeof(net::WorldEntity);
    if (payload.size() != need)
        return;

    auto engineIt = g_proxies.find(h.engineNetId);
    if (engineIt == g_proxies.end() || !engineIt->second.handle ||
        !DoesExist(engineIt->second.handle))
        return;
    const unsigned int engine = engineIt->second.handle;
    const Vec3 ep = GetCoords(engine);
    const float eh = GetHeading(engine);
    const float rad = eh * 0.017453292519943295f;
    const float cs = std::cos(rad);
    const float sn = std::sin(rad);

    int attachedNow = 0;
    for (uint16_t i = 0; i < h.carCount; ++i)
    {
        net::WorldEntity car{};
        std::memcpy(&car,
                    payload.data() + sizeof(h) +
                        static_cast<std::size_t>(i) * sizeof(net::WorldEntity),
                    sizeof(car));
        auto it = g_proxies.find(car.netId);
        if (it == g_proxies.end() || !it->second.handle ||
            !DoesExist(it->second.handle))
            continue;
        if (it->second.kind == net::kEntityKindPed)
            continue;
        if (it->second.trainAttached &&
            it->second.trainAttachedTo == engine)
            continue;
        if (it->second.trainAttached)
            DetachEntitySafe(it->second.handle);

        const Vec3 cp = GetCoords(it->second.handle);
        const float dx = cp.x - ep.x;
        const float dy = cp.y - ep.y;
        const float dz = cp.z - ep.z;
        // Перевод мировой дельты в локальную систему локомотива.
        const float localX = dx * cs - dy * sn;
        const float localY = dx * sn + dy * cs;
        const float localHeading = car.heading - eh;

        using namespace natives_api;
        Init(natives::ATTACH_ENTITY_TO_ENTITY);
        Push(it->second.handle); // entity1 (вагон)
        Push(engine);            // entity2 (локомотив)
        Push(static_cast<unsigned long long>(0)); // boneIndex
        Push(localX); Push(localY); Push(dz);
        Push(0.f); Push(0.f); Push(localHeading);
        Push(static_cast<unsigned long long>(1)); // p9
        Push(static_cast<unsigned long long>(1)); // useSoftPinning
        Push(static_cast<unsigned long long>(0)); // collision
        Push(static_cast<unsigned long long>(0)); // isPed
        Push(static_cast<unsigned long long>(0)); // vertexIndex
        Push(static_cast<unsigned long long>(1)); // fixedRot
        Push(static_cast<unsigned long long>(0)); // p15
        Push(static_cast<unsigned long long>(0)); // p16
        Call();

        Init(natives::IS_ENTITY_ATTACHED_TO_ENTITY);
        Push(it->second.handle);
        Push(engine);
        const bool attachedOk = Call() != 0;
        it->second.trainAttached = attachedOk;
        it->second.trainAttachedTo = attachedOk ? engine : 0;
        if (attachedOk)
            ++attachedNow;
        if (coopcfg::Get().trainDebug)
        {
            sh_log::Write("train: car netId=%u world=(%.1f %.1f %.1f) "
                          "local=(%.2f %.2f %.2f) rot=%.1f eng=(%.1f %.1f %.1f) "
                          "engHeading=%.1f attached=%d\n",
                          car.netId, cp.x, cp.y, cp.z, localX, localY, dz,
                          localHeading, ep.x, ep.y, ep.z, eh, attachedOk ? 1 : 0);
        }
        else if (!attachedOk && (tickCount % kLogEveryTicks) == 1)
        {
            sh_log::Write("train: attach FAILED car netId=%u engine=%u "
                          "(offset %.2f %.2f %.2f)\n",
                          car.netId, h.engineNetId, localX, localY, dz);
        }
    }
    if (attachedNow > 0)
    {
        sh_log::Write("train: attached %d car proxy(ies) to engine netId=%u\n",
                      attachedNow, h.engineNetId);
    }
    else if ((tickCount % kLogEveryTicks) == 1)
    {
        sh_log::Write("train: snapshot engine=%u cars=%u (no new attach)\n",
                      h.engineNetId, (unsigned)h.carCount);
    }
}

} // namespace

// E7.1: публичная обёртка visitor mode (вызывается из coop::Tick гостя).
void SuppressAmbientForVisitor(bool suppressVehicles)
{
    SuppressAmbientDensities(suppressVehicles);
}

void OnSnapshot(const std::vector<uint8_t>& payload)
{
    std::lock_guard<std::mutex> lk(g_snapshotMutex);
    g_pendingSnapshot = payload;
    g_hasPendingSnapshot = true;
    g_lastSnapshotMs.store(GetTickCount64());
}

// E3d-c (v19): сетевой поток -> inbox (нативы/attach только в Tick).
void OnVehicleOccupants(
    uint32_t vehicleNetId,
    const std::vector<net::VehicleOccupantEntry>& entries)
{
    std::lock_guard<std::mutex> lk(g_occupantsMutex);
    OccupantsInboxMsg msg;
    msg.vehicleNetId = vehicleNetId;
    msg.entries = entries;
    // Последнее сообщение на vehicle важнее; не копим очередь.
    for (OccupantsInboxMsg& m : g_occupantsInbox)
    {
        if (m.vehicleNetId == vehicleNetId)
        {
            m.entries = std::move(msg.entries);
            g_haveOccupantsInbox = true;
            return;
        }
    }
    if (g_occupantsInbox.size() < 64)
    {
        g_occupantsInbox.push_back(std::move(msg));
        g_haveOccupantsInbox = true;
    }
}

// E3d-b (v19): сетевой поток -> inbox (нативы только в Tick).
void OnTrainSnapshotV2(const net::TrainSnapshotV2Payload& payload)
{
    std::lock_guard<std::mutex> lk(g_trainV2Mutex);
    g_pendingTrainV2 = payload;
    g_hasPendingTrainV2 = true;
}

void OnTrainSnapshot(const std::vector<uint8_t>& payload)
{
    std::lock_guard<std::mutex> lk(g_trainMutex);
    g_pendingTrain = payload;
    g_hasPendingTrain = true;
}

bool GetNetIdForHandle(unsigned int handle, uint64_t& netId)
{
    if (!handle)
        return false;
    // Хост: handle своей сущности → netId.
    const auto it = g_hostIds.find(handle);
    if (it != g_hostIds.end() && it->second.netId != 0)
    {
        netId = it->second.netId;
        return true;
    }
    // Гость: handle прокси → netId (обратное направление целей).
    for (const auto& kv : g_proxies)
    {
        if (kv.second.handle == handle && kv.second.handle)
        {
            netId = kv.first;
            return true;
        }
    }
    return false;
}

bool GetHandleForNetId(uint64_t netId, unsigned int& handle)
{
    if (!netId)
        return false;
    // Гость: netId → свой прокси.
    const auto it = g_proxies.find(netId);
    if (it != g_proxies.end() && it->second.handle &&
        DoesExist(it->second.handle))
    {
        handle = it->second.handle;
        return true;
    }
    // Хост: netId → своя сущность (чтобы принять цель от гостя).
    for (const auto& kv : g_hostIds)
    {
        if (kv.second.netId == netId && kv.first && DoesExist(kv.first))
        {
            handle = kv.first;
            return true;
        }
    }
    return false;
}

void Reset()
{
    // task-143 P3: снять occupants/draft ДО DeleteProxy (handle ещё валиден).
    for (auto& kv : g_occupantVehicles)
    {
        for (SeatAttempt& e : kv.second.entries)
        {
            SeatAttempt tmp = e;
            CleanupSeatAttempt(tmp, kv.first);
        }
    }
    g_occupantVehicles.clear();

    for (auto& kv : g_proxies)
    {
        if (kv.second.kind == net::kEntityKindVehicle &&
            coopcfg::Get().npcDebug)
            sh_log::Write("world: vehicle despawn netId=%llu reason=2\n",
                          static_cast<unsigned long long>(kv.first));
        DeleteProxy(kv.second);
    }
    const std::size_t removed = g_proxies.size();
    g_proxies.clear();

    {
        std::lock_guard<std::mutex> lk(g_snapshotMutex);
        g_pendingSnapshot.clear();
        g_hasPendingSnapshot = false;
        g_lastSnapshotMs.store(0);
    }
    {
        std::lock_guard<std::mutex> lk(g_trainMutex);
        g_pendingTrain.clear();
        g_hasPendingTrain = false;
    }
    // E3d-c: снять occupants/draft со старого мира и очистить кэши 0x21.
    for (auto& kv : g_occupantVehicles)
    {
        for (SeatAttempt& e : kv.second.entries)
        {
            SeatAttempt tmp = e;
            CleanupSeatAttempt(tmp, kv.first);
        }
    }
    g_occupantVehicles.clear();
    {
        std::lock_guard<std::mutex> lk(g_occupantsMutex);
        g_occupantsInbox.clear();
        g_haveOccupantsInbox = false;
    }
    g_lastOccupantsSent.clear();
    g_lastOccupantsSendTick.clear();
    g_occupantsSendWindowTick = 0;
    g_occupantsSendWindowCount = 0;
    g_occupantsErrors = 0;
    // E3d-b: снять mission-train, восстановить random trains, очистить кэши.
    if (g_guestTrainRandomDisabled)
    {
        using namespace natives_api;
        Init(natives::SET_RANDOM_TRAINS);
        Push(true);
        Call();
        g_guestTrainRandomDisabled = false;
    }
    DeleteGuestMissionTrain(4);
    g_guestTrainSnapValid = false;
    g_guestTrainLastSnapTick = 0;
    {
        std::lock_guard<std::mutex> lk(g_trainV2Mutex);
        g_hasPendingTrainV2 = false;
    }
    g_trainScans.clear();
    g_trainConfigs.clear();
    g_trainConfigCars.clear();
    g_trainConfigModel0.clear();
    g_trainLastSeen.clear();
    g_trainIds.clear();
    g_modelIsTrainCache.clear();
    g_trainScanTick = 0;
    g_trainEngine = 0;
    g_trainEngineExpiry = 0;
    g_guestTrainSpawnAttempts = 0;
    g_guestTrainLastSpawnTick = 0;
    g_lastTrainSnapshot.clear();
    g_lastTrainSendTick.clear();
    g_snapshotSeq = 0;
    g_hostIds.clear();
    g_lastHostScanTick = 0;

    if (removed > 0)
        sh_log::Write("world: reset, removed %u proxies\n",
                      static_cast<unsigned int>(removed));
}

void Tick(unsigned int localPlayerPed, unsigned int tickCount,
          bool amHost, bool enabled, bool includeTrains, bool vehiclesOnly,
          int vehicleRadius, float partnerX, float partnerY,
          float partnerZ, bool partnerValid)
{
    if (!enabled)
    {
        if (!g_proxies.empty() || g_lastSnapshotMs.load() != 0)
            Reset();
        return;
    }

    // Fail closed: без worldGetAll* репликация невозможна — не зовём nullptr.
    if (!sh::HasWorldQueries())
    {
        if (!g_missingWorldQueriesLogged)
        {
            g_missingWorldQueriesLogged = true;
            sh_log::Write("world: worldGetAll* exports not found; world sync disabled\n");
        }
        return;
    }

    if (amHost)
    {
        if (!g_proxies.empty())
            Reset(); // смена роли: прокси больше не нужны
        SendHostSnapshot(localPlayerPed, tickCount, includeTrains,
                         vehiclesOnly, vehicleRadius, partnerX, partnerY,
                         partnerZ, partnerValid);
        // E3d-b: mission-train (0x22) отдельно от vehicle-бюджета.
        TickHostTrains(localPlayerPed, tickCount, partnerX, partnerY,
                       partnerValid);
        return;
    }

    // Гость.
    ApplyPendingSnapshot(tickCount, vehiclesOnly);
    if (!vehiclesOnly)
        ApplyPendingTrainSnapshot(tickCount);
    // E3d-c: 60 Гц интерполяция vehicle-прокси и attach occupants.
    InterpolateVehicleProxies();
    ApplyPendingOccupants(tickCount);
    TickGuestTrains(tickCount);

    const unsigned long long last = g_lastSnapshotMs.load();
    if (last == 0)
        return;

    const unsigned long long now = GetTickCount64();
    if (now > last && now - last > kGuestTimeoutMs)
    {
        sh_log::Write("world: guest timeout (no snapshots %llu ms), reset\n",
                      now - last);
        Reset();
        return;
    }

    if (!vehiclesOnly)
        SuppressLocalWorld();
}

} // namespace world_sync
} // namespace coop
