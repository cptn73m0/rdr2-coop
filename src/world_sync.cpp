#include "world_sync.h"

#include "coop.h"
#include "log.h"
#include "native_call.h"
#include "natives.h"
#include "net/protocol.h"
#include "script_hook.h"
#include "world_snapshot_parse.h"

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

void SendHostSnapshot(unsigned int localPlayerPed, unsigned int tickCount,
                      bool includeTrains)
{
    if (tickCount - g_lastHostScanTick < kSendEveryTicks)
        return;
    g_lastHostScanTick = tickCount;

    static int handles[2048];
    int count = sh::worldGetAllPeds(handles, 2048);
    if (count < 0) count = 0;
    if (count > 2048) count = 2048;

    const Vec3 origin = GetCoords(localPlayerPed);
    const unsigned int localMount = GetMountOf(localPlayerPed);

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
        if (d2 > kRadius2)
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

        std::unordered_set<unsigned int> seenVehicles;
        std::vector<unsigned int> trainEngines;

        // Добавляет один vehicle-handle как кандидат. Радиус/модель/netId —
        // те же правила, что и в основном цикле ниже.
        auto addVehicleCandidate = [&](unsigned int handle) -> bool
        {
            if (!handle || handle == localVehicle)
                return false;
            if (coop::IsWorldSyncExcluded(handle) || !DoesExist(handle))
                return false;
            const Vec3 p = GetCoords(handle);
            const float dx = p.x - origin.x;
            const float dy = p.y - origin.y;
            const float d2 = dx * dx + dy * dy;
            if (d2 > kRadius2)
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
            return true;
        };

        for (int i = 0; i < vehicleCount; ++i)
        {
            const unsigned int handle =
                static_cast<unsigned int>(vehicleHandles[i]);
            if (!handle || handle == localVehicle)
                continue;
            if (coop::IsWorldSyncExcluded(handle))
                continue;
            if (!DoesExist(handle))
                continue;
            seenVehicles.insert(handle);

            const Vec3 p = GetCoords(handle);
            const float dx = p.x - origin.x;
            const float dy = p.y - origin.y;
            const float d2 = dx * dx + dy * dy;
            if (d2 > kRadius2)
                continue;

            const unsigned int model = GetModel(handle);
            if (!model)
                continue;

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
                if (addVehicleCandidate(car))
                {
                    ++trainCarsAdded;
                    ++trainsIncluded;
                }
            }
            sh_log::Write("train: consist engine=%u trailerCount=%d added=%u "
                          "dir=%d speed=%.1f\n",
                          engine, trailerCount, trainCarsAdded,
                          trainDirection ? 1 : 0, trainSpeed);
        }
    }

    // Объекты: ТОЛЬКО mission-object. Амбиентные пропсы уже есть в мире
    // гостя (та же карта) — их дублирование дало бы двойные объекты/z-fighting.
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
            if (d2 > kRadius2)
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

    // Приоритет: mission-entity, затем ближайшие.
    std::sort(candidates.begin(), candidates.end(),
              [](const Candidate& a, const Candidate& b) {
                  if (a.mission != b.mission) return a.mission > b.mission;
                  return a.distance2 < b.distance2;
              });
    // Отдельные лимиты на Ped/Vehicle/Object, чтобы транспорт и объекты
    // не вытеснили людей.
    std::vector<Candidate> selected;
    selected.reserve(net::kMaxWorldEntities);
    int selectedPeds = 0;
    int selectedVehicles = 0;
    int selectedObjects = 0;
    for (const Candidate& c : candidates)
    {
        if (c.entity.kind == net::kEntityKindVehicle)
        {
            if (selectedVehicles >= kMaxVehicles) continue;
            ++selectedVehicles;
        }
        else if (c.entity.kind == net::kEntityKindObject)
        {
            if (selectedObjects >= kMaxObjects) continue;
            ++selectedObjects;
        }
        else
        {
            if (selectedPeds >= kMaxPeds) continue;
            ++selectedPeds;
        }
        selected.push_back(c);
    }
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

    if ((tickCount % kLogEveryTicks) == 1)
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
};

std::map<uint64_t, Proxy> g_proxies;
std::mutex g_snapshotMutex;
std::vector<uint8_t> g_pendingSnapshot;
bool g_hasPendingSnapshot = false;
std::atomic<unsigned long long> g_lastSnapshotMs{0};
uint64_t g_snapshotSeq = 0;

void SuppressLocalWorld()
{
    using namespace natives_api;
    Init(natives::_SET_AMBIENT_HUMAN_DENSITY_MULTIPLIER_THIS_FRAME);
    Push(0.0f); Call();
    Init(natives::_SET_AMBIENT_PED_DENSITY_MULTIPLIER_THIS_FRAME);
    Push(0.0f); Call();
    Init(natives::_SET_AMBIENT_ANIMAL_DENSITY_MULTIPLIER_THIS_FRAME);
    Push(0.0f); Call();
    Init(natives::SET_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME);
    Push(0.0f); Call();
    Init(natives::SET_RANDOM_VEHICLE_DENSITY_MULTIPLIER_THIS_FRAME);
    Push(0.0f); Call();
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
        // Транспорт/объекты: позиция из снапшота (физика не симулируется).
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

void DeleteProxy(Proxy& proxy)
{
    if (proxy.handle)
    {
        if (proxy.kind == net::kEntityKindVehicle)
            DeleteVehicleEntity(proxy.handle);
        else if (proxy.kind == net::kEntityKindObject)
            DeleteObjectEntity(proxy.handle);
        else
            DeleteEntity(proxy.handle);
    }
    proxy.handle = 0;
}

void ApplyPendingSnapshot(unsigned int tickCount)
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
        auto it = g_proxies.find(e.netId);
        if (it != g_proxies.end() && !DoesExist(it->second.handle))
        {
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
            DeleteProxy(it->second);
            it = g_proxies.erase(it);
        }
        else
        {
            ++it;
        }
    }

    if ((tickCount % kLogEveryTicks) == 1)
        sh_log::Write("world: guest proxies=%u seq=%llu\n",
                      static_cast<unsigned int>(g_proxies.size()),
                      static_cast<unsigned long long>(g_snapshotSeq));
}

} // namespace

void OnSnapshot(const std::vector<uint8_t>& payload)
{
    std::lock_guard<std::mutex> lk(g_snapshotMutex);
    g_pendingSnapshot = payload;
    g_hasPendingSnapshot = true;
    g_lastSnapshotMs.store(GetTickCount64());
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
    for (auto& kv : g_proxies)
        DeleteProxy(kv.second);
    const std::size_t removed = g_proxies.size();
    g_proxies.clear();

    {
        std::lock_guard<std::mutex> lk(g_snapshotMutex);
        g_pendingSnapshot.clear();
        g_hasPendingSnapshot = false;
        g_lastSnapshotMs.store(0);
    }
    g_snapshotSeq = 0;
    g_hostIds.clear();
    g_lastHostScanTick = 0;

    if (removed > 0)
        sh_log::Write("world: reset, removed %u proxies\n",
                      static_cast<unsigned int>(removed));
}

void Tick(unsigned int localPlayerPed, unsigned int tickCount,
          bool amHost, bool enabled, bool includeTrains)
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
        SendHostSnapshot(localPlayerPed, tickCount, includeTrains);
        return;
    }

    // Гость.
    ApplyPendingSnapshot(tickCount);

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

    SuppressLocalWorld();
}

} // namespace world_sync
} // namespace coop
