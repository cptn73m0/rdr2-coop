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
    // SET_ENTITY_COORDS — 8 аргументов (NativeDecl 1491.50):
    // (entity, x, y, z, xAxis, yAxis, zAxis, clearArea). Все 4 BOOL = 0:
    // оси не инвертируем, область вокруг телепорта НЕ чистим.
    Init(natives::SET_ENTITY_COORDS);
    Push(handle);
    Push(x); Push(y); Push(z);
    Push(false); // xAxis
    Push(false); // yAxis
    Push(false); // zAxis
    Push(false); // clearArea
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
    // v20 P1: тип сущности (net::kEntityKind*) — интенты транспорта ищут
    // только vehicle-netId (не actor/object).
    uint8_t kind = 0;
};

struct Candidate
{
    net::WorldEntity entity{};
    float distance2 = 0.f;
    bool mission = false;
};

std::unordered_map<unsigned int, HostIdState> g_hostIds;
uint64_t g_nextNetId = 1;
uint32_t EnsureHostNetId(unsigned int handle, unsigned int model,
                         unsigned int tickCount,
                         uint8_t kind = net::kEntityKindPed)
{
    HostIdState& state = g_hostIds[handle];
    if (state.netId == 0 || state.model != model || state.kind != kind)
    {
        state.netId = g_nextNetId++;
        state.model = model;
        state.kind = kind;
    }
    state.lastSeenTick = tickCount;
    return static_cast<uint32_t>(state.netId);
}

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
unsigned int g_lastOccupantsCollectTick = 0; // P1-5: 1 Гц
unsigned long long g_lastOccupantsCollectMs = 0; // task-160 wall-clock 1 c
std::map<uint32_t, unsigned long long> g_lastOccupantsSendMs; // task-160
unsigned long long g_occupantsSendWindowMs = 0; // task-160: 1 c window

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

// v20 P1: netId -> handle только для vehicle (интенты транспорта).
bool FindHostVehicleHandleByNetId(uint32_t netId, unsigned int& handle)
{
    for (const auto& kv : g_hostIds)
    {
        if (kv.second.netId == netId &&
            kv.second.kind == net::kEntityKindVehicle && kv.first &&
            DoesExist(kv.first))
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
    // e20.2 T3: диагностика occupants (npcDebug, 1/5 c per vehicle) — видит ли
    // хост кучера/лошадей и почему aid=0 (roster miss / mission / player).
    // ВАЖНО: handles пушатся в allowlist ДО lookup actorId (addPed выше), т.е.
    // водитель/draft-лошади известны discovery даже без actorId — эта строка
    // только диагностирует, а не меняет allowlist.
    if (coopcfg::Get().npcDebug)
    {
        static std::map<unsigned int, unsigned long long> lastOccDbgMs;
        const unsigned long long nowMs = GetTickCount64();
        unsigned long long& last = lastOccDbgMs[netId];
        if (last == 0 || nowMs - last >= 5000ull)
        {
            last = nowMs;
            uint32_t driverAid = 0;
            actor_sync::GetHostActorIdForHandle(driver, driverAid);
            int draft = 0;
            int harnessCount = 0;
            {
                using namespace natives_api;
                Init(natives::IS_DRAFT_VEHICLE);
                Push(vehicle);
                draft = Call() != 0 ? 1 : 0;
                if (draft)
                {
                    for (unsigned int h = 0;
                         h <= static_cast<unsigned int>(net::kMaxDraftHarness);
                         ++h)
                    {
                        Init(natives::_GET_PED_IN_DRAFT_HARNESS);
                        Push(vehicle);
                        Push(static_cast<unsigned long long>(h));
                        if (static_cast<unsigned int>(Call()) != 0)
                            ++harnessCount;
                    }
                }
            }
            sh_log::Write("world: occ dbg veh=%u model=0x%X driverH=%u "
                          "driverAid=%u attempted=%u entries=%u draft=%d "
                          "harness=%d\n",
                          netId, GetModel(vehicle), driver, driverAid,
                          static_cast<unsigned int>(attempted),
                          static_cast<unsigned int>(out.size()), draft,
                          harnessCount);
            if (lastOccDbgMs.size() > 64)
                lastOccDbgMs.erase(lastOccDbgMs.begin());
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
std::map<uint32_t, unsigned long long> g_lastTrainSendMs; // task-160
// E11 (task-149): occupant handles для actor discovery (vehicle + train).
std::vector<unsigned int> g_hostOccHandles;
// E11: последние отправленные train-occupants по trainId и heartbeat.
std::map<uint32_t, std::vector<net::VehicleOccupantEntry>> g_lastTrainOccupants;
std::map<uint32_t, unsigned long long> g_lastTrainOccupantsMs; // task-160

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

// P1-5 (task-156): единый кэш worldGetAllVehicles (TTL 6 тиков) для
// SendHostSnapshot и TickHostTrains; спасает от двух полных сканов на цикл.
std::vector<unsigned int> g_vehicleScanHandles;
unsigned int g_vehicleScanTick = 0;

void EnsureHostVehicleScan(unsigned int tickCount)
{
    if (g_vehicleScanTick != 0 &&
        tickCount - g_vehicleScanTick < kSendEveryTicks)
        return;
    g_vehicleScanTick = tickCount;
    static int handles[1024];
    int count = sh::worldGetAllVehicles(handles, 1024);
    if (count < 0) count = 0;
    if (count > 1024) count = 1024;
    g_vehicleScanHandles.clear();
    g_vehicleScanHandles.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i)
    {
        const unsigned int h = static_cast<unsigned int>(handles[i]);
        if (h)
            g_vehicleScanHandles.push_back(h);
    }
}

bool FindNearestTrain(unsigned int localPlayerPed, float partnerX,
                      float partnerY, bool partnerValid,
                      unsigned int tickCount,
                      unsigned int& outEngine, float& outDist)
{
    EnsureHostVehicleScan(tickCount);
    const std::vector<unsigned int>& handles = g_vehicleScanHandles;
    const int count = static_cast<int>(handles.size());
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

// E11 (task-149): точные пассажиры/машинист поезда через 0x21.
// vehicleNetId = trainId; entries carIndex = индекс вагона; cap 8 на сообщение
// (несколько чанков на поезд).
void SendTrainOccupants(unsigned int engine, uint32_t trainId, int cars,
                        unsigned int tickCount)
{
    if (!coopcfg::Get().npcActor || !coopcfg::Get().npcOccupants)
        return;
    if (!trainId || cars <= 0)
        return;
    std::vector<net::VehicleOccupantEntry> all;
    all.reserve(32);
    using namespace natives_api;
    for (int car = 0; car < cars; ++car)
    {
        Init(natives::GET_TRAIN_CARRIAGE);
        Push(engine);
        Push(static_cast<unsigned long long>(car));
        const unsigned int wagon = static_cast<unsigned int>(Call());
        if (!wagon || !DoesExist(wagon))
            continue;
        auto addPed = [&](unsigned int ped, int seat, uint8_t flags) {
            if (!ped || ped == 0xFFFFFFFFu || !DoesExist(ped))
                return;
            g_hostOccHandles.push_back(ped);
            uint32_t aid = 0;
            if (!actor_sync::GetHostActorIdForHandle(ped, aid))
                return; // roster ещё не содержит — повторим heartbeat
            net::VehicleOccupantEntry e{};
            e.actorId = aid;
            e.seat = static_cast<int8_t>(seat);
            e.flags = flags;
            e.carIndex = static_cast<uint8_t>(car);
            e.harness = 0;
            all.push_back(e);
        };
        Init(natives::GET_DRIVER_OF_VEHICLE);
        Push(wagon);
        addPed(static_cast<unsigned int>(Call()), -1,
               net::kOccupantFlagDriver);
        Init(natives::GET_VEHICLE_MAX_NUMBER_OF_PASSENGERS);
        Push(wagon);
        int maxPass = static_cast<int>(Call());
        if (maxPass < 0) maxPass = 0;
        if (maxPass > net::kMaxVehicleSeat + 1)
            maxPass = net::kMaxVehicleSeat + 1;
        for (int seat = 0; seat < maxPass; ++seat)
        {
            Init(natives::GET_PED_IN_VEHICLE_SEAT);
            Push(wagon);
            Push(static_cast<unsigned long long>(seat));
            addPed(static_cast<unsigned int>(Call()), seat,
                   net::kOccupantFlagPassenger);
        }
    }
    auto sameEntries = [](const std::vector<net::VehicleOccupantEntry>& a,
                          const std::vector<net::VehicleOccupantEntry>& b) {
        if (a.size() != b.size())
            return false;
        for (std::size_t i = 0; i < a.size(); ++i)
        {
            if (a[i].actorId != b[i].actorId || a[i].seat != b[i].seat ||
                a[i].flags != b[i].flags || a[i].carIndex != b[i].carIndex ||
                a[i].harness != b[i].harness)
                return false;
        }
        return true;
    };
    auto last = g_lastTrainOccupants.find(trainId);
    const bool changed = last == g_lastTrainOccupants.end() ||
                         !sameEntries(last->second, all);
    const unsigned long long occNowMs = GetTickCount64();
    const unsigned long long lastMs = g_lastTrainOccupantsMs[trainId];
    if (!changed && lastMs != 0 && occNowMs - lastMs < 1000ull)
        return;
    if (all.empty())
    {
        g_lastTrainOccupants[trainId] = all;
        g_lastTrainOccupantsMs[trainId] = occNowMs;
        return;
    }
    // Chunks <=8; сервер rate 20/с + общий throttle.
    for (std::size_t off = 0; off < all.size();
         off += net::kMaxVehicleOccupants)
    {
        if (g_occupantsSendWindowMs == 0 ||
            occNowMs - g_occupantsSendWindowMs >= 1000ull)
        {
            g_occupantsSendWindowMs = occNowMs;
            g_occupantsSendWindowCount = 0;
        }
        if (g_occupantsSendWindowCount >= 18)
            break;
        ++g_occupantsSendWindowCount;
        const std::size_t n = std::min<std::size_t>(
            net::kMaxVehicleOccupants, all.size() - off);
        std::vector<net::VehicleOccupantEntry> chunk(all.begin() + off,
                                                     all.begin() + off + n);
        coop::SendVehicleOccupants(trainId, chunk);
    }
    g_lastTrainOccupants[trainId] = all;
    g_lastTrainOccupantsMs[trainId] = occNowMs;
    if (coopcfg::Get().npcDebug)
        sh_log::Write("world: train occupants train=%u cars=%d n=%u chunks=%u\n",
                      trainId, cars, static_cast<unsigned int>(all.size()),
                      static_cast<unsigned int>((all.size() +
                          net::kMaxVehicleOccupants - 1) /
                          net::kMaxVehicleOccupants));
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
                              tickCount, engine, dist))
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
                    g_lastTrainSendMs.erase(idIt->second);
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
        const unsigned long long ttlNowMs = GetTickCount64();
        for (auto it = g_lastTrainSendMs.begin();
             it != g_lastTrainSendMs.end();)
        {
            bool alive = false;
            for (const auto& kv : g_trainIds)
                if (kv.second == it->first) { alive = true; break; }
            if (!alive || ttlNowMs - it->second > 900000ull)
                it = g_lastTrainSendMs.erase(it);
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
    // E11: точные пассажиры/машинист (0x21, carIndex).
    SendTrainOccupants(engine, EnsureTrainId(engine), cars, tickCount);
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
    const unsigned long long trainNowMs = GetTickCount64();
    const unsigned long long lastMs = g_lastTrainSendMs[snap.trainId];
    // 0x22 <=4/с wall-clock (250 мс), независимо от FPS.
    if (lastMs != 0 && trainNowMs - lastMs < 250ull)
        return;
    (void)changed;
    coop::SendTrainSnapshotV2(snap);
    g_lastTrainSnapshot[snap.trainId] = snap;
    g_lastTrainSendMs[snap.trainId] = trainNowMs;
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

// E11 (task-149): exact train occupants (0x21 carIndex) для mission-train гостя.
std::atomic<uint32_t> g_guestTrainNetId{0}; // trainId для callback routing
std::mutex g_trainOccMutex;
std::vector<net::VehicleOccupantEntry> g_trainOccInbox;
struct TrainSeatState
{
    uint32_t actorId = 0;
    int8_t seat = -1;
    uint8_t flags = 0;
    uint8_t carIndex = 0;
    unsigned int attempts = 0;
    unsigned int firstTick = 0;
    unsigned int nextRetryTick = 0;
    bool applied = false;
};
std::vector<TrainSeatState> g_trainSeats;
unsigned int g_trainSeatsTrainHandle = 0;
unsigned int g_perfGuestTrainSeatsApplied = 0;
unsigned int g_perfGuestTrainSeatsFailed = 0;
bool g_guestTrainAutoPopulationOff = false;

void RouteTrainOccupants(const std::vector<net::VehicleOccupantEntry>& entries)
{
    std::lock_guard<std::mutex> lk(g_trainOccMutex);
    g_trainOccInbox.insert(g_trainOccInbox.end(), entries.begin(),
                           entries.end());
    if (g_trainOccInbox.size() > 128)
        g_trainOccInbox.erase(g_trainOccInbox.begin(),
                              g_trainOccInbox.begin() +
                                  (g_trainOccInbox.size() - 128));
}

bool IsPedInVehicleForTrain(unsigned int ped, unsigned int veh)
{
    using namespace natives_api;
    Init(natives::IS_PED_IN_VEHICLE);
    Push(ped);
    Push(veh);
    Push(false);
    return Call() != 0;
}

void SetTrainEntityVisible(unsigned int handle, bool visible)
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

// Снять seated-флаги у точных пассажиров (despawn/role change/recreate).
void ClearGuestTrainOccupants(bool clearPending)
{
    for (TrainSeatState& st : g_trainSeats)
    {
        if (st.applied)
            actor_sync::ClearActorSeated(st.actorId);
        st.applied = false;
        st.attempts = 0;
        st.nextRetryTick = 0;
    }
    g_trainSeatsTrainHandle = 0;
    g_guestTrainAutoPopulationOff = false;
    if (clearPending)
    {
        g_trainSeats.clear();
        std::lock_guard<std::mutex> lk(g_trainOccMutex);
        g_trainOccInbox.clear();
        g_guestTrainNetId.store(0);
    }
}

void ProcessTrainOccupants(unsigned int tickCount)
{
    if (!coopcfg::Get().npcActor || !coopcfg::Get().npcOccupants)
        return;
    std::vector<net::VehicleOccupantEntry> inbox;
    {
        std::lock_guard<std::mutex> lk(g_trainOccMutex);
        inbox.swap(g_trainOccInbox);
    }
    for (const net::VehicleOccupantEntry& e : inbox)
    {
        if (!e.actorId)
            continue;
        bool found = false;
        for (TrainSeatState& st : g_trainSeats)
        {
            if (st.actorId == e.actorId)
            {
                st.seat = e.seat;
                st.flags = e.flags;
                st.carIndex = e.carIndex;
                found = true;
                break;
            }
        }
        if (!found)
        {
            TrainSeatState st;
            st.actorId = e.actorId;
            st.seat = e.seat;
            st.flags = e.flags;
            st.carIndex = e.carIndex;
            st.firstTick = tickCount;
            g_trainSeats.push_back(st);
        }
    }
    if (g_trainSeats.empty())
        return;
    if (!g_guestTrain)
        return; // ждём mission-train; heartbeat хоста повторит
    if (g_trainSeatsTrainHandle != g_guestTrain)
    {
        // Train пересоздан (config/carCount/role) — переприменить.
        for (TrainSeatState& st : g_trainSeats)
            st.applied = false;
        g_trainSeatsTrainHandle = g_guestTrain;
    }

    int budget = 2; // нативов/attach операций на тик
    for (std::size_t idx = 0; idx < g_trainSeats.size(); ++idx)
    {
        TrainSeatState& st = g_trainSeats[idx];
        if (st.applied || budget <= 0)
        {
            if (st.applied && (tickCount % 10) == 0)
            {
                unsigned int wagon = 0;
                using namespace natives_api;
                Init(natives::GET_TRAIN_CARRIAGE);
                Push(g_guestTrain);
                Push(static_cast<unsigned long long>(st.carIndex));
                wagon = static_cast<unsigned int>(Call());
                unsigned int actor = 0;
                if (wagon && DoesExist(wagon) &&
                    actor_sync::GetHandleForActorId(st.actorId, actor) &&
                    !IsPedInVehicleForTrain(actor, wagon))
                {
                    st.applied = false;
                    st.attempts = 0;
                }
            }
            continue;
        }
        if (st.nextRetryTick != 0 && tickCount < st.nextRetryTick)
            continue;
        unsigned int actor = 0;
        if (!actor_sync::GetHandleForActorId(st.actorId, actor) ||
            !DoesExist(actor))
        {
            if (tickCount - st.firstTick > 180)
            {
                ++g_perfGuestTrainSeatsFailed;
                if (coopcfg::Get().npcDebug)
                    sh_log::Write(
                        "actor: train seat attach actor=%u car=%u seat=%d "
                        "ok=0 (no actor)\n",
                        st.actorId, st.carIndex, st.seat);
                g_trainSeats.erase(g_trainSeats.begin() +
                                   static_cast<std::ptrdiff_t>(idx));
                --idx;
            }
            continue;
        }
        unsigned int wagon = 0;
        using namespace natives_api;
        Init(natives::GET_TRAIN_CARRIAGE);
        Push(g_guestTrain);
        Push(static_cast<unsigned long long>(st.carIndex));
        wagon = static_cast<unsigned int>(Call());
        if (!wagon || !DoesExist(wagon))
        {
            if (tickCount - st.firstTick > 120)
            {
                ++g_perfGuestTrainSeatsFailed;
                if (coopcfg::Get().npcDebug)
                    sh_log::Write(
                        "actor: train seat attach actor=%u car=%u seat=%d "
                        "ok=0 (no wagon)\n",
                        st.actorId, st.carIndex, st.seat);
                g_trainSeats.erase(g_trainSeats.begin() +
                                   static_cast<std::ptrdiff_t>(idx));
                --idx;
            }
            continue;
        }
        // Точные пассажиры пришли — авто-население вагонов больше не нужно.
        if (!g_guestTrainAutoPopulationOff)
        {
            g_guestTrainAutoPopulationOff = true;
            Init(natives::_GET_TRAIN_CARRIAGE_TRAILER_NUMBER);
            Push(g_guestTrain);
            const int trailers = static_cast<int>(Call());
            for (int car = 0; car <= trailers; ++car)
            {
                Init(natives::GET_TRAIN_CARRIAGE);
                Push(g_guestTrain);
                Push(static_cast<unsigned long long>(car));
                const unsigned int w = static_cast<unsigned int>(Call());
                if (w && DoesExist(w))
                {
                    Init(natives::_FORCE_TRAIN_WAGON_POPULATION);
                    Push(w); Push(false); Call();
                }
            }
        }
        SetTrainEntityVisible(actor, false);
        Init(natives::SET_PED_INTO_VEHICLE);
        Push(actor);
        Push(wagon);
        Push(static_cast<unsigned long long>(static_cast<long long>(st.seat)));
        Call();
        if (IsPedInVehicleForTrain(actor, wagon))
        {
            SetTrainEntityVisible(actor, true);
            actor_sync::SetActorSeated(st.actorId, wagon, st.seat, st.flags);
            st.applied = true;
            st.attempts = 0;
            ++g_perfGuestTrainSeatsApplied;
            if (coopcfg::Get().npcDebug)
                sh_log::Write(
                    "actor: train seat attach actor=%u car=%u seat=%d ok=1\n",
                    st.actorId, st.carIndex, st.seat);
        }
        else
        {
            ++st.attempts;
            st.nextRetryTick = tickCount + 30; // ~0.5 с
            if (st.attempts >= 3)
            {
                SetTrainEntityVisible(actor, true); // не оставляем invisible
                ++g_perfGuestTrainSeatsFailed;
                if (coopcfg::Get().npcDebug)
                    sh_log::Write(
                        "actor: train seat attach actor=%u car=%u seat=%d "
                        "ok=0\n",
                        st.actorId, st.carIndex, st.seat);
                g_trainSeats.erase(g_trainSeats.begin() +
                                   static_cast<std::ptrdiff_t>(idx));
                --idx;
            }
        }
        --budget;
    }
}

void DeleteGuestMissionTrain(unsigned int reason)
{
    ClearGuestTrainOccupants(false);
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
    g_guestTrainAutoPopulationOff = false;
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
            g_guestTrainNetId.store(g_pendingTrainV2.trainId);
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
    // Диагностика host-ветки: доказать, что снапшоты и occupants реально
    // считаются, и какие конфиг-флаги действуют (npc_debug может быть выключен).
    {
        static unsigned long long s_lastHostWorldLogMs = 0;
        const unsigned long long nowMs = GetTickCount64();
        if (s_lastHostWorldLogMs == 0 || nowMs - s_lastHostWorldLogMs >= 2000ull)
        {
            s_lastHostWorldLogMs = nowMs;
            sh_log::Write("world: host snapshot vehOnly=%d npcDebug=%d "
                          "npcActor=%d npcOcc=%d\n",
                          vehiclesOnly ? 1 : 0,
                          coopcfg::Get().npcDebug ? 1 : 0,
                          coopcfg::Get().npcActor ? 1 : 0,
                          coopcfg::Get().npcOccupants ? 1 : 0);
        }
    }

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

        const uint32_t netId = EnsureHostNetId(handle, model, tickCount,
                                            net::kEntityKindPed);
        net::WorldEntity e{};
        e.netId = netId;
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
        EnsureHostVehicleScan(tickCount);
        const std::vector<unsigned int>& vehicleHandles = g_vehicleScanHandles;
        int vehicleCount = static_cast<int>(vehicleHandles.size());
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

            const uint32_t netId = EnsureHostNetId(handle, model, tickCount,
                                            net::kEntityKindVehicle);
            net::WorldEntity e{};
            e.netId = netId;
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

            const uint32_t netId = EnsureHostNetId(handle, model, tickCount,
                                            net::kEntityKindVehicle);
            net::WorldEntity e{};
            e.netId = netId;
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

            const uint32_t netId = EnsureHostNetId(handle, model, tickCount,
                                            net::kEntityKindObject);
            net::WorldEntity e{};
            e.netId = netId;
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
    // P1-5 (task-156): occupants собираем 1/с (было 10/с на каждом скане);
    // handles остаются прошлыми между сборами, heartbeats/изменения — 1 Гц.
    const unsigned long long occScanNowMs = GetTickCount64();
    if (coopcfg::Get().npcActor && coopcfg::Get().npcOccupants &&
        (g_lastOccupantsCollectMs == 0 ||
         occScanNowMs - g_lastOccupantsCollectMs >= 1000ull))
    {
        g_lastOccupantsCollectMs = occScanNowMs;
        g_lastOccupantsCollectTick = tickCount;
        std::vector<unsigned int> occupantHandles;
        std::unordered_set<unsigned int> probedVehicles;
        int sendBudget = 6; // не флудим 0x21 в одном скане
        for (const Candidate& c : candidates)
        {
            if (c.entity.kind != net::kEntityKindVehicle)
                continue;
            unsigned int veh = 0;
            if (!FindHostHandleByNetId(c.entity.netId, veh))
                continue;
            probedVehicles.insert(veh);
            std::vector<net::VehicleOccupantEntry> entries;
            uint32_t driverActorId = 0;
            const std::vector<unsigned int> hs = CollectVehicleOccupants(
                veh, localPlayerPed, c.entity.netId, entries, driverActorId);
            occupantHandles.insert(occupantHandles.end(), hs.begin(), hs.end());

            auto sent = g_lastOccupantsSent.find(c.entity.netId);
            const bool changed = (sent == g_lastOccupantsSent.end())
                                     ? !entries.empty()
                                     : !OccupantsEqual(sent->second, entries);
            const unsigned long long lastMs =
                g_lastOccupantsSendMs[c.entity.netId];
            const bool heartbeat =
                !entries.empty() &&
                (occScanNowMs - lastMs) >= 1000ull; // ~1 c wall-clock
            if ((!changed && !heartbeat) || sendBudget <= 0)
                continue;
            if (g_occupantsSendWindowMs == 0 ||
                occScanNowMs - g_occupantsSendWindowMs >= 1000ull)
            {
                g_occupantsSendWindowMs = occScanNowMs;
                g_occupantsSendWindowCount = 0;
            }
            if (g_occupantsSendWindowCount >= 18)
                continue; // сервер всё равно rate-limit'ит 20/с
            ++g_occupantsSendWindowCount;
            coop::SendVehicleOccupants(c.entity.netId, entries);
            g_lastOccupantsSent[c.entity.netId] = entries;
            g_lastOccupantsSendMs[c.entity.netId] = occScanNowMs;
            g_lastOccupantsSendTick[c.entity.netId] = tickCount;
            --sendBudget;
            if (coopcfg::Get().npcDebug)
                sh_log::Write(
                    "world: occupants vehicle=%u n=%u driver=%u\n",
                    c.entity.netId, static_cast<unsigned int>(entries.size()),
                    driverActorId);
        }
        // Allowlist completeness (root-cause fix): actor discovery скипает
        // in-vehicle ped, которого нет в g_occupantHandles (P2.9), а handles
        // сейчас собираются только для vehicle, выбранных в снапшот (budget/
        // радиус). Если vehicle не попал в снапшот, его occupant-пед никогда
        // не получает actorId -> 0x21 навсегда неполный (n=1 driver=0) ->
        // гость не видит кучера/draft-лошадей. Поэтому в allowlist добавляем
        // handles occupants ВСЕХ vehicle в occ-радиусе (тем же radius, что в
        // discovery). Ничего не шлём и не создаём — только allowlist.
        {
            float occRadius = static_cast<float>(coopcfg::Get().npcVehicleRadius);
            const float cfgNpcRadius =
                static_cast<float>(coopcfg::Get().npcRadius);
            if (cfgNpcRadius > occRadius) occRadius = cfgNpcRadius;
            const float occRadius2 = occRadius * occRadius;
            unsigned int localVehicle = 0;
            {
                using namespace natives_api;
                Init(natives::GET_VEHICLE_PED_IS_USING);
                Push(localPlayerPed);
                localVehicle = static_cast<unsigned int>(Call());
            }
            for (unsigned int veh : g_vehicleScanHandles)
            {
                if (!veh || veh == localVehicle ||
                    probedVehicles.count(veh) != 0)
                    continue;
                if (coop::IsWorldSyncExcluded(veh) || !DoesExist(veh))
                    continue;
                const Vec3 p = GetCoords(veh);
                if (dist2ToPlayers(p.x, p.y) > occRadius2)
                    continue;
                const unsigned int model = GetModel(veh);
                if (!model || IsTrainModel(model))
                    continue; // поезд — отдельный путь (0x22 + train 0x21)
                std::vector<net::VehicleOccupantEntry> ignored;
                uint32_t ignoredDriver = 0;
                const std::vector<unsigned int> hs = CollectVehicleOccupants(
                    veh, localPlayerPed, 0, ignored, ignoredDriver);
                occupantHandles.insert(occupantHandles.end(), hs.begin(),
                                       hs.end());
            }
        }
        // E11: handles нужны и для train-passengers (TickHostTrains добавит).
        g_hostOccHandles = std::move(occupantHandles);
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
        // P1-6 (task-156): TTL host-кэшей occupants (despawn netId не чистился
        // в host-путях).
        for (auto it = g_lastOccupantsSent.begin();
             it != g_lastOccupantsSent.end();)
        {
            auto st = g_lastOccupantsSendTick.find(it->first);
            const unsigned int last =
                st == g_lastOccupantsSendTick.end() ? 0 : st->second;
            if (last == 0 || tickCount - last > 300)
                it = g_lastOccupantsSent.erase(it);
            else
                ++it;
        }
        for (auto it = g_lastOccupantsSendTick.begin();
             it != g_lastOccupantsSendTick.end();)
        {
            if (g_lastOccupantsSent.count(it->first) == 0 ||
                tickCount - it->second > 300)
                it = g_lastOccupantsSendTick.erase(it);
            else
                ++it;
        }
    }
}

// ================= v20 P1: host apply (vehicle interactions) =================
// Гость шлёт интент 0x23; хост валидирует (docs/INTERACTIVITY_P1.md §4.2),
// применяет ОДНУ натив-группу, читает state (docs/NATIVE_RESEARCH_V20_HEALTH.md
// §6) и отвечает 0x24 + 0x26. Всё — только игровой поток; сетевой поток лишь
// копирует payload в очередь под mutex.
constexpr int kHostIntentQueueCap = 8;
constexpr int kHostIntentOverflowCap = 4;
constexpr int kHostIntentsPerTick = 2; // perf-гейт: <=2 интента/тик
constexpr unsigned long long kIntentCooldownMs = 250ull;
constexpr unsigned long long kIntentCooldownUrgentMs = 100ull;
constexpr unsigned long long kIntentIdempotencyMs = 2000ull;
constexpr std::size_t kIntentResultCacheCap = 32;
// L3 (spec §2.6/§4.2): держим до 8 tracked vehicle states (было 4) — host шлёт
// heartbeat 1/с на каждый, окно kStateSendWindowCap=8/с и серверный лимит 10/с
// не превышаются; dirty-события сверх окна уйдут следующим тиком.
constexpr std::size_t kTrackedVehicleStates = 8;
constexpr uint32_t kDamageBudgetPerSec = 200; // HP/с на vehicle
constexpr unsigned long long kStateHeartbeatMs = 1000ull;
constexpr unsigned int kStateSendWindowCap = 8; // <=10/с лимит сервера
constexpr float kInteractionDamageRadius = 12.f;
constexpr float kInteractionSeatRadius = 6.f;
constexpr float kInteractionDoorRadius = 5.f;
constexpr float kInteractionDraftRadius = 5.f;
constexpr float kInteractionHeightLimit = 3.f;
constexpr uint16_t kDoorBreakDamageThreshold = 200; // A/B: damage -> broken
// A/B-гейты: lock enum/поведение не подтверждены -> lock/unlock в P1 всегда
// NotAllowed; setter'ы не вызываются до A/B (§4.2/R&D §4.2, D8).
constexpr bool kVehicleInteractionLocksEnabled = false;
// F3: detach колеса (_BREAK_OFF_VEHICLE_WHEEL) — one-way, mapping wheelIndex
// НЕ подтверждён (R&D §2.4/§10.3). В P1 detach ВЫКЛ: damage по зоне колеса
// применяется, но Applied только если destroyed-readback подтвердил эффект;
// иначе Correction (без one-way разрушения).
constexpr bool kVehicleInteractionBreakWheelDetach = false;
// F4: DoorBreak — one-way (deleteDoor), host-side A/B-гейт (capture-гейт в
// coop.cpp не защищает от fake/чужого клиента). До A/B (§4.3/§10.3) host
// отвечает NotAllowed(reason=policy) и натив НЕ вызывает.
constexpr bool kVehicleInteractionDoorBreak = false;

const char* VehicleActionName(uint8_t action)
{
    switch (static_cast<net::VehicleAction>(action))
    {
    case net::VehicleAction::Damage: return "damage";
    case net::VehicleAction::DoorOpen: return "door_open";
    case net::VehicleAction::DoorClose: return "door_close";
    case net::VehicleAction::DoorLock: return "door_lock";
    case net::VehicleAction::DoorUnlock: return "door_unlock";
    case net::VehicleAction::DoorBreak: return "door_break";
    case net::VehicleAction::Enter: return "enter";
    case net::VehicleAction::Exit: return "exit";
    case net::VehicleAction::Hijack: return "hijack";
    case net::VehicleAction::Loot: return "loot";
    case net::VehicleAction::Hitch: return "hitch";
    case net::VehicleAction::Unhitch: return "unhitch";
    default: return "none";
    }
}

const char* VehicleResultName(uint8_t result)
{
    switch (static_cast<net::VehicleResult>(result))
    {
    case net::VehicleResult::Applied: return "applied";
    case net::VehicleResult::NotAllowed: return "not_allowed";
    case net::VehicleResult::StaleEpoch: return "stale_epoch";
    case net::VehicleResult::NotFound: return "not_found";
    case net::VehicleResult::OutOfRange: return "out_of_range";
    case net::VehicleResult::SeatOccupied: return "seat_occupied";
    case net::VehicleResult::Locked: return "locked";
    case net::VehicleResult::RateLimited: return "rate_limited";
    case net::VehicleResult::Timeout: return "timeout";
    case net::VehicleResult::Correction: return "correction";
    default: return "unknown";
    }
}

float VehicleIntentRadius(uint8_t action)
{
    switch (static_cast<net::VehicleAction>(action))
    {
    case net::VehicleAction::Damage: return kInteractionDamageRadius;
    case net::VehicleAction::Enter:
    case net::VehicleAction::Exit:
    case net::VehicleAction::Hijack: return kInteractionSeatRadius;
    case net::VehicleAction::Hitch:
    case net::VehicleAction::Unhitch: return kInteractionDraftRadius;
    default: return kInteractionDoorRadius;
    }
}

// Host-кеш per vehicle: последний state, cooldown/budget, dirty для 0x26.
struct HostVehicleRuntime
{
    net::VehicleStatePayload lastState{};
    bool hasState = false;
    bool dirty = false;
    bool hijacked = false;
    bool hijackSeatPending = false;
    unsigned long long lastSentStateMs = 0;
    unsigned long long lastActionMs[13] = {}; // индекс = VehicleAction 1..12
    unsigned long long damageWindowMs = 0;
    uint32_t damageBudgetUsed = 0;
    uint16_t stateSeq = 0;
};

std::map<uint32_t, HostVehicleRuntime> g_hostVehicleRuntime;

// v20 P1 F2: seq 0x26 per netId — persistent-карта, НЕ вытесняется вместе с
// tracked-runtime (>kTrackedVehicleStates). Иначе при повторном добавлении
// netId в tracked-окно host начинал seq с 0→1, а гость (guard монотонности)
// дропал state до тех пор, пока новый seq не перегонит старый. Wire не
// расширяем (reserved 0x26 не трогаем): generation живёт host-side, гость
// дополнительно терпит restart (wrap/пауза >1.5 с, см. ApplyVehicleStateToProxy).
std::map<uint32_t, uint16_t> g_vehicleStateSeq;
constexpr std::size_t kVehicleStateSeqCap = 128;
uint16_t NextVehicleStateSeq(uint32_t netId)
{
    uint16_t& seq = g_vehicleStateSeq[netId];
    ++seq;
    if (seq == 0) ++seq; // seq=0 = «нет данных»; не отдаём
    if (g_vehicleStateSeq.size() > kVehicleStateSeqCap)
    {
        // netId монотонны и за сессию не переиспользуются — старейшую запись
        // (по возрастанию netId) безопасно выбросить.
        auto it = g_vehicleStateSeq.begin();
        if (it->first != netId)
            g_vehicleStateSeq.erase(it);
    }
    return seq;
}

// 0x23 inbox: сетевой поток -> очередь; overflow -> RateLimited.
std::mutex g_vehicleIntentsMutex;
std::vector<net::VehicleInteractionPayload> g_vehicleIntents;
std::vector<net::VehicleInteractionPayload> g_vehicleIntentOverflow;

// Idempotency: тот же (epoch,netId,seq) в пределах 2 с -> тот же result.
struct CachedIntentResult
{
    uint32_t epoch = 0;
    uint32_t netId = 0;
    uint16_t seq = 0;
    unsigned long long ms = 0;
    net::VehicleInteractionResultPayload result{};
};
std::vector<CachedIntentResult> g_intentResultCache;
unsigned long long g_vehicleInteractionLastLogMs = 0;

// ---------------- host native helpers (readback/apply) ----------------
float GetVehicleBodyHealthNative(unsigned int veh)
{
    using namespace natives_api;
    Init(natives::GET_VEHICLE_BODY_HEALTH);
    Push(veh);
    return CallFloat();
}

float GetVehicleEngineHealthNative(unsigned int veh)
{
    using namespace natives_api;
    Init(natives::GET_VEHICLE_ENGINE_HEALTH);
    Push(veh);
    return CallFloat();
}

bool IsEntityAVehicleNative(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::IS_ENTITY_A_VEHICLE);
    Push(handle);
    return Call() != 0;
}

bool IsEntityDeadNative(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::IS_ENTITY_DEAD);
    Push(handle);
    return Call() != 0;
}

bool IsVehicleWreckedNative(unsigned int veh)
{
    using namespace natives_api;
    Init(natives::IS_VEHICLE_WRECKED);
    Push(veh);
    return Call() != 0;
}

bool IsVehicleDoorFullyOpenNative(unsigned int veh, int door)
{
    using namespace natives_api;
    Init(natives::IS_VEHICLE_DOOR_FULLY_OPEN);
    Push(veh);
    Push(door);
    return Call() != 0;
}

bool IsVehicleDoorBrokenNative(unsigned int veh, int door)
{
    using namespace natives_api;
    Init(natives::_IS_VEHICLE_DOOR_BROKEN);
    Push(veh);
    Push(door);
    return Call() != 0;
}

bool IsVehicleWheelDestroyedNative(unsigned int veh, int wheel)
{
    using namespace natives_api;
    Init(natives::_IS_VEHICLE_WHEEL_DESTROYED);
    Push(veh);
    Push(wheel);
    return Call() != 0;
}

bool AreAnyVehicleWheelsDestroyedNative(unsigned int veh)
{
    using namespace natives_api;
    Init(natives::_ARE_ANY_VEHICLE_WHEELS_DESTROYED);
    Push(veh);
    return Call() != 0;
}

bool IsVehicleSeatFreeNative(unsigned int veh, int seat)
{
    using namespace natives_api;
    Init(natives::IS_VEHICLE_SEAT_FREE);
    Push(veh);
    Push(static_cast<unsigned long long>(static_cast<long long>(seat)));
    return Call() != 0;
}

unsigned int GetPedInVehicleSeatNative(unsigned int veh, int seat)
{
    using namespace natives_api;
    Init(natives::GET_PED_IN_VEHICLE_SEAT);
    Push(veh);
    Push(static_cast<unsigned long long>(static_cast<long long>(seat)));
    return static_cast<unsigned int>(Call());
}

int GetVehicleMaxPassengersNative(unsigned int veh)
{
    using namespace natives_api;
    Init(natives::GET_VEHICLE_MAX_NUMBER_OF_PASSENGERS);
    Push(veh);
    int maxPass = static_cast<int>(Call());
    if (maxPass < 0) maxPass = 0;
    if (maxPass > net::kMaxVehicleSeat + 1) maxPass = net::kMaxVehicleSeat + 1;
    return maxPass;
}

unsigned int GetDriverOfVehicleNative(unsigned int veh)
{
    using namespace natives_api;
    Init(natives::GET_DRIVER_OF_VEHICLE);
    Push(veh);
    return static_cast<unsigned int>(Call());
}

unsigned int GetVehiclePedIsUsingNative(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::GET_VEHICLE_PED_IS_USING);
    Push(ped);
    return static_cast<unsigned int>(Call());
}

bool IsPedDeadOrDyingNative(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::IS_PED_DEAD_OR_DYING);
    Push(ped);
    return Call() != 0;
}

bool GetHarnessPedNative(unsigned int veh, int harness, unsigned int& horse)
{
    using namespace natives_api;
    Init(natives::_GET_PED_IN_DRAFT_HARNESS);
    Push(veh);
    Push(static_cast<unsigned long long>(static_cast<long long>(harness)));
    horse = static_cast<unsigned int>(Call());
    return horse != 0;
}

bool GetDraftAnimalCountsNative(unsigned int veh, int& expected, int& actual)
{
    using namespace natives_api;
    expected = 0;
    actual = 0;
    Init(natives::GET_DRAFT_ANIMAL_COUNT);
    Push(veh);
    Push(reinterpret_cast<unsigned long long>(&expected));
    Push(reinterpret_cast<unsigned long long>(&actual));
    return Call() != 0;
}

// Хост real vehicle: минимальный набор флагов, чтобы damage/door проходили
// (R&D §2.5; mission-vehicle по умолчанию reject'ится политикой).
void PrepareHostVehicleForDamage(unsigned int veh)
{
    using namespace natives_api;
    SetInvincible(veh, false);
    Init(natives::SET_ENTITY_CAN_BE_DAMAGED);
    Push(veh);
    Push(true);
    Call();
    Init(natives::SET_VEHICLE_CAN_BE_VISIBLY_DAMAGED);
    Push(veh);
    Push(true);
    Call();
    Init(natives::SET_VEHICLE_CAN_BREAK);
    Push(veh);
    Push(true);
    Call();
    Init(natives::SET_VEHICLE_STRONG);
    Push(veh);
    Push(false);
    Call();
}

int FindSeatForPedNative(unsigned int veh, unsigned int ped)
{
    if (!veh || !ped || GetVehiclePedIsUsingNative(ped) != veh)
        return net::kVehicleSeatNone;
    const int maxPass = GetVehicleMaxPassengersNative(veh);
    for (int s = -1; s < maxPass; ++s)
    {
        if (GetPedInVehicleSeatNative(veh, s) == ped)
            return s;
    }
    return net::kVehicleSeatNone;
}

// Локальные (модельные) offset'ы для SET_VEHICLE_DAMAGE. A/B: у wagon своя
// геометрия; стартовые значения — body/engine из R&D, door/wheel — приближение.
void DoorDamageOffset(uint8_t door, float& x, float& y, float& z)
{
    const bool rightSide = door >= 5;
    y = rightSide ? 0.9f : -0.9f;
    x = (static_cast<float>(door % 5) - 2.f) * 0.55f;
    z = 0.4f;
}

void WheelDamageOffset(uint8_t part, float& x, float& y, float& z)
{
    const bool right = (part == net::kVehiclePartWheelFR ||
                        part == net::kVehiclePartWheelRR);
    const bool front = (part == net::kVehiclePartWheelFL ||
                        part == net::kVehiclePartWheelFR);
    x = front ? 1.2f : -1.2f;
    y = right ? 0.8f : -0.8f;
    z = -0.35f;
}

net::VehicleStatePayload BuildVehicleState(uint32_t netId, unsigned int veh,
                                           unsigned int localPlayerPed,
                                           unsigned int ghost,
                                           HostVehicleRuntime& rt);

void SendInteractionResult(const net::VehicleInteractionPayload& p,
                           uint8_t result, uint32_t detail)
{
    net::VehicleInteractionResultPayload r{};
    r.epoch = p.epoch;
    r.vehicleNetId = p.vehicleNetId;
    r.seq = p.seq;
    r.action = p.action;
    r.result = result;
    r.detail = detail;
    coop::SendVehicleInteractionResult(r);
    CachedIntentResult c;
    c.epoch = p.epoch;
    c.netId = p.vehicleNetId;
    c.seq = p.seq;
    c.ms = GetTickCount64();
    c.result = r;
    g_intentResultCache.push_back(c);
    if (g_intentResultCache.size() > kIntentResultCacheCap)
        g_intentResultCache.erase(g_intentResultCache.begin());
}

const net::VehicleInteractionResultPayload* FindCachedIntentResult(
    const net::VehicleInteractionPayload& p)
{
    const unsigned long long now = GetTickCount64();
    for (std::size_t i = g_intentResultCache.size(); i-- > 0;)
    {
        CachedIntentResult& c = g_intentResultCache[i];
        if (now - c.ms > kIntentIdempotencyMs)
        {
            g_intentResultCache.erase(g_intentResultCache.begin() +
                                      static_cast<std::ptrdiff_t>(i));
            continue;
        }
        if (c.epoch == p.epoch && c.netId == p.vehicleNetId && c.seq == p.seq)
            return &c.result;
    }
    return nullptr;
}

uint32_t NotAllowedDetail(uint8_t reason)
{
    return static_cast<uint32_t>(reason) << 8;
}

void MarkVehicleStateDirty(uint32_t netId)
{
    HostVehicleRuntime& rt = g_hostVehicleRuntime[netId];
    rt.dirty = true;
    while (g_hostVehicleRuntime.size() > kTrackedVehicleStates)
    {
        // Вытесняем самый давний tracked vehicle (кроме текущего).
        auto oldest = g_hostVehicleRuntime.end();
        for (auto it = g_hostVehicleRuntime.begin();
             it != g_hostVehicleRuntime.end(); ++it)
        {
            if (it->first == netId) continue;
            if (oldest == g_hostVehicleRuntime.end() ||
                it->second.lastSentStateMs < oldest->second.lastSentStateMs)
                oldest = it;
        }
        if (oldest == g_hostVehicleRuntime.end()) break;
        g_hostVehicleRuntime.erase(oldest);
    }
}

// Damage: SET_VEHICLE_DAMAGE + fallback health write; Applied только по
// фактическому readback (следующий тик для двери не ждём — openInstantly=true).
uint8_t ApplyDamageIntent(unsigned int veh, const net::VehicleInteractionPayload& p,
                          HostVehicleRuntime& rt, unsigned long long nowMs)
{
    using namespace natives_api;
    // budget 200 HP/с на vehicle
    if (rt.damageWindowMs == 0 || nowMs - rt.damageWindowMs >= 1000ull)
    {
        rt.damageWindowMs = nowMs;
        rt.damageBudgetUsed = 0;
    }
    if (rt.damageBudgetUsed + p.param > kDamageBudgetPerSec)
        return static_cast<uint8_t>(net::VehicleResult::RateLimited);
    rt.damageBudgetUsed += p.param;

    PrepareHostVehicleForDamage(veh);
    const uint16_t amount = p.param;
    const uint8_t part = p.door;
    if (part == net::kVehiclePartBody || part == net::kVehiclePartEngine)
    {
        const float beforeBody = GetVehicleBodyHealthNative(veh);
        const float beforeEngine = GetVehicleEngineHealthNative(veh);
        float ox = 0.f, oy = 0.f, oz = 0.5f;
        if (part == net::kVehiclePartEngine)
            oz = 1.0f;
        Init(natives::SET_VEHICLE_DAMAGE);
        Push(veh);
        Push(ox);
        Push(oy);
        Push(oz);
        Push(static_cast<float>(amount));
        Push(0.6f); // radius
        Push(false); // p6 UNCERTAIN -> false (A/B)
        Call();
        const float afterBody = GetVehicleBodyHealthNative(veh);
        const float afterEngine = GetVehicleEngineHealthNative(veh);
        if (part == net::kVehiclePartBody)
        {
            if (std::isfinite(afterBody) && afterBody < beforeBody - 0.5f)
                return static_cast<uint8_t>(net::VehicleResult::Applied);
            // fallback exact health write (SIGNATURE-ONLY range 0..1000 A/B)
            const float target = std::max(0.f, beforeBody - static_cast<float>(amount));
            Init(natives::SET_VEHICLE_BODY_HEALTH);
            Push(veh);
            Push(std::isfinite(beforeBody) ? target : 500.f);
            Call();
            const float written = GetVehicleBodyHealthNative(veh);
            return (std::isfinite(written) && written < beforeBody - 0.5f)
                       ? static_cast<uint8_t>(net::VehicleResult::Applied)
                       : static_cast<uint8_t>(net::VehicleResult::Correction);
        }
        if (std::isfinite(afterEngine) && afterEngine < beforeEngine - 0.5f)
            return static_cast<uint8_t>(net::VehicleResult::Applied);
        const float target = std::isfinite(beforeEngine)
                                 ? std::clamp(beforeEngine -
                                                  static_cast<float>(amount),
                                              -4000.f, 1000.f)
                                 : 500.f;
        Init(natives::SET_VEHICLE_ENGINE_HEALTH);
        Push(veh);
        Push(target);
        Call();
        const float written = GetVehicleEngineHealthNative(veh);
        return (std::isfinite(written) && written < beforeEngine - 0.5f)
                   ? static_cast<uint8_t>(net::VehicleResult::Applied)
                   : static_cast<uint8_t>(net::VehicleResult::Correction);
    }
    if (part <= 7) // дверь
    {
        const bool beforeOpen = IsVehicleDoorFullyOpenNative(veh, part);
        const bool beforeBroken = IsVehicleDoorBrokenNative(veh, part);
        float ox = 0.f, oy = 0.f, oz = 0.f;
        DoorDamageOffset(part, ox, oy, oz);
        Init(natives::SET_VEHICLE_DAMAGE);
        Push(veh);
        Push(ox);
        Push(oy);
        Push(oz);
        Push(static_cast<float>(amount));
        Push(0.5f);
        Push(false);
        Call();
        const bool broken = IsVehicleDoorBrokenNative(veh, part);
        const bool open = !broken && IsVehicleDoorFullyOpenNative(veh, part);
        if (broken != beforeBroken || open != beforeOpen)
            return static_cast<uint8_t>(net::VehicleResult::Applied);
        if (amount >= kDoorBreakDamageThreshold)
        {
            Init(natives::SET_DOOR_ALLOWED_TO_BE_BROKEN_OFF);
            Push(veh);
            Push(static_cast<unsigned long long>(part));
            Push(true);
            Call();
            Init(natives::SET_VEHICLE_DOOR_BROKEN);
            Push(veh);
            Push(static_cast<unsigned long long>(part));
            Push(false); // deleteDoor=false: keep part (A/B)
            Call();
            if (IsVehicleDoorBrokenNative(veh, part))
                return static_cast<uint8_t>(net::VehicleResult::Applied);
        }
        return static_cast<uint8_t>(net::VehicleResult::Correction);
    }
    if (part >= net::kVehiclePartWheelFL && part <= net::kVehiclePartWheelRR)
    {
        const int wheel = static_cast<int>(part - net::kVehiclePartWheelFL);
        const bool before = IsVehicleWheelDestroyedNative(veh, wheel);
        float ox = 0.f, oy = 0.f, oz = 0.f;
        WheelDamageOffset(part, ox, oy, oz);
        Init(natives::SET_VEHICLE_DAMAGE);
        Push(veh);
        Push(ox);
        Push(oy);
        Push(oz);
        Push(static_cast<float>(amount));
        Push(0.4f);
        Push(false);
        Call();
        if (!before && IsVehicleWheelDestroyedNative(veh, wheel))
            return static_cast<uint8_t>(net::VehicleResult::Applied);
        // F3: detach — one-way натив с неподтверждённым wheelIndex mapping.
        // По умолчанию выключен (A/B); damage уже применён, эффект нечитаем ->
        // Correction, чтобы гость не считал колесо оторванным.
        if (!kVehicleInteractionBreakWheelDetach)
            return static_cast<uint8_t>(net::VehicleResult::Correction);
        Init(natives::_BREAK_OFF_VEHICLE_WHEEL);
        Push(veh);
        Push(static_cast<unsigned long long>(wheel));
        Call();
        return IsVehicleWheelDestroyedNative(veh, wheel)
                   ? static_cast<uint8_t>(net::VehicleResult::Applied)
                   : static_cast<uint8_t>(net::VehicleResult::Correction);
    }
    return static_cast<uint8_t>(net::VehicleResult::NotAllowed);
}

uint8_t ApplyDoorIntent(unsigned int veh,
                        const net::VehicleInteractionPayload& p)
{
    using namespace natives_api;
    const int door = p.door;
    switch (static_cast<net::VehicleAction>(p.action))
    {
    case net::VehicleAction::DoorOpen:
        Init(natives::SET_VEHICLE_DOOR_OPEN);
        Push(veh);
        Push(static_cast<unsigned long long>(door));
        Push(false); // loose
        Push(true);  // openInstantly: детерминированный mirror
        Call();
        return IsVehicleDoorFullyOpenNative(veh, door)
                   ? static_cast<uint8_t>(net::VehicleResult::Applied)
                   : static_cast<uint8_t>(net::VehicleResult::Correction);
    case net::VehicleAction::DoorClose:
        Init(natives::SET_VEHICLE_DOOR_SHUT);
        Push(veh);
        Push(static_cast<unsigned long long>(door));
        Push(true); // closeInstantly
        Call();
        // false = closed ИЛИ ajar (R&D §2.3); точнее не читается.
        return IsVehicleDoorFullyOpenNative(veh, door)
                   ? static_cast<uint8_t>(net::VehicleResult::Correction)
                   : static_cast<uint8_t>(net::VehicleResult::Applied);
    case net::VehicleAction::DoorBreak:
        // F4: host-side A/B-гейт; натив one-way (deleteDoor) — до A/B не зовём.
        if (!kVehicleInteractionDoorBreak)
            return static_cast<uint8_t>(net::VehicleResult::NotAllowed);
        Init(natives::SET_DOOR_ALLOWED_TO_BE_BROKEN_OFF);
        Push(veh);
        Push(static_cast<unsigned long long>(door));
        Push(true);
        Call();
        Init(natives::SET_VEHICLE_DOOR_BROKEN);
        Push(veh);
        Push(static_cast<unsigned long long>(door));
        Push(true); // deleteDoor (d1: one-way)
        Call();
        return IsVehicleDoorBrokenNative(veh, door)
                   ? static_cast<uint8_t>(net::VehicleResult::Applied)
                   : static_cast<uint8_t>(net::VehicleResult::Correction);
    default:
        break;
    }
    return static_cast<uint8_t>(net::VehicleResult::NotAllowed);
}

uint8_t ApplySeatIntent(unsigned int veh,
                        const net::VehicleInteractionPayload& p,
                        unsigned int ghost, unsigned int localPlayerPed,
                        HostVehicleRuntime& rt)
{
    using namespace natives_api;
    switch (static_cast<net::VehicleAction>(p.action))
    {
    case net::VehicleAction::Enter:
    {
        int seat = p.seat;
        if (seat == net::kVehicleSeatNone)
        {
            // 127 = first free passenger (driver -1 в P1 только по политике).
            const int maxPass = GetVehicleMaxPassengersNative(veh);
            seat = net::kVehicleSeatNone;
            for (int s = 0; s < maxPass; ++s)
            {
                if (IsVehicleSeatFreeNative(veh, s))
                {
                    seat = s;
                    break;
                }
            }
            if (seat == net::kVehicleSeatNone)
                return static_cast<uint8_t>(net::VehicleResult::SeatOccupied);
        }
        else if (seat >= 0)
        {
            if (seat >= GetVehicleMaxPassengersNative(veh))
                return static_cast<uint8_t>(net::VehicleResult::NotAllowed);
            if (!IsVehicleSeatFreeNative(veh, seat))
                return static_cast<uint8_t>(net::VehicleResult::SeatOccupied);
        }
        else // seat == -1 (driver): host-vehicle policy проверена выше
        {
            if (!IsVehicleSeatFreeNative(veh, net::kVehicleSeatDriver))
                return static_cast<uint8_t>(net::VehicleResult::SeatOccupied);
        }
        Init(natives::SET_PED_INTO_VEHICLE);
        Push(ghost);
        Push(veh);
        Push(static_cast<unsigned long long>(static_cast<long long>(seat)));
        Call();
        const bool ok = GetPedInVehicleSeatNative(veh, seat) == ghost;
        return ok ? static_cast<uint8_t>(net::VehicleResult::Applied)
                  : static_cast<uint8_t>(net::VehicleResult::Correction);
    }
    case net::VehicleAction::Exit:
    {
        // Ghost должен быть в этом vehicle (иначе Exit — bad_param).
        if (GetVehiclePedIsUsingNative(ghost) != veh)
            return static_cast<uint8_t>(net::VehicleResult::NotAllowed);
        Init(natives::TASK_LEAVE_VEHICLE);
        Push(ghost);
        Push(veh);
        Push(static_cast<unsigned long long>(0)); // flags
        Push(static_cast<unsigned long long>(0)); // unkPed (arity 4, R&D §2.7)
        Call();
        return static_cast<uint8_t>(net::VehicleResult::Applied);
    }
    case net::VehicleAction::Hijack:
    {
        const unsigned int driver = GetDriverOfVehicleNative(veh);
        if (!driver || driver == localPlayerPed)
            return static_cast<uint8_t>(net::VehicleResult::NotAllowed);
        if (driver == ghost) // уже наш пед за рулём
            return static_cast<uint8_t>(net::VehicleResult::Applied);
        Init(natives::TASK_LEAVE_VEHICLE);
        Push(driver);
        Push(veh);
        Push(static_cast<unsigned long long>(0));
        Push(static_cast<unsigned long long>(0));
        Call();
        rt.hijackSeatPending = true;
        rt.hijacked = true;
        return static_cast<uint8_t>(net::VehicleResult::Applied);
    }
    default:
        break;
    }
    (void)localPlayerPed;
    return static_cast<uint8_t>(net::VehicleResult::NotAllowed);
}

uint8_t ApplyDraftIntent(unsigned int veh,
                         const net::VehicleInteractionPayload& p)
{
    using namespace natives_api;
    using namespace net;
    Init(natives::IS_DRAFT_VEHICLE);
    Push(veh);
    const bool isDraft = Call() != 0;
    if (static_cast<VehicleAction>(p.action) != VehicleAction::Hitch &&
        static_cast<VehicleAction>(p.action) != VehicleAction::Unhitch)
        return static_cast<uint8_t>(net::VehicleResult::NotAllowed);
    if (!isDraft)
        return static_cast<uint8_t>(net::VehicleResult::NotAllowed); // bad_param
    const int harness = p.door; // 0..5 (eDraftHarness)
    if (static_cast<VehicleAction>(p.action) == VehicleAction::Hitch)
    {
        unsigned int horse = 0;
        if (!p.targetId ||
            !FindHostHandleByNetId(p.targetId, horse) || !horse ||
            !DoesExist(horse) || IsEntityAVehicleNative(horse))
            return static_cast<uint8_t>(net::VehicleResult::NotAllowed);
        unsigned int occupied = 0;
        if (GetHarnessPedNative(veh, harness, occupied))
            return static_cast<uint8_t>(net::VehicleResult::SeatOccupied);
        int expected = 0, beforeActual = 0;
        GetDraftAnimalCountsNative(veh, expected, beforeActual);
        Init(natives::_ATTACH_DRAFT_VEHICLE_HARNESS_PED);
        Push(horse);
        Push(veh);
        Push(static_cast<unsigned long long>(harness));
        const bool ok = Call() != 0;
        unsigned int after = 0;
        int expected2 = 0, afterActual = 0;
        GetDraftAnimalCountsNative(veh, expected2, afterActual);
        const bool attached = ok && GetHarnessPedNative(veh, harness, after) &&
                              after == horse && afterActual >= beforeActual;
        return attached ? static_cast<uint8_t>(net::VehicleResult::Applied)
                        : static_cast<uint8_t>(net::VehicleResult::Correction);
    }
    unsigned int horse = 0;
    if (!GetHarnessPedNative(veh, harness, horse))
        return static_cast<uint8_t>(net::VehicleResult::NotAllowed);
    Init(natives::_DETACH_DRAFT_VEHICLE_HARNESS_PED);
    Push(veh);
    Push(horse);
    const bool ok = Call() != 0;
    unsigned int after = 0;
    const bool detached = ok && !GetHarnessPedNative(veh, harness, after);
    return detached ? static_cast<uint8_t>(net::VehicleResult::Applied)
                    : static_cast<uint8_t>(net::VehicleResult::Correction);
}

bool IsIntentAllowedByPolicy(const net::VehicleInteractionPayload& p,
                             unsigned int veh, unsigned int localPlayerPed,
                             uint8_t& result, uint32_t& detail)
{
    const auto action = static_cast<net::VehicleAction>(p.action);
    const bool dangerous = action == net::VehicleAction::Damage ||
                           action == net::VehicleAction::Hijack ||
                           action == net::VehicleAction::DoorBreak ||
                           action == net::VehicleAction::DoorLock ||
                           action == net::VehicleAction::DoorUnlock;
    if (IsMissionEntity(veh) && !coopcfg::Get().vehicleInteractionMission &&
        dangerous)
    {
        result = static_cast<uint8_t>(net::VehicleResult::NotAllowed);
        detail = NotAllowedDetail(net::kResultReasonMissionProtected);
        return false;
    }
    const unsigned int hostVehicle = GetVehiclePedIsUsingNative(localPlayerPed);
    if (hostVehicle && hostVehicle == veh &&
        !coopcfg::Get().vehicleInteractionHostVehicle && dangerous)
    {
        result = static_cast<uint8_t>(net::VehicleResult::NotAllowed);
        detail = NotAllowedDetail(net::kResultReasonHostVehicle);
        return false;
    }
    // L8: driver-seat Enter на host-vehicle при host_vehicle=0 — тоже reject
    // (иначе гость мог бы занять место водителя, пока хост пассажиром; policy
    // D2 разрешает только пассажирский Enter). Остальные Enter — ок.
    if (action == net::VehicleAction::Enter &&
        p.seat == net::kVehicleSeatDriver && hostVehicle &&
        hostVehicle == veh && !coopcfg::Get().vehicleInteractionHostVehicle)
    {
        result = static_cast<uint8_t>(net::VehicleResult::NotAllowed);
        detail = NotAllowedDetail(net::kResultReasonHostVehicle);
        return false;
    }
    if (p.action > 12)
    {
        result = static_cast<uint8_t>(net::VehicleResult::NotAllowed);
        detail = NotAllowedDetail(net::kResultReasonBadParam);
        return false;
    }
    // Поезда/вагоны идут отдельным путём 0x22 (mission-train) — P1 интерактив
    // по ним не применяем.
    if (IsTrainModel(GetModel(veh)))
    {
        result = static_cast<uint8_t>(net::VehicleResult::NotAllowed);
        detail = NotAllowedDetail(net::kResultReasonPolicy);
        return false;
    }
    return true;
}

// Один интент: validates + apply + result + dirty state. Возвращает true,
// если был вызван натив (per-tick budget считает только такие).
bool ApplyOneIntent(const net::VehicleInteractionPayload& p,
                    unsigned int localPlayerPed, unsigned int ghost,
                    bool ghostAlive, unsigned int tickCount)
{
    const unsigned long long nowMs = GetTickCount64();
    auto reject = [&](uint8_t result, uint32_t detail) {
        SendInteractionResult(p, result, detail);
        if (coopcfg::Get().vehicleInteractionDebug &&
            (g_vehicleInteractionLastLogMs == 0 ||
             nowMs - g_vehicleInteractionLastLogMs >= 1000ull))
        {
            g_vehicleInteractionLastLogMs = nowMs;
            sh_log::Write("vehicle: interaction action=%s net=%u seq=%u "
                          "result=%s guest=%u\n",
                          VehicleActionName(p.action), p.vehicleNetId, p.seq,
                          VehicleResultName(result), ghost);
        }
        return false;
    };

    if (!coopcfg::Get().vehicleInteraction)
        return false; // конфиг выключен: молча drop (не спамим гостю)
    if (!ghost || !ghostAlive)
        return reject(static_cast<uint8_t>(net::VehicleResult::NotAllowed),
                      NotAllowedDetail(net::kResultReasonDeadGuest));
    if (p.epoch == 0 || p.epoch != actor_sync::HostEpoch())
        return reject(static_cast<uint8_t>(net::VehicleResult::StaleEpoch), 0);
    if (const net::VehicleInteractionResultPayload* cached =
            FindCachedIntentResult(p))
    {
        coop::SendVehicleInteractionResult(*cached); // идемпотентный повтор
        return false;
    }

    unsigned int veh = 0;
    if (!FindHostVehicleHandleByNetId(p.vehicleNetId, veh) || !veh ||
        !DoesExist(veh) || !IsEntityAVehicleNative(veh))
        return reject(static_cast<uint8_t>(net::VehicleResult::NotFound), 0);

    uint8_t policyResult = static_cast<uint8_t>(net::VehicleResult::NotAllowed);
    uint32_t policyDetail = 0;
    if (!IsIntentAllowedByPolicy(p, veh, localPlayerPed, policyResult,
                                 policyDetail))
        return reject(policyResult, policyDetail);

    if (IsVehicleWreckedNative(veh) || IsEntityDeadNative(veh))
        return reject(static_cast<uint8_t>(net::VehicleResult::NotAllowed),
                      NotAllowedDetail(net::kResultReasonBadParam));

    // Дистанция: vehicle vs ghost (горизонталь) + высота <=3 м.
    const Vec3 vc = GetCoords(veh);
    const Vec3 gc = GetCoords(ghost);
    const float dx = vc.x - gc.x;
    const float dy = vc.y - gc.y;
    const float dz = vc.z - gc.z;
    const float radius = VehicleIntentRadius(p.action);
    if (dx * dx + dy * dy > radius * radius ||
        std::fabs(dz) > kInteractionHeightLimit)
        return reject(static_cast<uint8_t>(net::VehicleResult::OutOfRange), 0);

    HostVehicleRuntime& rt = g_hostVehicleRuntime[p.vehicleNetId];
    // cooldown per (vehicle,action): 250 мс, Urgent — 100 мс.
    const unsigned long long cooldownMs =
        (p.flags & net::kInteractionUrgent) ? kIntentCooldownUrgentMs
                                            : kIntentCooldownMs;
    const unsigned long long lastMs = rt.lastActionMs[p.action];
    if (lastMs != 0 && nowMs - lastMs < cooldownMs)
        return reject(static_cast<uint8_t>(net::VehicleResult::RateLimited),
                      static_cast<uint32_t>(cooldownMs - (nowMs - lastMs)));

    const auto action = static_cast<net::VehicleAction>(p.action);
    if (action == net::VehicleAction::DoorLock ||
        action == net::VehicleAction::DoorUnlock)
    {
        // enum lockStatus НЕ подтверждён (R&D §4.2) -> gated/NotAllowed.
        if (!kVehicleInteractionLocksEnabled)
            return reject(static_cast<uint8_t>(net::VehicleResult::NotAllowed),
                          NotAllowedDetail(net::kResultReasonPolicy));
    }
    if (action == net::VehicleAction::Loot)
    {
        // Generic vehicle inventory ABSENT (R&D §4.4): wire/fake-ready, P1.1.
        if (coopcfg::Get().vehicleInteractionDebug)
            sh_log::Write("vehicle: loot intent net=%u target=%u (P1.1)\n",
                          p.vehicleNetId, p.targetId);
        return reject(static_cast<uint8_t>(net::VehicleResult::NotAllowed),
                      NotAllowedDetail(net::kResultReasonPolicy));
    }

    uint8_t result = static_cast<uint8_t>(net::VehicleResult::NotAllowed);
    switch (action)
    {
    case net::VehicleAction::Damage:
        result = ApplyDamageIntent(veh, p, rt, nowMs);
        break;
    case net::VehicleAction::DoorOpen:
    case net::VehicleAction::DoorClose:
    case net::VehicleAction::DoorBreak:
        result = ApplyDoorIntent(veh, p);
        break;
    case net::VehicleAction::Enter:
    case net::VehicleAction::Exit:
    case net::VehicleAction::Hijack:
        result = ApplySeatIntent(veh, p, ghost, localPlayerPed, rt);
        break;
    case net::VehicleAction::Hitch:
    case net::VehicleAction::Unhitch:
        result = ApplyDraftIntent(veh, p);
        break;
    default:
        result = static_cast<uint8_t>(net::VehicleResult::NotAllowed);
        break;
    }

    rt.lastActionMs[p.action] = nowMs;
    if (result == static_cast<uint8_t>(net::VehicleResult::Applied) ||
        result == static_cast<uint8_t>(net::VehicleResult::Correction))
    {
        MarkVehicleStateDirty(p.vehicleNetId);
    }
    // F4: NotAllowed из apply (gated action, bad_param) несёт reason-биты
    // detail>>8, чтобы гость мог залогировать причину (§2.3).
    const uint32_t resultDetail =
        result == static_cast<uint8_t>(net::VehicleResult::NotAllowed)
            ? NotAllowedDetail(net::kResultReasonPolicy)
            : 0u;
    SendInteractionResult(p, result, resultDetail);
    if (coopcfg::Get().vehicleInteractionDebug)
        sh_log::Write("vehicle: interaction action=%s net=%u seq=%u result=%s "
                      "guest=%u\n",
                      VehicleActionName(p.action), p.vehicleNetId, p.seq,
                      VehicleResultName(result), ghost);
    return true;
}

// v20 P1: сетевой поток -> host inbox (без нативов; лимит очереди 8, overflow
// помечается RateLimited при разборе).
void EnqueueVehicleInteraction(const net::VehicleInteractionPayload& intent)
{
    std::lock_guard<std::mutex> lk(g_vehicleIntentsMutex);
    if (g_vehicleIntents.size() <
        static_cast<std::size_t>(kHostIntentQueueCap))
    {
        g_vehicleIntents.push_back(intent);
        return;
    }
    if (g_vehicleIntentOverflow.size() <
        static_cast<std::size_t>(kHostIntentOverflowCap))
        g_vehicleIntentOverflow.push_back(intent);
}

net::VehicleStatePayload BuildVehicleState(uint32_t netId, unsigned int veh,
                                           unsigned int localPlayerPed,
                                           unsigned int ghost,
                                           HostVehicleRuntime& rt)
{
    net::VehicleStatePayload s{};
    s.epoch = actor_sync::HostEpoch();
    s.vehicleNetId = netId;
    if (!veh || !DoesExist(veh) || !IsEntityAVehicleNative(veh))
        return s; // flags=0, seats=127
    const float body = GetVehicleBodyHealthNative(veh);
    const float engine = GetVehicleEngineHealthNative(veh);
    if (std::isfinite(body) && std::isfinite(engine))
    {
        s.bodyHealth = static_cast<uint16_t>(
            std::lround(std::clamp(body, 0.f, 1000.f)));
        s.engineHealth = static_cast<int16_t>(
            std::lround(std::clamp(engine, -4000.f, 1000.f)));
        s.flags |= net::kVehicleStateFlagDamageValid;
    }
    s.flags |= net::kVehicleStateFlagDoorValid;
    for (int d = 0; d < 8; ++d)
    {
        // 3 = unknown (readback failed/нет двери) не выставляем: P1 без per-door
        // статуса; broken=2, fullyOpen=1, closed/ajar=0 (R&D §2.3).
        const bool broken = IsVehicleDoorBrokenNative(veh, d);
        const bool open = !broken && IsVehicleDoorFullyOpenNative(veh, d);
        const uint16_t st = broken ? 2u : (open ? 1u : 0u);
        s.doorBits |= static_cast<uint16_t>(st << (2 * d));
    }
    for (int w = 0; w < 4; ++w)
    {
        if (IsVehicleWheelDestroyedNative(veh, w))
            s.wheelsBits |= static_cast<uint8_t>(2u << (2 * w));
    }
    if (AreAnyVehicleWheelsDestroyedNative(veh) && s.wheelsBits == 0)
        s.wheelsBits = 0xFF; // mapping/readback mismatch -> unknown
    if (IsMissionEntity(veh))
        s.flags |= net::kVehicleStateFlagMissionProtected;
    if (IsVehicleWreckedNative(veh) || IsEntityDeadNative(veh))
        s.flags |= net::kVehicleStateFlagDead;
    {
        using namespace natives_api;
        Init(natives::GET_VEHICLE_DOOR_LOCK_STATUS);
        Push(veh);
        // SIGNATURE-ONLY enum: raw != 0 — диагностический флаг (A/B).
        if (Call() != 0)
            s.flags |= net::kVehicleStateFlagLocked;
    }
    const int hostSeat = FindSeatForPedNative(veh, localPlayerPed);
    if (hostSeat != net::kVehicleSeatNone)
    {
        s.hostSeat = static_cast<int8_t>(hostSeat);
        s.flags |= net::kVehicleStateFlagHostSeatValid;
    }
    const int guestSeat = FindSeatForPedNative(veh, ghost);
    if (guestSeat != net::kVehicleSeatNone)
    {
        s.guestSeat = static_cast<int8_t>(guestSeat);
        s.flags |= net::kVehicleStateFlagGuestSeatValid;
    }
    if (rt.hijacked)
        s.flags |= net::kVehicleStateFlagHijacked;
    // F2: seq из persistent-карты по netId (не сбрасывается при eviction
    // tracked-runtime); rt.stateSeq остаётся синхронным для диагностики.
    rt.stateSeq = NextVehicleStateSeq(netId);
    s.seq = rt.stateSeq;
    return s;
}

// Разбор host-очереди: ≤2 нативных интента/тик, остаток — в следующий тик.
void ApplyVehicleInteractionIntents(unsigned int localPlayerPed,
                                    unsigned int tickCount)
{
    std::vector<net::VehicleInteractionPayload> intents;
    std::vector<net::VehicleInteractionPayload> overflow;
    {
        std::lock_guard<std::mutex> lk(g_vehicleIntentsMutex);
        intents.swap(g_vehicleIntents);
        overflow.swap(g_vehicleIntentOverflow);
    }
    // Конфиг выключен: интенты молча дропаем (не спамим result'ами, §4.2).
    if (!coopcfg::Get().vehicleInteraction)
        return;
    for (const auto& p : overflow)
        SendInteractionResult(p,
                              static_cast<uint8_t>(net::VehicleResult::RateLimited),
                              0);

    const unsigned int ghost = coop::PartnerGhostHandle();
    const bool ghostAlive =
        ghost && DoesExist(ghost) && !IsPedDeadOrDyingNative(ghost);
    int applied = 0;
    for (std::size_t i = 0; i < intents.size(); ++i)
    {
        if (applied >= kHostIntentsPerTick)
        {
            std::lock_guard<std::mutex> lk(g_vehicleIntentsMutex);
            for (std::size_t j = intents.size(); j-- > i;)
                g_vehicleIntents.insert(g_vehicleIntents.begin(), intents[j]);
            break;
        }
        if (ApplyOneIntent(intents[i], localPlayerPed, ghost, ghostAlive,
                           tickCount))
            ++applied;
    }

    // Hijack follow-through: водитель вышел -> следующим тиком сажаем ghost.
    for (auto& kv : g_hostVehicleRuntime)
    {
        if (!kv.second.hijackSeatPending)
            continue;
        unsigned int veh = 0;
        if (!FindHostVehicleHandleByNetId(kv.first, veh) || !veh ||
            !DoesExist(veh))
        {
            kv.second.hijackSeatPending = false;
            continue;
        }
        const unsigned int driver = GetDriverOfVehicleNative(veh);
        if (driver && driver == ghost)
        {
            kv.second.hijackSeatPending = false;
            continue;
        }
        if (driver && DoesExist(driver))
            continue; // NPC ещё не вышел — ждём следующий тик
        using namespace natives_api;
        Init(natives::SET_PED_INTO_VEHICLE);
        Push(ghost);
        Push(veh);
        Push(static_cast<unsigned long long>(
            static_cast<long long>(net::kVehicleSeatDriver)));
        Call();
        kv.second.hijackSeatPending = false;
        MarkVehicleStateDirty(kv.first);
        sh_log::Write("vehicle: hijack seat net=%u driver-assigned ok=%d\n",
                      kv.first,
                      GetPedInVehicleSeatNative(veh, net::kVehicleSeatDriver) ==
                              ghost
                          ? 1
                          : 0);
    }
}

// 0x26: dirty при изменении + heartbeat 1/с; окно <=8/с (сервер <=10/с).
void SendVehicleInteractionStates(unsigned int localPlayerPed,
                                  unsigned int tickCount)
{
    (void)tickCount;
    if (!coopcfg::Get().vehicleInteraction)
        return;
    const uint32_t epoch = actor_sync::HostEpoch();
    if (!epoch)
        return;
    const unsigned int ghost = coop::PartnerGhostHandle();
    if (!ghost)
        return;
    const unsigned long long nowMs = GetTickCount64();
    static unsigned long long windowMs = 0;
    static unsigned int windowCount = 0;
    if (windowMs == 0 || nowMs - windowMs >= 1000ull)
    {
        windowMs = nowMs;
        windowCount = 0;
    }
    for (auto it = g_hostVehicleRuntime.begin();
         it != g_hostVehicleRuntime.end();)
    {
        HostVehicleRuntime& rt = it->second;
        const bool heartbeat =
            rt.hasState && nowMs - rt.lastSentStateMs >= kStateHeartbeatMs;
        if (!rt.dirty && !heartbeat)
        {
            ++it;
            continue;
        }
        unsigned int veh = 0;
        if (!FindHostVehicleHandleByNetId(it->first, veh) || !veh ||
            !DoesExist(veh))
        {
            it = g_hostVehicleRuntime.erase(it);
            continue;
        }
        if (windowCount >= kStateSendWindowCap)
        {
            ++it;
            continue;
        }
        net::VehicleStatePayload state =
            BuildVehicleState(it->first, veh, localPlayerPed, ghost, rt);
        if (!coop::SendVehicleState(state))
        {
            ++it;
            continue;
        }
        rt.lastState = state;
        rt.hasState = true;
        rt.dirty = false;
        rt.lastSentStateMs = nowMs;
        ++windowCount;
        if (coopcfg::Get().vehicleInteractionDebug)
            sh_log::Write("vehicle: state net=%u body=%u engine=%d door=0x%04X "
                          "wheels=0x%02X flags=0x%02X hostSeat=%d guestSeat=%d\n",
                          it->first, state.bodyHealth,
                          static_cast<int>(state.engineHealth), state.doorBits,
                          state.wheelsBits, state.flags,
                          static_cast<int>(state.hostSeat),
                          static_cast<int>(state.guestSeat));
        ++it;
    }
}

void ResetHostVehicleInteraction()
{
    {
        std::lock_guard<std::mutex> lk(g_vehicleIntentsMutex);
        g_vehicleIntents.clear();
        g_vehicleIntentOverflow.clear();
    }
    g_intentResultCache.clear();
    g_hostVehicleRuntime.clear();
    g_vehicleStateSeq.clear(); // F2: новая сессия/роль — seq заново
    g_vehicleInteractionLastLogMs = 0;
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
    // v20 P1: интерактивность/зеркало транспорта у гостя (near <=20 м).
    bool interactive = false;
    bool mirrorStateValid = false;
    uint16_t mirrorStateSeq = 0;
    unsigned long long mirrorStateMs = 0; // F2: время последнего принятого state
    uint16_t mirrorDoorBits = 0;
    uint8_t mirrorWheelsBits = 0;
    uint16_t mirrorBodyHealth = net::kVehicleBodyHealthUnknown;
    int16_t mirrorEngineHealth = net::kVehicleEngineHealthUnknown;
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
    uint8_t carIndex = 0; // E11: для train occupants
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
            a.carIndex = ne.carIndex;
            for (const SeatAttempt& o : old)
            {
                if (o.actorId == a.actorId && o.flags == a.flags &&
                    o.seat == a.seat && o.harness == a.harness &&
                    o.carIndex == a.carIndex)
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
        // E11: 0x21 train-occupants могли прийти раньше 0x22; как только
        // trainId известен — перекладываем в train-очередь, не ждём timeout.
        if (g_guestTrainSnapValid && netId == g_guestTrainSnap.trainId)
        {
            std::vector<net::VehicleOccupantEntry> converted;
            for (const SeatAttempt& e : st.entries)
            {
                net::VehicleOccupantEntry ne{};
                ne.actorId = e.actorId;
                ne.seat = e.seat;
                ne.flags = e.flags;
                ne.carIndex = e.carIndex;
                ne.harness = e.harness;
                converted.push_back(ne);
            }
            RouteTrainOccupants(converted);
            it = g_occupantVehicles.erase(it);
            continue;
        }
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

// ================= v20 P1: guest mirror/interactive proxy =================
// Гость: 0x24 (результаты интентов) и 0x26 (authoritative state) -> зеркало
// локального прокси; near-proxy (<=20 м) становится "осязаемым"; пока guest
// сидит в прокси, vehicle-контролы подавлены (§4.5-§4.6).
constexpr float kVehicleInteractionProxyRadius = 20.f;
std::mutex g_viInboxMutex;
std::vector<net::VehicleInteractionResultPayload> g_viResultsInbox;
std::map<uint32_t, net::VehicleStatePayload> g_viStatesInbox;
bool g_guestControlsDisabled = false;
unsigned long long g_guestVehicleLogMs = 0;

// Сетевой поток -> inbox (без нативов/логов).
void EnqueueVehicleInteractionResult(
    const net::VehicleInteractionResultPayload& result)
{
    std::lock_guard<std::mutex> lk(g_viInboxMutex);
    if (g_viResultsInbox.size() < 16)
        g_viResultsInbox.push_back(result);
}

void EnqueueVehicleState(const net::VehicleStatePayload& state)
{
    std::lock_guard<std::mutex> lk(g_viInboxMutex);
    // Храним последний state на vehicle (дубли/старые перетираются).
    g_viStatesInbox[state.vehicleNetId] = state;
    while (g_viStatesInbox.size() > 16)
        g_viStatesInbox.erase(g_viStatesInbox.begin());
}

bool FindVehicleProxyNetId(unsigned int handle, uint32_t& netId)
{
    if (!handle) return false;
    for (const auto& kv : g_proxies)
    {
        if (kv.second.kind == net::kEntityKindVehicle &&
            kv.second.handle == handle)
        {
            netId = static_cast<uint32_t>(kv.first);
            return true;
        }
    }
    return false;
}

// Гость: водитель-NPC из последнего 0x21 (для выбора Enter vs Hijack).
bool FindProxyOccupantDriver(uint32_t netId, uint32_t& actorId)
{
    actorId = 0;
    const auto it = g_occupantVehicles.find(netId);
    if (it == g_occupantVehicles.end())
        return false;
    for (const SeatAttempt& e : it->second.entries)
    {
        if (e.flags & net::kOccupantFlagDriver)
        {
            actorId = e.actorId;
            return actorId != 0;
        }
    }
    return false;
}

// Позиция vehicle-прокси без лишних нативов: интерполированная цель снапшота,
// fallback — GET_ENTITY_COORDS (только при первом спавне).
Vec3 ProxyPos(const Proxy& p)
{
    if (p.vehHasTarget)
        return Vec3{p.vehCurX, p.vehCurY, p.vehCurZ};
    if (p.handle && DoesExist(p.handle))
        return GetCoords(p.handle);
    return Vec3{0.f, 0.f, 0.f};
}

// Гость: ближайший интерактивный vehicle-прокси в радиусе (capture C3).
bool FindNearestVehicleProxy(unsigned int localPed, float radius,
                            uint32_t& netId, unsigned int& handle)
{
    netId = 0;
    handle = 0;
    if (!localPed || !DoesExist(localPed))
        return false;
    const Vec3 pc = GetCoords(localPed);
    float best = radius * radius;
    for (const auto& kv : g_proxies)
    {
        const Proxy& p = kv.second;
        if (p.kind != net::kEntityKindVehicle || !p.handle ||
            p.trainAttached)
            continue;
        const Vec3 vc = ProxyPos(p);
        const float dx = vc.x - pc.x;
        const float dy = vc.y - pc.y;
        const float d2 = dx * dx + dy * dy;
        if (d2 > best)
            continue;
        best = d2;
        netId = static_cast<uint32_t>(kv.first);
        handle = p.handle;
    }
    return handle != 0;
}

// v20 P1: есть ли рядом (near) интерактивный vehicle-прокси. Без нативов —
// дешёвый gate для control-poll (C3): не дёргаем IS_CONTROL_*, пока мимо.
bool HasInteractiveProxyImpl()
{
    for (const auto& kv : g_proxies)
    {
        if (kv.second.kind == net::kEntityKindVehicle &&
            kv.second.interactive)
            return true;
    }
    return false;
}

Proxy* FindProxyByNetId(uint32_t netId)
{
    const auto it = g_proxies.find(static_cast<uint64_t>(netId));
    if (it == g_proxies.end() || !it->second.handle)
        return nullptr;
    return &it->second;
}

int FindLocalSeatInVehicle(unsigned int veh, unsigned int ped)
{
    using namespace natives_api;
    for (int s = -1; s <= net::kMaxVehicleSeat; ++s)
    {
        Init(natives::GET_PED_IN_VEHICLE_SEAT);
        Push(veh);
        Push(static_cast<unsigned long long>(static_cast<long long>(s)));
        if (static_cast<unsigned int>(Call()) == ped)
            return s;
    }
    return net::kVehicleSeatNone;
}

// Гость: локальный игрок в vehicle-прокси -> netId/handle/seat.
bool FindLocalProxyVehicle(unsigned int localPed, uint32_t& netId,
                          unsigned int& handle, int& seat)
{
    netId = 0;
    handle = 0;
    seat = net::kVehicleSeatNone;
    if (!localPed)
        return false;
    using namespace natives_api;
    unsigned int veh = 0;
    Init(natives::GET_VEHICLE_PED_IS_IN);
    Push(localPed);
    Push(false);
    veh = static_cast<unsigned int>(Call());
    if (!veh)
    {
        Init(natives::GET_VEHICLE_PED_IS_ENTERING);
        Push(localPed);
        veh = static_cast<unsigned int>(Call());
    }
    if (!veh)
        return false;
    if (!FindVehicleProxyNetId(veh, netId))
        return false; // локальный (не реплицированный) транспорт — не наш случай
    handle = veh;
    // Seat читается перебором (<=10 нативов) — кэшируем по (ped,vehicle):
    // каждый тик нужен только факт "в proxy", seat пересчитываем при смене.
    static unsigned int cachedVeh = 0;
    static unsigned int cachedPed = 0;
    static int cachedSeat = net::kVehicleSeatNone;
    if (veh != cachedVeh || localPed != cachedPed)
    {
        cachedVeh = veh;
        cachedPed = localPed;
        cachedSeat = FindLocalSeatInVehicle(veh, localPed);
    }
    seat = cachedSeat;
    return true;
}

void SetProxyInteractive(Proxy& p, bool on, uint32_t netId)
{
    using namespace natives_api;
    // SetNoCollision(handle,false) => collision ON (см. хелпер выше).
    SetNoCollision(p.handle, !on);
    Init(natives::SET_VEHICLE_IS_CONSIDERED_BY_PLAYER);
    Push(p.handle);
    Push(on ? 1 : 0);
    Call();
    p.interactive = on;
    if (coopcfg::Get().vehicleInteractionDebug)
        sh_log::Write("vehicle: proxy interactive net=%u on=%d\n", netId,
                      on ? 1 : 0);
}

// v20 P1: near-proxy tangibility (<=20 м): collision on/off + enter prompt.
void UpdateInteractiveProxy(unsigned int localPed, unsigned int tickCount)
{
    (void)tickCount;
    if (!coopcfg::Get().vehicleInteraction || !localPed || !DoesExist(localPed))
        return;
    const Vec3 pc = GetCoords(localPed);
    const float r2 = kVehicleInteractionProxyRadius *
                     kVehicleInteractionProxyRadius;
    for (auto& kv : g_proxies)
    {
        Proxy& p = kv.second;
        if (p.kind != net::kEntityKindVehicle || !p.handle ||
            !DoesExist(p.handle))
            continue;
        if (p.vehSnapWarp)
        {
            // Жёсткий warp/снап: на этот тик collision off, чтобы не толкать
            // игрока; флаг снимаем (остальные потребители уже отработали).
            if (p.interactive)
                SetProxyInteractive(p, false, static_cast<uint32_t>(kv.first));
            p.vehSnapWarp = false;
            continue;
        }
        const Vec3 vc = ProxyPos(p);
        const float dx = vc.x - pc.x;
        const float dy = vc.y - pc.y;
        // near/far — legacy-макросы Windows-заголовков, поэтому isNear.
        const bool isNear = !p.trainAttached && (dx * dx + dy * dy) <= r2;
        if (isNear != p.interactive)
            SetProxyInteractive(p, isNear, static_cast<uint32_t>(kv.first));
    }
}

// v20 P1: guest controls gate — пока локальный игрок в proxy, вождение
// подавлено (DISABLE_CONTROL_ACTION на vehicle-actions; restore на exit/reset).
void SetGuestVehicleControls(bool disabled)
{
    using namespace natives_api;
    static const unsigned long long kActions[] = {
        natives::input::INPUT_VEH_ACCELERATE,
        natives::input::INPUT_VEH_BRAKE,
        natives::input::INPUT_VEH_MOVE_LR,
        natives::input::INPUT_VEH_MOVE_UD,
        natives::input::INPUT_VEH_HANDBRAKE,
    };
    for (unsigned long long action : kActions)
    {
        if (disabled)
        {
            Init(natives::DISABLE_CONTROL_ACTION);
            Push(0); // control type 0 = vehicle context (R&D §3)
            Push(action);
            Push(true); // disableRelatedActions
        }
        else
        {
            Init(natives::ENABLE_CONTROL_ACTION);
            Push(0);
            Push(action);
            Push(true);
        }
        Call();
    }
    g_guestControlsDisabled = disabled;
}

void UpdateGuestVehicleControls(unsigned int localPed)
{
    if (!coopcfg::Get().vehicleInteraction)
    {
        if (g_guestControlsDisabled)
            SetGuestVehicleControls(false);
        return;
    }
    uint32_t netId = 0;
    unsigned int handle = 0;
    int seat = net::kVehicleSeatNone;
    const bool inProxy = FindLocalProxyVehicle(localPed, netId, handle, seat);
    if (inProxy)
        SetGuestVehicleControls(true); // каждый тик, пока в proxy (R&D §3.2)
    else if (g_guestControlsDisabled)
        SetGuestVehicleControls(false);
}

// Mirror health: временно снять invincible и записать точные значения (самый
// детерминированный путь; SET_VEHICLE_DAMAGE для зеркала не нужен).
void MirrorVehicleHealth(Proxy& p, const net::VehicleStatePayload& st)
{
    using namespace natives_api;
    const bool bodyChanged =
        st.bodyHealth != net::kVehicleBodyHealthUnknown &&
        st.bodyHealth != p.mirrorBodyHealth;
    const bool engineChanged =
        st.engineHealth != net::kVehicleEngineHealthUnknown &&
        st.engineHealth != p.mirrorEngineHealth;
    if (!bodyChanged && !engineChanged)
        return;
    SetInvincible(p.handle, false);
    Init(natives::SET_ENTITY_CAN_BE_DAMAGED);
    Push(p.handle);
    Push(true);
    Call();
    Init(natives::SET_VEHICLE_CAN_BE_VISIBLY_DAMAGED);
    Push(p.handle);
    Push(true);
    Call();
    Init(natives::SET_VEHICLE_CAN_BREAK);
    Push(p.handle);
    Push(true);
    Call();
    Init(natives::SET_VEHICLE_STRONG);
    Push(p.handle);
    Push(false);
    Call();
    if (bodyChanged)
    {
        Init(natives::SET_VEHICLE_BODY_HEALTH);
        Push(p.handle);
        Push(static_cast<float>(st.bodyHealth));
        Call();
        p.mirrorBodyHealth = st.bodyHealth;
    }
    if (engineChanged)
    {
        Init(natives::SET_VEHICLE_ENGINE_HEALTH);
        Push(p.handle);
        Push(static_cast<float>(st.engineHealth));
        Call();
        p.mirrorEngineHealth = st.engineHealth;
    }
    SetInvincible(p.handle, true);
    if (coopcfg::Get().vehicleInteractionDebug)
        sh_log::Write("vehicle: mirror health net=%u body=%u engine=%d\n",
                      st.vehicleNetId, st.bodyHealth,
                      static_cast<int>(st.engineHealth));
}

// 0x26 -> локальный прокси: двери/колёса/health/seat (best-effort mirror).
void ApplyVehicleStateToProxy(Proxy& p, const net::VehicleStatePayload& st,
                              unsigned int localPed)
{
    using namespace natives_api;
    if (!p.handle || !DoesExist(p.handle))
        return;
    // Stale seq guard: host seq монотонен per netId (persistent-карта), но
    // терпим restart — uint16-wrap ИЛИ seq ушёл назад после паузы >1.5 с
    // (host re-created tracked-runtime/старый хост/потеря сообщений). Иначе
    // eviction (>kTrackedVehicleStates tracked) давал бы «навсегда дропнутый»
    // state.
    const unsigned long long stateNowMs = GetTickCount64();
    if (p.mirrorStateValid && st.seq != 0 && p.mirrorStateSeq != 0)
    {
        const uint16_t cur = static_cast<uint16_t>(st.seq);
        const uint16_t prev = static_cast<uint16_t>(p.mirrorStateSeq);
        const bool wrapRestart =
            static_cast<unsigned int>(cur) + 64u <
            static_cast<unsigned int>(prev);
        const bool stale =
            !wrapRestart && static_cast<int16_t>(cur - prev) <= 0;
        const bool timeoutRestart =
            stale && (p.mirrorStateMs == 0 || stateNowMs - p.mirrorStateMs >
                                                  1500ull);
        if (stale && !timeoutRestart)
            return; // старый/reordered state
        if (timeoutRestart && coopcfg::Get().vehicleInteractionDebug)
            sh_log::Write("vehicle: state seq reset net=%u %u -> %u\n",
                          st.vehicleNetId, prev, cur);
    }
    p.mirrorStateValid = true;
    p.mirrorStateSeq = st.seq;
    p.mirrorStateMs = stateNowMs;
    if (st.flags & net::kVehicleStateFlagDoorValid)
    {
        for (int d = 0; d < 8; ++d)
        {
            const uint8_t want =
                static_cast<uint8_t>((st.doorBits >> (2 * d)) & 3u);
            const uint8_t had = static_cast<uint8_t>(
                (p.mirrorDoorBits >> (2 * d)) & 3u);
            if (want == had) continue;
            if (want == 2)
            {
                Init(natives::SET_DOOR_ALLOWED_TO_BE_BROKEN_OFF);
                Push(p.handle);
                Push(static_cast<unsigned long long>(d));
                Push(true);
                Call();
                Init(natives::SET_VEHICLE_DOOR_BROKEN);
                Push(p.handle);
                Push(static_cast<unsigned long long>(d));
                Push(false); // deleteDoor=false: только визуал mirror
                Call();
            }
            else if (want == 1)
            {
                Init(natives::SET_VEHICLE_DOOR_OPEN);
                Push(p.handle);
                Push(static_cast<unsigned long long>(d));
                Push(false); // loose
                Push(true);  // openInstantly
                Call();
            }
            else if (had == 1)
            {
                Init(natives::SET_VEHICLE_DOOR_SHUT);
                Push(p.handle);
                Push(static_cast<unsigned long long>(d));
                Push(true); // closeInstantly
                Call();
            }
        }
        p.mirrorDoorBits = st.doorBits;
    }
    if (st.wheelsBits != 0)
    {
        for (int w = 0; w < 4; ++w)
        {
            const uint8_t want =
                static_cast<uint8_t>((st.wheelsBits >> (2 * w)) & 3u);
            const uint8_t had = static_cast<uint8_t>(
                (p.mirrorWheelsBits >> (2 * w)) & 3u);
            if (want == 2 && had != 2)
            {
                // wheelIndex mapping UNCERTAIN: P1 использует wire-порядок 0..3
                // (A/B на wagon; см. §10.3).
                Init(natives::_BREAK_OFF_VEHICLE_WHEEL);
                Push(p.handle);
                Push(static_cast<unsigned long long>(w));
                Call();
            }
        }
        p.mirrorWheelsBits = st.wheelsBits;
    }
    if (st.flags & net::kVehicleStateFlagDamageValid)
        MirrorVehicleHealth(p, st);
    // Seat reconcile: host state — источник истины для guestSeat.
    if ((st.flags & net::kVehicleStateFlagGuestSeatValid) && localPed)
    {
        uint32_t curNet = 0;
        unsigned int curHandle = 0;
        int curSeat = net::kVehicleSeatNone;
        if (FindLocalProxyVehicle(localPed, curNet, curHandle, curSeat) &&
            curNet == st.vehicleNetId)
        {
            const int wantSeat = st.guestSeat;
            if (wantSeat != net::kVehicleSeatNone && curSeat != wantSeat)
            {
                Init(natives::SET_PED_INTO_VEHICLE);
                Push(localPed);
                Push(curHandle);
                Push(static_cast<unsigned long long>(
                    static_cast<long long>(wantSeat)));
                Call();
                sh_log::Write("vehicle: seat correction net=%u %d -> %d\n",
                              st.vehicleNetId, curSeat, wantSeat);
            }
        }
    }
    if (coopcfg::Get().vehicleInteractionDebug)
    {
        sh_log::Write("vehicle: state apply net=%u door=0x%04X wheels=0x%02X "
                      "flags=0x%02X guestSeat=%d seq=%u\n",
                      st.vehicleNetId, st.doorBits, st.wheelsBits, st.flags,
                      static_cast<int>(st.guestSeat), st.seq);
    }
}

// v20 P1: guest Tick — разобрать 0x24/0x26: зеркало + rollback + логи.
void ApplyPendingVehicleInteraction(unsigned int localPed,
                                    unsigned int tickCount)
{
    (void)tickCount;
    std::vector<net::VehicleInteractionResultPayload> results;
    std::vector<net::VehicleStatePayload> states;
    {
        std::lock_guard<std::mutex> lk(g_viInboxMutex);
        results.swap(g_viResultsInbox);
        for (auto& kv : g_viStatesInbox)
            states.push_back(kv.second);
        g_viStatesInbox.clear();
    }
    const unsigned long long nowMs = GetTickCount64();
    const bool debug = coopcfg::Get().vehicleInteractionDebug;
    const uint32_t guestEpoch = actor_sync::GuestEpoch();
    for (const auto& r : results)
    {
        // Stale epoch (смена хоста/сессии): результат чужой эпохи не применяем.
        if (guestEpoch == 0 || r.epoch != guestEpoch)
        {
            if (debug)
                sh_log::Write("vehicle: drop stale result epoch=%u (own=%u)\n",
                              r.epoch, guestEpoch);
            continue;
        }
        if (debug)
            sh_log::Write("vehicle: interaction action=%s result=%s net=%u "
                          "seq=%u detail=%u\n",
                          VehicleActionName(r.action), VehicleResultName(r.result),
                          r.vehicleNetId, r.seq, r.detail);
        if (r.result == static_cast<uint8_t>(net::VehicleResult::Applied))
            continue;
        // Reject посадки/хайджека: гость выходит из локального proxy.
        const bool seatAction =
            r.action == static_cast<uint8_t>(net::VehicleAction::Enter) ||
            r.action == static_cast<uint8_t>(net::VehicleAction::Hijack);
        if (seatAction && localPed)
        {
            uint32_t netId = 0;
            unsigned int handle = 0;
            int seat = net::kVehicleSeatNone;
            if (FindLocalProxyVehicle(localPed, netId, handle, seat) &&
                netId == r.vehicleNetId)
            {
                using namespace natives_api;
                Init(natives::TASK_LEAVE_VEHICLE);
                Push(localPed);
                Push(handle);
                Push(static_cast<unsigned long long>(0));
                Push(static_cast<unsigned long long>(0));
                Call();
                sh_log::Write("vehicle: proxy exit after result=%s net=%u\n",
                              VehicleResultName(r.result), r.vehicleNetId);
            }
        }
    }
    for (const auto& st : states)
    {
        if (guestEpoch == 0 || st.epoch != guestEpoch)
            continue; // stale epoch state
        Proxy* p = FindProxyByNetId(st.vehicleNetId);
        if (!p)
            continue;
        ApplyVehicleStateToProxy(*p, st, localPed);
    }
    if (!results.empty() && debug &&
        (g_guestVehicleLogMs == 0 || nowMs - g_guestVehicleLogMs >= 1000ull))
        g_guestVehicleLogMs = nowMs;
}

// Сброс guest-состояния (роль/leave/disable): controls restore + inbox/caches.
void ResetGuestVehicleInteraction()
{
    if (g_guestControlsDisabled)
        SetGuestVehicleControls(false);
    // Вернуть considered-by-player флаги на живых прокси до их удаления.
    for (auto& kv : g_proxies)
    {
        Proxy& p = kv.second;
        if (p.kind != net::kEntityKindVehicle || !p.handle ||
            !DoesExist(p.handle) || !p.interactive)
            continue;
        using namespace natives_api;
        Init(natives::SET_VEHICLE_IS_CONSIDERED_BY_PLAYER);
        Push(p.handle);
        Push(0);
        Call();
        SetNoCollision(p.handle, true);
        p.interactive = false;
    }
    {
        std::lock_guard<std::mutex> lk(g_viInboxMutex);
        g_viResultsInbox.clear();
        g_viStatesInbox.clear();
    }
    g_guestVehicleLogMs = 0;
}

// v20 (opt-in npc_vehicle_wheels=1): колёса guest vehicle-прокси не крутятся —
// позицию задаёт интерполяция, значит физическая скорость прокси = 0. Публичного
// натива «задать вращение колёс» в 1491.50 нет; единственный кандидат —
// SET_VEHICLE_FORWARD_SPEED (SIGNATURE-ONLY): оцениваем скорость хоста из
// дельты positions/dt между двумя снапшотами (clamp 0..30 м/с) и повторяем её
// прокси КАЖДЫЙ тик. Позицию/поворот по-прежнему задаёт интерполяция
// (SetCoords/SetHeading) — их не трогаем. Риск: натив может «бороться» с
// интерполяцией и двигать прокси, поэтому default 0 (только A/B в игре).
void ApplyGuestVehicleWheelSpeed(const Proxy& p)
{
    if (!coopcfg::Get().npcVehicleWheels)
        return;
    const unsigned long long span =
        p.vehCurMs > p.vehPrevMs ? p.vehCurMs - p.vehPrevMs : 0ull;
    if (span < 10ull)
        return; // нет осмысленной дельты (первый снапшот/замер)
    const float dx = p.vehCurX - p.vehPrevX;
    const float dy = p.vehCurY - p.vehPrevY;
    const float dz = p.vehCurZ - p.vehPrevZ;
    float speed = std::sqrt(dx * dx + dy * dy + dz * dz) /
                  (static_cast<float>(span) / 1000.f);
    if (speed < 0.f) speed = 0.f;
    if (speed > 30.f) speed = 30.f; // защита от мусорных дельт/warp
    using namespace natives_api;
    // e20.2 T4 (A/B candidate B): mission/invincible/no-collision прокси не
    // крутит колёса от одного SET_VEHICLE_FORWARD_SPEED. При движении включаем
    // физику/гравитацию/двигатель — только при speed>0.1 (стоя не трогаем).
    // Позицию/поворот жёстко перезапишет интерполяция СРАЗУ после этого вызова
    // (InterpolateVehicleProxies: ApplyGuestVehicleWheelSpeed -> SetCoords/
    // SetHeading), поэтому «уезжание» ограничено одним тиком; риск push
    // игрока/дрейфа — только при npc_vehicle_wheels=1 (opt-in A/B).
    if (speed > 0.1f)
    {
        Init(natives::SET_ENTITY_DYNAMIC);
        Push(p.handle);
        Push(true);
        Call();
        Init(natives::SET_ENTITY_HAS_GRAVITY);
        Push(p.handle);
        Push(true);
        Call();
        Init(natives::SET_VEHICLE_ENGINE_ON);
        Push(p.handle);
        Push(true); // value: завести
        Push(true); // instantly: без анимации водителя (arity 3, R&D/NativeDecl)
        Call();
    }
    Init(natives::SET_VEHICLE_FORWARD_SPEED);
    Push(p.handle);
    Push(speed);
    Call();
    static unsigned long long lastLogMs = 0;
    const unsigned long long now = GetTickCount64();
    if (coopcfg::Get().npcDebug &&
        (lastLogMs == 0 || now - lastLogMs >= 1000ull))
    {
        lastLogMs = now;
        sh_log::Write("world: vehicle wheels speed=%.1f\n", speed);
    }
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
        // e20.2 T4: порядок «нативы (dynamic/gravity/engine + forward speed) ->
        // жёсткая позиция/поворот из интерполяции», чтобы физика не уводила
        // прокси больше чем на тик.
        ApplyGuestVehicleWheelSpeed(p);
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
        if (g_perfGuestTrainSeatsApplied || g_perfGuestTrainSeatsFailed)
        {
            sh_log::Write("actor: guest train seats applied=%u failed=%u\n",
                          g_perfGuestTrainSeatsApplied,
                          g_perfGuestTrainSeatsFailed);
            g_perfGuestTrainSeatsApplied = 0;
            g_perfGuestTrainSeatsFailed = 0;
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

// ================= v20 P1: публичный API интерактивности транспорта =========
// Внутренние реализации — в anonymous namespace (inbox без нативов/мутаций
// движка); публичные обёртки дают внешнюю линковку для coop.cpp/net callbacks.
void OnVehicleInteraction(const net::VehicleInteractionPayload& intent)
{
    EnqueueVehicleInteraction(intent);
}

void OnVehicleInteractionResult(
    const net::VehicleInteractionResultPayload& result)
{
    EnqueueVehicleInteractionResult(result);
}

void OnVehicleState(const net::VehicleStatePayload& state)
{
    EnqueueVehicleState(state);
}

bool GetVehicleProxyNetId(unsigned int handle, uint32_t& netId)
{
    return FindVehicleProxyNetId(handle, netId);
}

bool GetLocalProxyVehicle(unsigned int localPed, uint32_t& netId,
                          unsigned int& handle, int& seat)
{
    return FindLocalProxyVehicle(localPed, netId, handle, seat);
}

bool GetProxyOccupantDriver(uint32_t netId, uint32_t& actorId)
{
    return FindProxyOccupantDriver(netId, actorId);
}

bool GetNearestVehicleProxy(unsigned int localPed, float radius,
                            uint32_t& netId, unsigned int& handle)
{
    return FindNearestVehicleProxy(localPed, radius, netId, handle);
}

bool HasInteractiveVehicleProxy()
{
    return HasInteractiveProxyImpl();
}

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
    // E11: 0x21 с vehicleNetId == trainId — это вагонные пассажиры поезда
    // (carIndex в entry); не путать с world vehicle-прокси.
    if (g_guestTrainNetId.load() != 0 &&
        vehicleNetId == g_guestTrainNetId.load())
    {
        RouteTrainOccupants(entries);
        return;
    }
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
    // v20 P1: guest — вернуть контролы/considerd-флаги до удаления прокси.
    ResetGuestVehicleInteraction();
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
    g_hostOccHandles.clear();
    g_vehicleScanHandles.clear();
    g_vehicleScanTick = 0;
    g_lastOccupantsCollectTick = 0;
    g_lastOccupantsCollectMs = 0;
    g_lastOccupantsSendMs.clear();
    g_occupantsSendWindowMs = 0;
    g_lastTrainOccupants.clear();
    g_lastTrainOccupantsMs.clear();
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
    ClearGuestTrainOccupants(true);
    g_perfGuestTrainSeatsApplied = 0;
    g_perfGuestTrainSeatsFailed = 0;
    g_lastTrainSnapshot.clear();
    g_lastTrainSendMs.clear();
    g_snapshotSeq = 0;
    g_hostIds.clear();
    g_lastHostScanTick = 0;
    // v20 P1: host — очередь интентов, idempotency-кеш, runtime states.
    ResetHostVehicleInteraction();

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
        // E11: vehicle + train occupant handles одним SetOccupantHandles
        // (actor discovery идёт после world_sync::Tick).
        if (coopcfg::Get().npcActor && coopcfg::Get().npcOccupants)
            actor_sync::SetOccupantHandles(g_hostOccHandles);
        // v20 P1: применить интенты гостя (<=2/тик) и разослать 0x24/0x26.
        ApplyVehicleInteractionIntents(localPlayerPed, tickCount);
        SendVehicleInteractionStates(localPlayerPed, tickCount);
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
    ProcessTrainOccupants(tickCount);
    // v20 P1: зеркало 0x24/0x26, осязаемый near-proxy и подавление контролов.
    ApplyPendingVehicleInteraction(localPlayerPed, tickCount);
    UpdateInteractiveProxy(localPlayerPed, tickCount);
    UpdateGuestVehicleControls(localPlayerPed);

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
