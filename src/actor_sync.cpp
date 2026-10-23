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

// E2a: meta-outfit внешность NPC-прокси (guest) и сглаживание движения.
constexpr int kMaxAppearanceRequestsPerTick = 2;        // бюджет REQUEST
constexpr unsigned int kAppearanceTimeoutTicks = 180;  // 3 с ожидания наряда
constexpr float kRadPerDeg = 0.017453292519943295f;

// E2a.2: smooth walk gait — trail-following (полилиния позиций хоста) +
// одна долгоживущая go-to задача с in-place update цели.
constexpr int kTrailLen = 10;                    // 1 с истории позиций (10 Гц)
constexpr float kVelocityEwma = 0.3f;            // EWMA скорости/направления
constexpr float kSpeedByteToMetersPerSec = 8.0f / 255.0f; // ActorFrame.speed -> 0..8 м/с
constexpr float kLookaheadBase = 0.5f;           // L = 0.5 + hostSpeed*0.45
constexpr float kLookaheadPerSpeed = 0.45f;
constexpr float kLookaheadMinMeters = 0.9f;      // clamp L
constexpr float kLookaheadMaxMeters = 3.0f;
constexpr float kSlowHostSpeed = 0.2f;           // хост почти стоит
constexpr float kSlowCatchupStep = 0.3f;         // цель = позиция хоста + 0.3 по трейлу
constexpr float kArriveDistMeters = 0.5f;        // idle: ближе — StandStill
constexpr float kFollowStoppingRange = 0.25f;    // 0.2..0.3: не гасим задачу у точки
constexpr unsigned int kMotionUpdateTicks = 6;   // in-place update цели 10 Гц
constexpr unsigned int kMotionStatusCheckTicks = 6; // проверка status 10 Гц
constexpr unsigned int kGaitRefreshTicks = 10;   // gait не реже 1 раза в ~10 тиков
constexpr float kFollowHeadingSentinel = 40000.f; // «не поворачивать» (как в rdr2-runtime)
constexpr float kIdleNoMoveDist = 0.5f;          // idle: ближе — не трогаем

// E2b: всадник + лошадь как ОДИН actor. ActorSpawn.reserved = хэш модели
// лошади (0 = пеший); протокол/размер не меняются. Host moveStyle=RideHorse
// (3, как договорено в E2b) для всадника; гость ведёт ЛОШАДЬ, rider в седле.
constexpr uint8_t kMoveStyleRideHorse = 3;          // host moveStyle для всадника
constexpr int kMaxMountsPerTick = 1;                // создание лошади/тик
constexpr unsigned int kMountDismountDelayTicks = 45;  // 0.75 с — удалить horse
constexpr unsigned int kMountDeathDeleteTicks = 150;   // 2.5 с — удалить horse
constexpr unsigned int kMountReseatCheckTicks = 60;    // 1 с — reseat watchdog
constexpr int kMountMaxSeatAttempts = 3;            // после — fallback пешком
// Причины actor: mount despawn (контракт игрового слоя).
constexpr unsigned int kMountReasonDismount = 1;
constexpr unsigned int kMountReasonDespawn = 2;
constexpr unsigned int kMountReasonDead = 3;
constexpr unsigned int kMountReasonSeatFailed = 4;

// E3a: сценарий (поза) и смерть/труп.
constexpr unsigned int kScenarioVerifyTicks = 120; // 2 с на вход в сценарий
constexpr int kHumanMaxHealth = 200;              // fallback, если натив вернул 0
// E3a.3: оптимистичное подтверждение — после TASK_START_SCENARIO_IN_PLACE_HASH
// считаем позу применённой (verify-геттеры для части поз врут). Варпающие
// fallback'и (scenario point / at-position) оставлены заготовкой под флагом,
// но в цепочке НЕ используются: они переставляли/сваривали NPC.
constexpr bool kScenarioFallbackEnabled = false;

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

    void Clear()
    {
        count = 0;
        pos = 0;
    }
};

PerfP95 g_perfScan;
PerfP95 g_perfSpawn;
PerfP95 g_perfApply;
// E2a: ошибка позиции (target vs ped), семплим в мм, логируем/1000 = метры.
PerfP95 g_motionErr;
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

// E3a.1: dyn2 — «kneeling, sitting, squating, sleeping» scenario hashes.
uint32_t GetDynamicScenario2(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::_GET_ACTIVE_DYNAMIC_SCENARIO_2);
    Push(ped);
    return static_cast<uint32_t>(Call());
}

// E3a.1: hash сценария для репликации: dyn2 (валидный для поз), иначе dyn1.
uint32_t GetHostScenarioHash(unsigned int ped)
{
    const uint32_t h2 = GetDynamicScenario2(ped);
    if (h2) return h2;
    return GetDynamicScenario(ped);
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

// ---------------- E2a: meta-ped-outfit запросы (per-actor, не singleton) ----------------
// Как в coop.cpp для player-ghost: REQUEST -> HAS_LOADED -> APPLY -> RELEASE.
int RequestMetaPedOutfit(unsigned int model, uint32_t outfit)
{
    using namespace natives_api;
    Init(natives::REQUEST_META_PED_OUTFIT);
    Push(static_cast<unsigned long long>(model));
    Push(static_cast<unsigned long long>(outfit));
    return static_cast<int>(Call());
}

bool HasMetaPedOutfitLoaded(int requestId)
{
    if (!requestId) return false;
    using namespace natives_api;
    Init(natives::HAS_META_PED_OUTFIT_LOADED);
    Push(static_cast<unsigned long long>(static_cast<int>(requestId)));
    return Call() != 0;
}

bool ApplyMetaPedOutfit(int requestId, unsigned int ped)
{
    if (!requestId || !ped) return false;
    using namespace natives_api;
    Init(natives::APPLY_PED_META_PED_OUTFIT);
    Push(static_cast<unsigned long long>(static_cast<int>(requestId)));
    Push(ped);
    Push(true);
    Push(true);
    return Call() != 0;
}

void ReleaseMetaPedOutfitRequest(int requestId)
{
    if (!requestId) return;
    using namespace natives_api;
    Init(natives::RELEASE_META_PED_OUTFIT_REQUEST);
    Push(static_cast<unsigned long long>(static_cast<int>(requestId)));
    Call();
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

// E3a: health/смерть/оживление/сценарий-поза.
void SetEntityHealthValue(unsigned int handle, int health)
{
    using namespace natives_api;
    Init(natives::SET_ENTITY_HEALTH);
    Push(handle);
    Push(health);
    Push(static_cast<unsigned long long>(0)); // entityKilledBy
    Call();
}

int GetEntityMaxHealthValue(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::GET_ENTITY_MAX_HEALTH);
    Push(handle);
    Push(false);
    return static_cast<int>(Call());
}

void ResurrectPedEntity(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::RESURRECT_PED);
    Push(handle);
    Call();
}

// TASK_START_SCENARIO_IN_PLACE_HASH(ped, hash, duration, playEnterAnim,
// conditionalHash, heading, p6) — 7 арг по NativeDecl.
void StartScenarioInPlace(unsigned int ped, uint32_t scenarioHash)
{
    using namespace natives_api;
    Init(natives::TASK_START_SCENARIO_IN_PLACE_HASH);
    Push(ped);
    Push(static_cast<unsigned long long>(scenarioHash));
    Push(static_cast<unsigned long long>(0xFFFFFFFF)); // duration -1 (бессрочно)
    Push(true);                                        // playEnterAnim
    Push(static_cast<unsigned long long>(0));          // conditionalHash
    Push(0.f);                                         // heading (как есть)
    Push(false);                                       // p6
    Call();
}

// E3a.1: fallback-поза в текущей точке (если in-place hash не подошёл).
// TASK_START_SCENARIO_AT_POSITION(ped,hash,x,y,z,heading,duration,sitting,
// teleport,p9str,p10f,p11bool) — 12 арг по NativeDecl.
void StartScenarioAtPosition(unsigned int ped, uint32_t scenarioHash,
                             float x, float y, float z, float heading,
                             bool sitting)
{
    static char emptyString[] = "";
    using namespace natives_api;
    Init(natives::TASK_START_SCENARIO_AT_POSITION);
    Push(ped);
    Push(static_cast<unsigned long long>(scenarioHash));
    Push(x); Push(y); Push(z); Push(heading);
    Push(static_cast<unsigned long long>(0xFFFFFFFF)); // duration -1
    Push(sitting); // isSittingScenario
    Push(false);   // teleportToScenario
    Push(reinterpret_cast<unsigned long long>(emptyString)); // p9: ""
    Push(-1.0f); // p10
    Push(false); // p11
    Call();
}

// E3a.2: canonical scenario point path (sit/work с пропом: стул/костёр).
int CreateScenarioPointHash(uint32_t scenarioHash, float x, float y, float z,
                            float heading)
{
    using namespace natives_api;
    Init(natives::CREATE_SCENARIO_POINT_HASH);
    Push(static_cast<unsigned long long>(scenarioHash));
    Push(x); Push(y); Push(z); Push(heading);
    Push(static_cast<unsigned long long>(0)); // p5
    Push(static_cast<unsigned long long>(0)); // p6
    Push(false);                              // p7
    return static_cast<int>(Call());
}

bool DoesScenarioPointExist(int point)
{
    if (!point) return false;
    using namespace natives_api;
    Init(natives::DOES_SCENARIO_POINT_EXIST);
    Push(static_cast<unsigned long long>(static_cast<int>(point)));
    return Call() != 0;
}

uint32_t GetScenarioPointType(int point)
{
    if (!point) return 0;
    using namespace natives_api;
    Init(natives::_GET_SCENARIO_POINT_TYPE);
    Push(static_cast<unsigned long long>(static_cast<int>(point)));
    return static_cast<uint32_t>(Call());
}

void DeleteScenarioPoint(int point)
{
    if (!point) return;
    using namespace natives_api;
    Init(natives::_DELETE_SCENARIO_POINT);
    Push(static_cast<unsigned long long>(static_cast<int>(point)));
    Call();
}

// TASK_USE_SCENARIO_POINT(ped, point, "", 0, false, false, 0, false, 0.0f, false)
void UseScenarioPoint(unsigned int ped, int point)
{
    static char emptyString[] = "";
    using namespace natives_api;
    Init(natives::TASK_USE_SCENARIO_POINT);
    Push(ped);
    Push(static_cast<unsigned long long>(static_cast<int>(point)));
    Push(reinterpret_cast<unsigned long long>(emptyString)); // conditionalAnim ""
    Push(static_cast<unsigned long long>(0)); // p3
    Push(false); // p4
    Push(false); // p5
    Push(static_cast<unsigned long long>(0)); // p6
    Push(false); // p7
    Push(0.0f);  // p8
    Push(false); // p9
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

// E2a.2: держим желаемый gait (walk/run/sprint) без перезапуска анимации.
void SetDesiredMoveBlend(unsigned int ped, float value)
{
    using namespace natives_api;
    Init(natives::SET_PED_DESIRED_MOVE_BLEND_RATIO);
    Push(ped);
    Push(value);
    Call();
}

// E2a.2: фактический desired gait хоста (упаковывается в ActorFrame.pad).
float GetDesiredMoveBlendRatio(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::GET_PED_DESIRED_MOVE_BLEND_RATIO);
    Push(ped);
    return CallFloat();
}

// E2a.2: статус script-задачи (7 not_found / 8 finished → задачу надо выдать заново).
int GetScriptTaskStatus(unsigned int ped, uint32_t taskHash)
{
    using namespace natives_api;
    Init(natives::GET_SCRIPT_TASK_STATUS);
    Push(ped);
    Push(static_cast<unsigned long long>(taskHash));
    Push(true);
    return static_cast<int>(Call());
}

// E2a.2: in-place update уже бегущей go-to задачи (не перезапускает анимацию).
void UpdateTaskGoToCoord(unsigned int ped, float x, float y, float z,
                         float speed, float tolerance)
{
    using namespace natives_api;
    Init(natives::_UPDATE_TASK_GO_TO_COORD_WITH_OFFSET);
    Push(ped);
    Push(x); Push(y); Push(z);
    Push(0.f); Push(0.f); Push(0.f); // offset XYZ
    Push(speed);
    Push(tolerance);
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
    Push(kFollowStoppingRange);                        // 0.25: не гасим у точки
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

// E2a: внешность NPC — состояния requested/applied/failed (каждое изменение).
void LogAppearance(uint32_t actorId, uint32_t model, uint32_t outfit,
                   const char* state)
{
    sh_log::Write("actor: appearance id=%u model=0x%X outfit=0x%X state=%s\n",
                  actorId, model, outfit, state);
}

// E2b: логи всадника/лошади (гейт enabled снаружи — module пути).
void LogMountSpawn(uint32_t actorId, uint32_t model)
{
    sh_log::Write("actor: mount spawn id=%u model=0x%X\n", actorId, model);
}

void LogMountFailed(uint32_t actorId, const char* reason)
{
    sh_log::Write("actor: mount failed id=%u reason=%s\n", actorId, reason);
}

void LogMountDespawn(uint32_t actorId, unsigned int reason)
{
    sh_log::Write("actor: mount despawn id=%u reason=%u\n", actorId, reason);
}

// E3a: сценарии/позы NPC и смерть/труп (гейт enabled снаружи).
// E3a.3: если verify != nullptr — диагностическая строка оптимистичного
// подтверждения (state=ok verify=flag|assumed).
void LogScenarioApply(uint32_t actorId, uint32_t hash, bool ok,
                      const char* verify = nullptr)
{
    if (verify)
    {
        sh_log::Write(
            "actor: scenario apply id=%u hash=0x%X state=%s verify=%s\n",
            actorId, hash, ok ? "ok" : "failed", verify);
        return;
    }
    sh_log::Write("actor: scenario apply id=%u hash=0x%X state=%s\n",
                  actorId, hash, ok ? "ok" : "failed");
}

// E3a.1: диагностика, какой вариант упал (inplace/atpos).
void LogScenarioFailed(uint32_t actorId, uint32_t hash, const char* stage)
{
    sh_log::Write("actor: scenario failed id=%u hash=0x%X stage=%s\n",
                  actorId, hash, stage);
}

// E3a.2: scenario point: создание/использование и его тип.
void LogScenarioPoint(uint32_t actorId, uint32_t hash, int point,
                      uint32_t type, bool ok)
{
    sh_log::Write(
        "actor: scenario stage=point %s id=%u hash=0x%X point=%d type=0x%X\n",
        ok ? "ok" : "failed", actorId, hash, point, type);
}

void LogDeath(uint32_t actorId)
{
    sh_log::Write("actor: death id=%u\n", actorId);
}

void LogRevive(uint32_t actorId)
{
    sh_log::Write("actor: revive id=%u\n", actorId);
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
    // E2b: всадник — один actor. ActorSpawn.reserved = хэш модели лошади
    // (только когда флаг mounted); mountActorId не используем (лошадь не
    // отдельный actor, чтобы не менять протокол/идентификаторы).
    const unsigned int mountHandle = GetMountOf(handle);
    const bool mounted = mountHandle != 0 && DoesExist(mountHandle);
    if (mounted)
    {
        s.flags |= net::kActorFlagMounted;
        s.reserved = GetModelOf(mountHandle);
        s.moveStyle = kMoveStyleRideHorse;
    }
    else
    {
        s.reserved = 0;
        s.moveStyle = MoveStateFromSpeed(GetSpeed(handle));
    }
    s.mountActorId = 0;
    const uint32_t held = GetHeldWeapon(handle);
    s.weaponHash = held ? held : GetCurrentWeapon(handle);
    // E3a: сценарий-поза (sit/work). Для мёртвых сценарий не шлём: dead flag +
    // health=0, труп остаётся на месте.
    const bool dead = IsDeadPed(handle);
    const bool scenario = !dead && IsScenarioActive(handle);
    // E3a.1: dyn2 даёт валидные scenario-hash поз (sit/work); dyn1 — fallback.
    s.scenarioHash = scenario ? GetHostScenarioHash(handle) : 0;
    if (dead)
        s.flags &= static_cast<uint8_t>(~net::kActorFlagScenario);
    const Vec3 p = GetCoords(handle);
    s.x = p.x; s.y = p.y; s.z = p.z;
    s.heading = GetHeading(handle);
    s.health = GetHealthByte(handle);
    s.phase = 0;
    return s;
}

net::ActorFrame BuildFrame(unsigned int handle, uint32_t actorId)
{
    net::ActorFrame f{};
    f.actorId = actorId;
    // E2b: для всадника скорость/heading берём с лошади (rider стоит на месте
    // относительно седла), moveState=RideHorse; гость по флагу mounted ведёт
    // лошадь трейл-целью.
    const unsigned int mountHandle = GetMountOf(handle);
    const bool mounted = mountHandle != 0 && DoesExist(mountHandle);
    const float speed = mounted ? GetSpeed(mountHandle) : GetSpeed(handle);
    const uint8_t move = mounted ? kMoveStyleRideHorse
                                 : MoveStateFromSpeed(speed);
    f.flags = BuildFlags(handle);
    if (mounted)
        f.flags |= net::kActorFlagMounted;
    // E3a: у мёртвых сценарий не реплицируем (только dead flag + труп).
    if (IsDeadPed(handle))
        f.flags &= static_cast<uint16_t>(~net::kActorFlagScenario);
    f.animState = move;
    // E2a.2: ActorFrame.speed — u8 0..255 соответствует 0..8 м/с (шкала
    // поднята с 3 м/с: sprint/галоп больше не сатурируется на 3).
    float sp = speed / 8.0f;
    if (sp < 0.f) sp = 0.f;
    if (sp > 1.f) sp = 1.f;
    f.speed = static_cast<uint8_t>(sp * 255.0f);
    // E2a.2: gait хоста (GET_PED_DESIRED_MOVE_BLEND_RATIO, 0..3) пакуем в
    // ActorFrame.pad без изменения размера кадра/протокола.
    float gait = GetDesiredMoveBlendRatio(handle);
    if (gait < 0.f) gait = 0.f;
    if (gait > 6.5f) gait = 6.5f; // защита от мусора: 0..6.5 -> 0..6500
    f.pad = static_cast<uint16_t>(gait * 1000.0f + 0.5f);
    const Vec3 p = GetCoords(handle);
    f.x = p.x; f.y = p.y; f.z = p.z;
    f.heading = mounted ? GetHeading(mountHandle) : GetHeading(handle);
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
    uint32_t metaOutfit = 0; // E2a: внешность (0 = random variation)
    float tx = 0.f, ty = 0.f, tz = 0.f, heading = 0.f;
    uint8_t moveState = 0;
    uint8_t speed = 0; // E2a: ActorFrame.speed (0..255 -> 0..3 м/с)
    uint16_t flags = 0;
    unsigned int lastFrameTick = 0;
    unsigned int spawnedTick = 0;
    int lastMoveState = -1;
    bool moveTaskValid = false;
    Vec3 moveTarget{0.f, 0.f, 0.f};
    bool dead = false;
    bool resyncLogged = false;
    // E2a: per-actor meta-outfit request state (не блокирует прокси).
    int outfitRequest = 0;              // id REQUEST_META_PED_OUTFIT
    unsigned int outfitRequestTick = 0; // тик запроса (таймаут 3 с)
    uint8_t appearanceState = 0;        // 0 none, 1 requested, 2 applied, 3 failed
    // E2a.2: trail движения (последние ~1 с позиций) и сглаженные
    // скорость/направление для lookahead-ведения цели.
    struct TrailPoint
    {
        float x = 0.f, y = 0.f, z = 0.f;
    };
    TrailPoint trail[kTrailLen]{};
    int trailCount = 0;
    int trailHead = 0;
    float dirX = 0.f;
    float dirY = 1.f;
    bool dirValid = false;
    float ewmaSpeed = 0.f; // м/с
    uint16_t gaitQ = 0;    // E2a.2: gait хоста из ActorFrame.pad (ratio*1000)
    float lastGait = -1.f;
    unsigned int lastGaitTick = 0;
    // E2a.2: одна долгоживущая go-to задача + in-place update цели.
    uint32_t taskHash = 0;
    bool taskActive = false;
    unsigned int taskIssuedTick = 0;
    unsigned int lastMotionUpdateTick = 0;
    unsigned int lastStatusTick = 0;
    // E2b: лошадь всадника (один actor = rider+horse; mountModel = reserved).
    unsigned int mountHandle = 0;
    unsigned int mountModel = 0;   // ActorSpawn.reserved (хэш модели лошади)
    bool mounted = false;          // текущее состояние из кадров (flags)
    bool mountFailedLogged = false;
    unsigned int dismountTick = 0; // тик, когда mounted стало 0
    unsigned int mountDeathTick = 0; // тик смерти rider (horse удаляем позже)
    unsigned int lastReseatTick = 0;
    int mountSeatAttempts = 0;
    // Долгоживущая go-to задача для ЛОШАДИ (rider не двигаем).
    uint32_t mountTaskHash = 0;
    bool mountTaskActive = false;
    unsigned int mountLastUpdateTick = 0;
    unsigned int mountLastStatusTick = 0;
    unsigned int mountLastGaitTick = 0;
    float mountLastGait = -1.f;
    Vec3 mountMoveTarget{0.f, 0.f, 0.f};
    // E3a: сценарий (поза) и смерть/труп.
    uint32_t scenarioHash = 0;    // ActorSpawn.scenarioHash (dyn scenario)
    uint32_t scenarioApplied = 0; // какой hash реально применяли
    bool scenarioFlag = false;    // kActorFlagScenario из кадров
    uint8_t scenarioState = 0;    // 0 none,1 inplace,2 active,3 point,4 atpos,5 failed
    unsigned int scenarioTick = 0;
    int scenarioPoint = 0;        // E3a.2: handle scenario point (для sit/work)
    uint32_t scenarioPointType = 0;
    bool deathApplied = false;    // health=0 + invincible=false применены
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
int g_appearanceRequestsThisTick = 0; // E2a: бюджет REQUEST_META_PED_OUTFIT
int g_mountsCreatedThisTick = 0;      // E2b: <=1 лошадь/тик
unsigned int g_appliedFrames = 0;
unsigned int g_timeouts = 0;
unsigned int g_lastNoCollisionTick = 0;
// E2a.2: счётчики движения за окно сводки (5 с), лог при npc_debug=1.
unsigned int g_motionIssued = 0;
unsigned int g_motionReissue = 0;
unsigned int g_motionGaitRestart = 0;

// E2b: общая настройка созданного педа (rider или horse) — mission/visible/
// invincible/LOD/no-collision + ground snap у вызывающего.
unsigned int CreatePedWithSetup(unsigned int model, float x, float y, float z,
                                float heading, const TickContext& ctx,
                                bool isMount)
{
    const unsigned int handle = CreatePed(model, x, y, z, heading);
    if (!handle)
        return 0;
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
    SetCoordsNative(handle, x, y, z);
    SetHeadingNative(handle, heading);
    if (isMount)
        SetMoveBlend(handle, 3.0f);
    return handle;
}

// Удаление одной сущности прокси с mission-unmark; при неудаче (DoesExist)
// НЕ теряем учёт: handle остаётся, вызывающий сохраняет proxy (как в E1).
bool DeleteEntitySafe(unsigned int& handle, unsigned int model,
                      uint32_t actorId)
{
    if (!handle)
        return true;
    SetMissionEntityFlag(handle, false);
    DeletePedHandle(handle);
    if (DoesExist(handle))
    {
        LogDeleteFailed(actorId, handle);
        return false;
    }
    SetModelAsNoLongerNeeded(model);
    handle = 0;
    return true;
}

// E2b: удалить лошадь прокси; возвращает true, когда учёт можно чистить.
bool TryDeleteMount(Proxy& p, unsigned int reason)
{
    if (!p.mountHandle)
        return true;
    if (!DeleteEntitySafe(p.mountHandle, p.mountModel, p.actorId))
        return false;
    p.mounted = false;
    p.mountDeathTick = 0;
    LogMountDespawn(p.actorId, reason);
    return true;
}

// E2b: посадить rider в седло лошади (seatIndex -1 = любое, p3=false).
bool SeatRiderOnMount(Proxy& p)
{
    if (!p.handle || !p.mountHandle)
        return false;
    if (!DoesExist(p.handle) || !DoesExist(p.mountHandle))
        return false;
    using namespace natives_api;
    Init(natives::SET_PED_ONTO_MOUNT);
    Push(p.handle);
    Push(p.mountHandle);
    Push(static_cast<unsigned long long>(0xFFFFFFFF)); // seatIndex -1
    Push(false);
    Call();
    return true;
}

// P2-фикс (task-45): перед DELETE_PED снимаем mission-entity; если сущность
// всё равно существует — НЕ теряем учёт (proxy остаётся в g_proxies, повтор).
bool DeleteProxy(Proxy& proxy)
{
    // E2a: не оставляем висящий meta-outfit-запрос при удалении прокси.
    if (proxy.outfitRequest)
    {
        ReleaseMetaPedOutfitRequest(proxy.outfitRequest);
        proxy.outfitRequest = 0;
        proxy.appearanceState = 3;
    }

    bool ok = true;

    // E3a.2: удалить scenario point (проп) при despawn/epoch cleanup.
    if (proxy.scenarioPoint)
    {
        DeleteScenarioPoint(proxy.scenarioPoint);
        proxy.scenarioPoint = 0;
        proxy.scenarioPointType = 0;
    }

    // E2b: всадник — это rider+horse; удаляем обе сущности. Если хоть одна
    // не удалилась — proxy остаётся в учёте (повтор/таймаут), не теряем.
    if (proxy.handle)
    {
        if (DeleteEntitySafe(proxy.handle, proxy.model, proxy.actorId))
            g_handleByActor.erase(proxy.actorId);
        else
            ok = false;
    }
    else
    {
        g_handleByActor.erase(proxy.actorId);
    }

    if (proxy.mountHandle)
    {
        if (!TryDeleteMount(proxy, kMountReasonDespawn))
            ok = false;
    }

    return ok;
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

    // E2b: всадник. Пока модель лошади не загружена или исчерпан бюджет
    // (1 лошадь/тик) — pending ждёт; при неудаче CREATE_PED rider остаётся
    // пешим (fallback, лог actor: mount failed).
    const bool wantMount =
        (s.flags & net::kActorFlagMounted) != 0 && s.reserved != 0;
    if (wantMount)
    {
        if (!HasModelLoaded(s.reserved))
            return false;
        if (g_mountsCreatedThisTick >= kMaxMountsPerTick)
            return false;
    }

    float z = s.z;
    float gz = 0.f;
    if (GroundZ(s.x, s.y, s.z + 1.0f, gz))
        z = gz + 0.2f;

    const unsigned int handle =
        CreatePedWithSetup(s.model, s.x, s.y, z, s.heading, ctx, false);
    if (!handle)
        return false;

    Proxy p;
    p.actorId = s.actorId;
    p.epoch = g_guestEpoch;
    p.handle = handle;
    p.model = s.model;
    p.metaOutfit = s.metaOutfit;
    p.tx = s.x; p.ty = s.y; p.tz = z;
    p.heading = s.heading;
    p.moveState = s.moveStyle;
    p.speed = (s.moveStyle >= 3) ? 255
            : (s.moveStyle == 2) ? 170
            : (s.moveStyle == 1) ? 85 : 0;
    p.flags = s.flags;
    p.lastFrameTick = ctx.tickCount;
    p.spawnedTick = ctx.tickCount;
    p.lastMoveState = -1;
    p.dead = (s.flags & net::kActorFlagDead) != 0;
    p.appearanceState = 0; // E2a: запрос наряда сделает GuestTick (бюджет)
    // E2a.2: стартовый trail/направление для lookahead (heading = взгляд педа).
    p.trail[0].x = s.x;
    p.trail[0].y = s.y;
    p.trail[0].z = z;
    p.trailCount = 1;
    p.trailHead = 1 % kTrailLen;
    {
        const float rad = s.heading * kRadPerDeg;
        p.dirX = -std::sin(rad);
        p.dirY = std::cos(rad);
        p.dirValid = true;
    }
    p.ewmaSpeed = static_cast<float>(p.speed) * kSpeedByteToMetersPerSec;
    p.gaitQ = static_cast<uint16_t>(p.moveState * 1000);
    p.lastGait = static_cast<float>(p.moveState);
    p.lastGaitTick = ctx.tickCount;
    p.taskHash = natives::SCRIPT_TASK_FOLLOW_NAV_MESH_TO_COORD;
    p.taskActive = false;
    p.taskIssuedTick = ctx.tickCount;
    p.lastMotionUpdateTick = ctx.tickCount;
    p.lastStatusTick = ctx.tickCount;
    // E2b: состояние лошади (mountModel = ActorSpawn.reserved).
    p.mountModel = wantMount ? s.reserved : 0;
    p.mountTaskHash = natives::SCRIPT_TASK_FOLLOW_NAV_MESH_TO_COORD;
    p.mountTaskActive = false;
    p.mountLastUpdateTick = ctx.tickCount;
    p.mountLastStatusTick = ctx.tickCount;
    p.mountLastGaitTick = ctx.tickCount;
    // E2a.2: максимальный move-blend задаём один раз; фактический gait
    // хоста держим через SET_PED_DESIRED_MOVE_BLEND_RATIO.
    SetMoveBlend(handle, 3.0f);

    // E2b: создать лошадь и посадить rider (fallback — rider пеший).
    if (wantMount)
    {
        ++g_mountsCreatedThisTick;
        const unsigned int horse = CreatePedWithSetup(
            s.reserved, s.x, s.y, z, s.heading, ctx, true);
        if (horse)
        {
            SetNoCollisionWith(horse, handle);
            p.mountHandle = horse;
            p.mountModel = s.reserved;
            p.mounted = true;
            p.mountSeatAttempts = 0;
            p.lastReseatTick = ctx.tickCount;
            p.mountLastGait = -1.f;
            SeatRiderOnMount(p);
            LogMountSpawn(p.actorId, s.reserved);
        }
        else
        {
            p.mounted = false;
            p.mountModel = 0;
            p.mountFailedLogged = true;
            LogMountFailed(p.actorId, "create");
        }
    }

    // E3a: сценарий/смерть. Если actor уже мёртв в спавне — сразу труп
    // (invincible off + health 0), без ожидания кадров; сценарий не применяем.
    p.scenarioHash = s.scenarioHash;
    p.scenarioFlag = (s.flags & net::kActorFlagScenario) != 0;
    p.scenarioState = 0;
    p.scenarioApplied = 0;
    p.deathApplied = false;
    if (p.dead)
    {
        SetInvincible(handle, false);
        SetEntityHealthValue(handle, 0);
        p.deathApplied = true;
        LogDeath(p.actorId);
    }

    g_handleByActor[handle] = p.actorId;
    g_proxies[p.actorId] = p;
    LogSpawn(p.actorId, p.model, static_cast<unsigned int>(g_proxies.size()));
    return true;
}

// E2a: per-actor meta-outfit внешность (как player-ghost, но без singleton):
// REQUEST -> HAS_LOADED -> APPLY -> RELEASE. Прокси не блокируется; при
// таймауте/неудаче остаётся random variation (видимый не «голый» пед).
void ProcessActorAppearance(unsigned int tick)
{
    if (!coopcfg::Get().npcAppearance)
        return;

    for (auto& kv : g_proxies)
    {
        Proxy& p = kv.second;
        if (p.dead)
            continue; // труп не переодеваем — скоро despawn
        if (!p.metaOutfit || !p.handle || !DoesExist(p.handle))
            continue;
        if (p.appearanceState == 2 || p.appearanceState == 3)
            continue; // applied / failed — больше не трогаем

        if (p.appearanceState == 0)
        {
            if (g_appearanceRequestsThisTick >= kMaxAppearanceRequestsPerTick)
                continue;
            ++g_appearanceRequestsThisTick;
            const int req = RequestMetaPedOutfit(p.model, p.metaOutfit);
            p.outfitRequestTick = tick;
            if (req)
            {
                p.outfitRequest = req;
                p.appearanceState = 1;
                LogAppearance(p.actorId, p.model, p.metaOutfit, "requested");
            }
            else
            {
                p.appearanceState = 3;
                RandomizeComponents(p.handle);
                SetVisible(p.handle);
                LogAppearance(p.actorId, p.model, p.metaOutfit, "failed");
            }
            continue;
        }

        // state == 1 (requested): таймаут 3 с -> random variation
        if (tick - p.outfitRequestTick > kAppearanceTimeoutTicks)
        {
            ReleaseMetaPedOutfitRequest(p.outfitRequest);
            p.outfitRequest = 0;
            p.appearanceState = 3;
            RandomizeComponents(p.handle);
            SetVisible(p.handle);
            LogAppearance(p.actorId, p.model, p.metaOutfit, "failed");
            continue;
        }
        if (!HasMetaPedOutfitLoaded(p.outfitRequest))
            continue;

        const bool ok = ApplyMetaPedOutfit(p.outfitRequest, p.handle);
        ReleaseMetaPedOutfitRequest(p.outfitRequest);
        p.outfitRequest = 0;
        p.appearanceState = ok ? 2 : 3;
        if (ok)
        {
            SetVisible(p.handle); // наряд мог пересоздать компоненты
        }
        else
        {
            RandomizeComponents(p.handle);
            SetVisible(p.handle);
        }
        LogAppearance(p.actorId, p.model, p.metaOutfit,
                      ok ? "applied" : "failed");
    }
}

// E2a.2: точка на полилинии трейла впереди ПРОКСИ на advance метров.
// Трейл — последние позиции ХОСТА (10 Гц); прокси может отставать, поэтому
// находим проекцию прокси на полилинию и идём вперёд по ней (фактический
// маршрут хоста, без срезания углов). Публичного геттера navmesh-пути нет.
bool ComputeTrailTarget(const Proxy& p, float proxyX, float proxyY,
                        float advance, float& outX, float& outY)
{
    if (p.trailCount <= 0)
        return false;

    auto pointAt = [&](int k) -> const Proxy::TrailPoint& {
        const int idx =
            (p.trailHead - p.trailCount + k + kTrailLen * 2) % kTrailLen;
        return p.trail[idx];
    };

    if (p.trailCount == 1)
    {
        const Proxy::TrailPoint& pt = pointAt(0);
        outX = pt.x;
        outY = pt.y;
        return true;
    }

    // 1) проекция прокси на ближайший сегмент + длина пути до неё.
    float cum = 0.f;
    float bestDist2 = 1e30f;
    float bestS = 0.f;
    for (int k = 1; k < p.trailCount; ++k)
    {
        const Proxy::TrailPoint& a = pointAt(k - 1);
        const Proxy::TrailPoint& b = pointAt(k);
        const float sx = b.x - a.x;
        const float sy = b.y - a.y;
        const float len2 = sx * sx + sy * sy;
        if (len2 > 1e-6f)
        {
            float t = ((proxyX - a.x) * sx + (proxyY - a.y) * sy) / len2;
            if (t < 0.f) t = 0.f;
            if (t > 1.f) t = 1.f;
            const float px = a.x + sx * t;
            const float py = a.y + sy * t;
            const float ddx = proxyX - px;
            const float ddy = proxyY - py;
            const float d2 = ddx * ddx + ddy * ddy;
            if (d2 < bestDist2)
            {
                bestDist2 = d2;
                bestS = cum + t * std::sqrt(len2);
            }
            cum += std::sqrt(len2);
        }
    }

    // 2) вперёд по полилинии на advance (не выходим за newest точку хоста).
    float targetS = bestS + advance;
    if (targetS > cum) targetS = cum;
    float s = 0.f;
    for (int k = 1; k < p.trailCount; ++k)
    {
        const Proxy::TrailPoint& a = pointAt(k - 1);
        const Proxy::TrailPoint& b = pointAt(k);
        const float sx = b.x - a.x;
        const float sy = b.y - a.y;
        const float segLen = std::sqrt(sx * sx + sy * sy);
        if (segLen > 1e-6f)
        {
            if (s + segLen >= targetS)
            {
                const float t = (targetS - s) / segLen;
                outX = a.x + sx * t;
                outY = a.y + sy * t;
                return true;
            }
            s += segLen;
        }
    }
    const Proxy::TrailPoint& last = pointAt(p.trailCount - 1);
    outX = last.x;
    outY = last.y;
    return true;
}

// E2b: ведение всадника — двигаем ЛОШАДЬ (rider в седле, задач ему не даём).
// Цель — трейл от rider-кадра (позиции хоста), gait из pad; reseat-watchdog.
void MoveMountedProxy(Proxy& p)
{
    const unsigned int horse = p.mountHandle;
    if (!horse || !DoesExist(horse) || !DoesExist(p.handle))
        return;
    const unsigned int tick = g_logTick;

    // Reseat-watchdog раз в ~1 с: rider мог «сползти»/движок снял.
    if (tick - p.lastReseatTick >= kMountReseatCheckTicks)
    {
        p.lastReseatTick = tick;
        bool onHorse = false;
        {
            using namespace natives_api;
            Init(natives::IS_PED_ON_MOUNT);
            Push(p.handle);
            onHorse = Call() != 0;
            if (onHorse)
            {
                Init(natives::GET_MOUNT);
                Push(p.handle);
                onHorse = static_cast<unsigned int>(Call()) == horse;
            }
        }
        if (onHorse)
        {
            p.mountSeatAttempts = 0;
        }
        else
        {
            SeatRiderOnMount(p);
            ++p.mountSeatAttempts;
            if (p.mountSeatAttempts == kMountMaxSeatAttempts)
                LogMountFailed(p.actorId, "seat");
            else if (p.mountSeatAttempts > kMountMaxSeatAttempts)
            {
                // Не смогли удержать в седле — fallback пешком, лошадь удалить.
                LogMountFailed(p.actorId, "seat-fallback");
                p.mounted = false;
                p.dismountTick = tick;
                p.taskActive = false;
                p.moveTaskValid = false;
                return;
            }
        }
    }

    const Vec3 horsePos = GetCoords(horse);
    const float dx = p.tx - horsePos.x;
    const float dy = p.ty - horsePos.y;
    const float dz = p.tz - horsePos.z;
    const float dist = std::sqrt(dx * dx + dy * dy);

    if (dist > kTeleportDist || std::fabs(dz) > 8.0f)
    {
        // Телепорт: обе сущности в цель, затем reseat.
        SetCoordsNative(horse, p.tx, p.ty, p.tz);
        SetHeadingNative(horse, p.heading);
        SetCoordsNative(p.handle, p.tx, p.ty, p.tz);
        SeatRiderOnMount(p);
        p.mountTaskActive = false;
        p.lastReseatTick = tick;
        return;
    }

    g_motionErr.AddUs(static_cast<unsigned long long>(dist * 1000.0f));

    const float rawSpeedMs =
        static_cast<float>(p.speed) * kSpeedByteToMetersPerSec;
    p.ewmaSpeed = p.ewmaSpeed * (1.0f - kVelocityEwma) +
                  rawSpeedMs * kVelocityEwma;

    float gait = (p.gaitQ > 0)
        ? static_cast<float>(p.gaitQ) / 1000.0f
        : ((p.moveState >= 3) ? 3.0f : (p.moveState == 2) ? 2.0f : 1.0f);
    if (gait < 0.1f) gait = 0.1f;
    if (gait > 3.0f) gait = 3.0f;

    // Всадник движется, если едет лошадь: moveState=RideHorse не означает
    // «движется», поэтому смотрим сглаженную скорость.
    const bool hostMoving = p.ewmaSpeed > 0.15f;
    float targetX = p.tx;
    float targetY = p.ty;
    float gotoGait = gait;
    bool idleStand = false;

    if (!hostMoving)
    {
        // Хост остановил лошадь: StandStill только когда лошадь уже рядом.
        if (dist < kArriveDistMeters)
        {
            if (p.mountTaskActive)
            {
                SetDesiredMoveBlend(horse, 0.0f);
                StandStill(horse);
                p.mountTaskActive = false;
                p.mountLastGait = 0.f;
                p.mountLastGaitTick = tick;
            }
            idleStand = true;
        }
        else
        {
            gotoGait = 1.0f;
        }
    }
    else
    {
        float L = kLookaheadBase + p.ewmaSpeed * kLookaheadPerSpeed;
        if (L < kLookaheadMinMeters) L = kLookaheadMinMeters;
        if (L > kLookaheadMaxMeters) L = kLookaheadMaxMeters;
        if (p.ewmaSpeed < kSlowHostSpeed)
        {
            L = kSlowCatchupStep;
            targetX = p.tx;
            targetY = p.ty;
        }
        else if (!ComputeTrailTarget(p, horsePos.x, horsePos.y, L,
                                     targetX, targetY))
        {
            const float rad = p.heading * kRadPerDeg;
            targetX = p.tx - std::sin(rad) * L;
            targetY = p.ty + std::cos(rad) * L;
        }
    }

    if (!idleStand)
    {
        // Одна долгоживущая задача для лошади + in-place update.
        bool needIssue = !p.mountTaskActive;
        const bool wasActive = p.mountTaskActive;
        if (!needIssue &&
            tick - p.mountLastStatusTick >= kMotionStatusCheckTicks)
        {
            p.mountLastStatusTick = tick;
            const int st = GetScriptTaskStatus(horse, p.mountTaskHash);
            if (st == 7 || st == 8)
            {
                needIssue = true;
                p.mountTaskActive = false;
            }
        }
        if (needIssue)
        {
            FollowNavmesh(horse, targetX, targetY, p.tz, gotoGait,
                          kFollowHeadingSentinel);
            p.mountTaskHash = natives::SCRIPT_TASK_FOLLOW_NAV_MESH_TO_COORD;
            p.mountTaskActive = true;
            p.mountLastUpdateTick = tick;
            p.mountLastStatusTick = tick;
            p.mountMoveTarget = Vec3{targetX, targetY, p.tz};
            if (wasActive)
                ++g_motionReissue;
            else
                ++g_motionIssued;
        }
        else if (tick - p.mountLastUpdateTick >= kMotionUpdateTicks)
        {
            UpdateTaskGoToCoord(horse, targetX, targetY, p.tz, gotoGait,
                                kFollowStoppingRange);
            p.mountLastUpdateTick = tick;
            p.mountMoveTarget = Vec3{targetX, targetY, p.tz};
        }

        if (std::fabs(gotoGait - p.mountLastGait) > 0.01f)
        {
            SetDesiredMoveBlend(horse, gotoGait);
            p.mountLastGait = gotoGait;
            p.mountLastGaitTick = tick;
            ++g_motionGaitRestart;
        }
        else if (tick - p.mountLastGaitTick >= kGaitRefreshTicks)
        {
            SetDesiredMoveBlend(horse, gotoGait);
            p.mountLastGaitTick = tick;
        }
    }
}

void MoveProxy(Proxy& p)
{
    if (!DoesExist(p.handle))
    {
        // E2b: rider пропал (битый handle/движок удалил) — лошадь не бросаем.
        if (p.mountHandle)
            TryDeleteMount(p, kMountReasonDespawn);
        return;
    }

    const unsigned int tickNow = g_logTick;
    if (p.dead)
    {
        // E2b: rider мёртв — лошадь не двигаем, удаляем с задержкой 2.5 с.
        if (p.mountHandle)
        {
            if (!p.mountDeathTick)
                p.mountDeathTick = tickNow;
            else if (tickNow - p.mountDeathTick > kMountDeathDeleteTicks)
                TryDeleteMount(p, kMountReasonDead);
        }
        return; // труп не двигаем; хост пришлёт despawn
    }

    if (p.mounted)
    {
        if (p.mountHandle && DoesExist(p.mountHandle))
        {
            MoveMountedProxy(p);
            return;
        }
        p.mounted = false;
        if (!p.mountFailedLogged)
        {
            p.mountFailedLogged = true;
            LogMountFailed(p.actorId, "no-horse");
        }
    }

    // E3a: пока хост в сценарии-позе (sit/work), движение не выдаём — поза
    // держится сценарной задачей (ProcessActorScenarios). При снятии флага
    // движение возобновится на следующем тике.
    if (p.scenarioFlag)
        return;

    const Vec3 cur = GetCoords(p.handle);
    const float dx = p.tx - cur.x;
    const float dy = p.ty - cur.y;
    const float dz = p.tz - cur.z;
    const float dist = std::sqrt(dx * dx + dy * dy);

    if (dist > kTeleportDist || std::fabs(dz) > 8.0f)
    {
        SetCoordsNative(p.handle, p.tx, p.ty, p.tz);
        SetHeadingNative(p.handle, p.heading);
        p.moveTaskValid = false;
        p.taskActive = false;
        p.lastMoveState = -1;
        return;
    }

    // E2a: ошибка позиции target-vs-ped для err_p95 (семпл в мм). Телепорты
    // выше не семплим — это не ошибка сглаживания, а ресинк.
    g_motionErr.AddUs(static_cast<unsigned long long>(dist * 1000.0f));

    // E2a.2: сглаженная скорость хоста из кадра (EWMA 0.3).
    const float rawSpeedMs =
        static_cast<float>(p.speed) * kSpeedByteToMetersPerSec;
    p.ewmaSpeed = p.ewmaSpeed * (1.0f - kVelocityEwma) +
                  rawSpeedMs * kVelocityEwma;

    // E2a.2: gait из ActorFrame.pad (фактический desired у хоста), fallback —
    // по moveState, если fake-host/старый кадр пришли без pad.
    float gait = (p.gaitQ > 0)
        ? static_cast<float>(p.gaitQ) / 1000.0f
        : ((p.moveState >= 3) ? 3.0f : (p.moveState == 2) ? 2.0f : 1.0f);
    if (gait < 0.1f) gait = 0.1f;
    if (gait > 3.0f) gait = 3.0f;

    const unsigned int tick = g_logTick;
    float targetX = p.tx;
    float targetY = p.ty;
    float gotoGait = gait;
    bool idleStand = false;

    if (p.moveState == 0)
    {
        // Хост стоит: StandStill только если прокси уже <0.5 м от позиции
        // хоста; иначе догоняем пешком (после лага/телепорта).
        if (dist < kArriveDistMeters)
        {
            if (p.lastMoveState != 0)
            {
                SetDesiredMoveBlend(p.handle, 0.0f);
                StandStill(p.handle);
                p.lastGait = 0.f;
                p.lastGaitTick = tick;
            }
            p.taskActive = false;
            p.moveTaskValid = false;
            p.lastMoveState = 0;
            idleStand = true;
        }
        else
        {
            targetX = p.tx;
            targetY = p.ty;
            gotoGait = 1.0f;
            p.lastMoveState = 0;
        }
    }
    else
    {
        // Хост движется: цель = точка на трейле впереди прокси.
        // L = clamp(0.5 + hostSpeed*0.45, 0.9, 3.0); медленный хост -> 0.3.
        float L = kLookaheadBase + p.ewmaSpeed * kLookaheadPerSpeed;
        if (L < kLookaheadMinMeters) L = kLookaheadMinMeters;
        if (L > kLookaheadMaxMeters) L = kLookaheadMaxMeters;
        if (p.ewmaSpeed < kSlowHostSpeed)
        {
            L = kSlowCatchupStep;
            targetX = p.tx;
            targetY = p.ty;
        }
        else if (!ComputeTrailTarget(p, cur.x, cur.y, L, targetX, targetY))
        {
            // Трейла ещё нет — по heading хоста.
            const float rad = p.heading * kRadPerDeg;
            targetX = p.tx - std::sin(rad) * L;
            targetY = p.ty + std::cos(rad) * L;
        }
        p.lastMoveState = static_cast<int>(p.moveState);
    }

    if (!idleStand)
    {
        // Одна долгоживущая задача: in-place update цели; ре-issue только
        // если задача реально завершилась (status 7 not_found / 8 finished).
        bool needIssue = !p.taskActive;
        const bool wasActive = p.taskActive;
        if (!needIssue &&
            tick - p.lastStatusTick >= kMotionStatusCheckTicks)
        {
            p.lastStatusTick = tick;
            const int st = GetScriptTaskStatus(p.handle, p.taskHash);
            if (st == 7 || st == 8)
            {
                needIssue = true;
                p.taskActive = false;
            }
        }
        if (needIssue)
        {
            // heading sentinel 40000 = «не поворачивать» (как в rdr2-runtime).
            FollowNavmesh(p.handle, targetX, targetY, p.tz, gotoGait,
                          kFollowHeadingSentinel);
            p.taskHash = natives::SCRIPT_TASK_FOLLOW_NAV_MESH_TO_COORD;
            p.taskActive = true;
            p.taskIssuedTick = tick;
            p.lastMotionUpdateTick = tick;
            p.lastStatusTick = tick;
            p.moveTarget = Vec3{targetX, targetY, p.tz};
            p.moveTaskValid = true;
            if (wasActive)
                ++g_motionReissue; // задача завершилась и выдана заново
            else
                ++g_motionIssued;  // самая первая задача для прокси
        }
        else if (tick - p.lastMotionUpdateTick >= kMotionUpdateTicks)
        {
            // in-place update: не перезапускает анимацию/gait.
            UpdateTaskGoToCoord(p.handle, targetX, targetY, p.tz, gotoGait,
                                kFollowStoppingRange);
            p.lastMotionUpdateTick = tick;
            p.moveTarget = Vec3{targetX, targetY, p.tz};
        }

        // Gait: держим из кадра хоста; при изменении — сразу, иначе раз в
        // ~10 тиков (без перезапуска задачи/анимации).
        if (std::fabs(gotoGait - p.lastGait) > 0.01f)
        {
            SetDesiredMoveBlend(p.handle, gotoGait);
            p.lastGait = gotoGait;
            p.lastGaitTick = tick;
            ++g_motionGaitRestart;
        }
        else if (tick - p.lastGaitTick >= kGaitRefreshTicks)
        {
            SetDesiredMoveBlend(p.handle, gotoGait);
            p.lastGaitTick = tick;
        }
    }

    // E2a: heading руками — только в idle (в walk/run ведёт навмеш).
    if (p.moveState == 0 && !idleStand)
    {
        float norm = p.heading - GetHeading(p.handle);
        while (norm > 180.f) norm -= 360.f;
        while (norm < -180.f) norm += 360.f;
        if (std::fabs(norm) > kHeadingDeadzone)
            SetHeadingNative(p.handle, p.heading);
    }
}

// E3a: смерть прокси по dead-флагу хоста: снять бессмертие, health 0 — остаётся
// труп на месте (mission entity не исчезает/не респавнится сам).
void ApplyProxyDeath(Proxy& p)
{
    if (!p.handle || !DoesExist(p.handle))
        return;
    p.taskActive = false;
    p.moveTaskValid = false;
    p.scenarioState = 0;
    p.scenarioApplied = 0;
    if (p.scenarioPoint)
    {
        DeleteScenarioPoint(p.scenarioPoint);
        p.scenarioPoint = 0;
        p.scenarioPointType = 0;
    }
    SetInvincible(p.handle, false);
    SetEntityHealthValue(p.handle, 0);
    p.deathApplied = true;
    LogDeath(p.actorId);
}

// E3a: оживление (dead-флаг снят): resurrect + health max + invincible,
// затем сценарий/движение вернутся обычным путём.
void ApplyProxyRevive(Proxy& p)
{
    if (!p.handle || !DoesExist(p.handle))
        return;
    ResurrectPedEntity(p.handle);
    int maxHp = GetEntityMaxHealthValue(p.handle);
    if (maxHp <= 0) maxHp = kHumanMaxHealth;
    SetEntityHealthValue(p.handle, maxHp);
    SetInvincible(p.handle, true);
    SetVisible(p.handle);
    SetLodDist(p.handle, kActorLodDist);
    p.deathApplied = false;
    p.mountDeathTick = 0;
    p.taskActive = false;
    p.moveTaskValid = false;
    if (p.scenarioFlag && p.scenarioHash)
        p.scenarioState = 0; // переприменить позу после revive
    LogRevive(p.actorId);
}

// E3a: сценарии-позы (sit/work). Пока сценарий активен — движение не выдаётся
// (MoveProxy проверяет scenarioFlag); при снятии флага возвращаем движение.
// E3a.2: ped реально в сценарии (поза)? ok-критерий для всех вариантов.
bool IsPedInScenario(unsigned int ped)
{
    return IsScenarioActive(ped) || GetDynamicScenario2(ped) != 0 ||
           GetDynamicScenario(ped) != 0;
}

void ProcessActorScenarios(unsigned int tick)
{
    for (auto& kv : g_proxies)
    {
        Proxy& p = kv.second;
        if (!p.handle || !DoesExist(p.handle))
            continue;
        if (p.dead)
            continue; // труп в сценарий не сажаем

        const bool want = p.scenarioFlag && !p.mounted;
        if (!want)
        {
            if (p.scenarioState != 0)
            {
                // Хост вышел из сценария — снимаем задачу, вернём движение.
                StandStill(p.handle);
                p.taskActive = false;
                p.moveTaskValid = false;
            }
            if (p.scenarioPoint)
            {
                DeleteScenarioPoint(p.scenarioPoint);
                p.scenarioPoint = 0;
                p.scenarioPointType = 0;
            }
            p.scenarioState = 0;
            p.scenarioApplied = 0;
            continue;
        }

        if (!p.scenarioHash)
        {
            // Флаг есть, хэша ещё нет (heartbeat не принёс) — стоим и ждём.
            if (p.scenarioState == 0)
            {
                StandStill(p.handle);
                p.taskActive = false;
                p.moveTaskValid = false;
                p.scenarioState = 1;
                p.scenarioTick = tick;
            }
            else if (p.scenarioState == 1 &&
                     tick - p.scenarioTick > kScenarioVerifyTicks)
            {
                // Хэш так и не пришёл — не двигаем, но не варпаем.
                LogScenarioApply(p.actorId, 0, false);
                p.scenarioState = 5;
            }
            continue;
        }

        // Первый запуск / смена хэша: ровно один in-place вызов.
        const bool needApply =
            p.scenarioState == 0 || p.scenarioApplied != p.scenarioHash;
        if (needApply)
        {
            // Снять текущую позу/движение, очистить старый point (если был).
            if (p.scenarioPoint)
            {
                DeleteScenarioPoint(p.scenarioPoint);
                p.scenarioPoint = 0;
                p.scenarioPointType = 0;
            }
            StandStill(p.handle);
            SetMoveBlend(p.handle, 0.0f);
            p.taskActive = false;
            p.moveTaskValid = false;
            // E3a.3: единственный путь применения позы — in-place. Верификация
            // ниже только диагностика; варпающих fallback'ов в цепочке нет.
            StartScenarioInPlace(p.handle, p.scenarioHash);
            p.scenarioState = 1;
            p.scenarioTick = tick;
            p.scenarioApplied = p.scenarioHash;
            continue;
        }

        if (p.scenarioState == 1)
        {
            if (tick - p.scenarioTick > kScenarioVerifyTicks)
            {
                // Оптимистичное подтверждение: поза уже играет (verify-геттеры
                // для части сценариев врут). state=ok в любом случае.
                const bool flagOk = IsPedInScenario(p.handle);
                p.scenarioState = 2;
                LogScenarioApply(p.actorId, p.scenarioHash, true,
                                 flagOk ? "flag" : "assumed");
            }
            continue;
        }

        // E3a.3: fallback-цепочка (scenario point / at-position) НЕ используется
        // в рабочей цепочке — оставлена заготовкой под kScenarioFallbackEnabled=false:
        // она переставляла/сваривала NPC. При включении флага — старый путь.
        if constexpr (kScenarioFallbackEnabled)
        {
            if (p.scenarioState == 3)
            {
                if (IsPedInScenario(p.handle))
                {
                    p.scenarioState = 2;
                    LogScenarioPoint(p.actorId, p.scenarioHash, p.scenarioPoint,
                                     p.scenarioPointType, true);
                    LogScenarioApply(p.actorId, p.scenarioHash, true);
                }
                else if (tick - p.scenarioTick > kScenarioVerifyTicks)
                {
                    LogScenarioPoint(p.actorId, p.scenarioHash, p.scenarioPoint,
                                     p.scenarioPointType, false);
                    StandStill(p.handle);
                    if (p.scenarioPoint)
                    {
                        DeleteScenarioPoint(p.scenarioPoint);
                        p.scenarioPoint = 0;
                        p.scenarioPointType = 0;
                    }
                    const Vec3 pos = GetCoords(p.handle);
                    StartScenarioAtPosition(p.handle, p.scenarioHash,
                                            pos.x, pos.y, pos.z,
                                            GetHeading(p.handle), true);
                    p.scenarioState = 4;
                    p.scenarioTick = tick;
                }
                continue;
            }
            if (p.scenarioState == 4)
            {
                if (IsPedInScenario(p.handle))
                {
                    p.scenarioState = 2;
                    LogScenarioApply(p.actorId, p.scenarioHash, true);
                }
                else if (tick - p.scenarioTick > kScenarioVerifyTicks)
                {
                    LogScenarioFailed(p.actorId, p.scenarioHash, "atpos");
                    StandStill(p.handle);
                    LogScenarioApply(p.actorId, p.scenarioHash, false);
                    p.scenarioState = 5;
                }
                continue;
            }
        }
        // state 2 (ok/поза держится) / 5 (не двигаем): ничего не делаем.
    }
}

// E2a.2: трейл движения (последние ~1 с позиций) — по нему сглаживаем
// направление и ведём прокси по фактическому маршруту хоста без срезания углов.
void AppendTrailPoint(Proxy& p, float x, float y, float z)
{
    if (p.trailCount > 0)
    {
        const int last = (p.trailHead - 1 + kTrailLen) % kTrailLen;
        const float dx = x - p.trail[last].x;
        const float dy = y - p.trail[last].y;
        if (dx * dx + dy * dy < 0.0025f)
            return; // <5 см — не засоряем окно
    }
    Proxy::TrailPoint& pt = p.trail[p.trailHead];
    pt.x = x;
    pt.y = y;
    pt.z = z;
    p.trailHead = (p.trailHead + 1) % kTrailLen;
    if (p.trailCount < kTrailLen)
        ++p.trailCount;
}

// P3-фикс (task-45): только ставит акторов в pending; epoch-change и
// удаление старых прокси делает GuestTick ВНЕ g_inboxMutex.
void QueueSpawnBatch(uint32_t epoch,
                     const std::vector<net::ActorSpawn>& actors,
                     unsigned int tick)
{
    for (const net::ActorSpawn& s : actors)
    {
        // E3a: heartbeat-спавн для уже созданного актора обновляет хэш
        // сценария (сценарий мог начаться/смениться после первого спавна).
        auto existing = g_proxies.find(s.actorId);
        if (existing != g_proxies.end())
        {
            Proxy& p = existing->second;
            if (!p.dead && s.scenarioHash != 0 &&
                p.scenarioApplied != s.scenarioHash)
            {
                p.scenarioHash = s.scenarioHash;
                p.scenarioFlag = true;
                p.scenarioState = 0; // применить заново в ProcessActorScenarios
            }
            continue;
        }
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
    g_motionErr.Clear();
    g_mountsCreatedThisTick = 0;
}

void GuestTick(const TickContext& ctx)
{
    const unsigned int tick = ctx.tickCount;
    g_modelRequestsThisTick = 0;
    g_appearanceRequestsThisTick = 0;
    g_mountsCreatedThisTick = 0;

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
            // E2b fallback: всадник, чья модель лошади так и не загрузилась,
            // не должен пропасть — спавним rider пешим (как в E1).
            const bool wantMount =
                (s.flags & net::kActorFlagMounted) != 0 && s.reserved != 0;
            if (wantMount && HasModelLoaded(s.model))
            {
                net::ActorSpawn fallback = s;
                fallback.flags = static_cast<uint8_t>(
                    fallback.flags & ~net::kActorFlagMounted);
                fallback.reserved = 0;
                fallback.moveStyle = 0;
                if (CreateProxy(fallback, ctx))
                    LogMountFailed(s.actorId, "model-timeout");
            }
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
        // E2b: модель лошади (reserved) — тот же бюджет model requests (≤2/тик).
        const bool wantMount =
            (s.flags & net::kActorFlagMounted) != 0 && s.reserved != 0;
        if (wantMount && !HasModelLoaded(s.reserved))
        {
            if (g_modelRequestsThisTick < kMaxModelRequestsPerTick)
            {
                RequestModel(s.reserved);
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

    // 3b) E2a: внешность — meta-outfit request/apply для новых и ожидающих
    // прокси (бюджет 2 REQUEST/тик, таймаут 3 с, не блокирует прокси).
    ProcessActorAppearance(tick);

    // 3c) E3a: сценарии-позы (sit/work) + fallback stand при неудаче.
    ProcessActorScenarios(tick);

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
            // E2a.2: направление из фактического смещения (трейл), EWMA 0.3;
            // heading — fallback для первого кадра/стояния.
            const float oldTx = p.tx;
            const float oldTy = p.ty;
            const float segX = f.x - oldTx;
            const float segY = f.y - oldTy;
            const float seg2 = segX * segX + segY * segY;
            if (seg2 > 0.0025f) // >5 см
            {
                const float inv = 1.0f / std::sqrt(seg2);
                const float nx = segX * inv;
                const float ny = segY * inv;
                if (p.dirValid)
                {
                    p.dirX = p.dirX * (1.0f - kVelocityEwma) + nx * kVelocityEwma;
                    p.dirY = p.dirY * (1.0f - kVelocityEwma) + ny * kVelocityEwma;
                    const float l = std::sqrt(p.dirX * p.dirX + p.dirY * p.dirY);
                    if (l > 0.0001f)
                    {
                        p.dirX /= l;
                        p.dirY /= l;
                    }
                }
                else
                {
                    p.dirX = nx;
                    p.dirY = ny;
                    p.dirValid = true;
                }
            }
            AppendTrailPoint(p, f.x, f.y, f.z);
            p.tx = f.x; p.ty = f.y; p.tz = f.z;
            p.heading = f.heading;
            p.moveState = f.animState;
            p.speed = f.speed; // E2a.2: 0..255 -> 0..8 м/с (lookahead/трейл)
            p.gaitQ = f.pad;   // E2a.2: desired gait хоста (ratio*1000)
            p.flags = static_cast<uint16_t>(f.flags);
            p.dead = (f.flags & net::kActorFlagDead) != 0;
            // E3a: сценарий-флаг из кадров; смерть/оживление — health/invincible.
            p.scenarioFlag =
                (f.flags & net::kActorFlagScenario) != 0 && !p.mounted;
            if (p.dead && !p.deathApplied)
                ApplyProxyDeath(p);
            else if (!p.dead && p.deathApplied)
                ApplyProxyRevive(p);
            // E2b: состояние всадника из кадров хоста (spawn мог прийти без
            // лошади/fake-host мог пересесть) — reseat/переход в пеший режим.
            const bool frameMounted =
                (f.flags & net::kActorFlagMounted) != 0 && !p.dead;
            if (frameMounted && !p.mounted && p.mountHandle &&
                DoesExist(p.mountHandle))
            {
                p.mounted = true;
                p.dismountTick = 0;
                p.mountSeatAttempts = 0;
                p.lastReseatTick = tick;
                SeatRiderOnMount(p);
            }
            else if (!frameMounted && p.mounted)
            {
                p.mounted = false;
                p.dismountTick = tick;
                p.taskActive = false;
                p.moveTaskValid = false;
            }
            if (frameMounted && !p.mountHandle && !p.mountFailedLogged)
            {
                p.mountFailedLogged = true;
                LogMountFailed(p.actorId, "no-model");
            }
            p.lastFrameTick = tick;
            p.resyncLogged = false;
            ++g_appliedFrames;
        }
        g_haveFrames = false;
    }

    // 5) движение/поворот каждый тик
    for (auto& kv : g_proxies)
        MoveProxy(kv.second);

    // E2b: удалить лошадь после слезания/смерти с задержкой (учёт не теряем:
    // TryDeleteMount при неудаче оставляет mountHandle на повтор/таймаут).
    for (auto& kv : g_proxies)
    {
        Proxy& p = kv.second;
        if (!p.mountHandle) continue;
        if (!p.mounted && p.dismountTick &&
            tick - p.dismountTick > kMountDismountDelayTicks)
        {
            TryDeleteMount(p, kMountReasonDismount);
        }
        if (p.dead && p.mountDeathTick &&
            tick - p.mountDeathTick > kMountDeathDeleteTicks)
        {
            TryDeleteMount(p, kMountReasonDead);
        }
    }

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
            // E2b: лошадь — тоже не толкаем игроков и rider.
            if (p.mountHandle && DoesExist(p.mountHandle))
            {
                SetNoCollisionWith(p.mountHandle, ctx.localPed);
                SetNoCollisionWith(p.mountHandle, ctx.partnerGhost);
                SetNoCollisionWith(p.mountHandle, ctx.partnerMount);
                SetNoCollisionWith(p.mountHandle, p.handle);
            }
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
        // E2a.2: motion issued/reissue/gait_restart + err_p95 — только при
        // npc_debug=1 (для оценки "до/после" сглаживания ходьбы).
        if (coopcfg::Get().npcDebug)
        {
            sh_log::Write(
                "actor: motion issued=%u reissue=%u gait_restart=%u "
                "err_p95=%.2f\n",
                g_motionIssued, g_motionReissue, g_motionGaitRestart,
                g_motionErr.P95Ms() / 1000.0);
            g_motionIssued = 0;
            g_motionReissue = 0;
            g_motionGaitRestart = 0;
            g_motionErr.Clear();
        }
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
