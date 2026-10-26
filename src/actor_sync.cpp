#include "actor_sync.h"

#include "config.h"
#include "coop.h"
#include "gen_metaped_outfits.h" // E2c: база model->outfit-кандидатов
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
// E5.2: удержание трупа в roster, пока host-труп существует (npc_corpse_max_sec,
// default 300=5 мин — верхний предел; 0 = не держать).
constexpr unsigned int kCorpseHoldTicksPerSec = 60;
// E5.3: сглаживание всадника/лошади: дальний lead и редкий retarget.
constexpr float kMountLookaheadSeconds = 0.28f;   // lead 0.25-0.3 с
constexpr float kMountLookaheadMin = 0.8f;
constexpr float kMountLookaheadMax = 4.5f;
constexpr float kMountRetargetMeters = 1.75f;     // 1.5-2.0 м
constexpr float kMountStuckStoppingRange = 1.0f;  // fallback после 2 неудач
constexpr unsigned int kMountStuckRetargets = 2;
// E7.2: smooth NPC motion — интерполяция кадров, дальняя цель, редкий reissue.
constexpr int kHostSampleCount = 3;              // история кадров хоста на proxy
constexpr float kHostExtrapolateMaxMs = 150.0f;  // экстраполяция не дальше 150 мс
constexpr float kHostHoldMs = 300.0f;            // нет кадров >300 мс — держим позу
constexpr float kHumanLeadSeconds = 0.7f;        // lead = clamp(speed*0.7, 4, 12)
constexpr float kHumanLeadMin = 4.0f;
constexpr float kHumanLeadMax = 12.0f;
constexpr float kHorseLeadSeconds = 0.7f;        // лошади: 0.5-0.8 с, clamp 5-20
constexpr float kHorseLeadMin = 5.0f;
constexpr float kHorseLeadMax = 20.0f;
constexpr float kGoalMinProxyDist = 2.5f;        // dist(goal,proxy) >= 2.5 м
constexpr unsigned int kStatusIntervalE72 = 3;   // статус задачи 2-3 тика
constexpr unsigned int kUpdateIntervalE72 = 3;   // in-place update 2-3 тика
constexpr float kUpdateMinShift = 0.4f;          // сдвиг цели для update, м
constexpr float kTeleportDistE72 = 25.0f;        // телепорт: >25 м / |dz|>15
constexpr float kTeleportDzE72 = 15.0f;
constexpr unsigned long long kNoFramesSnapMs = 2000ull; // нет кадров >2 с
constexpr float kStopSpeedE72 = 0.2f;            // стоп-гистерезис
constexpr float kStopDistE72 = 1.0f;
constexpr unsigned int kStopConfirmTicks = 15;   // ~0.25 с
constexpr float kResumeSpeedE72 = 0.3f;
constexpr unsigned int kResumeConfirmTicks = 12; // ~0.2 с
constexpr float kMbrHysteresis = 0.15f;
constexpr unsigned int kStuckConfirmTicks = 30;  // 0.5 с
constexpr float kStuckDistE72 = 2.0f;
// A2: marker-entity (invisible prop) — первый валидный из списка.
constexpr uint32_t kMarkerModels[] = {0x04AC59BCu, 0x877C6202u, 0x85218677u,
                                      0x8B944ACCu, 0x85F46DF3u};
constexpr float kMarkerFollowSpeed = 1.0f;
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
// E3a.4: анти-наложение поз. PRE-CHECK: не сажать сценарий, если другой прокси
// со сценарием (pending/active) ближе kScenarioSpacingMeters — иначе оба
// варпаются движком к одной scenario point. POST-CHECK: если через 2 с пед
// сместился от spawn-позиции > kScenarioWarpRevertMeters — движок увёл его к
// scenario point, откатываем координаты назад и не двигаем, пока флаг активен.
constexpr float kScenarioSpacingMeters = 8.0f;
constexpr float kScenarioWarpRevertMeters = 3.0f;

// E3b: боевые визуалы NPC (оружие/aim/shoot/reload).
constexpr unsigned int kWeaponDrawForceTicks = 30;    // форс equipNow ~0.5 с
constexpr unsigned int kWeaponChangeWaitTicks = 20;   // ожидание при Changing
constexpr unsigned int kWeaponAimReissueTicks = 10;   // перевыдача aim не чаще
constexpr unsigned int kWeaponShootCooldownTicks = 60; // <= 1 выстрел/с
constexpr unsigned int kWeaponReloadTimeoutTicks = 120; // вернуть infinite clip
constexpr float kWeaponAimDistance = 25.0f;           // цель «вперёд по heading»

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

// E4: p95-метрики фаз NPC-подсистемы (только npc_debug=1, окно 10 с).
PerfP95 g_perfHostTick;
PerfP95 g_perfHostDiscover;
PerfP95 g_perfHostCapture;
PerfP95 g_perfHostBuild;
PerfP95 g_perfGuestTick;
PerfP95 g_perfGuestSpawn;
PerfP95 g_perfGuestAppearance;
PerfP95 g_perfGuestMotion;
PerfP95 g_perfGuestWeapons;
// E2a: ошибка позиции (target vs ped), семплим в мм, логируем/1000 = метры.
PerfP95 g_motionErr;
PerfP95 g_mountErr; // E5.3: ошибка позиции лошади (диагностика mounted)
unsigned int g_mountedRetargets = 0; // E5.3: retarget/update целей за окно
unsigned int g_lastPerfLogTick = 0;
// E4: счётчики окна (сбрасываются после строки perf: npc ...).
unsigned long long g_perfHostSpawns = 0;
unsigned long long g_perfHostFrames = 0;
unsigned long long g_perfHostDespawns = 0;
unsigned long long g_perfHostBytes = 0; // оценка отправленных actor-байт
unsigned long long g_perfGuestSpawns = 0;
unsigned long long g_perfGuestAppearanceApplied = 0;
unsigned long long g_perfGuestAppearanceFailed = 0;
unsigned long long g_perfGuestTimeouts = 0;
unsigned long long g_perfGuestDespawns = 0;
// E4: бюджет тика NPC-подсистемы (предупреждение в perf-строке).
constexpr double kNpcPerfBudgetMs = 2.0;
constexpr unsigned int kPerfWindowSeconds = kPerfLogEveryTicks / 60; // ~10 с

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

// E3c: kind semantics (wire-compatible v16, ActorSpawn.kind):
// 0 = human (как E1-E3b), 1 = animal (дичь/скот), 2 = horse (A_C_HORSE_*).
bool IsHorseModel(uint32_t model)
{
    using namespace natives_api;
    Init(natives::_IS_THIS_MODEL_A_HORSE);
    Push(static_cast<unsigned long long>(model));
    return Call() != 0;
}

bool IsBirdEntity(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::_GET_IS_BIRD);
    Push(handle);
    return Call() != 0;
}

bool IsFishEntity(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::_IS_META_PED_FISH);
    Push(handle);
    return Call() != 0;
}

uint8_t DetectActorKind(unsigned int handle)
{
    if (IsHumanPed(handle))
        return net::kActorKindHuman;
    return IsHorseModel(GetModelOf(handle)) ? 2u : 1u;
}

bool IsAnimalKind(uint8_t kind)
{
    return kind == 1u || kind == 2u;
}

// E3c: у лошади есть всадник (тогда она уходит как mounted-пара human, а не
// отдельным animal-актором) или это животное/лошадь игрока.
bool HasRider(unsigned int mount)
{
    using namespace natives_api;
    Init(natives::_GET_RIDER_OF_MOUNT);
    Push(mount);
    Push(false);
    const unsigned int rider = static_cast<unsigned int>(Call());
    return rider != 0 && rider != 0xFFFFFFFFu && DoesExist(rider);
}

bool IsAnimalControlledByPlayer(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::_IS_ANIMAL_CONTROLLED_BY_A_PLAYER);
    Push(ped);
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

// E3b.2: «вооружён» по движку — мягкий критерий armed (IS_PED_ARMED(ped,1)).
bool IsPedArmedEntity(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::IS_PED_ARMED);
    Push(ped);
    Push(static_cast<unsigned long long>(1));
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

// ---------------- E3b: оружие/бой NPC-прокси (визуалы) ----------------
// Арности и порядок аргументов повторяют рабочий player-ghost из coop.cpp
// (GIVE/equip/aim/shoot/reload там проверены в игре).
bool IsWeaponHashUsable(uint32_t hash)
{
    return hash != 0 && hash != static_cast<uint32_t>(natives::WEAPON_UNARMED);
}

uint32_t GiveWeaponToPedEntity(unsigned int ped, uint32_t weaponHash)
{
    using namespace natives_api;
    Init(natives::GIVE_WEAPON_TO_PED);
    Push(ped);
    Push(static_cast<unsigned long long>(weaponHash));
    Push(static_cast<unsigned long long>(999)); // ammoCount
    Push(static_cast<unsigned long long>(0));   // bForceInHand
    Push(static_cast<unsigned long long>(0));   // bForceInHolster
    Push(static_cast<unsigned long long>(0));   // attachPoint
    Push(static_cast<unsigned long long>(0));   // bAllowMultipleCopies
    Push(0.5f);                                 // p7
    Push(1.0f);                                 // p8
    Push(static_cast<unsigned long long>(0));   // addReason
    Push(static_cast<unsigned long long>(1));   // bIgnoreUnlocks
    Push(0.5f);                                 // permanentDegradation
    Push(static_cast<unsigned long long>(0));   // p12
    return static_cast<uint32_t>(Call());       // E3b.2: ret для диагностики
}

void RemoveWeaponFromPedEntity(unsigned int ped, uint32_t weaponHash)
{
    if (!weaponHash) return;
    using namespace natives_api;
    Init(natives::REMOVE_WEAPON_FROM_PED);
    Push(ped);
    Push(static_cast<unsigned long long>(weaponHash));
    Push(true); // p2
    Push(static_cast<unsigned long long>(0)); // removeReason Default
    Call();
}

void SetCurrentPedWeaponEntity(unsigned int ped, uint32_t weaponHash,
                               bool equipNow)
{
    using namespace natives_api;
    Init(natives::SET_CURRENT_PED_WEAPON);
    Push(ped);
    Push(static_cast<unsigned long long>(weaponHash));
    Push(equipNow);
    Push(static_cast<unsigned long long>(0));
    Push(static_cast<unsigned long long>(0));
    Push(static_cast<unsigned long long>(0));
    Call();
}

void HidePedWeaponsEntity(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::_HIDE_PED_WEAPONS);
    Push(ped);
    Push(static_cast<unsigned long long>(2)); // p0 (как в coop)
    Push(false);                              // immediately=false -> анимация
    Call();
}

void SetPedInfiniteAmmoEntity(unsigned int ped, uint32_t weaponHash)
{
    using namespace natives_api;
    Init(natives::SET_PED_INFINITE_AMMO);
    Push(ped);
    Push(static_cast<unsigned long long>(1));
    Push(static_cast<unsigned long long>(weaponHash));
    Call();
}

void SetPedInfiniteClipEntity(unsigned int ped, bool on)
{
    using namespace natives_api;
    Init(natives::_SET_PED_INFINITE_AMMO_CLIP);
    Push(ped);
    Push(on ? 1 : 0);
    Call();
}

void SetAmmoInClipEntity(unsigned int ped, uint32_t weaponHash, int ammo)
{
    using namespace natives_api;
    Init(natives::SET_AMMO_IN_CLIP);
    Push(ped);
    Push(static_cast<unsigned long long>(weaponHash));
    Push(ammo);
    Call();
}

// E3b.3: резерв патронов (иначе после SET_AMMO_IN_CLIP(0) перезарядка
// мгновенно пополняется/не играет).
void SetPedAmmoEntity(unsigned int ped, uint32_t weaponHash, int ammo)
{
    using namespace natives_api;
    Init(natives::SET_PED_AMMO);
    Push(ped);
    Push(static_cast<unsigned long long>(weaponHash));
    Push(ammo);
    Call();
}

void TaskAimGunAtCoordEntity(unsigned int ped, float x, float y, float z)
{
    using namespace natives_api;
    Init(natives::TASK_AIM_GUN_AT_COORD);
    Push(ped);
    Push(x); Push(y); Push(z);
    Push(static_cast<unsigned long long>(0xFFFFFFFF)); // time -1
    Push(true);  // p5 (E3b.3: instant aim)
    Push(false); // p6
    Call();
}

// E3b.4: duration 1000-1500 мс — движок сам отстреляет несколько патронов
// с темпом оружия; 200 мс — fallback-режим одиночных выстрелов.
void TaskShootAtCoordEntity(unsigned int ped, float x, float y, float z,
                            int durationMs)
{
    using namespace natives_api;
    Init(natives::TASK_SHOOT_AT_COORD);
    Push(ped);
    Push(x); Push(y); Push(z);
    Push(static_cast<unsigned long long>(static_cast<unsigned int>(durationMs)));
    Push(static_cast<unsigned long long>(0)); // firingPattern=0 (E3b.1)
    Push(static_cast<unsigned long long>(0)); // p6
    Call();
}

void TaskReloadWeaponEntity(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::TASK_RELOAD_WEAPON);
    Push(ped);
    Push(true); // p1 (как в SDK/coop)
    Call();
}

// E3b.1: патроны в магазине (для диагностики reload/shoot).
int GetAmmoInClipEntity(unsigned int ped, uint32_t weaponHash)
{
    using namespace natives_api;
    int ammo = 0;
    Init(natives::GET_AMMO_IN_CLIP);
    Push(ped);
    Push(reinterpret_cast<unsigned long long>(&ammo));
    Push(static_cast<unsigned long long>(weaponHash));
    Call();
    return ammo;
}

// ---------------- E2c-b (v16): meta-ped assets/tint (точная внешность NPC) ----------------
// Индексы компонентов 0-based; count — из _GET_NUM_COMPONENTS_IN_PED.
// v1 использует обычные теги (bundle-ассеты зарезервированы под будущее).
constexpr std::size_t kMaxAppearanceComponents =
    net::kActorAppearanceMaxComponents; // v16: 16 (не хардкодим 12)
constexpr unsigned int kAppearanceCapturePeriodTicks = 120; // ~2 c на актора

int GetPedComponentCountEntity(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::GET_NUM_COMPONENTS_IN_PED);
    Push(ped);
    return static_cast<int>(Call());
}

bool GetMetaPedAssetGuidsEntity(unsigned int ped, int index, uint32_t& drawable,
                                uint32_t& albedo, uint32_t& normal,
                                uint32_t& material)
{
    using namespace natives_api;
    unsigned long long d = 0, a = 0, n = 0, m = 0;
    Init(natives::GET_META_PED_ASSET_GUIDS);
    Push(ped);
    Push(index);
    Push(reinterpret_cast<unsigned long long>(&d));
    Push(reinterpret_cast<unsigned long long>(&a));
    Push(reinterpret_cast<unsigned long long>(&n));
    Push(reinterpret_cast<unsigned long long>(&m));
    const bool ok = Call() != 0;
    drawable = static_cast<uint32_t>(d);
    albedo = static_cast<uint32_t>(a);
    normal = static_cast<uint32_t>(n);
    material = static_cast<uint32_t>(m);
    return ok;
}

bool GetMetaPedAssetTintEntity(unsigned int ped, int index, uint32_t& palette,
                               int& tint0, int& tint1, int& tint2)
{
    using namespace natives_api;
    unsigned long long pal = 0;
    long long t0 = 0, t1 = 0, t2 = 0;
    Init(natives::GET_META_PED_ASSET_TINT);
    Push(ped);
    Push(index);
    Push(reinterpret_cast<unsigned long long>(&pal));
    Push(reinterpret_cast<unsigned long long>(&t0));
    Push(reinterpret_cast<unsigned long long>(&t1));
    Push(reinterpret_cast<unsigned long long>(&t2));
    const bool ok = Call() != 0;
    palette = static_cast<uint32_t>(pal);
    tint0 = static_cast<int>(t0);
    tint1 = static_cast<int>(t1);
    tint2 = static_cast<int>(t2);
    return ok;
}

void ResetPedComponentsEntity(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::RESET_PED_COMPONENTS);
    Push(ped);
    Call();
}

void SetMetaPedTagEntity(unsigned int ped,
                         const net::ActorAppearanceEntry& e)
{
    using namespace natives_api;
    Init(natives::_SET_META_PED_TAG);
    Push(ped);
    Push(static_cast<unsigned long long>(e.drawable));
    Push(static_cast<unsigned long long>(e.albedo));
    Push(static_cast<unsigned long long>(e.normal));
    Push(static_cast<unsigned long long>(e.material));
    Push(static_cast<unsigned long long>(e.palette));
    Push(static_cast<int>(e.tint0));
    Push(static_cast<int>(e.tint1));
    Push(static_cast<int>(e.tint2));
    Call();
}

void CommitMetaPedComponentsEntity(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::_SET_ACTIVE_META_PED_COMPONENTS_UPDATED);
    Push(ped);
    Push(false); // isMP=false
    Call();
    Init(natives::UPDATE_PED_VARIATION);
    Push(ped);
    Push(false); // p1
    Push(true);  // p2
    Push(true);  // p3
    Push(true);  // p4
    Push(false); // p5
    Call();
}

bool IsPedReadyToRenderEntity(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::IS_PED_READY_TO_RENDER);
    Push(ped);
    return Call() != 0;
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

// E5: сырое здоровье и APPLY_DAMAGE_TO_PED (host apply; fallback — SetEntityHealth).
int GetEntityHealthValue(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::GET_ENTITY_HEALTH);
    Push(handle);
    return static_cast<int>(Call());
}

void ApplyDamageToPedEntity(unsigned int ped, int damage)
{
    using namespace natives_api;
    Init(natives::APPLY_DAMAGE_TO_PED);
    Push(ped);
    Push(damage);
    Push(static_cast<unsigned long long>(0)); // p2
    Push(static_cast<unsigned long long>(0)); // boneId
    Push(static_cast<unsigned long long>(0)); // pedKiller
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
int GetScriptTaskStatus(unsigned int ped, uint32_t taskHash, bool p2 = true)
{
    using namespace natives_api;
    Init(natives::GET_SCRIPT_TASK_STATUS);
    Push(ped);
    Push(static_cast<unsigned long long>(taskHash));
    Push(p2);
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
                   float heading, float stoppingRange = kFollowStoppingRange)
{
    using namespace natives_api;
    Init(natives::TASK_FOLLOW_NAV_MESH_TO_COORD);
    Push(ped);
    Push(x); Push(y); Push(z);
    Push(speed);
    Push(static_cast<unsigned long long>(0xFFFFFFFF)); // timeout = -1
    Push(stoppingRange); // 0.25 по умолчанию / 1.0 при stuck-fallback
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
    if (std::strcmp(state, "failed") == 0)
        ++g_perfGuestAppearanceFailed;
    else if (std::strcmp(state, "applied") == 0 ||
             std::strcmp(state, "reapplied") == 0)
        ++g_perfGuestAppearanceApplied;
    sh_log::Write("actor: appearance id=%u model=0x%X outfit=0x%X state=%s\n",
                  actorId, model, outfit, state);
}

// E2c: host-резолв одежды перебором кандидатов модели.
void LogOutfitResolved(uint32_t actorId, uint32_t model, uint32_t hash,
                       unsigned int scans)
{
    sh_log::Write("actor: outfit resolved id=%u model=0x%X hash=0x%X scans=%u\n",
                  actorId, model, hash, scans);
}

void LogOutfitFailed(uint32_t actorId, uint32_t model, unsigned int cands)
{
    sh_log::Write("actor: outfit failed id=%u model=0x%X cands=%u\n",
                  actorId, model, cands);
}

// E2c-b: диагностика capture/apply asset/tint (v16).
void LogAssetCapture(uint32_t actorId, unsigned int count, unsigned int valid,
                     uint32_t firstDrawable, uint32_t firstAlbedo)
{
    sh_log::Write(
        "actor: asset capture id=%u count=%u valid=%u first=0x%X,0x%X\n",
        actorId, count, valid, firstDrawable, firstAlbedo);
}

void LogAssetCaptureFailed(uint32_t actorId, unsigned int count)
{
    sh_log::Write("actor: asset capture id=%u count=%u valid=0 state=failed\n",
                  actorId, count);
}

void LogAssetSent(uint32_t actorId, unsigned int count)
{
    sh_log::Write("actor: asset send id=%u n=%u\n", actorId, count);
}

void LogAssetApplied(uint32_t actorId, unsigned int count, const char* state)
{
    if (std::strcmp(state, "failed") == 0)
        ++g_perfGuestAppearanceFailed;
    else
        ++g_perfGuestAppearanceApplied;
    sh_log::Write("actor: appearance assets id=%u n=%u state=%s\n",
                  actorId, count, state);
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

// E3a.4: анти-наложение поз (spacing skip + warp revert).
void LogScenarioSkip(uint32_t actorId, uint32_t otherId, float dist)
{
    sh_log::Write("actor: scenario skip id=%u reason=spacing other=%u dist=%.1f\n",
                  actorId, otherId, dist);
}

void LogScenarioReverted(uint32_t actorId, float dist)
{
    sh_log::Write("actor: scenario reverted id=%u reason=warp dist=%.1f\n",
                  actorId, dist);
}

// E3b: переходы оружия/боя (drawn/holstered/aim/shoot/reload).
void LogWeaponEvent(uint32_t actorId, uint32_t hash, const char* state)
{
    sh_log::Write("actor: weapon id=%u hash=0x%X state=%s\n",
                  actorId, hash, state);
}

// E3b.2: диагностика give (возврат натива) и fail (armed не появился).
void LogWeaponGive(uint32_t actorId, uint32_t hash, uint32_t ret, int ammo)
{
    sh_log::Write("actor: weapon give id=%u hash=0x%X ret=0x%X ammo=%d\n",
                  actorId, hash, ret, ammo);
}

void LogWeaponFail(uint32_t actorId, uint32_t hash, uint32_t giveRet)
{
    sh_log::Write("actor: weapon fail id=%u hash=0x%X give_ret=0x%X armed=0\n",
                  actorId, hash, giveRet);
}

// E3b.1/E3b.2/E3b.4: диагностика оружейного состояния движка (npc_debug=1, 5 с).
void LogWeaponDebug(uint32_t actorId, bool ready, bool armed, uint32_t cur,
                    uint32_t held, uint32_t hash, int ammo, bool drawn,
                    bool aimTask, bool shootTask, bool moving,
                    unsigned int shootTasks)
{
    sh_log::Write(
        "actor: weapon dbg id=%u ready=%d armed=%d cur=0x%X held=0x%X "
        "hash=0x%X ammo=%d drawn=%d aimTask=%d shootTask=%d moving=%d "
        "shootTasks=%u\n",
        actorId, ready ? 1 : 0, armed ? 1 : 0, cur, held, hash, ammo,
        drawn ? 1 : 0, aimTask ? 1 : 0, shootTask ? 1 : 0, moving ? 1 : 0,
        shootTasks);
}

// E3c: животные и трупы лошадей.
void LogAnimalSpawn(uint32_t actorId, unsigned int kind, uint32_t model)
{
    sh_log::Write("actor: animal spawn id=%u kind=%u model=0x%X\n",
                  actorId, kind, model);
}

void LogAnimalDeath(uint32_t actorId)
{
    sh_log::Write("actor: animal death id=%u\n", actorId);
}

// E5 (v17): отправка/применение урона по NPC (npc_debug=1).
void LogDamageSend(uint32_t actorId, uint16_t damage, uint32_t weaponHash,
                   bool headshot)
{
    sh_log::Write(
        "actor: damage send id=%u dmg=%u weapon=0x%X headshot=%d\n",
        actorId, static_cast<unsigned int>(damage), weaponHash,
        headshot ? 1 : 0);
}

void LogDamageApply(uint32_t actorId, uint16_t damage, int hp)
{
    sh_log::Write("actor: damage apply id=%u dmg=%u hp=%d\n",
                  actorId, static_cast<unsigned int>(damage), hp);
}

// E5.2: удержание/despawn трупов (roster dead-retention).
void LogCorpseHold(uint32_t actorId, unsigned int kind)
{
    sh_log::Write("actor: corpse hold id=%u kind=%u\n", actorId, kind);
}

void LogCorpseDespawn(uint32_t actorId, unsigned int afterSec)
{
    sh_log::Write("actor: corpse despawn id=%u reason=dead after=%u\n",
                  actorId, afterSec);
}

void LogDeath(uint32_t actorId)
{
    sh_log::Write("actor: death id=%u\n", actorId);
}

void LogRevive(uint32_t actorId)
{
    sh_log::Write("actor: revive id=%u\n", actorId);
}

// ======================= inbox сетевого потока =======================
// Объявлен ДО host/guest-логики: HostTick проверяет resync-запрос, гостевые
// функции забирают батчи. Только копии под mutex, без нативов в callback.
std::mutex g_inboxMutex;
// E2c-b (v16): входящий asset/tint (0x1E) — хост -> сервер -> гости.
struct AppearanceMsg
{
    uint32_t epoch = 0;
    uint32_t actorId = 0;
    std::vector<net::ActorAppearanceEntry> entries;
};
struct Inbox
{
    bool haveSpawn = false;
    uint32_t spawnEpoch = 0;
    std::vector<net::ActorSpawn> spawns;
    bool haveFrames = false;
    uint32_t framesEpoch = 0;
    uint16_t framesSeq = 0;
    unsigned long long framesRecvMs = 0;
    std::vector<net::ActorFrame> frames;
    std::deque<net::ActorDespawnPayload> despawns;
    std::vector<AppearanceMsg> appearances; // E2c-b (v16)
    std::deque<net::ActorDamagePayload> damage; // E5 (v17): запросы урона
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
    g_inbox.appearances.clear();
    g_inbox.damage.clear();
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
    uint8_t kind = 0; // E3c: 0 human, 1 animal, 2 horse
    unsigned int lastSeenTick = 0;
    // E2c: поздний резолв meta-outfit перебором кандидатов модели.
    uint32_t metaOutfit = 0;   // найденный/нативный outfit hash (в спавн)
    uint16_t outfitCursor = 0; // сколько кандидатов уже проверено
    uint8_t outfitState = 0;   // 0 none, 1 scanning, 2 resolved, 3 failed
    // E5.2: тик смерти для удержания трупа (0 = жив/не удерживается).
    unsigned int deadSinceTick = 0;
    // E2c-b (v16): захваченные asset/tint для отправки гостям.
    std::vector<net::ActorAppearanceEntry> appearance;
    uint32_t appearanceFingerprint = 0;
    uint32_t appearanceSentFingerprint = 0;
    bool appearanceSent = false;
    bool appearanceCaptureFailedLogged = false;
    unsigned int lastAppearanceCaptureTick = 0;
};

std::map<unsigned int, RosterEntry> g_roster;
// E3d-c (v19): handles педов/лошадей, реально сидящих в реплицируемом
// транспорте. Их разрешаем в discovery даже если mission; остальных mission
// NPC по-прежнему не реплицируем.
std::unordered_set<unsigned int> g_occupantHandles;
uint32_t g_nextActorId = 1;
uint32_t g_hostEpoch = 0;
uint32_t g_sessionCounter = 0;
unsigned int g_lastDiscoveryTick = 0;
unsigned int g_lastHeartbeatTick = 0;
uint16_t g_frameSeq = 0;
std::deque<net::ActorDespawnPayload> g_despawnQueue;
bool g_resendAllSpawns = false;
// E5 (v17): host-authoritative урон от гостя. Лимиты: <=8 применений/тик,
// <=15/с суммарно, per-actor не чаще 100 мс; APPLY-верификация с fallback.
unsigned int g_hostDamageWindowTick = 0;
unsigned int g_hostDamageWindowCount = 0;
std::unordered_map<uint32_t, unsigned int> g_hostLastDamageTick;
struct DamageVerify
{
    unsigned int handle = 0;
    int hpBefore = 0;
    uint16_t damage = 0;
    unsigned int tick = 0;
};
std::vector<DamageVerify> g_damageVerify;

uint32_t MakeEpoch(unsigned int tickCount)
{
    ++g_sessionCounter;
    uint32_t e = (static_cast<uint32_t>(tickCount) ^
                  (g_sessionCounter * 0x9E3779B9u)) & 0xFFFFu;
    if (!e) e = 1;
    return e;
}

net::ActorSpawn BuildSpawn(unsigned int handle, uint32_t actorId,
                           uint32_t resolvedMetaOutfit, uint8_t kind)
{
    net::ActorSpawn s{};
    s.actorId = actorId;
    s.epoch = static_cast<uint16_t>(g_hostEpoch & 0xFFFFu);
    s.kind = kind;
    s.model = GetModelOf(handle);
    // E2c: нативный hash истинный, если !=0; иначе — поздний резолв из roster
    // (перебор кандидатов модели на хосте). Для лошадей это седло/сбруя.
    const uint32_t nativeOutfit = GetMetaOutfit(handle);
    s.metaOutfit = nativeOutfit ? nativeOutfit : resolvedMetaOutfit;
    const bool dead = IsDeadPed(handle);
    if (kind == net::kActorKindHuman)
    {
        s.flags = static_cast<uint8_t>(BuildFlags(handle));
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
        // E3a: сценарий-поза (sit/work). Для мёртвых сценарий не шлём.
        const bool scenario = !dead && IsScenarioActive(handle);
        s.scenarioHash = scenario ? GetHostScenarioHash(handle) : 0;
        if (dead)
            s.flags &= static_cast<uint8_t>(~net::kActorFlagScenario);
    }
    else
    {
        // E3c: животные/бесхозные лошади — без оружия/сценария/всадника.
        // Dead flag нужен гостю, чтобы показать труп (лошадиный труп).
        s.flags = dead ? static_cast<uint8_t>(net::kActorFlagDead) : 0;
        s.reserved = 0;
        s.mountActorId = 0;
        s.moveStyle = MoveStateFromSpeed(GetSpeed(handle));
        s.weaponHash = 0;
        s.scenarioHash = 0;
    }
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
    // E3b: shoot-пульс для гостя (поле targetActorId до E3b не использовалось,
    // протокол/размер кадра не меняются): 1 = хост сейчас стреляет.
    f.targetActorId = IsShootingPed(handle) ? 1u : 0u;
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

// E3d-a: ped за рулём/в транспорте — его vehicle реплицирует world_sync;
// human-прокси водителя дал бы дубль (P1 attach пассажиров позже).
bool IsPedInVehicleForDiscovery(unsigned int ped)
{
    using namespace natives_api;
    Init(natives::IS_PED_IN_ANY_VEHICLE);
    Push(ped);
    Push(false);
    return Call() != 0;
}

void HostDiscovery(const TickContext& ctx)
{
    const unsigned int tick = ctx.tickCount;
    if (g_hostEpoch == 0)
        g_hostEpoch = MakeEpoch(tick);
    if (tick - g_lastDiscoveryTick < kDiscoveryPeriodTicks)
        return;
    g_lastDiscoveryTick = tick;
    const bool perfOn = coopcfg::Get().npcDebug;
    const unsigned long long perfDiscoverStart = perfOn ? QpcNow() : 0;
    unsigned long long perfBuildStart = 0;

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
        uint8_t kind = 0; // E3c: 0 human
    };
    static std::vector<Cand> candidates;
    static std::vector<Cand> animalCandidates; // E3c: животные/бесхозные лошади
    candidates.clear();
    animalCandidates.clear();
    candidates.reserve(64);
    animalCandidates.reserve(64);

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
        const bool isOccupant = g_occupantHandles.count(handle) != 0;
        if (IsMissionEntity(handle) && !isOccupant)
            continue; // mission NPC — только occupants реплицируем
        if (IsDeadPed(handle))
            continue;
        // npc_occupants=0: старое E3d-a поведение — водителей не дублируем.
        if (!coopcfg::Get().npcOccupants && coopcfg::Get().worldVehicles &&
            IsPedInVehicleForDiscovery(handle))
            continue;

        Cand c;
        c.handle = handle;
        c.distance2 = d2;
        c.kind = net::kActorKindHuman;
        candidates.push_back(c);
    }

    // E3c: ВТОРОЙ проход — животные и бесхозные лошади. Только наземные
    // (!bird/!fish), живые, не mission, в радиусе; cap позже (npc_animal_cap).
    static std::unordered_set<uint32_t> loggedAnimalSkips;
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
        if (IsHumanPed(handle))
            continue;
        const bool isOccupant = g_occupantHandles.count(handle) != 0;
        if (IsMissionEntity(handle) && !isOccupant)
            continue;
        if (IsDeadPed(handle))
            continue;
        // E3c: только бесхозные наземные животные; всадников и лошадей
        // игроков не дублируем (rider идёт как human mounted-пара).
        if (HasRider(handle))
            continue;
        if (!isOccupant && IsAnimalControlledByPlayer(handle))
            continue;
        const uint32_t model = GetModelOf(handle);
        if (IsBirdEntity(handle) || IsFishEntity(handle))
        {
            if (coopcfg::Get().npcDebug &&
                loggedAnimalSkips.insert(model).second)
            {
                sh_log::Write(
                    "actor: animal skip model=0x%X reason=not-ground\n",
                    model);
            }
            continue;
        }
        Cand c;
        c.handle = handle;
        c.distance2 = d2;
        c.kind = IsHorseModel(model) ? 2u : 1u;
        animalCandidates.push_back(c);
    }

    std::sort(candidates.begin(), candidates.end(),
              [](const Cand& a, const Cand& b) {
                  return a.distance2 < b.distance2;
              });
    std::sort(animalCandidates.begin(), animalCandidates.end(),
              [](const Cand& a, const Cand& b) {
                  return a.distance2 < b.distance2;
              });

    // E3d-c: occupants (реальные водители/пассажиры/draft-лошади) получают
    // слоты в первую очередь — иначе cap вытеснит их дальними NPC.
    auto occupantFirst = [](const Cand& a, const Cand& b) {
        const bool oa = g_occupantHandles.count(a.handle) != 0;
        const bool ob = g_occupantHandles.count(b.handle) != 0;
        if (oa != ob)
            return oa;
        return a.distance2 < b.distance2;
    };
    std::sort(candidates.begin(), candidates.end(), occupantFirst);
    std::sort(animalCandidates.begin(), animalCandidates.end(), occupantFirst);

    // Стабильный roster: старые акторы сохраняют actorId, новые получают
    // следующий монотонный id. cap ограничивает число активных акторов.
    std::vector<unsigned int> selected;
    selected.reserve(cap);
    std::unordered_set<unsigned int> present;
    present.reserve(candidates.size() * 2 + 1);
    for (const Cand& c : candidates)
        present.insert(c.handle);

    // E5.2: удержанные трупы (deadSinceTick != 0) — их не отдаём candidates,
    // но держим в roster, пока host-труп существует (верхний предел
    // npc_corpse_max_sec, default 300 с).
    std::vector<unsigned int> retainedCorpses;

    // Порог despawn — 70 м (radius+10): старые акторы между радиусом
    // обнаружения и 70 м не пропадают, продолжаем слать им кадры.
    const float keepRadius2 = (radius + 10.0f) * (radius + 10.0f);
    for (auto& kv : g_roster)
    {
        if (present.count(kv.first)) continue;
        const unsigned int handle = kv.first;
        if (!DoesExist(handle)) continue;
        if (IsDeadPed(handle))
        {
            // Держим мёртвого актора, пока на хосте существует труп; верхний
            // предел (npc_corpse_max_sec) — защита от вечного удержания.
            const unsigned int maxSec =
                static_cast<unsigned int>(coopcfg::Get().npcCorpseMaxSec);
            const unsigned int maxTicks =
                maxSec * kCorpseHoldTicksPerSec;
            if (maxTicks == 0)
                continue; // npc_corpse_max_sec=0: не держим (despawn сразу)
            RosterEntry& e = kv.second;
            if (e.deadSinceTick == 0)
            {
                e.deadSinceTick = tick;
                LogCorpseHold(e.actorId, e.kind);
            }
            if (tick - e.deadSinceTick <= maxTicks)
            {
                present.insert(handle); // не даём reconciliation удалить
                retainedCorpses.push_back(handle);
            }
            continue; // предел истёк — уйдёт в despawn(reason=dead)
        }
        kv.second.deadSinceTick = 0; // ожил/новый — сброс hold
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
        c.kind = kv.second.kind;
        if (c.kind == net::kActorKindHuman)
            candidates.push_back(c);
        else
            animalCandidates.push_back(c);
        present.insert(handle);
    }
    std::sort(candidates.begin(), candidates.end(), occupantFirst);
    std::sort(animalCandidates.begin(), animalCandidates.end(), occupantFirst);

    // E3c: люди занимают слоты первыми, животные — свободные ПОСЛЕ них
    // (общий roster <= kMaxActors 12; npc_animal_cap=0 выключает животных).
    int animalCap = coopcfg::Get().npcAnimalCap;
    if (animalCap < 0) animalCap = 0;
    if (animalCap > net::kMaxActors) animalCap = net::kMaxActors;

    // E5.2: удержанные трупы сохраняют слоты первыми (actorId стабилен,
    // heartbeat/кадры продолжают идти с dead flag).
    std::unordered_set<unsigned int> selectedSet;
    for (unsigned int h : retainedCorpses)
    {
        if (static_cast<int>(selected.size()) >= net::kMaxActors) break;
        selected.push_back(h);
        selectedSet.insert(h);
    }
    // 1) уже известные люди, которые всё ещё в кандидатах
    for (const auto& kv : g_roster)
    {
        if (static_cast<int>(selected.size()) >= cap) break;
        if (kv.second.kind != net::kActorKindHuman) continue;
        if (!present.count(kv.first)) continue;
        if (selectedSet.count(kv.first)) continue;
        selected.push_back(kv.first);
        selectedSet.insert(kv.first);
    }
    // 2) добор новыми ближайшими людьми
    for (const Cand& c : candidates)
    {
        if (static_cast<int>(selected.size()) >= cap) break;
        if (selectedSet.count(c.handle)) continue;
        selected.push_back(c.handle);
        selectedSet.insert(c.handle);
    }
    // 3) животные: известные (hysteresis) + новые, пока есть слоты/cap
    int remaining = net::kMaxActors - static_cast<int>(selected.size());
    if (animalCap > remaining) animalCap = remaining;
    int animalsSelected = 0;
    for (const auto& kv : g_roster)
    {
        if (animalsSelected >= animalCap) break;
        if (kv.second.kind == net::kActorKindHuman) continue;
        if (!present.count(kv.first)) continue;
        if (selectedSet.count(kv.first)) continue;
        selected.push_back(kv.first);
        selectedSet.insert(kv.first);
        ++animalsSelected;
    }
    for (const Cand& c : animalCandidates)
    {
        if (animalsSelected >= animalCap) break;
        if (selectedSet.count(c.handle)) continue;
        selected.push_back(c.handle);
        selectedSet.insert(c.handle);
        ++animalsSelected;
    }

    // 3) остальных из roster — despawn (умер/вытеснен/ушёл)
    std::vector<unsigned int> dropHandles;
    for (const auto& kv : g_roster)
    {
        if (selectedSet.count(kv.first)) continue;
        const bool dead = DoesExist(kv.first) && IsDeadPed(kv.first);
        const bool heldCorpse = kv.second.deadSinceTick != 0;
        const bool stillPresent = present.count(kv.first) != 0;
        if (heldCorpse)
        {
            const unsigned int afterSec =
                (tick - kv.second.deadSinceTick) / kCorpseHoldTicksPerSec;
            LogCorpseDespawn(kv.second.actorId, afterSec);
        }
        QueueDespawn(kv.second.actorId,
                     (dead || heldCorpse)
                         ? kDespawnDead
                         : (stillPresent ? kDespawnEvicted : kDespawnGone));
        dropHandles.push_back(kv.first);
    }
    for (unsigned int h : dropHandles)
        g_roster.erase(h);

    // E4: discover-часть закончена (пул+фильтры); дальше build-часть
    // (BuildSpawn/BuildFrame + Send).
    if (perfOn)
    {
        g_perfHostDiscover.AddUs(QpcDeltaUs(perfDiscoverStart));
        perfBuildStart = QpcNow();
    }

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
        e.kind = DetectActorKind(handle);
        e.lastSeenTick = tick;
        // E2c: нативный outfit (если есть) или перебор кандидатов позже.
        // E3c: животных не сканируем (kind!=human -> resolver пропускает).
        e.metaOutfit = GetMetaOutfit(handle);
        e.outfitState = (e.kind == net::kActorKindHuman)
                            ? (e.metaOutfit ? 2u : 0u)
                            : 2u;
        g_roster[handle] = e;
        newSpawns.push_back(BuildSpawn(handle, e.actorId, e.metaOutfit,
                                       e.kind));
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
                out.push_back(BuildSpawn(kv.first, kv.second.actorId,
                                         kv.second.metaOutfit,
                                         kv.second.kind));
            }
            // E2c-b: heartbeat/join/resync — переслать asset/tint всем.
            for (auto& kv : g_roster)
                kv.second.appearanceSent = false;
        }
        else
        {
            out = newSpawns;
        }
        if (!out.empty())
        {
            coop::SendActorSpawnBatch(g_hostEpoch, out);
            g_perfHostSpawns += static_cast<unsigned long long>(out.size());
            g_perfHostBytes += static_cast<unsigned long long>(out.size()) *
                               sizeof(net::ActorSpawn);
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
        g_perfHostFrames += static_cast<unsigned long long>(frames.size());
        g_perfHostBytes += static_cast<unsigned long long>(frames.size()) *
                           sizeof(net::ActorFrame);
    }

    if (perfOn && perfBuildStart)
        g_perfHostBuild.AddUs(QpcDeltaUs(perfBuildStart));
}

void HostResetState()
{
    g_occupantHandles.clear();
    g_roster.clear();
    g_despawnQueue.clear();
    g_hostEpoch = 0;
    g_nextActorId = 1;
    g_lastDiscoveryTick = 0;
    g_lastHeartbeatTick = 0;
    g_resendAllSpawns = false;
    g_hostDamageWindowTick = 0;
    g_hostDamageWindowCount = 0;
    g_hostLastDamageTick.clear();
    g_damageVerify.clear();
}

// E2c: host-резолв meta-outfit перебором кандидатов модели (round-robin,
// бюджет нативных вызовов на тик). При успехе — форсируем heartbeat spawn,
// чтобы гость получил hash ≤2.5 с (обычный heartbeat cadence).
void ProcessHostOutfitResolution(unsigned int tick)
{
    (void)tick;
    if (g_roster.empty())
        return;

    // Кто ещё сканируется (без нативов) + ротация стартовой позиции, чтобы
    // при числе акторов > бюджета никто не голодал.
    std::vector<unsigned int> order;
    order.reserve(g_roster.size());
    for (const auto& kv : g_roster)
    {
        const RosterEntry& e = kv.second;
        if (e.kind != net::kActorKindHuman) continue; // E3c: animals без резолва
        if (e.metaOutfit != 0 || e.outfitState == 3) continue;
        order.push_back(kv.first);
    }
    if (order.empty())
        return;

    static unsigned int s_rotate = 0;
    const int scanning = static_cast<int>(order.size());
    int budget = 24; // ≤24 нативных вызовов/тик суммарно
    const int perActorCap = (budget / scanning) > 0 ? (budget / scanning) : 1;
    for (int i = 0; i < scanning; ++i)
    {
        if (budget <= 0) break;
        const unsigned int handle =
            order[(s_rotate + static_cast<unsigned int>(i)) %
                  static_cast<unsigned int>(scanning)];
        auto it = g_roster.find(handle);
        if (it == g_roster.end()) continue;
        RosterEntry& e = it->second;
        if (e.kind != net::kActorKindHuman) continue;
        if (e.metaOutfit != 0 || e.outfitState == 3) continue;

        const uint32_t* hashes = nullptr;
        uint16_t count = 0;
        if (!metaped_outfits::LookupMetaOutfits(e.model, &hashes, &count) ||
            count == 0)
        {
            e.outfitState = 3;
            LogOutfitFailed(e.actorId, e.model, 0);
            continue;
        }

        e.outfitState = 1;
        int scanned = 0;
        while (budget > 0 && e.outfitCursor < count && scanned < perActorCap)
        {
            const uint32_t cand = hashes[e.outfitCursor++];
            --budget;
            ++scanned;
            if (IsMetaOutfitEquipped(handle, cand))
            {
                e.metaOutfit = cand;
                e.outfitState = 2;
                g_resendAllSpawns = true; // ближайший heartbeat пошлёт hash
                LogOutfitResolved(e.actorId, e.model, cand, e.outfitCursor);
                break;
            }
        }
        if (e.outfitState == 1 && e.outfitCursor >= count)
        {
            e.outfitState = 3;
            LogOutfitFailed(e.actorId, e.model, count);
        }
    }
    s_rotate = (s_rotate + 1) % static_cast<unsigned int>(scanning);
}

// E2c-b (v16): захват asset/tint внешности на хосте. 1 актор/тик, период
// ~2 c на актора; fingerprint не изменился — не шлём. Главный риск (shop-
// coverage): GET_META_PED_ASSET_GUIDS может вернуть false для видимых
// компонентов — тогда лог `asset capture ... valid=0 state=failed` и fallback
// на E2a metaOutfit.
void ProcessHostAppearanceCapture(unsigned int tick)
{
    if (!coopcfg::Get().npcAppearance)
        return;
    if (g_roster.empty())
        return;

    static unsigned int s_captureRotate = 0;
    std::vector<unsigned int> order;
    order.reserve(g_roster.size());
    for (const auto& kv : g_roster)
        order.push_back(kv.first);
    if (order.empty())
        return;
    const unsigned int start =
        s_captureRotate % static_cast<unsigned int>(order.size());
    s_captureRotate = (s_captureRotate + 1) % static_cast<unsigned int>(order.size());

    for (std::size_t i = 0; i < order.size(); ++i)
    {
        const unsigned int handle =
            order[(start + static_cast<unsigned int>(i)) % order.size()];
        auto it = g_roster.find(handle);
        if (it == g_roster.end())
            continue;
        RosterEntry& e = it->second;
        if (e.kind != net::kActorKindHuman)
            continue; // E3c: asset capture только для людей
        const bool due =
            e.lastAppearanceCaptureTick == 0 ||
            (tick - e.lastAppearanceCaptureTick) >=
                kAppearanceCapturePeriodTicks;
        if (!due)
            continue;
        e.lastAppearanceCaptureTick = tick;
        if (!DoesExist(handle) || IsDeadPed(handle))
            continue;

        const int rawCount = GetPedComponentCountEntity(handle);
        const int count = rawCount > 0 ? rawCount : 0;
        std::vector<net::ActorAppearanceEntry> entries;
        entries.reserve(kMaxAppearanceComponents);
        for (int idx = 0;
             idx < count && entries.size() < kMaxAppearanceComponents; ++idx)
        {
            uint32_t drawable = 0, albedo = 0, normal = 0, material = 0;
            if (!GetMetaPedAssetGuidsEntity(handle, idx, drawable, albedo,
                                            normal, material))
                continue;
            if (drawable == 0)
                continue;
            net::ActorAppearanceEntry entry;
            entry.drawable = drawable;
            entry.albedo = albedo;
            entry.normal = normal;
            entry.material = material;
            uint32_t palette = 0;
            int t0 = 255, t1 = 255, t2 = 255;
            if (GetMetaPedAssetTintEntity(handle, idx, palette, t0, t1, t2))
            {
                entry.palette = palette;
                entry.tint0 = static_cast<int16_t>(t0);
                entry.tint1 = static_cast<int16_t>(t1);
                entry.tint2 = static_cast<int16_t>(t2);
            }
            else
            {
                // TINT недоступен — нейтральные значения (по консалту worldsync).
                entry.palette = 0;
                entry.tint0 = 255;
                entry.tint1 = 255;
                entry.tint2 = 255;
            }
            entries.push_back(entry);
        }

        if (entries.empty())
        {
            if (!e.appearanceCaptureFailedLogged)
            {
                e.appearanceCaptureFailedLogged = true;
                LogAssetCaptureFailed(e.actorId, static_cast<unsigned int>(count));
            }
            continue; // остаёмся на E2a metaOutfit / random variation
        }
        e.appearanceCaptureFailedLogged = false;

        // fingerprint: FNV-1a по массиву записей (смена внешности -> ресенд).
        uint32_t fp = 2166136261u;
        const uint8_t* bytes =
            reinterpret_cast<const uint8_t*>(entries.data());
        const std::size_t nbytes = entries.size() * sizeof(entries[0]);
        for (std::size_t b = 0; b < nbytes; ++b)
        {
            fp ^= bytes[b];
            fp *= 16777619u;
        }
        e.appearance = entries;
        e.appearanceFingerprint = fp;

        if (!e.appearanceSent || fp != e.appearanceSentFingerprint)
        {
            coop::SendActorAppearance(g_hostEpoch, e.actorId, e.appearance);
            e.appearanceSent = true;
            e.appearanceSentFingerprint = fp;
            if (coopcfg::Get().npcDebug)
                LogAssetSent(e.actorId,
                             static_cast<unsigned int>(e.appearance.size()));
        }
        if (coopcfg::Get().npcDebug)
        {
            LogAssetCapture(e.actorId, static_cast<unsigned int>(count),
                            static_cast<unsigned int>(e.appearance.size()),
                            e.appearance[0].drawable, e.appearance[0].albedo);
        }
        break; // бюджет: 1 актор/тик
    }
}

// E5 (v17): host применяет урон, присланный гостем (0x1F). Лимиты: <=8
// применений/тик, <=15/с, per-actor 100 мс, дистанция до стрелка <=150 м.
// Смерть — естественная (движок), далее dead flag/despawn как у E1-E3.
void ProcessActorDamageRequests(const TickContext& ctx)
{
    const unsigned int tick = ctx.tickCount;

    // 0) Верификация APPLY_DAMAGE_TO_PED: если через ~100 мс health не
    // уменьшился (натив не сработал) — fallback SET_ENTITY_HEALTH(hp-damage).
    for (auto it = g_damageVerify.begin(); it != g_damageVerify.end();)
    {
        if (tick - it->tick < 6)
        {
            ++it;
            continue;
        }
        if (it->handle && DoesExist(it->handle) && !IsDeadPed(it->handle))
        {
            const int hp = GetEntityHealthValue(it->handle);
            if (hp > 0 && hp >= it->hpBefore)
            {
                int newHp = hp - static_cast<int>(it->damage);
                if (newHp < 0) newHp = 0;
                SetEntityHealthValue(it->handle, newHp);
            }
        }
        it = g_damageVerify.erase(it);
    }

    // Забираем очередь из inbox (сетевой поток только копирует).
    std::deque<net::ActorDamagePayload> reqs;
    {
        std::lock_guard<std::mutex> lk(g_inboxMutex);
        reqs.swap(g_inbox.damage);
    }
    if (!coopcfg::Get().npcDamage)
        return; // рубильник: запросы дренируются без применения

    if (g_hostDamageWindowTick == 0 || tick - g_hostDamageWindowTick >= 60)
    {
        g_hostDamageWindowTick = tick;
        g_hostDamageWindowCount = 0;
    }
    int applied = 0;
    for (const net::ActorDamagePayload& req : reqs)
    {
        if (applied >= 8) break;
        if (g_hostDamageWindowCount >= 15) break;
        if (req.epoch != g_hostEpoch || req.actorId == 0) continue;
        if (req.damage < net::kMinActorDamage ||
            req.damage > net::kMaxActorDamage)
            continue;

        unsigned int handle = 0;
        bool found = false;
        for (const auto& kv : g_roster)
        {
            if (kv.second.actorId == req.actorId)
            {
                handle = kv.first;
                found = true;
                break;
            }
        }
        if (!found || !DoesExist(handle) || IsDeadPed(handle))
            continue;

        if (!ctx.partnerValid)
            continue;
        const Vec3 p = GetCoords(handle);
        const float dx = p.x - ctx.partnerX;
        const float dy = p.y - ctx.partnerY;
        const float dz = p.z - ctx.partnerZ;
        if (dx * dx + dy * dy + dz * dz > 150.0f * 150.0f)
            continue;

        auto last = g_hostLastDamageTick.find(req.actorId);
        if (last != g_hostLastDamageTick.end() && tick - last->second < 6)
            continue; // не чаще 100 мс на actorId

        const int hpBefore = GetEntityHealthValue(handle);
        if (hpBefore <= 0)
            continue;
        ApplyDamageToPedEntity(handle, static_cast<int>(req.damage));
        g_damageVerify.push_back({handle, hpBefore, req.damage, tick});
        g_hostLastDamageTick[req.actorId] = tick;
        ++g_hostDamageWindowCount;
        ++applied;
        if (coopcfg::Get().npcDebug)
            LogDamageApply(req.actorId, req.damage, hpBefore);
    }
    if (g_hostLastDamageTick.size() > 64)
        g_hostLastDamageTick.clear(); // защита от роста карты
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

    // E2c: поздний резолв outfit для 78% NPC без нативного metaOutfit.
    ProcessHostOutfitResolution(ctx.tickCount);

    // E2c-b (v16): захват asset/tint и отправка гостям (1 актор/тик).
    if (coopcfg::Get().npcDebug)
    {
        const unsigned long long t = QpcNow();
        ProcessHostAppearanceCapture(ctx.tickCount);
        g_perfHostCapture.AddUs(QpcDeltaUs(t));
    }
    else
    {
        ProcessHostAppearanceCapture(ctx.tickCount);
    }

    // E5: применить урон гостей по roster-акторам (v17 0x1F).
    ProcessActorDamageRequests(ctx);

    // despawn — не чаще 1 сообщения за тик
    if (!g_despawnQueue.empty())
    {
        net::ActorDespawnPayload d = g_despawnQueue.front();
        g_despawnQueue.pop_front();
        coop::SendActorDespawn(d);
        ++g_perfHostDespawns;
        g_perfHostBytes += sizeof(net::ActorDespawnPayload);
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
    uint8_t kind = 0;        // E3c: 0 human, 1 animal, 2 horse
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
    bool appearanceReapply = false;     // E2c: metaOutfit обновился на heartbeat
    // E2c-b (v16): точная внешность (asset/tint) — приоритетнее metaOutfit.
    std::vector<net::ActorAppearanceEntry> appearanceAssets;
    bool appearanceAssetsApplyNeeded = false;
    bool appearanceAssetsIssued = false;
    unsigned int appearanceAssetsWaitTick = 0;
    unsigned int appearanceAssetsIssuedTick = 0;
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
    unsigned int mountStuckCount = 0; // E5.3: 2 завершённые задачи -> fallback
    // E3d-c (v19): occupant attach state. Пока seated/draftAttached —
    // MoveProxy не двигает прокси (позицию ведёт vehicle/attachment).
    bool seatedInVehicle = false;
    unsigned int seatedVehicle = 0;
    int seatedSeat = -1;
    uint8_t occupantFlags = 0;
    bool draftAttached = false;
    unsigned int draftVehicle = 0;
    unsigned int draftHarness = 0;
    // Долгоживущая go-to задача для ЛОШАДИ (rider не двигаем).
    uint32_t mountTaskHash = 0;
    bool mountTaskActive = false;
    unsigned int mountLastUpdateTick = 0;
    unsigned int mountLastStatusTick = 0;
    unsigned int mountLastGaitTick = 0;
    float mountLastGait = -1.f;
    Vec3 mountMoveTarget{0.f, 0.f, 0.f};
    // E7.2: интерполяция кадров хоста на 60 Гц + smooth far-target.
    struct HostSample
    {
        float x = 0.f, y = 0.f, z = 0.f, heading = 0.f;
        unsigned long long recvMs = 0;
    };
    HostSample hostSamples[kHostSampleCount]{};
    int hostSampleCount = 0;
    int hostSampleHead = 0;
    float hostX = 0.f, hostY = 0.f, hostZ = 0.f, hostHeading = 0.f;
    float hostVelX = 0.f, hostVelY = 0.f;
    float hostSpeedInterp = 0.f;
    unsigned long long lastFrameMs = 0;
    float frameIntervalMs = 100.0f;
    bool interpValid = false;
    bool hostStale = false; // нет кадров >300 мс — держим позу/стоим
    bool idleHeld = false;
    unsigned int stopTicks = 0;
    unsigned int moveTicks = 0;
    unsigned int stuckTicks = 0;
    float issuedGoalX = 0.f, issuedGoalY = 0.f;
    bool issuedGoalValid = false;
    float lastMbr = -1.f;
    bool maxBlendSet = false;
    float lastMinMbr = 0.f;
    unsigned int lastUpdateTickE72 = 0;
    unsigned int lastStatusTickE72 = 0;
    // A/B: update-натив и marker-follow (A2) — per-actor, чтобы можно было
    // откатить на A2 только для проблемного педа.
    bool updateNativeOk = true;
    unsigned int markerHandle = 0;
    int markerModelRequest = 0;
    unsigned int markerRequestTick = 0;
    // диагностика окна 5 с для `actor: motion id=...`
    unsigned int winReissue = 0;
    unsigned int winSnap = 0;
    unsigned int winGaitRestart = 0;
    unsigned int winUpdate = 0;
    unsigned int winStatusHist[9]{};
    PerfP95 winErr;
    // E3a: сценарий (поза) и смерть/труп.
    uint32_t scenarioHash = 0;    // ActorSpawn.scenarioHash (dyn scenario)
    uint32_t scenarioApplied = 0; // какой hash реально применяли
    bool scenarioFlag = false;    // kActorFlagScenario из кадров
    uint8_t scenarioState = 0;    // 0 none,1 inplace,2 active,3 point,4 atpos,5 failed,6 skipped/reverted
    unsigned int scenarioTick = 0;
    int scenarioPoint = 0;        // E3a.2: handle scenario point (для sit/work)
    uint32_t scenarioPointType = 0;
    // E3a.4: позиция/heading на момент выдачи сценария — для warp-guard.
    float scenarioSpawnX = 0.f, scenarioSpawnY = 0.f, scenarioSpawnZ = 0.f;
    float scenarioSpawnHeading = 0.f;
    bool scenarioSpawnValid = false;
    bool deathApplied = false;    // health=0 + invincible=false применены
    // E3b: оружие/бой (только визуалы на госте).
    uint32_t weaponHash = 0;      // ActorSpawn.weaponHash (хост, held weapon)
    uint32_t weaponGiven = 0;     // выданное оружие (GIVE_WEAPON_TO_PED)
    bool weaponInHands = false;   // латч «оружие в руках»
    bool weaponDrawPending = false;
    bool weaponHolsterPending = false;
    unsigned int weaponDrawStartTick = 0;
    unsigned int weaponHolsterStartTick = 0;
    // Флаги из кадров (E3b).
    bool weaponDrawnFlag = false;
    bool weaponChangingFlag = false;
    bool weaponAimFlag = false;
    bool weaponCombatFlag = false;
    bool weaponReloadFlag = false;
    bool weaponShootPulse = false; // f.targetActorId != 0
    // Aim/shoot/reload task state.
    bool weaponAimTaskActive = false;
    unsigned int lastAimTaskTick = 0;
    float aimPointX = 0.f, aimPointY = 0.f, aimPointZ = 0.f;
    bool aimPointValid = false;
    unsigned int lastShootTick = 0;
    bool reloadInProgress = false;
    unsigned int reloadStartTick = 0;
    // E3b.1: aim захватывается ОДИН раз на эпизод Aiming; reissue только при
    // смене оружия / heading > 25° / 5 с (не от джиттера).
    bool aimCaptured = false;
    float aimCaptureX = 0.f, aimCaptureY = 0.f, aimCaptureZ = 0.f;
    float aimCaptureHeading = 0.f;
    uint32_t aimWeapon = 0;
    unsigned int aimCaptureTick = 0;
    bool prevAimFlag = false;
    // E3b.1: готовность оружия (draw/equip ждём до 2 с) и диагностика.
    unsigned int equipWaitStartTick = 0;
    bool equipWaitActive = false;
    unsigned int lastWeaponDbgTick = 0;
    // E3b.2: возврат GIVE_WEAPON_TO_PED, retry и диагностика give/armed.
    uint32_t weaponGiveRet = 0;
    int weaponGiveAttempts = 0;
    unsigned int weaponGiveTick = 0;
    bool weaponGiveFailLogged = false;
    bool weaponDrawnUnverified = false;
    unsigned int weaponEquipDemandTick = 0;
    bool weaponFailLogged = false;
    // E3b.3: стойка в бою (StandStill один раз) и флаг выдачи shoot-задачи.
    bool combatStance = false;
    unsigned int combatStanceStartTick = 0;
    bool shootTaskActive = false;
    // E3b.4: continuous-fire state machine (aim -> long shoot -> wait -> aim).
    uint8_t firePhase = 0;        // 0 aim, 1 shoot-ready, 2 wait burst end
    unsigned int firePhaseTick = 0;
    unsigned int shootTaskCount = 0; // всего выданных shoot-задач (для dbg/лога)
    bool fireUseFallback = false;    // движок не стреляет длинной очередью
    bool fireShotWorked = false;     // в текущем эпизоде реально стрелял/тратил ammo
    int fireAmmoBefore = -1;
};

std::map<uint32_t, Proxy> g_proxies;
std::unordered_map<uint32_t, unsigned int> g_handleByActor;
// E5 (v17): reverse-map handle -> actorId для детекта aim-target гостя.
std::unordered_map<unsigned int, uint32_t> g_actorByHandle;
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
unsigned long long g_latestFramesRecvMs = 0;
std::deque<net::ActorDespawnPayload> g_guestDespawnQueue;
int g_modelRequestsThisTick = 0;
int g_appearanceRequestsThisTick = 0; // E2a: бюджет REQUEST_META_PED_OUTFIT
int g_mountsCreatedThisTick = 0;      // E2b: <=1 лошадь/тик
// E2c-b (v16): asset/tint, пришедший ДО создания прокси (pending-применение
// сразу после CREATE_PED). Только игровой поток.
std::unordered_map<uint32_t, std::vector<net::ActorAppearanceEntry>>
    g_pendingAppearanceAssets;
unsigned int g_appliedFrames = 0;
unsigned int g_timeouts = 0;
unsigned int g_lastNoCollisionTick = 0;
// E2a.2: счётчики движения за окно сводки (5 с), лог при npc_debug=1.
unsigned int g_motionIssued = 0;
unsigned int g_motionReissue = 0;
unsigned int g_motionGaitRestart = 0;
// E7.2: snap/teleport/update-натив/stuck в окне сводки.
unsigned int g_motionSnap = 0;
unsigned int g_motionUpdateNative = 0;
unsigned int g_motionStuck = 0;
unsigned int g_lastTeleportLogTick = 0;

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
void DeleteMarkerObject(unsigned int marker);

bool DeleteProxy(Proxy& proxy)
{
    // E7.2 A2: удалить marker-entity, если был (fallback marker-follow).
    if (proxy.markerHandle)
    {
        DeleteMarkerObject(proxy.markerHandle);
        proxy.markerHandle = 0;
    }
    // E2a: не оставляем висящий meta-outfit-запрос при удалении прокси.
    if (proxy.outfitRequest)
    {
        ReleaseMetaPedOutfitRequest(proxy.outfitRequest);
        proxy.outfitRequest = 0;
        proxy.appearanceState = 3;
    }

    bool ok = true;

    // E5: убрать reverse-map до удаления (handle обнуляется в DeleteEntitySafe).
    if (proxy.handle)
        g_actorByHandle.erase(proxy.handle);

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
    p.kind = s.kind; // E3c: 0 human, 1 animal, 2 horse
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
    // E2c-b (v16): asset/tint мог прийти ДО создания прокси — применить сразу
    // после CREATE_PED; assets приоритетнее metaOutfit (E2a не запускаем).
    if (p.kind == net::kActorKindHuman)
    {
        auto pa = g_pendingAppearanceAssets.find(s.actorId);
        if (pa != g_pendingAppearanceAssets.end())
        {
            p.appearanceAssets = pa->second;
            p.appearanceAssetsApplyNeeded = true;
            p.appearanceAssetsWaitTick = ctx.tickCount;
            p.appearanceState = 3; // E2a не трогаем: asset-данные главнее
            g_pendingAppearanceAssets.erase(pa);
        }
    }
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
    // E3b: стартовое оружие/флаги из спавна (Drawn/Changing/Aiming/Reloading).
    p.weaponHash = s.weaponHash;
    p.weaponGiven = 0; // выдадим в ProcessActorWeapons
    p.weaponDrawnFlag = (s.flags & net::kActorFlagDrawn) != 0;
    p.weaponChangingFlag = (s.flags & net::kActorFlagChanging) != 0;
    p.weaponAimFlag = (s.flags & net::kActorFlagAiming) != 0;
    p.weaponCombatFlag = (s.flags & net::kActorFlagCombat) != 0;
    p.weaponReloadFlag = (s.flags & net::kActorFlagReloading) != 0;
    if (p.dead)
    {
        SetInvincible(handle, false);
        SetEntityHealthValue(handle, 0);
        p.deathApplied = true;
        LogDeath(p.actorId);
    }

    g_handleByActor[handle] = p.actorId;
    g_actorByHandle[handle] = p.actorId;
    g_proxies[p.actorId] = p;
    ++g_perfGuestSpawns;
    if (p.kind == net::kActorKindHuman)
        LogSpawn(p.actorId, p.model,
                 static_cast<unsigned int>(g_proxies.size()));
    else
        LogAnimalSpawn(p.actorId, p.kind, p.model);
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
        if (!p.appearanceAssets.empty())
            continue; // E2c-b: asset/tint приоритетнее metaOutfit (E2a не нужен)
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
        // E2c: повторное применение после heartbeat-обновления metaOutfit.
        const char* state = !ok ? "failed"
            : (p.appearanceReapply ? "reapplied" : "applied");
        if (ok)
            p.appearanceReapply = false;
        LogAppearance(p.actorId, p.model, p.metaOutfit, state);
    }
}

// E2c-b (v16): применение asset/tint на госте. ≤1 актор/тик, staged:
// wait ready -> reset+tags+commit -> readback -> applied / failed(fallback).
// При невидимом/пустом результате обязателен RandomizeComponents+SetVisible,
// чтобы NPC не остался невидимым.
void ProcessGuestAppearanceAssets(unsigned int tick)
{
    if (!coopcfg::Get().npcAppearance)
        return;

    for (auto& kv : g_proxies)
    {
        Proxy& p = kv.second;
        if (p.kind != net::kActorKindHuman)
            continue; // E3c: assets не применяем для животных
        if (!p.appearanceAssetsApplyNeeded)
            continue;
        if (!p.handle || !DoesExist(p.handle) || p.dead ||
            p.appearanceAssets.empty())
        {
            p.appearanceAssetsApplyNeeded = false;
            if (p.dead)
                p.appearanceAssets.clear();
            continue;
        }

        if (!p.appearanceAssetsIssued)
        {
            if (!IsPedReadyToRenderEntity(p.handle))
            {
                if (p.appearanceAssetsWaitTick == 0)
                    p.appearanceAssetsWaitTick = tick;
                else if (tick - p.appearanceAssetsWaitTick > 180)
                {
                    // Не отрендерился за ~3 с — fallback, не оставляем пустым.
                    RandomizeComponents(p.handle);
                    SetVisible(p.handle);
                    LogAssetApplied(
                        p.actorId,
                        static_cast<unsigned int>(p.appearanceAssets.size()),
                        "failed");
                    p.appearanceAssetsApplyNeeded = false;
                    p.appearanceAssets.clear();
                }
                break; // бюджет: ≤1 актор/тик
            }
            ResetPedComponentsEntity(p.handle);
            for (const net::ActorAppearanceEntry& e : p.appearanceAssets)
                SetMetaPedTagEntity(p.handle, e);
            CommitMetaPedComponentsEntity(p.handle);
            p.appearanceAssetsIssued = true;
            p.appearanceAssetsIssuedTick = tick;
            break; // readback на следующих тиках
        }

        if (tick - p.appearanceAssetsIssuedTick < 6)
            break; // дать движку применить компоненты

        // Readback: count>0 и первый валидный drawable.
        const int count = GetPedComponentCountEntity(p.handle);
        uint32_t d = 0, a = 0, n = 0, m = 0;
        const bool firstOk =
            count > 0 &&
            GetMetaPedAssetGuidsEntity(p.handle, 0, d, a, n, m) && d != 0;
        if (firstOk)
        {
            SetVisible(p.handle);
        }
        else
        {
            // Невидимый NPC недопустим: fallback random variation.
            RandomizeComponents(p.handle);
            SetVisible(p.handle);
        }
        LogAssetApplied(p.actorId,
                        static_cast<unsigned int>(p.appearanceAssets.size()),
                        firstOk ? "applied" : "failed");
        p.appearanceAssetsApplyNeeded = false;
        p.appearanceAssetsIssued = false;
        break; // бюджет: ≤1 актор/тик
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
// ================= E7.2: smooth NPC motion helpers =================
// Кольцо сэмплов кадра хоста (индекс 0 = самый свежий).
Proxy::HostSample& HostSampleAt(Proxy& p, int newestIndex)
{
    int idx = (p.hostSampleHead - 1 - newestIndex) % kHostSampleCount;
    if (idx < 0)
        idx += kHostSampleCount;
    return p.hostSamples[idx];
}

void PushHostSample(Proxy& p, float x, float y, float z, float heading,
                    unsigned long long recvMs)
{
    if (p.hostSampleCount > 0)
    {
        const Proxy::HostSample& newest = HostSampleAt(p, 0);
        if (recvMs > newest.recvMs)
        {
            const float dtMs = static_cast<float>(recvMs - newest.recvMs);
            if (dtMs > 1.0f && dtMs < 2000.0f)
                p.frameIntervalMs =
                    p.frameIntervalMs * 0.7f + dtMs * 0.3f;
        }
    }
    Proxy::HostSample& s = p.hostSamples[p.hostSampleHead];
    s.x = x; s.y = y; s.z = z; s.heading = heading; s.recvMs = recvMs;
    p.hostSampleHead = (p.hostSampleHead + 1) % kHostSampleCount;
    if (p.hostSampleCount < kHostSampleCount)
        ++p.hostSampleCount;
    p.lastFrameMs = recvMs;
}

// Интерполяция/экстраполяция (<=150 мс) позы хоста на каждом тике (60 Гц).
void UpdateHostInterpolation(Proxy& p, bool& outHavePose)
{
    outHavePose = false;
    if (p.hostSampleCount == 0)
        return;
    const Proxy::HostSample& latest = HostSampleAt(p, 0);
    const unsigned long long now = GetTickCount64();
    const float ageMs =
        (now > latest.recvMs) ? static_cast<float>(now - latest.recvMs) : 0.f;
    float x = latest.x, y = latest.y, z = latest.z, heading = latest.heading;
    if (p.hostSampleCount >= 2)
    {
        const Proxy::HostSample& older = HostSampleAt(p, 1);
        const float dt =
            (latest.recvMs > older.recvMs)
                ? static_cast<float>(latest.recvMs - older.recvMs) / 1000.f
                : 0.f;
        if (dt > 0.001f)
        {
            const float vx = (latest.x - older.x) / dt;
            const float vy = (latest.y - older.y) / dt;
            p.hostVelX = p.hostVelX * 0.5f + vx * 0.5f;
            p.hostVelY = p.hostVelY * 0.5f + vy * 0.5f;
        }
        if (ageMs <= kHostHoldMs)
        {
            const float ex =
                std::min(ageMs, kHostExtrapolateMaxMs) / 1000.f;
            float dx = p.hostVelX * ex;
            float dy = p.hostVelY * ex;
            const float len = std::sqrt(dx * dx + dy * dy);
            const float maxStep =
                std::min(p.ewmaSpeed, 8.0f) *
                    (kHostExtrapolateMaxMs / 1000.f) +
                0.5f;
            if (len > maxStep && len > 0.0001f)
            {
                dx *= maxStep / len;
                dy *= maxStep / len;
            }
            x += dx;
            y += dy;
        }
    }
    p.hostStale = ageMs > kHostHoldMs;
    if (p.hostStale)
    {
        p.hostVelX = 0.f;
        p.hostVelY = 0.f;
    }
    p.hostX = x;
    p.hostY = y;
    p.hostZ = z;
    p.hostHeading = heading;
    const float sp = p.hostStale ? 0.f : std::sqrt(p.hostVelX * p.hostVelX +
                                                   p.hostVelY * p.hostVelY);
    p.hostSpeedInterp = std::max(0.f, std::min(sp, 8.f));
    p.ewmaSpeed = p.ewmaSpeed * 0.6f +
                  std::min(p.hostSpeedInterp, 8.0f) * 0.4f;
    p.interpValid = true;
    outHavePose = true;
}

// Дальняя цель впереди хоста по ходу/heading; не клипаем к newest host point.
void ComputeFarGoal(const Proxy& p, bool horse, float proxyX, float proxyY,
                    float& outX, float& outY)
{
    // Нет кадров >300 мс — цель = поза хоста (держим позицию, не убегаем).
    if (p.hostStale)
    {
        outX = p.hostX;
        outY = p.hostY;
        return;
    }
    const float speed = p.hostSpeedInterp;
    const float leadMin = horse ? kHorseLeadMin : kHumanLeadMin;
    const float leadMax = horse ? kHorseLeadMax : kHumanLeadMax;
    float lead = speed * (horse ? kHorseLeadSeconds : kHumanLeadSeconds);
    if (lead < leadMin) lead = leadMin;
    if (lead > leadMax) lead = leadMax;

    float dirX = 0.f, dirY = 0.f;
    if (speed > 0.3f &&
        (std::fabs(p.hostVelX) + std::fabs(p.hostVelY)) > 0.0001f)
    {
        dirX = p.hostVelX;
        dirY = p.hostVelY;
    }
    else
    {
        const float rad = p.hostHeading * kRadPerDeg;
        dirX = -std::sin(rad);
        dirY = std::cos(rad);
    }
    float dl = std::sqrt(dirX * dirX + dirY * dirY);
    if (dl < 0.0001f)
    {
        dirX = 0.f;
        dirY = 1.f;
        dl = 1.f;
    }
    dirX /= dl;
    dirY /= dl;

    // Разворот: если heading и направление движения расходятся — short lead.
    const float hrad = p.hostHeading * kRadPerDeg;
    const float hfx = -std::sin(hrad);
    const float hfy = std::cos(hrad);
    const float dotHV = dirX * hfx + dirY * hfy;
    if (dotHV < 0.3f)
        lead = leadMin;

    // Минимальная дистанция goal-proxy 2.5 м.
    outX = p.hostX + dirX * lead;
    outY = p.hostY + dirY * lead;
    float gdx = outX - proxyX, gdy = outY - proxyY;
    float gd = std::sqrt(gdx * gdx + gdy * gdy);
    if (gd < kGoalMinProxyDist)
    {
        const float need = kGoalMinProxyDist - gd;
        outX += dirX * need;
        outY += dirY * need;
    }
    // Точка должна быть впереди хоста (dot > 0).
    if ((outX - p.hostX) * dirX + (outY - p.hostY) * dirY <= 0.f)
    {
        outX = p.hostX + dirX * leadMin;
        outY = p.hostY + dirY * leadMin;
    }
}

// Точный MBR по target speed (м/с); graceful clamp/fallback.
float ComputeMoveBlendForSpeed(unsigned int ped, float targetSpeedMps)
{
    using namespace natives_api;
    Init(natives::_COMPUTE_SPEED_FOR_PED_MOVE_BLEND_RATIO);
    Push(ped);
    Push(targetSpeedMps);
    float mbr = CallFloat();
    if (!(mbr >= 0.f && mbr <= 3.5f))
        mbr = targetSpeedMps * 0.4f; // fallback, если натив не дал значение
    if (mbr < 0.f) mbr = 0.f;
    if (mbr > 3.f) mbr = 3.f;
    return mbr;
}

void SetMaxMoveBlend(unsigned int ped, float value)
{
    using namespace natives_api;
    Init(natives::SET_PED_MAX_MOVE_BLEND_RATIO);
    Push(ped);
    Push(value);
    Call();
}

void SetMinMoveBlend(unsigned int ped, float value)
{
    using namespace natives_api;
    Init(natives::SET_PED_MIN_MOVE_BLEND_RATIO);
    Push(ped);
    Push(value);
    Call();
}

void LogTeleportE72(uint32_t actorId, float dist)
{
    if (g_logTick - g_lastTeleportLogTick < kLogEveryTicks)
        return;
    g_lastTeleportLogTick = g_logTick;
    sh_log::Write("actor: teleport id=%u dist=%.1f\n", actorId, dist);
}

float GetEntitySpeedNative(unsigned int handle)
{
    using namespace natives_api;
    Init(natives::GET_ENTITY_SPEED);
    Push(handle);
    return CallFloat();
}

void DeleteMarkerObject(unsigned int marker)
{
    if (!marker)
        return;
    {
        int h = static_cast<int>(marker);
        using namespace natives_api;
        Init(natives::DELETE_OBJECT);
        Push(reinterpret_cast<unsigned long long>(&h));
        Call();
    }
    if (DoesExist(marker))
    {
        int h2 = static_cast<int>(marker);
        using namespace natives_api;
        Init(natives::SET_OBJECT_AS_NO_LONGER_NEEDED);
        Push(reinterpret_cast<unsigned long long>(&h2));
        Call();
    }
}

bool IsModelValidSafe(uint32_t model)
{
    using namespace natives_api;
    Init(natives::IS_MODEL_VALID);
    Push(static_cast<unsigned long long>(model));
    return Call() != 0;
}

// E7.2: marker-entity для A2 (TASK_FOLLOW_TO_OFFSET_OF_ENTITY).
unsigned int EnsureMarkerPed(Proxy& p, float x, float y, float z)
{
    if (p.markerHandle && DoesExist(p.markerHandle))
        return p.markerHandle;
    if (!coopcfg::Get().npcMotionMarker)
        return 0;
    if (p.markerModelRequest < 0)
        return 0;
    const unsigned int tick = g_logTick;
    if (p.markerModelRequest == 0)
    {
        for (uint32_t model : kMarkerModels)
        {
            if (IsModelValidSafe(model))
            {
                RequestModel(model);
                p.markerModelRequest = static_cast<int>(model);
                p.markerRequestTick = tick;
                return 0;
            }
        }
        p.markerModelRequest = -1; // ни одна модель не валидна — A2 off
        return 0;
    }
    const uint32_t model = static_cast<uint32_t>(p.markerModelRequest);
    if (!HasModelLoaded(model))
    {
        if (tick - p.markerRequestTick > 300)
            p.markerModelRequest = -1; // не загрузилась — A2 off
        return 0;
    }
    using namespace natives_api;
    Init(natives::CREATE_OBJECT);
    Push(static_cast<unsigned long long>(model));
    Push(x); Push(y); Push(z);
    Push(false); // isNetwork
    Push(false); // bScriptHostObj
    Push(false); // dynamic
    Push(false); // p7
    Push(false); // p8
    const unsigned int obj = static_cast<unsigned int>(Call());
    if (!obj)
        return 0;
    SetMissionEntity(obj);
    Init(natives::SET_ENTITY_VISIBLE);
    Push(obj);
    Push(false);
    Call();
    Init(natives::SET_ENTITY_COMPLETELY_DISABLE_COLLISION);
    Push(obj);
    Push(true);
    Push(false);
    Call();
    Init(natives::FREEZE_ENTITY_POSITION);
    Push(obj);
    Push(true);
    Call();
    p.markerHandle = obj;
    return obj;
}

void MoveMarker(Proxy& p, float x, float y, float z)
{
    if (!p.markerHandle || !DoesExist(p.markerHandle))
        return;
    SetCoordsNative(p.markerHandle, x, y, z);
}

void IssueMarkerFollow(unsigned int ped, unsigned int marker, float mbr,
                       float stoppingRange)
{
    using namespace natives_api;
    Init(natives::TASK_FOLLOW_TO_OFFSET_OF_ENTITY);
    Push(ped);
    Push(marker);
    Push(0.f); Push(0.f); Push(0.f);
    Push(mbr);                                    // movementSpeed
    Push(static_cast<unsigned long long>(0xFFFFFFFF)); // timeout -1
    Push(stoppingRange);                          // 0.1
    Push(true);  // p8: relativeOffset/persistFollowing
    Push(true);  // p9
    Push(false); // p10 walkOnly
    Push(false); // p11
    Push(true);  // p12
    Push(false); // p13
    Call();
}

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
                LogMountFailed(p.actorId, "seat-fallback");
                p.mounted = false;
                p.dismountTick = tick;
                p.taskActive = false;
                p.mountTaskActive = false;
                p.moveTaskValid = false;
                return;
            }
        }
    }

    bool havePose = false;
    UpdateHostInterpolation(p, havePose);
    const Vec3 horsePos = GetCoords(horse);
    if (!havePose)
        return;

    // E7.2: нет кадров >2 с — snap в последнюю известную точку.
    const unsigned long long nowMs = GetTickCount64();
    if (p.lastFrameMs != 0 && nowMs - p.lastFrameMs > kNoFramesSnapMs)
    {
        const float dist = std::sqrt((p.tx - horsePos.x) * (p.tx - horsePos.x) +
                                     (p.ty - horsePos.y) * (p.ty - horsePos.y));
        SetCoordsNative(horse, p.tx, p.ty, p.tz);
        SetHeadingNative(horse, p.heading);
        SetCoordsNative(p.handle, p.tx, p.ty, p.tz);
        SeatRiderOnMount(p);
        p.mountTaskActive = false;
        p.lastReseatTick = tick;
        p.issuedGoalValid = false;
        ++p.winSnap;
        ++g_motionSnap;
        LogTeleportE72(p.actorId, dist);
        return;
    }

    const float dx = p.hostX - horsePos.x;
    const float dy = p.hostY - horsePos.y;
    const float dz = p.hostZ - horsePos.z;
    const float dist = std::sqrt(dx * dx + dy * dy);
    if (dist > kTeleportDistE72 || std::fabs(dz) > kTeleportDzE72)
    {
        SetCoordsNative(horse, p.hostX, p.hostY, p.hostZ);
        SetHeadingNative(horse, p.hostHeading);
        SetCoordsNative(p.handle, p.hostX, p.hostY, p.hostZ);
        SeatRiderOnMount(p);
        p.mountTaskActive = false;
        p.taskActive = false;
        p.lastReseatTick = tick;
        p.issuedGoalValid = false;
        ++p.winSnap;
        ++g_motionSnap;
        LogTeleportE72(p.actorId, dist);
        return;
    }

    g_motionErr.AddUs(static_cast<unsigned long long>(dist * 1000.0f));
    g_mountErr.AddUs(static_cast<unsigned long long>(dist * 1000.0f));
    p.winErr.AddUs(static_cast<unsigned long long>(dist * 1000.0f));

    // Стоп/старт гистерезис (лошадь).
    const bool hostSlow = p.hostSpeedInterp < kStopSpeedE72;
    if (hostSlow && dist < kStopDistE72)
    {
        ++p.stopTicks;
        p.moveTicks = 0;
    }
    else
    {
        p.stopTicks = 0;
        if (p.hostSpeedInterp > kResumeSpeedE72)
            ++p.moveTicks;
        else
            p.moveTicks = 0;
    }
    if (p.stopTicks >= kStopConfirmTicks)
    {
        if (!p.idleHeld)
        {
            SetDesiredMoveBlend(horse, 0.0f);
            StandStill(horse);
            SetMinMoveBlend(horse, 0.0f);
            p.lastMinMbr = 0.f;
            p.mountTaskActive = false;
            p.mountLastGait = 0.f;
            p.mountLastGaitTick = tick;
            p.idleHeld = true;
            p.lastMbr = 0.f;
        }
        return;
    }
    if (p.idleHeld && p.hostSpeedInterp > kResumeSpeedE72 &&
        p.moveTicks >= kResumeConfirmTicks)
        p.idleHeld = false;

    float gx = 0.f, gy = 0.f;
    ComputeFarGoal(p, true, horsePos.x, horsePos.y, gx, gy);
    const float gdx = gx - horsePos.x;
    const float gdy = gy - horsePos.y;
    const float distGoal = std::sqrt(gdx * gdx + gdy * gdy);

    float targetSpeed = p.hostSpeedInterp + std::max(0.f, distGoal - kHorseLeadMin) * 0.8f;
    if (targetSpeed > 8.f) targetSpeed = 8.f;
    float mbr = ComputeMoveBlendForSpeed(horse, targetSpeed);
    if (p.hostSpeedInterp > 0.3f && mbr < 1.f) mbr = 1.f;
    if (!p.maxBlendSet)
    {
        SetMaxMoveBlend(horse, 3.0f);
        p.maxBlendSet = true;
    }
    if (std::fabs(mbr - p.lastMbr) > kMbrHysteresis)
    {
        SetDesiredMoveBlend(horse, mbr);
        p.lastMbr = mbr;
        ++g_motionGaitRestart;
        ++p.winGaitRestart;
    }
    const float minMbr = (distGoal > 10.f && p.hostSpeedInterp > 1.f) ? 0.5f : 0.f;
    if (std::fabs(minMbr - p.lastMinMbr) > 0.05f)
    {
        SetMinMoveBlend(horse, minMbr);
        p.lastMinMbr = minMbr;
    }

    const float stopping = 0.05f; // E7.2: 0.05-0.1
    const bool markerMode = coopcfg::Get().npcMotionMarker &&
                            (!p.updateNativeOk ||
                             !coopcfg::Get().npcMotionUpdate);
    if (markerMode)
    {
        const unsigned int marker = EnsureMarkerPed(p, gx, gy, p.hostZ);
        if (marker)
        {
            bool needIssue = !p.mountTaskActive;
            const bool wasActive = p.mountTaskActive;
            if (!needIssue &&
                tick - p.mountLastStatusTick >= kStatusIntervalE72)
            {
                p.mountLastStatusTick = tick;
                const int st = GetScriptTaskStatus(
                    horse, p.mountTaskHash,
                    coopcfg::Get().npcMotionStatusP2);
                if (st >= 0 && st <= 8)
                    ++p.winStatusHist[st];
                if (st == 7 || st == 8)
                {
                    needIssue = true;
                    p.mountTaskActive = false;
                }
            }
            if (needIssue)
            {
                IssueMarkerFollow(horse, marker, mbr, 0.1f);
                p.mountTaskHash = natives::SCRIPT_TASK_FOLLOW_TO_OFFSET_OF_ENTITY;
                p.mountTaskActive = true;
                p.mountLastUpdateTick = tick;
                p.mountLastStatusTick = tick;
                p.mountMoveTarget = Vec3{gx, gy, p.hostZ};
                p.issuedGoalX = gx;
                p.issuedGoalY = gy;
                p.issuedGoalValid = true;
                ++g_motionIssued;
                if (wasActive)
                {
                    ++p.winReissue;
                    ++g_motionReissue;
                }
            }
            else if (tick - p.mountLastUpdateTick >= kUpdateIntervalE72)
            {
                const float sx = gx - p.issuedGoalX;
                const float sy = gy - p.issuedGoalY;
                if (sx * sx + sy * sy >= kUpdateMinShift * kUpdateMinShift)
                {
                    MoveMarker(p, gx, gy, p.hostZ);
                    p.issuedGoalX = gx;
                    p.issuedGoalY = gy;
                    p.mountMoveTarget = Vec3{gx, gy, p.hostZ};
                    p.mountLastUpdateTick = tick;
                    ++p.winUpdate;
                }
            }
            return;
        }
    }

    // A: одна задача на фазу; reissue только 7/8/stuck.
    bool needIssue = !p.mountTaskActive;
    const bool wasActive = p.mountTaskActive;
    if (!needIssue && tick - p.mountLastStatusTick >= kStatusIntervalE72)
    {
        p.mountLastStatusTick = tick;
        const int st = GetScriptTaskStatus(horse, p.mountTaskHash,
                                           coopcfg::Get().npcMotionStatusP2);
        if (st >= 0 && st <= 8)
            ++p.winStatusHist[st];
        if (st == 7 || st == 8)
        {
            needIssue = true;
            p.mountTaskActive = false;
        }
        else if (st == 1)
        {
            const float es = GetEntitySpeedNative(horse);
            if (p.hostSpeedInterp > 0.5f &&
                es < 0.15f * p.hostSpeedInterp && distGoal > kStuckDistE72)
            {
                if (++p.stuckTicks >= kStuckConfirmTicks)
                {
                    needIssue = true;
                    p.mountTaskActive = false;
                    p.stuckTicks = 0;
                    ++g_motionStuck;
                }
            }
            else
            {
                p.stuckTicks = 0;
            }
        }
        else
        {
            p.stuckTicks = 0;
        }
    }
    if (needIssue)
    {
        FollowNavmesh(horse, gx, gy, p.hostZ, mbr, kFollowHeadingSentinel,
                      stopping);
        p.mountTaskHash = natives::SCRIPT_TASK_FOLLOW_NAV_MESH_TO_COORD;
        p.mountTaskActive = true;
        p.mountLastStatusTick = tick;
        p.mountLastUpdateTick = tick;
        p.mountMoveTarget = Vec3{gx, gy, p.hostZ};
        p.issuedGoalX = gx;
        p.issuedGoalY = gy;
        p.issuedGoalValid = true;
        p.stuckTicks = 0;
        ++g_motionIssued;
        if (wasActive)
        {
            ++p.winReissue;
            ++g_motionReissue;
        }
    }
    else if (coopcfg::Get().npcMotionUpdate && p.updateNativeOk &&
             tick - p.mountLastUpdateTick >= kUpdateIntervalE72)
    {
        const float sx = gx - p.issuedGoalX;
        const float sy = gy - p.issuedGoalY;
        if (sx * sx + sy * sy >= kUpdateMinShift * kUpdateMinShift)
        {
            UpdateTaskGoToCoord(horse, gx, gy, p.hostZ, mbr, stopping);
            p.issuedGoalX = gx;
            p.issuedGoalY = gy;
            p.mountMoveTarget = Vec3{gx, gy, p.hostZ};
            p.mountLastUpdateTick = tick;
            ++g_motionUpdateNative;
            ++p.winUpdate;
        }
    }
}

void MoveProxy(Proxy& p)
{
    if (!DoesExist(p.handle))
    {
        // E2b: rider пропал — лошадь не бросаем.
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
        return;
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

    // E3d-c (v19): occupant сидит в vehicle / draft-лошадь прицеплена —
    // позицию/анимацию ведёт vehicle/attachment, MoveProxy не вмешивается.
    if (p.seatedInVehicle || p.draftAttached)
        return;
    // E3a: сценарий-поза — движение не выдаём.
    if (p.scenarioFlag)
        return;
    // E3b.3: бой (aim/shoot/reload) приоритетнее движения.
    if (p.weaponAimFlag || p.weaponShootPulse || p.weaponReloadFlag)
        return;

    bool havePose = false;
    UpdateHostInterpolation(p, havePose);
    if (!havePose)
        return;
    const Vec3 cur = GetCoords(p.handle);

    // E7.2: нет кадров >2 с — snap в последнюю известную точку.
    const unsigned long long nowMs = GetTickCount64();
    if (p.lastFrameMs != 0 && nowMs - p.lastFrameMs > kNoFramesSnapMs)
    {
        const float dist = std::sqrt((p.tx - cur.x) * (p.tx - cur.x) +
                                     (p.ty - cur.y) * (p.ty - cur.y));
        SetCoordsNative(p.handle, p.tx, p.ty, p.tz);
        SetHeadingNative(p.handle, p.heading);
        p.taskActive = false;
        p.issuedGoalValid = false;
        ++p.winSnap;
        ++g_motionSnap;
        LogTeleportE72(p.actorId, dist);
        return;
    }

    const float dx = p.hostX - cur.x;
    const float dy = p.hostY - cur.y;
    const float dz = p.hostZ - cur.z;
    const float dist = std::sqrt(dx * dx + dy * dy);
    if (dist > kTeleportDistE72 || std::fabs(dz) > kTeleportDzE72)
    {
        SetCoordsNative(p.handle, p.hostX, p.hostY, p.hostZ);
        SetHeadingNative(p.handle, p.hostHeading);
        p.moveTaskValid = false;
        p.taskActive = false;
        p.issuedGoalValid = false;
        ++p.winSnap;
        ++g_motionSnap;
        LogTeleportE72(p.actorId, dist);
        return;
    }

    g_motionErr.AddUs(static_cast<unsigned long long>(dist * 1000.0f));
    p.winErr.AddUs(static_cast<unsigned long long>(dist * 1000.0f));

    // E7.2: стоп/старт гистерезис (0.25-0.3 с / 0.2 с).
    const bool hostSlow = p.hostSpeedInterp < kStopSpeedE72;
    if (hostSlow && dist < kStopDistE72)
    {
        ++p.stopTicks;
        p.moveTicks = 0;
    }
    else
    {
        p.stopTicks = 0;
        if (p.hostSpeedInterp > kResumeSpeedE72)
            ++p.moveTicks;
        else
            p.moveTicks = 0;
    }
    if (p.stopTicks >= kStopConfirmTicks)
    {
        if (!p.idleHeld)
        {
            SetDesiredMoveBlend(p.handle, 0.0f);
            StandStill(p.handle);
            SetMinMoveBlend(p.handle, 0.0f);
            p.lastMinMbr = 0.f;
            p.taskActive = false;
            p.moveTaskValid = false;
            p.issuedGoalValid = false;
            p.lastMbr = 0.f;
            p.idleHeld = true;
        }
        // idle: heading — руками, по интерполированному host heading.
        float norm = p.hostHeading - GetHeading(p.handle);
        while (norm > 180.f) norm -= 360.f;
        while (norm < -180.f) norm += 360.f;
        if (std::fabs(norm) > kHeadingDeadzone)
            SetHeadingNative(p.handle, p.hostHeading);
        return;
    }
    if (p.idleHeld && p.hostSpeedInterp > kResumeSpeedE72 &&
        p.moveTicks >= kResumeConfirmTicks)
        p.idleHeld = false;

    float gx = 0.f, gy = 0.f;
    ComputeFarGoal(p, false, cur.x, cur.y, gx, gy);
    const float gdx = gx - cur.x;
    const float gdy = gy - cur.y;
    const float distGoal = std::sqrt(gdx * gdx + gdy * gdy);

    // MBR: targetSpeed = hostSpeed + k*(distErr), затем точный MBR-натив.
    float targetSpeed =
        p.hostSpeedInterp + std::max(0.f, distGoal - kHumanLeadMin) * 0.8f;
    if (targetSpeed > 8.f) targetSpeed = 8.f;
    float mbr = ComputeMoveBlendForSpeed(p.handle, targetSpeed);
    if (p.hostSpeedInterp > 0.3f && mbr < 1.f) mbr = 1.f;
    if (!p.maxBlendSet)
    {
        SetMaxMoveBlend(p.handle, 3.0f);
        p.maxBlendSet = true;
    }
    if (std::fabs(mbr - p.lastMbr) > kMbrHysteresis)
    {
        SetDesiredMoveBlend(p.handle, mbr);
        p.lastMbr = mbr;
        ++g_motionGaitRestart;
        ++p.winGaitRestart;
    }
    // При догоне — min MBR > 0, чтобы пед реально бежал, а не «шагал».
    const float minMbr = (distGoal > 10.f && p.hostSpeedInterp > 1.f) ? 0.5f : 0.f;
    if (std::fabs(minMbr - p.lastMinMbr) > 0.05f)
    {
        SetMinMoveBlend(p.handle, minMbr);
        p.lastMinMbr = minMbr;
    }

    const float stopping = 0.05f; // E7.2: 0.05-0.1
    const bool markerMode = coopcfg::Get().npcMotionMarker &&
                            (!p.updateNativeOk ||
                             !coopcfg::Get().npcMotionUpdate);
    if (markerMode)
    {
        const unsigned int marker = EnsureMarkerPed(p, gx, gy, p.hostZ);
        if (marker)
        {
            bool needIssue = !p.taskActive;
            const bool wasActive = p.taskActive;
            if (!needIssue && tickNow - p.lastStatusTickE72 >= kStatusIntervalE72)
            {
                p.lastStatusTickE72 = tickNow;
                const int st = GetScriptTaskStatus(
                    p.handle, p.taskHash, coopcfg::Get().npcMotionStatusP2);
                if (st >= 0 && st <= 8)
                    ++p.winStatusHist[st];
                if (st == 7 || st == 8)
                {
                    needIssue = true;
                    p.taskActive = false;
                }
            }
            if (needIssue)
            {
                IssueMarkerFollow(p.handle, marker, mbr, 0.1f);
                p.taskHash = natives::SCRIPT_TASK_FOLLOW_TO_OFFSET_OF_ENTITY;
                p.taskActive = true;
                p.taskIssuedTick = tickNow;
                p.lastUpdateTickE72 = tickNow;
                p.lastStatusTickE72 = tickNow;
                p.issuedGoalX = gx;
                p.issuedGoalY = gy;
                p.issuedGoalValid = true;
                ++g_motionIssued;
                if (wasActive)
                {
                    ++p.winReissue;
                    ++g_motionReissue;
                }
            }
            else if (tickNow - p.lastUpdateTickE72 >= kUpdateIntervalE72)
            {
                const float sx = gx - p.issuedGoalX;
                const float sy = gy - p.issuedGoalY;
                if (sx * sx + sy * sy >= kUpdateMinShift * kUpdateMinShift)
                {
                    MoveMarker(p, gx, gy, p.hostZ);
                    p.issuedGoalX = gx;
                    p.issuedGoalY = gy;
                    p.lastUpdateTickE72 = tickNow;
                    ++p.winUpdate;
                }
            }
            return;
        }
    }

    // A: одна задача на фазу; reissue только по статусу 7/8 или stuck.
    bool needIssue = !p.taskActive;
    const bool wasActive = p.taskActive;
    if (!needIssue && tickNow - p.lastStatusTickE72 >= kStatusIntervalE72)
    {
        p.lastStatusTickE72 = tickNow;
        const int st = GetScriptTaskStatus(p.handle, p.taskHash,
                                           coopcfg::Get().npcMotionStatusP2);
        if (st >= 0 && st <= 8)
            ++p.winStatusHist[st];
        if (st == 7 || st == 8)
        {
            needIssue = true;
            p.taskActive = false;
        }
        else if (st == 1)
        {
            // stuck: задача «performing», но пед стоит при движущемся хосте.
            const float es = GetEntitySpeedNative(p.handle);
            if (p.hostSpeedInterp > 0.5f &&
                es < 0.15f * p.hostSpeedInterp && distGoal > kStuckDistE72)
            {
                if (++p.stuckTicks >= kStuckConfirmTicks)
                {
                    needIssue = true;
                    p.taskActive = false;
                    p.stuckTicks = 0;
                    ++g_motionStuck;
                }
            }
            else
            {
                p.stuckTicks = 0;
            }
        }
        else
        {
            p.stuckTicks = 0;
        }
    }
    if (needIssue)
    {
        FollowNavmesh(p.handle, gx, gy, p.hostZ, mbr, kFollowHeadingSentinel,
                      stopping);
        p.taskHash = natives::SCRIPT_TASK_FOLLOW_NAV_MESH_TO_COORD;
        p.taskActive = true;
        p.taskIssuedTick = tickNow;
        p.lastStatusTickE72 = tickNow;
        p.lastUpdateTickE72 = tickNow;
        p.moveTarget = Vec3{gx, gy, p.hostZ};
        p.moveTaskValid = true;
        p.issuedGoalX = gx;
        p.issuedGoalY = gy;
        p.issuedGoalValid = true;
        p.stuckTicks = 0;
        ++g_motionIssued;
        if (wasActive)
        {
            ++p.winReissue;
            ++g_motionReissue;
        }
    }
    else if (coopcfg::Get().npcMotionUpdate && p.updateNativeOk &&
             tickNow - p.lastUpdateTickE72 >= kUpdateIntervalE72)
    {
        const float sx = gx - p.issuedGoalX;
        const float sy = gy - p.issuedGoalY;
        if (sx * sx + sy * sy >= kUpdateMinShift * kUpdateMinShift)
        {
            UpdateTaskGoToCoord(p.handle, gx, gy, p.hostZ, mbr, stopping);
            p.issuedGoalX = gx;
            p.issuedGoalY = gy;
            p.moveTarget = Vec3{gx, gy, p.hostZ};
            p.lastUpdateTickE72 = tickNow;
            ++g_motionUpdateNative;
            ++p.winUpdate;
        }
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
    p.scenarioSpawnValid = false;
    if (p.scenarioPoint)
    {
        DeleteScenarioPoint(p.scenarioPoint);
        p.scenarioPoint = 0;
        p.scenarioPointType = 0;
    }
    SetInvincible(p.handle, false);
    SetEntityHealthValue(p.handle, 0);
    // E3b: смерть — сброс боевых латчей/задач (оружие остаётся выданным).
    p.weaponInHands = false;
    p.weaponDrawPending = false;
    p.weaponHolsterPending = false;
    p.weaponAimTaskActive = false;
    p.aimPointValid = false;
    p.reloadInProgress = false;
    p.aimCaptured = false;
    p.prevAimFlag = false;
    p.equipWaitActive = false;
    p.combatStance = false;
    p.shootTaskActive = false;
    p.firePhase = 0;
    p.fireShotWorked = false;
    p.deathApplied = true;
    if (p.kind == net::kActorKindHuman)
        LogDeath(p.actorId);
    else
        LogAnimalDeath(p.actorId);
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
    // E3b: revive — восстановить оружие по weaponHash (выдадим заново) и
    // сбросить боевые латчи; Task-задачи перевыдаст ProcessActorWeapons.
    if (IsWeaponHashUsable(p.weaponHash))
        p.weaponGiven = 0;
    p.weaponInHands = false;
    p.weaponDrawPending = false;
    p.weaponHolsterPending = false;
    p.weaponAimTaskActive = false;
    p.aimPointValid = false;
    p.reloadInProgress = false;
    p.aimCaptured = false;
    p.prevAimFlag = false;
    p.equipWaitActive = false;
    p.weaponGiveRet = 0;
    p.weaponGiveAttempts = 0;
    p.weaponGiveFailLogged = false;
    p.weaponFailLogged = false;
    p.weaponDrawnUnverified = false;
    p.combatStance = false;
    p.shootTaskActive = false;
    p.firePhase = 0;
    p.fireShotWorked = false;
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
    // E3a.4: ближайший ДРУГОЙ прокси со сценарием (pending/active) в радиусе
    // kScenarioSpacingMeters. Детерминизм: выигрывает меньший actorId (map
    // обходится по возрастанию, поэтому он применяет первым).
    auto findNearbyScenarioOther = [&](const Proxy& self,
                                       float& outDist) -> uint32_t {
        outDist = 0.f;
        const Vec3 selfPos = self.scenarioSpawnValid
            ? Vec3{self.scenarioSpawnX, self.scenarioSpawnY, self.scenarioSpawnZ}
            : GetCoords(self.handle);
        uint32_t blocker = 0;
        float bestDist = 1e30f;
        for (auto& okv : g_proxies)
        {
            const Proxy& o = okv.second;
            if (&o == &self) continue;
            // Детерминизм: выигрывает тот, кто начал сценарий РАНЬШЕ; при
            // равном времени (тот же тик) — меньший actorId.
            const bool startedEarlier =
                o.scenarioTick != 0 && o.scenarioTick < tick;
            const bool sameTickSmaller =
                o.scenarioTick == tick && o.actorId < self.actorId;
            if (!startedEarlier && !sameTickSmaller) continue;
            if (!o.scenarioFlag || o.dead || o.mounted) continue;
            if (o.scenarioState != 1 && o.scenarioState != 2) continue;
            Vec3 op;
            if (o.scenarioSpawnValid)
                op = Vec3{o.scenarioSpawnX, o.scenarioSpawnY, o.scenarioSpawnZ};
            else if (o.handle && DoesExist(o.handle))
                op = GetCoords(o.handle);
            else
                continue;
            const float dx = selfPos.x - op.x;
            const float dy = selfPos.y - op.y;
            const float dz = selfPos.z - op.z;
            const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
            if (d < kScenarioSpacingMeters && d < bestDist)
            {
                bestDist = d;
                blocker = o.actorId;
            }
        }
        if (blocker)
            outDist = bestDist;
        return blocker;
    };

    for (auto& kv : g_proxies)
    {
        Proxy& p = kv.second;
        if (p.kind != net::kActorKindHuman)
            continue; // E3c: животные без сценариев
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
            p.scenarioSpawnValid = false;
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
                LogScenarioApply(p.actorId, 0, false);
                p.scenarioState = 5;
            }
            continue;
        }

        // Первый запуск / смена хэша: pre-check + ровно один in-place вызов.
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

            // E3a.4: запоминаем spawn-позицию для warp-guard и spacing-check.
            const Vec3 pos = GetCoords(p.handle);
            p.scenarioSpawnX = pos.x;
            p.scenarioSpawnY = pos.y;
            p.scenarioSpawnZ = pos.z;
            p.scenarioSpawnHeading = GetHeading(p.handle);
            p.scenarioSpawnValid = true;

            // PRE-CHECK: не сажаем в позу рядом с другим сценарным прокси —
            // иначе движок сварпает обоих к одной scenario point.
            float blockDist = 0.f;
            const uint32_t blocker = findNearbyScenarioOther(p, blockDist);
            if (blocker)
            {
                LogScenarioSkip(p.actorId, blocker, blockDist);
                p.scenarioState = 6; // skipped: стоим на spawn, поза не нужна
                p.scenarioApplied = p.scenarioHash;
                p.scenarioTick = tick;
                continue;
            }

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
                // E3a.4 POST-CHECK: если движок увёл педа к scenario point
                // (поза in-place часто варпает к ближайшей курилке/стулу) —
                // вернуть на spawn-позицию и не двигать, пока флаг активен.
                const Vec3 cur = GetCoords(p.handle);
                float warpDist = 0.f;
                if (p.scenarioSpawnValid)
                {
                    const float ddx = cur.x - p.scenarioSpawnX;
                    const float ddy = cur.y - p.scenarioSpawnY;
                    const float ddz = cur.z - p.scenarioSpawnZ;
                    warpDist = std::sqrt(ddx * ddx + ddy * ddy + ddz * ddz);
                }
                if (p.scenarioSpawnValid &&
                    warpDist > kScenarioWarpRevertMeters)
                {
                    StandStill(p.handle);
                    SetMoveBlend(p.handle, 0.0f);
                    SetCoordsNative(p.handle, p.scenarioSpawnX, p.scenarioSpawnY,
                                    p.scenarioSpawnZ);
                    SetHeadingNative(p.handle, p.scenarioSpawnHeading);
                    p.taskActive = false;
                    p.moveTaskValid = false;
                    p.scenarioState = 6; // reverted: стоим на spawn, поза снята
                    LogScenarioReverted(p.actorId, warpDist);
                }
                else
                {
                    // Оптимистичное подтверждение: поза уже играет (verify для
                    // части сценариев врёт). state=ok в любом случае.
                    const bool flagOk = IsPedInScenario(p.handle);
                    p.scenarioState = 2;
                    LogScenarioApply(p.actorId, p.scenarioHash, true,
                                     flagOk ? "flag" : "assumed");
                }
            }
            continue;
        }

        // E3a.3/E3a.4: fallback-цепочка (point/at-position) НЕ используется
        // в рабочей цепочке — оставлена заготовкой под kScenarioFallbackEnabled.
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
        // state 2 (ok/поза), 5 (failed), 6 (skipped/reverted): не двигаем.
    }
}

// E2a.2: трейл движения (последние ~1 с позиций) — по нему сглаживаем
// направление и ведём прокси по фактическому маршруту хоста без срезания углов.
// E3b: боевые визуалы NPC. Не трогаем dead/mounted/сценарий; не более
// 2 task-вызовов на актора за тик. Арности повторяют рабочий player-ghost
// из coop.cpp (GIVE/equip/aim/shoot/reload проверены в игре).
void ProcessActorWeapons(unsigned int tick)
{
    for (auto& kv : g_proxies)
    {
        Proxy& p = kv.second;
        if (!p.handle || !DoesExist(p.handle))
            continue;
        if (p.kind != net::kActorKindHuman)
            continue; // E3c: у животных нет оружия/боя
        if (p.dead || p.mounted || p.scenarioFlag)
        {
            // Труп/седло/поза — никаких боевых задач и латчей.
            p.aimCaptured = false;
            p.weaponAimTaskActive = false;
            p.prevAimFlag = false;
            p.firePhase = 0;
            p.shootTaskActive = false;
            continue;
        }
        int taskCalls = 0;

        const uint32_t wantWeapon =
            IsWeaponHashUsable(p.weaponHash) ? p.weaponHash : 0;

        // 1) Выдать/сменить/убрать оружие (E3b.2: ret GIVE + 1 retry).
        if (wantWeapon != p.weaponGiven)
        {
            if (p.weaponGiven)
            {
                RemoveWeaponFromPedEntity(p.handle, p.weaponGiven);
                p.weaponGiven = 0;
                p.weaponInHands = false;
                p.aimCaptured = false;
                p.weaponAimTaskActive = false;
                p.equipWaitActive = false;
                p.weaponGiveFailLogged = false;
                p.weaponFailLogged = false;
                p.firePhase = 0;
            }
            if (wantWeapon)
            {
                p.weaponGiveRet = GiveWeaponToPedEntity(p.handle, wantWeapon);
                p.weaponGiveAttempts = 1;
                p.weaponGiveTick = tick;
                SetPedInfiniteAmmoEntity(p.handle, wantWeapon);
                SetPedInfiniteClipEntity(p.handle, true);
                p.weaponGiven = wantWeapon;
                LogWeaponGive(p.actorId, wantWeapon, p.weaponGiveRet, 999);
            }
        }
        else if (wantWeapon && p.weaponGiveAttempts == 1 &&
                 p.weaponGiveRet == 0 && !p.weaponGiveFailLogged &&
                 (tick - p.weaponGiveTick) >= 30)
        {
            p.weaponGiveRet = GiveWeaponToPedEntity(p.handle, wantWeapon);
            p.weaponGiveAttempts = 2;
            p.weaponGiveTick = tick;
            LogWeaponGive(p.actorId, wantWeapon, p.weaponGiveRet, 999);
            if (p.weaponGiveRet == 0)
            {
                p.weaponGiveFailLogged = true;
                LogWeaponFail(p.actorId, wantWeapon, p.weaponGiveRet);
            }
        }
        if (!p.weaponGiven)
        {
            p.weaponInHands = false;
            p.aimCaptured = false;
            p.prevAimFlag = false;
            p.firePhase = 0;
            continue;
        }

        // боевые желания
        const bool wantShoot =
            p.weaponShootPulse || (p.weaponAimFlag && p.weaponCombatFlag);
        const bool wantAim = p.weaponAimFlag || p.weaponShootPulse;
        const bool wantReload = p.weaponReloadFlag;

        // E3b.3: при входе в бой — StandStill один раз; MoveProxy в это время
        // не выдаёт TASK_FOLLOW.
        const bool combatActive = wantAim || wantShoot || wantReload;
        if (combatActive && !p.combatStance)
        {
            StandStill(p.handle);
            p.taskActive = false;
            p.moveTaskValid = false;
            p.combatStance = true;
            p.combatStanceStartTick = tick;
            ++taskCalls;
        }
        else if (!combatActive && p.combatStance)
        {
            p.combatStance = false;
            p.aimCaptured = false;
            p.weaponAimTaskActive = false;
            p.taskActive = false;
            p.moveTaskValid = false;
        }

        // 2) DRAW не гейтится готовностью: SET_CURRENT(equipNow=true) всегда
        // при Drawn/aim/shoot/reload; drawn при armed, иначе через 2 с
        // drawn(unverified).
        const bool wantDraw =
            p.weaponDrawnFlag || wantAim || wantShoot || wantReload;
        if (wantDraw && !p.weaponInHands)
        {
            SetCurrentPedWeaponEntity(p.handle, p.weaponGiven, true);
            if (!p.equipWaitActive)
            {
                p.equipWaitActive = true;
                p.equipWaitStartTick = tick;
            }
            if (!p.weaponDrawnUnverified)
            {
                p.weaponDrawnUnverified = true;
                p.weaponEquipDemandTick = tick;
            }
            const uint32_t curNow = GetCurrentWeapon(p.handle);
            const uint32_t heldNow = GetHeldWeapon(p.handle);
            const bool armedNow =
                IsPedArmedEntity(p.handle) || curNow == p.weaponGiven ||
                (heldNow != 0 &&
                 heldNow != static_cast<uint32_t>(natives::WEAPON_UNARMED));
            if (armedNow)
            {
                p.weaponInHands = true;
                p.equipWaitActive = false;
                p.weaponDrawnUnverified = false;
                p.weaponFailLogged = false;
                LogWeaponEvent(p.actorId, p.weaponGiven, "drawn");
            }
            else if (tick - p.weaponEquipDemandTick > 120)
            {
                p.weaponInHands = true;
                p.weaponDrawnUnverified = false;
                LogWeaponEvent(p.actorId, p.weaponGiven, "drawn(unverified)");
            }
        }
        else if (!wantDraw && p.weaponInHands)
        {
            HidePedWeaponsEntity(p.handle);
            p.weaponInHands = false;
            p.aimCaptured = false;
            p.weaponAimTaskActive = false;
            p.equipWaitActive = false;
            p.weaponDrawnUnverified = false;
            LogWeaponEvent(p.actorId, p.weaponGiven, "holstered");
        }

        // 3) Мягкий armed (E3b.2).
        const uint32_t cur = GetCurrentWeapon(p.handle);
        const uint32_t held = GetHeldWeapon(p.handle);
        const bool heldValid =
            held != 0 && held != static_cast<uint32_t>(natives::WEAPON_UNARMED);
        const bool armed = IsPedArmedEntity(p.handle) ||
                           cur == p.weaponGiven || heldValid;
        const bool ready = armed && (IsWeaponReady(p.handle) || heldValid);
        if (armed)
        {
            p.weaponInHands = true;
            p.weaponFailLogged = false;
        }

        // Ждём armed до 2 с; если так и не появился — один лог fail.
        if ((wantAim || wantShoot || wantReload) && !armed &&
            !p.equipWaitActive)
        {
            SetCurrentPedWeaponEntity(p.handle, p.weaponGiven, true);
            p.equipWaitActive = true;
            p.equipWaitStartTick = tick;
        }
        const bool equipExpired =
            p.equipWaitActive && (tick - p.equipWaitStartTick) > 120;
        if ((wantAim || wantShoot || wantReload) && !armed && equipExpired &&
            !p.weaponFailLogged)
        {
            p.weaponFailLogged = true;
            LogWeaponFail(p.actorId, p.weaponGiven, p.weaponGiveRet);
        }

        // ================= E3b.4: continuous fire =================
        // aim -> длинная очередь (1200 мс) -> ждать окончание -> aim -> ...
        // Если длинная очередь не стреляет — fallback: 200 мс + пауза
        // 350-500 мс, каждый цикл через переприцеливание.
        const bool fireCycle = wantShoot && armed && !wantReload &&
                               !p.reloadInProgress;
        if (fireCycle)
        {
            // В shoot-окне держим бесконечный клип и патроны.
            SetPedInfiniteClipEntity(p.handle, true);
            if (GetAmmoInClipEntity(p.handle, p.weaponGiven) <= 0)
                SetAmmoInClipEntity(p.handle, p.weaponGiven, 999);

            const unsigned int phaseAge = tick - p.firePhaseTick;
            if (p.firePhase == 0)
            {
                // Переприцеливание перед очередью (каждый цикл).
                if (phaseAge >= 3 && taskCalls < 2)
                {
                    const Vec3 pos = GetCoords(p.handle);
                    const float rad = p.heading * kRadPerDeg;
                    p.aimCaptureX = pos.x - std::sin(rad) * kWeaponAimDistance;
                    p.aimCaptureY = pos.y + std::cos(rad) * kWeaponAimDistance;
                    p.aimCaptureZ = pos.z;
                    p.aimCaptureHeading = p.heading;
                    p.aimWeapon = p.weaponGiven;
                    p.aimCaptured = true;
                    p.aimCaptureTick = tick;
                    TaskAimGunAtCoordEntity(p.handle, p.aimCaptureX,
                                            p.aimCaptureY, p.aimCaptureZ);
                    p.weaponAimTaskActive = true;
                    p.lastAimTaskTick = tick;
                    p.firePhase = 1;
                    p.firePhaseTick = tick;
                    ++taskCalls;
                }
            }
            else if (p.firePhase == 1)
            {
                // Дать цель-задаче осесть, затем одна длинная shoot-задача.
                if (phaseAge >= 3 && taskCalls < 2)
                {
                    if (IsWeaponReady(p.handle))
                    {
                        p.fireAmmoBefore = GetAmmoInClipEntity(
                            p.handle, p.weaponGiven);
                        TaskShootAtCoordEntity(
                            p.handle, p.aimCaptureX, p.aimCaptureY,
                            p.aimCaptureZ, p.fireUseFallback ? 200 : 1200);
                        ++p.shootTaskCount;
                        p.shootTaskActive = true;
                        p.lastShootTick = tick;
                        p.fireShotWorked = false;
                        p.weaponAimTaskActive = false;
                        p.firePhase = 2;
                        p.firePhaseTick = tick;
                        ++taskCalls;
                        LogWeaponEvent(p.actorId, p.weaponGiven, "shoot");
                    }
                    else if (phaseAge > 90)
                    {
                        p.firePhase = 0;
                        p.firePhaseTick = tick;
                    }
                }
            }
            else // firePhase == 2: ждём окончания очереди
            {
                if (IsShootingPed(p.handle))
                    p.fireShotWorked = true;
                const int ammoNow =
                    GetAmmoInClipEntity(p.handle, p.weaponGiven);
                if (p.fireAmmoBefore >= 0 && ammoNow < p.fireAmmoBefore)
                    p.fireShotWorked = true;

                if (p.fireUseFallback)
                {
                    // Fallback: 200 мс выстрел, пауза 350-500 мс.
                    if ((phaseAge >= 21 && !IsShootingPed(p.handle) &&
                         IsWeaponReady(p.handle)) ||
                        phaseAge >= 30)
                    {
                        p.firePhase = 0;
                        p.firePhaseTick = tick;
                    }
                }
                else
                {
                    // Длинная очередь: ждём 1.2-1.3 с / окончание стрельбы.
                    if (phaseAge >= 78 ||
                        (phaseAge >= 24 && !IsShootingPed(p.handle) &&
                         IsWeaponReady(p.handle)))
                    {
                        if (!p.fireShotWorked && phaseAge >= 24)
                            p.fireUseFallback = true; // длинная очередь не пошла
                        p.firePhase = 0;
                        p.firePhaseTick = tick;
                    }
                }
            }
        }
        else if (!wantShoot)
        {
            p.firePhase = 0;
            p.shootTaskActive = false;
        }

        // 4) AIM для aim-эпизодов без стрельбы (fire-cycle управляет сам).
        const bool aimEdge = wantAim && !p.prevAimFlag;
        const bool aimCooldownOk =
            p.lastAimTaskTick == 0 || (tick - p.lastAimTaskTick) >= 300;
        if (!fireCycle && aimEdge && armed && aimCooldownOk && taskCalls < 2)
        {
            const Vec3 pos = GetCoords(p.handle);
            const float rad = p.heading * kRadPerDeg;
            p.aimCaptureX = pos.x - std::sin(rad) * kWeaponAimDistance;
            p.aimCaptureY = pos.y + std::cos(rad) * kWeaponAimDistance;
            p.aimCaptureZ = pos.z;
            p.aimCaptureHeading = p.heading;
            p.aimWeapon = p.weaponGiven;
            p.aimCaptured = true;
            p.aimCaptureTick = tick;
            p.weaponAimTaskActive = true;
            p.lastAimTaskTick = tick;
            TaskAimGunAtCoordEntity(p.handle, p.aimCaptureX, p.aimCaptureY,
                                    p.aimCaptureZ);
            ++taskCalls;
            LogWeaponEvent(p.actorId, p.weaponGiven, "aim");
        }
        else if (!fireCycle && wantAim && p.aimCaptured && armed &&
                 aimCooldownOk && taskCalls < 2)
        {
            float dHeading = std::fabs(p.heading - p.aimCaptureHeading);
            while (dHeading > 180.f) dHeading = std::fabs(dHeading - 360.f);
            const bool weaponChanged = p.aimWeapon != p.weaponGiven;
            const bool headingChanged = dHeading > 25.f;
            const bool timed = (tick - p.aimCaptureTick) > 300; // 5 с
            const bool needReissue = !p.weaponAimTaskActive ||
                                     weaponChanged || headingChanged || timed;
            if (needReissue)
            {
                if (weaponChanged || headingChanged || timed)
                {
                    const Vec3 pos = GetCoords(p.handle);
                    const float rad = p.heading * kRadPerDeg;
                    p.aimCaptureX = pos.x - std::sin(rad) * kWeaponAimDistance;
                    p.aimCaptureY = pos.y + std::cos(rad) * kWeaponAimDistance;
                    p.aimCaptureZ = pos.z;
                    p.aimCaptureHeading = p.heading;
                    p.aimWeapon = p.weaponGiven;
                    p.aimCaptureTick = tick;
                }
                TaskAimGunAtCoordEntity(p.handle, p.aimCaptureX, p.aimCaptureY,
                                        p.aimCaptureZ);
                p.weaponAimTaskActive = true;
                p.lastAimTaskTick = tick;
                ++taskCalls;
            }
        }
        else if (!wantAim && p.weaponAimTaskActive)
        {
            StandStill(p.handle);
            p.weaponAimTaskActive = false;
            p.taskActive = false;
            p.moveTaskValid = false;
            ++taskCalls;
        }
        if (!wantAim)
        {
            p.aimCaptured = false;
            p.weaponAimTaskActive = false;
        }

        // 5) RELOAD (E3b.3): резерв 999 -> infinite clip off -> ammo 0 ->
        // TASK_RELOAD; вернуть через 2 с/снятия флага.
        if (wantReload && armed && taskCalls < 2 && !p.reloadInProgress)
        {
            SetPedAmmoEntity(p.handle, p.weaponGiven, 999);
            SetPedInfiniteClipEntity(p.handle, false);
            SetAmmoInClipEntity(p.handle, p.weaponGiven, 0);
            TaskReloadWeaponEntity(p.handle);
            p.reloadInProgress = true;
            p.reloadStartTick = tick;
            ++taskCalls;
            LogWeaponEvent(p.actorId, p.weaponGiven, "reload");
        }
        else if (p.reloadInProgress &&
                 (!wantReload ||
                  tick - p.reloadStartTick >= kWeaponReloadTimeoutTicks))
        {
            SetPedAmmoEntity(p.handle, p.weaponGiven, 999);
            SetPedInfiniteClipEntity(p.handle, true);
            p.reloadInProgress = false;
        }
        if (p.shootTaskActive && (tick - p.lastShootTick) > 30)
            p.shootTaskActive = false;
        if (equipExpired)
            p.equipWaitActive = false; // следующий retry на след. тике

        // 6) Диагностика движка: 1 строка / 5 с при npc_debug=1.
        if (coopcfg::Get().npcDebug && (tick - p.lastWeaponDbgTick) >= 300)
        {
            p.lastWeaponDbgTick = tick;
            const int ammo = GetAmmoInClipEntity(p.handle, p.weaponGiven);
            const bool moving = p.moveTaskValid && !p.combatStance;
            LogWeaponDebug(p.actorId, ready, armed, cur, held, p.weaponGiven,
                           ammo, p.weaponInHands, p.weaponAimTaskActive,
                           p.shootTaskActive, moving, p.shootTaskCount);
        }

        p.prevAimFlag = wantAim;
    }
}


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
            // E2c: поздний outfit с heartbeat — если metaOutfit изменился
            // (0→hash или hash→другой), повторить appearance-применение (E2a).
            if (!p.dead && s.metaOutfit != 0 &&
                s.metaOutfit != p.metaOutfit)
            {
                if (p.outfitRequest)
                {
                    ReleaseMetaPedOutfitRequest(p.outfitRequest);
                    p.outfitRequest = 0;
                }
                p.metaOutfit = s.metaOutfit;
                p.appearanceState = 0; // ProcessActorAppearance запросит заново
                p.outfitRequestTick = 0;
                p.appearanceReapply = true;
            }
            // E3b: heartbeat обновляет оружие (hash сменился/убрали) —
            // перевыдадим в ProcessActorWeapons.
            if (!p.dead && s.weaponHash != p.weaponHash)
            {
                p.weaponHash = s.weaponHash;
                p.weaponGiven = 0;
                p.weaponInHands = false;
                p.weaponDrawPending = false;
                p.weaponHolsterPending = false;
                p.weaponAimTaskActive = false;
                p.reloadInProgress = false;
                p.weaponGiveRet = 0;
                p.weaponGiveAttempts = 0;
                p.weaponGiveFailLogged = false;
                p.weaponFailLogged = false;
                p.weaponDrawnUnverified = false;
                p.combatStance = false;
                p.shootTaskActive = false;
                p.firePhase = 0;
                p.fireShotWorked = false;
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
    g_pendingAppearanceAssets.clear(); // E2c-b (v16)
    g_latestFrames.clear();
    g_haveFrames = false;
    g_guestDespawnQueue.clear();
    g_guestEpoch = newEpoch;
}

void GuestResetState()
{
    g_occupantHandles.clear();
    g_guestEpoch = 0;
    g_pendingSpawns.clear();
    g_pendingSpawnIds.clear();
    g_pendingAppearanceAssets.clear(); // E2c-b (v16)
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
    const bool perfOn = coopcfg::Get().npcDebug;

    // 1) inbox: spawn/despawn/frames. Под мьютексом — ТОЛЬКО копирование;
    // нативы (в т.ч. удаление прокси при смене epoch) — после unlock.
    std::vector<net::ActorSpawn> spawns;
    bool haveSpawn = false;
    uint32_t spawnEpoch = 0;
    std::vector<net::ActorFrame> frames;
    bool haveFrames = false;
    uint32_t framesEpoch = 0;
    std::deque<net::ActorDespawnPayload> despawns;
    std::vector<AppearanceMsg> appearanceMsgs; // E2c-b (v16)
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
            g_latestFramesRecvMs = g_inbox.framesRecvMs;
            g_inbox.haveFrames = false;
            haveFrames = true;
        }
        despawns.swap(g_inbox.despawns);
        appearanceMsgs.swap(g_inbox.appearances);
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
            ++g_perfGuestDespawns;
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
    const unsigned long long perfSpawnStart = perfOn ? QpcNow() : 0;
    for (auto it = g_pendingSpawns.begin();
         it != g_pendingSpawns.end() && spawned < kMaxSpawnsPerTick;)
    {
        const net::ActorSpawn& s = it->spawn;
        if (tick - it->firstSeenTick > kPendingSpawnTimeoutTicks)
        {
            LogSpawnTimeout(s.actorId);
            ++g_perfGuestTimeouts;
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
    if (perfOn)
        g_perfGuestSpawn.AddUs(QpcDeltaUs(perfSpawnStart));

    // 3b-pre) E2c-b (v16): asset/tint из inbox -> прокси/pending. Asset-данные
    // приоритетнее metaOutfit: при их наличии E2a больше не применяем.
    if (coopcfg::Get().npcAppearance)
    {
        for (AppearanceMsg& m : appearanceMsgs)
        {
            if (m.actorId == 0 || m.entries.empty())
                continue;
            if (m.epoch != 0 && g_guestEpoch != 0 && m.epoch != g_guestEpoch)
                continue; // чужая эпоха
            if (m.entries.size() > kMaxAppearanceComponents)
                m.entries.resize(kMaxAppearanceComponents);
            auto it = g_proxies.find(m.actorId);
            if (it == g_proxies.end())
            {
                // Спавн мог прийти позже: применим сразу после CREATE_PED.
                g_pendingAppearanceAssets[m.actorId] = m.entries;
                continue;
            }
            Proxy& p = it->second;
            if (p.kind != net::kActorKindHuman)
                continue; // E3c: animals assets не нужны
            if (p.outfitRequest)
            {
                ReleaseMetaPedOutfitRequest(p.outfitRequest);
                p.outfitRequest = 0;
            }
            p.appearanceAssets = m.entries;
            p.appearanceAssetsApplyNeeded = true;
            p.appearanceAssetsIssued = false;
            p.appearanceAssetsWaitTick = tick;
            p.appearanceState = 3; // E2a не трогаем: assets главнее
        }
    }
    appearanceMsgs.clear();

    // 3b) E2a: внешность — meta-outfit request/apply для новых и ожидающих
    // прокси (бюджет 2 REQUEST/тик, таймаут 3 с, не блокирует прокси).
    const unsigned long long perfAppearanceStart = perfOn ? QpcNow() : 0;
    ProcessActorAppearance(tick);

    // 3b2) E2c-b (v16): применение asset/tint (≤1 актор/тик).
    ProcessGuestAppearanceAssets(tick);
    if (perfOn)
        g_perfGuestAppearance.AddUs(QpcDeltaUs(perfAppearanceStart));

    // 3c) E3a: сценарии-позы (sit/work) + fallback stand при неудаче.
    ProcessActorScenarios(tick);

    // 4) кадры -> target позиция/флаги; 5) движение — p95 motion (E4).
    const unsigned long long perfMotionStart = perfOn ? QpcNow() : 0;
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
            // E3b: боевые флаги из кадров (targetActorId = shoot-пульс хоста).
            p.weaponDrawnFlag = (f.flags & net::kActorFlagDrawn) != 0;
            p.weaponChangingFlag = (f.flags & net::kActorFlagChanging) != 0;
            p.weaponAimFlag = (f.flags & net::kActorFlagAiming) != 0;
            p.weaponCombatFlag = (f.flags & net::kActorFlagCombat) != 0;
            p.weaponReloadFlag = (f.flags & net::kActorFlagReloading) != 0;
            p.weaponShootPulse = f.targetActorId != 0;
            // E3a: сценарий-флаг из кадров; смерть/оживление — health/invincible.
            p.scenarioFlag =
                (f.flags & net::kActorFlagScenario) != 0 && !p.mounted;
            if (p.dead && !p.deathApplied)
                ApplyProxyDeath(p);
            else if (!p.dead && p.deathApplied)
            {
                if (p.kind == net::kActorKindHuman)
                    ApplyProxyRevive(p);
                else
                    p.dead = true; // E3c: животных не оживляем — остаётся труп
            }
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
            PushHostSample(p, f.x, f.y, f.z, f.heading,
                           g_latestFramesRecvMs);
            p.lastFrameTick = tick;
            p.resyncLogged = false;
            ++g_appliedFrames;
        }
        g_haveFrames = false;
    }

    // 5) движение/поворот каждый тик
    for (auto& kv : g_proxies)
        MoveProxy(kv.second);
    if (perfOn)
        g_perfGuestMotion.AddUs(QpcDeltaUs(perfMotionStart));

    // E3b: боевые визуалы NPC (оружие/aim/shoot/reload) — после движения,
    // чтобы боевые задачи имели приоритет над walk-задачами.
    const unsigned long long perfWeaponsStart = perfOn ? QpcNow() : 0;
    ProcessActorWeapons(tick);
    if (perfOn)
        g_perfGuestWeapons.AddUs(QpcDeltaUs(perfWeaponsStart));

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
    // E4: остаток (mount delete + no-collision) входит в guest tick, не в motion.

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
                ++g_perfGuestDespawns;
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
        unsigned int animals = 0, horses = 0, corpses = 0;
        for (const auto& kv : g_proxies)
        {
            const Proxy& p = kv.second;
            if (p.kind == net::kActorKindHuman) continue;
            ++animals;
            if (p.kind == 2u) ++horses;
            if (p.dead) ++corpses;
        }
        sh_log::Write(
            "actor: proxies=%u appliedFrames=%u timeouts=%u "
            "animals=%u horses=%u corpses=%u\n",
            static_cast<unsigned int>(g_proxies.size()), g_appliedFrames,
            g_timeouts, animals, horses, corpses);
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
            // E5.3: диагностика сглаживания всадников/лошадей.
            sh_log::Write(
                "actor: motion mounted err_p95=%.2f retargets=%u\n",
                g_mountErr.P95Ms() / 1000.0, g_mountedRetargets);
            // E7.2: per-actor диагностика окна 5 с.
            const double winSec =
                static_cast<double>(kSummaryEveryTicks) / 60.0;
            for (auto& kv : g_proxies)
            {
                Proxy& p = kv.second;
                if (!p.winReissue && !p.winSnap && !p.winUpdate &&
                    !p.winGaitRestart && p.winErr.count == 0)
                    continue;
                unsigned int s1 = 0, s2 = 0, s3 = 0;
                for (int st = 0; st <= 8; ++st)
                {
                    const unsigned int v = p.winStatusHist[st];
                    if (v > p.winStatusHist[s1]) { s3 = s2; s2 = s1; s1 = st; }
                    else if (v > p.winStatusHist[s2]) { s3 = s2; s2 = st; }
                    else if (v > p.winStatusHist[s3]) { s3 = st; }
                }
                if (p.updateNativeOk && p.winUpdate > 0 &&
                    p.winReissue >= 2 && coopcfg::Get().npcMotionUpdate)
                {
                    p.updateNativeOk = false;
                    sh_log::Write(
                        "actor: motion warn update-native A/B id=%u "
                        "reissue=%u updates=%u -> fallback\n",
                        p.actorId, p.winReissue, p.winUpdate);
                }
                sh_log::Write(
                    "actor: motion id=%u reissue=%.2f/s snap=%.2f/s "
                    "target_err_p95=%.2f status=%u/%u/%u gait_restart=%u\n",
                    p.actorId, p.winReissue / winSec, p.winSnap / winSec,
                    p.winErr.P95Ms() / 1000.0, s1, s2, s3,
                    p.winGaitRestart);
                p.winReissue = 0;
                p.winSnap = 0;
                p.winUpdate = 0;
                p.winGaitRestart = 0;
                p.winErr.Clear();
                for (int st = 0; st <= 8; ++st)
                    p.winStatusHist[st] = 0;
            }
            g_motionIssued = 0;
            g_motionReissue = 0;
            g_motionGaitRestart = 0;
            g_motionSnap = 0;
            g_motionUpdateNative = 0;
            g_motionStuck = 0;
            g_motionErr.Clear();
            g_mountErr.Clear();
            g_mountedRetargets = 0;
        }
    }
}

} // namespace

// ======================= E4: perf NPC-подсистемы (npc_debug=1) =======================
// 1-2 строки раз в ~10 с: p95 фаз + счётчики окна. Без аллокаций: фикс.
// кольца PerfP95 (128 семплов) и счётчики; при npc_debug=0 не логируем и не
// снимаем QPC в новых фазах (см. perfOn в Tick/HostTick/GuestTick).
void ResetPerfWindow()
{
    g_perfHostTick.Clear();
    g_perfHostDiscover.Clear();
    g_perfHostCapture.Clear();
    g_perfHostBuild.Clear();
    g_perfGuestTick.Clear();
    g_perfGuestSpawn.Clear();
    g_perfGuestAppearance.Clear();
    g_perfGuestMotion.Clear();
    g_perfGuestWeapons.Clear();
    g_perfHostSpawns = 0;
    g_perfHostFrames = 0;
    g_perfHostDespawns = 0;
    g_perfHostBytes = 0;
    g_perfGuestSpawns = 0;
    g_perfGuestAppearanceApplied = 0;
    g_perfGuestAppearanceFailed = 0;
    g_perfGuestTimeouts = 0;
    g_perfGuestDespawns = 0;
}

void LogNpcPerfIfDue(unsigned int tick, bool host)
{
    if (!coopcfg::Get().npcDebug)
        return;
    if (g_lastPerfLogTick != 0 &&
        tick - g_lastPerfLogTick < kPerfLogEveryTicks)
        return;
    g_lastPerfLogTick = tick;

    if (host)
    {
        sh_log::Write(
            "perf: npc host tick=%.2f p95=%.2f max=%.2f | "
            "discover=%.2f p95=%.2f | capture=%.2f p95=%.2f | "
            "build=%.2f p95=%.2f\n",
            g_perfHostTick.AvgMs(), g_perfHostTick.P95Ms(),
            g_perfHostTick.MaxMs(),
            g_perfHostDiscover.AvgMs(), g_perfHostDiscover.P95Ms(),
            g_perfHostCapture.AvgMs(), g_perfHostCapture.P95Ms(),
            g_perfHostBuild.AvgMs(), g_perfHostBuild.P95Ms());
        sh_log::Write(
            "perf: npc host actors=%u spawns=%llu frames=%llu despawns=%llu "
            "bytes=%llu/s\n",
            static_cast<unsigned int>(g_roster.size()), g_perfHostSpawns,
            g_perfHostFrames, g_perfHostDespawns,
            kPerfWindowSeconds ? g_perfHostBytes / kPerfWindowSeconds
                               : g_perfHostBytes);
        if (g_perfHostTick.P95Ms() > kNpcPerfBudgetMs)
            sh_log::Write("perf: npc warn host tick=%.2f > %.1f ms\n",
                          g_perfHostTick.P95Ms(), kNpcPerfBudgetMs);
    }
    else
    {
        sh_log::Write(
            "perf: npc guest tick=%.2f p95=%.2f max=%.2f | "
            "spawn=%.2f p95=%.2f | appearance=%.2f p95=%.2f | "
            "motion=%.2f p95=%.2f | weapons=%.2f p95=%.2f\n",
            g_perfGuestTick.AvgMs(), g_perfGuestTick.P95Ms(),
            g_perfGuestTick.MaxMs(),
            g_perfGuestSpawn.AvgMs(), g_perfGuestSpawn.P95Ms(),
            g_perfGuestAppearance.AvgMs(), g_perfGuestAppearance.P95Ms(),
            g_perfGuestMotion.AvgMs(), g_perfGuestMotion.P95Ms(),
            g_perfGuestWeapons.AvgMs(), g_perfGuestWeapons.P95Ms());
        sh_log::Write(
            "perf: npc guest proxies=%u spawns=%llu applied=%llu failed=%llu "
            "timeouts=%llu despawns=%llu\n",
            static_cast<unsigned int>(g_proxies.size()), g_perfGuestSpawns,
            g_perfGuestAppearanceApplied, g_perfGuestAppearanceFailed,
            g_perfGuestTimeouts, g_perfGuestDespawns);
        if (g_perfGuestTick.P95Ms() > kNpcPerfBudgetMs)
            sh_log::Write("perf: npc warn guest tick=%.2f > %.1f ms\n",
                          g_perfGuestTick.P95Ms(), kNpcPerfBudgetMs);
    }
    ResetPerfWindow();
}

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
    const bool perfOn = coopcfg::Get().npcDebug;
    const unsigned long long perfTickStart = perfOn ? QpcNow() : 0;

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

    // E4: wall-time всего actor_sync::Tick и строки perf: npc ... (npc_debug=1).
    if (perfOn)
    {
        const unsigned long long us = QpcDeltaUs(perfTickStart);
        (ctx.amHost ? g_perfHostTick : g_perfGuestTick).AddUs(us);
    }
    LogNpcPerfIfDue(ctx.tickCount, ctx.amHost);
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

// E3d-c (v19): host — actorId для handle из roster (0/false если ещё не в
// roster; world_sync повторит на следующем 0x21).
bool GetHostActorIdForHandle(unsigned int handle, uint32_t& actorId)
{
    actorId = 0;
    auto it = g_roster.find(handle);
    if (it == g_roster.end())
        return false;
    actorId = it->second.actorId;
    return actorId != 0;
}

// E3d-c (v19): occupant-handles от world_sync (только реально сидящие в
// реплицируемом vehicle педы/лошади); действует до следующего обновления.
void SetOccupantHandles(const std::vector<unsigned int>& handles)
{
    g_occupantHandles.clear();
    for (unsigned int h : handles)
    {
        if (h)
            g_occupantHandles.insert(h);
    }
}

// E3d-c (v19): guest — пометить actor_as_seated (skip MoveProxy) или снять.
void SetActorSeated(uint32_t actorId, unsigned int vehicleHandle, int seat,
                    uint8_t flags)
{
    auto it = g_proxies.find(actorId);
    if (it == g_proxies.end())
        return;
    it->second.seatedInVehicle = true;
    it->second.seatedVehicle = vehicleHandle;
    it->second.seatedSeat = seat;
    it->second.occupantFlags = flags;
    it->second.draftAttached = false;
    it->second.taskActive = false;
    it->second.moveTaskValid = false;
}

void ClearActorSeated(uint32_t actorId)
{
    auto it = g_proxies.find(actorId);
    if (it == g_proxies.end())
        return;
    it->second.seatedInVehicle = false;
    it->second.seatedVehicle = 0;
    it->second.seatedSeat = -1;
    it->second.occupantFlags = 0;
}

void SetActorDraftAttached(uint32_t actorId, unsigned int vehicleHandle,
                           unsigned int harness)
{
    auto it = g_proxies.find(actorId);
    if (it == g_proxies.end())
        return;
    it->second.draftAttached = true;
    it->second.draftVehicle = vehicleHandle;
    it->second.draftHarness = harness;
    it->second.seatedInVehicle = false;
    it->second.taskActive = false;
    it->second.moveTaskValid = false;
}

void ClearActorDraftAttached(uint32_t actorId)
{
    auto it = g_proxies.find(actorId);
    if (it == g_proxies.end())
        return;
    it->second.draftAttached = false;
    it->second.draftVehicle = 0;
    it->second.draftHarness = 0;
}

// E5 (v17): гость — handle прокси -> actorId; false для не-прокси/мёртвых.
bool GetDamageTargetInfo(unsigned int handle, uint32_t& actorId)
{
    actorId = 0;
    if (!handle)
        return false;
    auto it = g_actorByHandle.find(handle);
    if (it == g_actorByHandle.end())
        return false;
    auto pit = g_proxies.find(it->second);
    if (pit == g_proxies.end() || pit->second.dead)
        return false;
    actorId = it->second;
    return true;
}

// E5 (v17): гость шлёт запрос урона хосту (0x1F). Лимиты: 100 мс на actorId,
// <=10/с суммарно; damage clamp 1..1000; при npc_actor/npc_damage=0 — ничего.
bool SendDamageToActor(unsigned int handle, uint16_t damage,
                       uint32_t weaponHash, bool headshot)
{
    if (!coopcfg::Get().npcActor || !coopcfg::Get().npcDamage)
        return false;
    if (g_guestEpoch == 0)
        return false;
    uint32_t actorId = 0;
    if (!GetDamageTargetInfo(handle, actorId))
        return false;
    uint16_t dmg = damage;
    if (dmg < net::kMinActorDamage) dmg = net::kMinActorDamage;
    if (dmg > net::kMaxActorDamage) dmg = net::kMaxActorDamage;

    const unsigned long long nowMs = GetTickCount64();
    static unsigned long long s_winMs = 0;
    static unsigned int s_winCount = 0;
    static std::unordered_map<uint32_t, unsigned long long> s_lastMs;
    if (s_winMs == 0 || nowMs - s_winMs >= 1000)
    {
        s_winMs = nowMs;
        s_winCount = 0;
    }
    if (s_winCount >= 10)
        return false; // общий лимит <=10/с
    auto last = s_lastMs.find(actorId);
    if (last != s_lastMs.end() && nowMs - last->second < 100)
        return false; // per-actor 100 мс
    if (!coop::SendActorDamage(g_guestEpoch, actorId, dmg, weaponHash))
        return false;
    s_lastMs[actorId] = nowMs;
    ++s_winCount;
    if (s_lastMs.size() > 64)
        s_lastMs.clear();
    if (coopcfg::Get().npcDebug)
        LogDamageSend(actorId, dmg, weaponHash, headshot);
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
    g_inbox.framesRecvMs = GetTickCount64();
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

// E2c-b (v16): asset/tint внешности NPC — только копия в inbox (сетевой поток).
void OnActorAppearance(uint32_t epoch, uint32_t actorId,
                       const std::vector<net::ActorAppearanceEntry>& entries)
{
    if (!coopcfg::Get().npcActor) return;
    std::lock_guard<std::mutex> lk(g_inboxMutex);
    if (g_inbox.appearances.size() >= 64) return; // защита от спама
    AppearanceMsg msg;
    msg.epoch = epoch;
    msg.actorId = actorId;
    msg.entries = entries;
    g_inbox.appearances.push_back(std::move(msg));
}

// E5 (v17): запрос урона от гостя — только копия в inbox под mutex.
void OnActorDamageRequest(uint32_t epoch, uint32_t actorId, uint16_t damage,
                          uint32_t weaponHash)
{
    if (!coopcfg::Get().npcActor) return;
    std::lock_guard<std::mutex> lk(g_inboxMutex);
    if (g_inbox.damage.size() >= 64) return; // защита от флуда
    net::ActorDamagePayload d{};
    d.epoch = epoch;
    d.actorId = actorId;
    d.damage = damage;
    d.weaponHash = weaponHash;
    g_inbox.damage.push_back(d);
}

// E7.1-hardening: forced full spawn batch для нового хоста (heartbeat now).
void ForceFullSpawnBatch()
{
    if (!coopcfg::Get().npcActor)
        return;
    g_resendAllSpawns = true;
    g_lastHeartbeatTick = 0; // следующий HostTick пошлёт всех
}

// E7.1-hardening: новый гость сразу просит resync у нового хоста.
void RequestImmediateResync()
{
    if (!coopcfg::Get().npcActor)
        return;
    // Эпоха после Reset неизвестна (0) — шлём ненулевой маркер; хост
    // OnActorResyncRequest эпоху не валидирует, только форсирует resend.
    const uint32_t epoch = g_guestEpoch ? g_guestEpoch : 1u;
    coop::SendActorResync(epoch, 0, 1);
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
