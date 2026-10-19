#include "world_sync.h"

#include "coop.h"
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

void SetFrozen(unsigned int handle, bool frozen)
{
    using namespace natives_api;
    Init(natives::FREEZE_ENTITY_POSITION);
    Push(handle);
    Push(static_cast<unsigned long long>(frozen ? 1 : 0));
    Call();
}

void FreezeEntitySafely(unsigned int handle, bool frozen)
{
    if (handle && DoesExist(handle))
        SetFrozen(handle, frozen);
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
unsigned int g_lastTrainConsistLogTick = 0;

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

        const bool mission = IsMissionEntity(handle);
        if (mission && !coopcfg::Get().worldMissionEntities)
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
        std::vector<unsigned int> scannedVehicles;

        // Добавляет один vehicle-handle как кандидат. Радиус/модель/netId —
        // те же правила, что и в основном цикле ниже.
        auto addVehicleCandidate = [&](unsigned int handle,
                                       net::WorldEntity* outEntity = nullptr) -> bool
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
            const bool mission = IsMissionEntity(handle);
            if (mission && !coopcfg::Get().worldMissionEntities)
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

            const bool mission = IsMissionEntity(handle);
            if (mission && !coopcfg::Get().worldMissionEntities)
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
                    // R&D: берём первый train-vehicle как «головной»; остальные
                    // train-хендлы могут быть теми же вагонами/локомотивом.
                    Init(natives::_GET_TRAIN_CARRIAGE_TRAILER_NUMBER);
                    Push(handle);
                    if (static_cast<int>(Call()) > 0 && trainEngines.empty())
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
            scannedVehicles.push_back(handle);
        }

        // R&D: собрать вагоны состава. Важно: worldGetAllVehicles обычно уже
        // содержит вагоны, поэтому НЕ фильтруем по seenVehicles для TrainSnapshot;
        // добавляем в WorldSnapshot только отсутствовавшие.
        unsigned int trainCarsAdded = 0;
        unsigned int trainCarsCollected = 0;
        int trainTrailerCount = 0;
        bool trainDirection = false;
        float trainSpeed = 0.f;
        std::vector<net::WorldEntity> trainCarEntities;
        if (!trainEngines.empty())
        {
            using namespace natives_api;
            const unsigned int engine = trainEngines.front();
            Init(natives::_GET_TRAIN_CARRIAGE_TRAILER_NUMBER);
            Push(engine);
            trainTrailerCount = static_cast<int>(Call());
            Init(natives::_GET_TRAIN_DIRECTION);
            Push(engine);
            trainDirection = Call() != 0;
            trainSpeed = GetSpeed(engine);

            std::unordered_set<unsigned int> consistHandles;
            for (int carIndex = 0; carIndex <= trainTrailerCount; ++carIndex)
            {
                Init(natives::GET_TRAIN_CARRIAGE);
                Push(engine);
                Push(static_cast<unsigned long long>(carIndex));
                const unsigned int car = static_cast<unsigned int>(Call());
                if (!car || car == engine || !consistHandles.insert(car).second)
                    continue;

                if (!seenVehicles.count(car))
                {
                    seenVehicles.insert(car);
                    net::WorldEntity candidateEntity{};
                    if (addVehicleCandidate(car, &candidateEntity))
                    {
                        ++trainCarsAdded;
                        ++trainsIncluded;
                    }
                }

                // Сущность для TrainSnapshot — даже если вагон уже был в
                // WorldSnapshot. netId выдаём/переиспользуем через g_hostIds.
                HostIdState& st = g_hostIds[car];
                const unsigned int model = GetModel(car);
                if (!model)
                    continue;
                if (st.netId == 0 || st.model != model)
                {
                    st.netId = g_nextNetId++;
                    st.model = model;
                }
                st.lastSeenTick = tickCount;

                net::WorldEntity ce{};
                ce.netId = st.netId;
                ce.model = model;
                const Vec3 cp = GetCoords(car);
                ce.x = cp.x; ce.y = cp.y; ce.z = cp.z;
                ce.heading = GetHeading(car);
                ce.kind = net::kEntityKindVehicle;
                ce.flags = 0;
                ce.state = 0;
                if (trainCarEntities.size() <
                    static_cast<std::size_t>(net::kMaxTrainCars))
                {
                    trainCarEntities.push_back(ce);
                    ++trainCarsCollected;
                }
            }

            // Лог не чаще раза в секунду (раньше 10 Гц спамил лог).
            if ((tickCount - g_lastTrainConsistLogTick) >= 60)
            {
                g_lastTrainConsistLogTick = tickCount;
                sh_log::Write("train: consist engine=%u trailerCount=%d "
                              "collected=%u added=%u dir=%d speed=%.1f\n",
                              engine, trainTrailerCount, trainCarsCollected,
                              trainCarsAdded, trainDirection ? 1 : 0,
                              trainSpeed);
            }
        }

        // R&D TrainSnapshot: engine netId/model + direction/speed + вагоны
        // по порядку (собранные через GET_TRAIN_CARRIAGE).
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

        // P1 crew: кто сидит в транспорте (водитель/пассажиры). Шлём привязки
        // netId-ов; гость сажает уже созданные ped-прокси в vehicle-прокси.
        if (coopcfg::Get().vehicleCrew)
        {
            using namespace natives_api;
            for (const unsigned int handle : scannedVehicles)
            {
                auto vit = g_hostIds.find(handle);
                if (vit == g_hostIds.end() || vit->second.netId == 0)
                    continue;

                Init(natives::GET_VEHICLE_MAX_NUMBER_OF_PASSENGERS);
                Push(handle);
                int maxPassengers = static_cast<int>(Call());
                if (maxPassengers < 0) maxPassengers = 0;
                if (maxPassengers > net::kMaxVehicleSeats - 1)
                    maxPassengers = net::kMaxVehicleSeats - 1;

                for (int seat = net::kVehicleSeatDriver;
                     seat < maxPassengers; ++seat)
                {
                    Init(natives::GET_PED_IN_VEHICLE_SEAT);
                    Push(handle);
                    Push(static_cast<int>(seat));
                    const unsigned int ped =
                        static_cast<unsigned int>(Call());
                    if (!ped || !DoesExist(ped))
                        continue;
                    auto pit = g_hostIds.find(ped);
                    if (pit == g_hostIds.end() || pit->second.netId == 0)
                        continue;

                    net::VehicleCrewPayload crew{};
                    crew.vehicleNetId = static_cast<uint32_t>(vit->second.netId);
                    crew.pedNetId = static_cast<uint32_t>(pit->second.netId);
                    crew.seat = static_cast<int8_t>(seat);
                    coop::SendVehicleCrew(crew);
                }
            }
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
            if (!coopcfg::Get().worldMissionEntities)
                continue; // safe mode: mission-objects отключены

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

    if ((g_hostSnapshotCount % 50) == 1)
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
    if ((g_hostSnapshotCount % 500) == 0)
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
    uint64_t netId = 0;
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
    // Плавная интерполяция позиции/heading между 10 Гц снапшотами.
    bool interpReady = false;
    bool hasInterp = false;
    Vec3 interpFrom{0.f, 0.f, 0.f};
    Vec3 interpTo{0.f, 0.f, 0.f};
    float interpFromHeading = 0.f;
    float interpToHeading = 0.f;
    uint64_t interpStartMs = 0;
    uint64_t interpDurationMs = 100;
    // Crew: если это водитель/пассажир, позицию ведёт транспорт.
    int8_t crewSeat = -1;
    uint32_t crewVehicleNetId = 0;
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
std::mutex g_crewMutex;
std::vector<net::VehicleCrewPayload> g_pendingCrew;
std::atomic<unsigned long long> g_lastCrewMs{0};

void SuppressLocalWorld()
{
    // Safe mode: глушим ТОЛЬКО ambient людей. Животные и транспорт не
    // глушатся — из-за этого раньше пропадали лошади/тягловые.
    using namespace natives_api;
    Init(natives::_SET_AMBIENT_HUMAN_DENSITY_MULTIPLIER_THIS_FRAME);
    Push(0.0f); Call();
    Init(natives::_SET_AMBIENT_PED_DENSITY_MULTIPLIER_THIS_FRAME);
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
        SetFrozen(obj, true);
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
        SetFrozen(veh, true);
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
    SetFrozen(handle, true);
    StandStill(handle);
    return handle;
}

constexpr uint64_t kProxyInterpMs = 100; // хост шлёт снапшоты ~10 Гц

float NormalizeAngleDelta(float from, float to)
{
    float d = std::fmod(to - from + 540.0f, 360.0f) - 180.0f;
    return d;
}

void BeginProxyInterp(Proxy& proxy, const net::WorldEntity& e)
{
    proxy.interpFrom = GetCoords(proxy.handle);
    proxy.interpTo = Vec3{e.x, e.y, e.z};
    proxy.interpFromHeading = GetHeading(proxy.handle);
    proxy.interpToHeading = e.heading;
    proxy.interpStartMs = GetTickCount64();
    proxy.interpDurationMs = kProxyInterpMs;
    proxy.hasInterp = true;
    if (coopcfg::Get().worldDebug)
    {
        static unsigned int dbgCount = 0;
        if ((++dbgCount % 90) == 0)
        {
            sh_log::Write("world: dbg interp netId=%u kind=%u "
                          "from=(%.1f %.1f %.1f) to=(%.1f %.1f %.1f)\n",
                          (unsigned)proxy.netId, proxy.kind,
                          proxy.interpFrom.x, proxy.interpFrom.y,
                          proxy.interpFrom.z, e.x, e.y, e.z);
        }
    }
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
        if (!proxy.interpReady)
        {
            // Первое появление: ставим сразу, без интерполяции.
            SetCoords(proxy.handle, e.x, e.y, e.z);
            SetHeading(proxy.handle, e.heading);
            proxy.interpReady = true;
            return;
        }
        // Дальше не телепортируем на каждом снапшоте, а плавно едем к
        // новой позиции в TickProxyInterpolation().
        BeginProxyInterp(proxy, e);
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
    // Crew: посадкой/позицией управляет транспорт; не двигаем педа.
    if (proxy.crewVehicleNetId != 0)
        return;

    // Все прокси двигаются плавной интерполяцией (шаг 10 Гц снапшотов),
    // а не задачами навигации. Это стабильнее для лошадей/педов в составе.
    if (!proxy.interpReady)
    {
        SetCoords(proxy.handle, e.x, e.y, e.z);
        SetHeading(proxy.handle, e.heading);
        proxy.interpReady = true;
        return;
    }
    BeginProxyInterp(proxy, e);
    return;
    // Ниже — старый task-путь; оставлен для истории/будущего AI-режима.
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

// Crew: посадить ped-прокси в vehicle-прокси по сообщению хоста.
void ApplyPendingCrew()
{
    std::vector<net::VehicleCrewPayload> batch;
    {
        std::lock_guard<std::mutex> lk(g_crewMutex);
        batch.swap(g_pendingCrew);
    }
    const unsigned long long now = GetTickCount64();
    const unsigned long long lastCrew = g_lastCrewMs.load();
    // Никто не сидит в транспорте: через 0.7 с снимаем stale crew-флаги.
    if (batch.empty())
    {
        if (lastCrew != 0 && now - lastCrew < 700)
            return;
        for (auto& kv : g_proxies)
        {
            if (kv.second.crewVehicleNetId == 0)
                continue;
            auto vit = g_proxies.find(kv.second.crewVehicleNetId);
            if (vit != g_proxies.end() && DoesExist(vit->second.handle) &&
                DoesExist(kv.second.handle))
            {
                using namespace natives_api;
                Init(natives::TASK_LEAVE_VEHICLE);
                Push(kv.second.handle);
                Push(vit->second.handle);
                Push(0);
                Push(0);
                Call();
            }
            kv.second.crewVehicleNetId = 0;
            kv.second.crewSeat = -1;
            FreezeEntitySafely(kv.second.handle, true);
        }
        return;
    }

    std::unordered_set<uint64_t> active;
    for (const net::VehicleCrewPayload& crew : batch)
    {
        active.insert((static_cast<uint64_t>(crew.vehicleNetId) << 32) |
                      static_cast<uint64_t>(crew.pedNetId));
        auto vit = g_proxies.find(crew.vehicleNetId);
        auto pit = g_proxies.find(crew.pedNetId);
        if (vit == g_proxies.end() || pit == g_proxies.end())
            continue;
        if (!DoesExist(vit->second.handle) || !DoesExist(pit->second.handle))
            continue;
        if (pit->second.kind != net::kEntityKindPed ||
            vit->second.kind == net::kEntityKindPed)
            continue;

        using namespace natives_api;
        Init(natives::IS_PED_IN_VEHICLE);
        Push(pit->second.handle);
        Push(vit->second.handle);
        Push(false);
        if (!Call())
        {
            Init(natives::SET_PED_INTO_VEHICLE);
            Push(pit->second.handle);
            Push(vit->second.handle);
            Push(static_cast<int>(crew.seat));
            Call();

            Init(natives::IS_PED_IN_VEHICLE);
            Push(pit->second.handle);
            Push(vit->second.handle);
            Push(false);
            if (!Call())
                continue; // повторим на следующем снапшоте
            FreezeEntitySafely(pit->second.handle, false);
            sh_log::Write("vehicle: crew seated ped=%u vehicle=%u seat=%d\n",
                          crew.pedNetId, crew.vehicleNetId, (int)crew.seat);
        }
        pit->second.crewSeat = crew.seat;
        pit->second.crewVehicleNetId = crew.vehicleNetId;
    }

    // Снимаем crew с тех, кого больше нет в активном наборе хоста.
    for (auto& kv : g_proxies)
    {
        if (kv.second.crewVehicleNetId == 0)
            continue;
        const uint64_t key =
            (static_cast<uint64_t>(kv.second.crewVehicleNetId) << 32) |
            static_cast<uint64_t>(kv.first);
        if (active.count(key))
            continue;
        auto vit = g_proxies.find(kv.second.crewVehicleNetId);
        if (vit != g_proxies.end() && DoesExist(vit->second.handle) &&
            DoesExist(kv.second.handle))
        {
            using namespace natives_api;
            Init(natives::TASK_LEAVE_VEHICLE);
            Push(kv.second.handle);
            Push(vit->second.handle);
            Push(0);
            Push(0);
            Call();
        }
        kv.second.crewVehicleNetId = 0;
        kv.second.crewSeat = -1;
        FreezeEntitySafely(kv.second.handle, true);
    }
}

// Плавное движение vehicle/object прокси между снапшотами (каждый игровой
// тик, а не только при приходе 10 Гц снапшота).
void TickProxyInterpolation()
{
    if (g_proxies.empty())
        return;
    static unsigned int dbgTick = 0;
    ++dbgTick;
    const bool dbgLog = coopcfg::Get().worldDebug && (dbgTick % 30) == 0;
    bool dbgLogged = false;
    const uint64_t now = GetTickCount64();
    for (auto& kv : g_proxies)
    {
        Proxy& p = kv.second;
        if (p.kind == net::kEntityKindPed && p.crewVehicleNetId != 0)
            continue;
        if (!p.hasInterp || !p.handle || !DoesExist(p.handle) ||
            p.trainAttached)
            continue;
        float alpha = 1.0f;
        if (p.interpDurationMs > 0)
        {
            alpha = static_cast<float>(now - p.interpStartMs) /
                    static_cast<float>(p.interpDurationMs);
            if (alpha < 0.f) alpha = 0.f;
            if (alpha > 1.f) alpha = 1.f;
        }
        const float x = p.interpFrom.x + (p.interpTo.x - p.interpFrom.x) * alpha;
        const float y = p.interpFrom.y + (p.interpTo.y - p.interpFrom.y) * alpha;
        const float z = p.interpFrom.z + (p.interpTo.z - p.interpFrom.z) * alpha;
        const float heading = p.interpFromHeading +
            NormalizeAngleDelta(p.interpFromHeading, p.interpToHeading) * alpha;
        SetCoords(p.handle, x, y, z);
        SetHeading(p.handle, heading);
        if (dbgLog && !dbgLogged)
        {
            const Vec3 actual = GetCoords(p.handle);
            sh_log::Write("world: dbg move netId=%u kind=%u alpha=%.2f "
                          "set=(%.1f %.1f %.1f) actual=(%.1f %.1f %.1f)\n",
                          (unsigned)p.netId, p.kind, alpha, x, y, z,
                          actual.x, actual.y, actual.z);
            dbgLogged = true;
        }
        if (alpha >= 1.0f)
        {
            p.hasInterp = false;
            p.interpFrom = p.interpTo;
            p.interpFromHeading = p.interpToHeading;
        }
    }
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
            proxy.netId = e.netId;
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

void OnSnapshot(const std::vector<uint8_t>& payload)
{
    std::lock_guard<std::mutex> lk(g_snapshotMutex);
    g_pendingSnapshot = payload;
    g_hasPendingSnapshot = true;
    g_lastSnapshotMs.store(GetTickCount64());
}

void OnTrainSnapshot(const std::vector<uint8_t>& payload)
{
    std::lock_guard<std::mutex> lk(g_trainMutex);
    g_pendingTrain = payload;
    g_hasPendingTrain = true;
}

void OnVehicleCrew(const net::VehicleCrewPayload& crew)
{
    std::lock_guard<std::mutex> lk(g_crewMutex);
    g_lastCrewMs.store(GetTickCount64());
    // Храним последние привязки; повторные снапшоты могут дублироваться.
    for (const auto& existing : g_pendingCrew)
    {
        if (existing.vehicleNetId == crew.vehicleNetId &&
            existing.pedNetId == crew.pedNetId &&
            existing.seat == crew.seat)
            return;
    }
    if (g_pendingCrew.size() < 256)
        g_pendingCrew.push_back(crew);
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
    {
        std::lock_guard<std::mutex> lk(g_trainMutex);
        g_pendingTrain.clear();
        g_hasPendingTrain = false;
    }
    {
        std::lock_guard<std::mutex> lk(g_crewMutex);
        g_pendingCrew.clear();
        g_lastCrewMs.store(0);
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
    ApplyPendingTrainSnapshot(tickCount);
    ApplyPendingCrew();

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

    if (coopcfg::Get().worldSuppressAmbient)
        SuppressLocalWorld();

    TickProxyInterpolation();
}

} // namespace world_sync
} // namespace coop
